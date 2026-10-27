#include "platform/engine_c_api.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <stb_image.h>
#include "assets/TextureManager.h"
#include "engine/Engine.h"

// O tipo opaco da API C é o estado do motor (eng::Engine, em src/engine/Engine.h)
struct Engine : eng::Engine {};

using eng::AABB;
using eng::Animator;
using eng::AnimationClip;
using eng::ColliderType;
using eng::GameObject;
using eng::Gizmo;
using eng::Mat;
using eng::RigidBody;
using eng::RigidBodyPool;
using eng::Scene;
using eng::TouchPoint;

// ---------- ciclo de vida ----------
Engine* engine_create(const char* shader_dir) {
    eng::TextureManager::instance().forgetAll();  // contexto GL novo: ids antigos não valem
    Engine* e = new Engine();
    if (!e->renderer.init(shader_dir)) { delete e; return nullptr; }
    e->objects.reserve(Scene::kMaxObjects);
    eng::ensureBasics(e);
    return e;
}

void engine_destroy(Engine* e) {
    if (!e) return;
    eng::clearWorld(e);
    e->scene.forEach([&](GameObject& g, Scene::Handle) { eng::releaseComponents(e, g); });
    eng::TextureManager::instance().releaseAll();
    e->renderer.shutdown();
    delete e;
}

void engine_resize(Engine* e, int w, int h) {
    if (!e || w <= 0 || h <= 0) return;
    e->width = w; e->height = h;
    e->camera.aspect = float(w) / float(h);
    e->renderer.resize(w, h);
}

void engine_set_target_fbo(Engine* e, uint32_t fbo) { if (e) e->renderer.targetFbo = fbo; }

void engine_frame(Engine* e, float dt) {
    if (!e) return;
    auto t0 = std::chrono::steady_clock::now();
    dt = std::clamp(dt, 0.f, 0.1f);

    if (e->playing) {
        eng::updatePlayer(e, dt);
        e->physics.step(dt);
    } else {
        for (auto& t : e->tracers) t.life = 0.f;
    }
    for (Animator* a : e->animators) a->update(dt, e->playing);
    eng::updateWorld(e);

    // Câmera: near/far e distância de sombra acompanham o zoom e o tamanho do mapa
    float d = e->editorCam.distance;
    e->camera.nearZ = std::clamp(d * 0.01f, 0.05f, 5.f);
    e->camera.farZ = std::max(e->mapKind >= 0 ? 900.f : 200.f, d * 40.f);
    e->renderer.shadowDistance = std::clamp(d * 3.5f, e->playing ? 40.f : 12.f, 1500.f);
    e->editorCam.apply(e->camera);
    e->scene.updateTransforms();
    eng::updateAttachments(e);

    e->renderer.beginFrame(e->camera, e->sun);
    const glm::vec3 camPos = e->camera.position;
    e->scene.forEach([&](GameObject& go, Scene::Handle) {
        const eng::MeshRenderer* mr = go.meshRenderer;
        if (!mr) return;
        const glm::mat4* palette;
        glm::mat4 m = eng::renderMatrix(e, go, &palette);
        if (mr->lodNear > 0.f || mr->lodFar < 1e29f) {  // LOD por distância ao centro do mesh
            const eng::Mesh* mesh = e->renderer.mesh(mr->mesh);
            if (!mesh) return;
            glm::vec3 c = glm::vec3(m * glm::vec4((mesh->bounds.min + mesh->bounds.max) * 0.5f, 1.f));
            glm::vec3 d = c - camPos;
            float d2 = glm::dot(d, d);
            if (d2 < mr->lodNear * mr->lodNear || d2 >= mr->lodFar * mr->lodFar) return;
        }
        e->renderer.submit(go, m, palette);
    });

    glm::mat4 vp = e->camera.viewProj();
    if (!e->playing) {  // overlay do editor
        if (e->showGrid && e->mapKind < 0) {
            float step = d < 25.f ? 1.f : d < 250.f ? 10.f : 100.f;
            int n = 12;
            float ext = step * n;
            for (int i = -n; i <= n; ++i) {
                float x = i * step;
                glm::vec4 c = i == 0 ? glm::vec4(0.3f, 0.5f, 1.f, 0.6f) : glm::vec4(1.f, 1.f, 1.f, 0.13f);
                glm::vec4 c2 = i == 0 ? glm::vec4(1.f, 0.3f, 0.3f, 0.6f) : glm::vec4(1.f, 1.f, 1.f, 0.13f);
                e->renderer.line(vp, {x, 0.f, -ext}, {x, 0.f, ext}, c, 1.5f, true);
                e->renderer.line(vp, {-ext, 0.f, x}, {ext, 0.f, x}, c2, 1.5f, true);
            }
        }
        eng::drawMapOverlay(e, vp);
        if (GameObject* sel = e->scene.get(e->selected)) {
            AABB b;
            if (eng::subtreeBounds(e, *sel, b)) {
                glm::vec3 c[8];
                for (int k = 0; k < 8; ++k) c[k] = {k & 1 ? b.max.x : b.min.x, k & 2 ? b.max.y : b.min.y, k & 4 ? b.max.z : b.min.z};
                const int Ed[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (auto& ed : Ed) e->renderer.line(vp, c[ed[0]], c[ed[1]], {1.f, 0.6f, 0.1f, 0.9f}, 2.f, false);
            }
            e->gizmo.draw(e->renderer, e->camera);
        }
    }
    for (auto& t : e->tracers)
        if (t.life > 0.f) e->renderer.line(vp, t.a, t.b, {1.f, 0.85f, 0.4f, std::min(1.f, t.life * 12.f)}, 3.f, true);

    e->renderer.render(e->camera, e->sun, e->env, e->points, e->pointCount);

    // estatísticas (janela de 0,5 s)
    float cpu = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    e->statTime += dt;
    e->cpuAccum += cpu;
    ++e->statFrames;
    if (e->statTime >= 0.5f) {
        e->fps = e->statFrames / e->statTime;
        e->frameMs = e->statTime * 1000.f / e->statFrames;
        e->cpuMs = e->cpuAccum / e->statFrames;
        e->statTime = 0.f; e->cpuAccum = 0.f; e->statFrames = 0;
    }
}

// ---------- Play / Stop ----------
void engine_set_play(Engine* e, int playing) {
    if (!e) return;
    bool p = playing != 0;
    if (p == e->playing) return;
    if (p) {
        e->snapshot.clear();
        e->scene.forEach([&](GameObject& g, Scene::Handle h) {
            if (g.id >= 0) e->snapshot.push_back({h, g.transform.position, g.transform.scale, g.transform.rotation});
        });
        e->animSnap.clear();
        for (Animator* a : e->animators) e->animSnap.push_back({a->time, a->clipIndex});
        e->bodies.forEach([&](RigidBody& rb, RigidBodyPool::Handle) { e->physics.syncFromTransform(rb); });
        e->physics.resetAccumulator();
        e->gizmo.onTouchUp();
        e->savedCam = {e->editorCam.target, e->editorCam.yaw, e->editorCam.pitch, e->editorCam.distance};
        e->moveInput = glm::vec2(0.f);
        e->runInput = e->jumpRequest = e->fireRequest = false;
        if (e->player >= 0) {  // câmera atrás do jogador
            if (GameObject* pg = e->scene.get(e->characters[e->player].root)) {
                glm::vec3 f = pg->transform.rotation * glm::vec3(0, 0, 1);
                e->editorCam.yaw = std::atan2(-f.x, -f.z);
                e->editorCam.pitch = 0.3f;
                e->editorCam.distance = 5.5f;
            }
        }
    } else {
        for (auto& s : e->snapshot)
            if (GameObject* g = e->scene.get(s.h)) {
                g->transform.setPosition(s.p); g->transform.setRotation(s.r); g->transform.setScale(s.s);
            }
        for (size_t i = 0; i < e->animators.size() && i < e->animSnap.size(); ++i) {
            e->animators[i]->clipIndex = e->animSnap[i].clip;
            e->animators[i]->time = e->animSnap[i].time;
            e->animators[i]->speed = 1.f;
        }
        e->bodies.forEach([&](RigidBody& rb, RigidBodyPool::Handle) { e->physics.syncFromTransform(rb); });
        e->editorCam.target = e->savedCam.target; e->editorCam.yaw = e->savedCam.yaw;
        e->editorCam.pitch = e->savedCam.pitch; e->editorCam.distance = e->savedCam.distance;
    }
    e->playing = p;
}
int engine_is_playing(Engine* e) { return e && e->playing; }

// ---------- cenas ----------
void engine_new_scene(Engine* e) {
    if (!e) return;
    eng::clearScene(e);
    eng::addFloor(e);
    e->editorCam.target = glm::vec3(0.f, 0.5f, 0.f);
    e->editorCam.distance = 8.f;
}

void engine_load_demo(Engine* e) {
    if (!e) return;
    eng::clearScene(e);
    eng::addFloor(e);
    auto add = [&](const char* name, eng::MeshId mesh, eng::MaterialId m, glm::vec3 pos, glm::vec3 scale, glm::vec3 eul, ColliderType ct) {
        Scene::Handle h = eng::addMeshObject(e, name, mesh, m, pos, scale, eul);
        eng::addBody(e, h, ct, 1.f);
    };
    add("Cubo laranja", e->cube, e->matPalette[0], {-1.6f, 0.5f, 0.f}, glm::vec3(1.f), {0.f, 20.f, 0.f}, ColliderType::Box);
    add("Cubo metal", e->cube, e->matPalette[1], {0.f, 0.4f, -0.4f}, glm::vec3(0.8f), {0.f, 35.f, 0.f}, ColliderType::Box);
    add("Esfera ouro", e->sphere, e->matPalette[2], {1.5f, 0.5f, 0.3f}, glm::vec3(1.f), {}, ColliderType::Sphere);
    add("Esfera azul", e->sphere, e->matPalette[3], {0.3f, 0.35f, 1.3f}, glm::vec3(0.7f), {}, ColliderType::Sphere);
    add("Capsula", e->capsule, e->matPalette[1], {-0.4f, 0.6f, 1.8f}, glm::vec3(0.6f), {0.f, 0.f, 90.f}, ColliderType::Capsule);
    add("Pedra", e->rock, e->matPalette[5], {2.6f, 0.45f, -1.2f}, glm::vec3(1.f), {10.f, 30.f, 0.f}, ColliderType::ConvexMesh);
    e->points[0].position = {-2.5f, 2.f, 2.f};
    e->points[0].color = {1.f, 0.55f, 0.25f};
    e->points[0].intensity = 8.f;
    e->points[0].radius = 8.f;
    e->pointCount = 1;
    e->editorCam.target = glm::vec3(0.f, 0.4f, 0.f);
    e->editorCam.distance = 7.f;
    e->editorCam.yaw = 0.6f; e->editorCam.pitch = 0.45f;
    eng::selectHandle(e, e->objects.size() > 2 ? e->objects[2] : Scene::Handle{});
}

void engine_load_physics_demo(Engine* e) {
    if (!e) return;
    eng::clearScene(e);
    eng::addFloor(e);
    int k = 0;
    for (int row = 0; row < 5; ++row)
        for (int i = 0; i < 5 - row; ++i) {
            char name[32]; std::snprintf(name, sizeof(name), "Caixa %d", ++k);
            Scene::Handle h = eng::addMeshObject(e, name, e->cube, e->matPalette[4],
                                                 {(i - (4 - row) * 0.5f) * 0.82f, 0.4f + row * 0.8f, 0.f}, glm::vec3(0.8f));
            eng::addBody(e, h, ColliderType::Box, 1.f);
        }
    for (int i = 0; i < 12; ++i) {
        char name[32];
        float x = -2.5f + (i % 4) * 1.6f, z = -1.5f + (i / 4) * 1.4f, y = 5.f + (i % 3) * 1.2f;
        glm::vec3 eul(i * 23.f, i * 41.f, i * 17.f);
        int kind = i % 3;
        Scene::Handle h;
        if (kind == 0) {
            std::snprintf(name, sizeof(name), "Esfera %d", i);
            h = eng::addMeshObject(e, name, e->sphere, e->matPalette[i % 2 ? 2 : 3], {x, y, z}, glm::vec3(0.7f));
            eng::addBody(e, h, ColliderType::Sphere, 1.f);
        } else if (kind == 1) {
            std::snprintf(name, sizeof(name), "Capsula %d", i);
            h = eng::addMeshObject(e, name, e->capsule, e->matPalette[0], {x, y, z}, glm::vec3(0.5f), eul);
            eng::addBody(e, h, ColliderType::Capsule, 1.f);
        } else {
            std::snprintf(name, sizeof(name), "Pedra %d", i);
            h = eng::addMeshObject(e, name, e->rock, e->matPalette[5], {x, y, z}, glm::vec3(0.9f), eul);
            eng::addBody(e, h, ColliderType::ConvexMesh, 2.f);
        }
    }
    e->editorCam.target = glm::vec3(0.f, 1.5f, 0.f);
    e->editorCam.distance = 13.f;
    e->editorCam.yaw = 0.5f; e->editorCam.pitch = 0.35f;
}

int engine_load_model(Engine* e, const char* path) {
    if (!e || !path) return -1;
    auto model = std::make_unique<eng::LoadedModel>();
    eng::LoadContext ctx{e->renderer, e->scene, e->meshRenderers, e->animatorPool};
    if (!eng::loadGltf(path, ctx, *model)) {
        e->lastError = model->error;
        for (auto h : model->allNodes) if (GameObject* g = e->scene.get(h)) eng::releaseComponents(e, *g);
        for (auto h : model->roots) e->scene.destroy(h);
        return -1;
    }
    int rootId = -1;
    for (auto h : model->allNodes) {
        int id = eng::registerObject(e, h);
        if (rootId < 0) rootId = id;
    }
    for (Animator* a : model->animators) e->animators.push_back(a);
    e->models.push_back(std::move(model));
    e->lastError.clear();

    // Modelo pousado no chão e centrado no foco; enquadra a câmera nele
    if (GameObject* root = eng::objectById(e, rootId)) {
        e->scene.updateTransforms();
        AABB b;
        if (eng::subtreeBounds(e, *root, b)) {
            glm::vec3 c = (b.min + b.max) * 0.5f;
            glm::vec3 at = e->mapKind >= 0 ? e->editorCam.target : glm::vec3(0.f);
            float ground = eng::mapHeight(e, at.x, at.z);
            root->transform.setPosition(root->transform.position + glm::vec3(at.x - c.x, ground - b.min.y, at.z - c.z));
            float extent = std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 4.f;
            e->scene.forEach([&](GameObject& g, Scene::Handle) {
                if (std::strcmp(g.name, "Chao") != 0 || g.transform.scale.x >= extent) return;
                g.transform.setScale({extent, g.transform.scale.y, extent});
                if (g.rigidBody) e->physics.syncFromTransform(*g.rigidBody);
            });
        }
        eng::selectHandle(e, e->objects[rootId]);
        eng::focusSceneOrSelection(e);
    }
    return rootId;
}

const char* engine_last_error(Engine* e) { return e ? e->lastError.c_str() : ""; }

int engine_add_object(Engine* e, int kind, int withPhysics) {
    if (!e) return -1;
    eng::ensureBasics(e);
    static int counter = 0;
    char name[32];
    eng::MeshId mesh = e->cube; ColliderType ct = ColliderType::Box; eng::MaterialId mat = e->matPalette[0];
    glm::vec3 scale(1.f);
    switch (kind) {
        case 1: mesh = e->sphere; ct = ColliderType::Sphere; mat = e->matPalette[3]; std::snprintf(name, sizeof(name), "Esfera %d", ++counter); break;
        case 2: mesh = e->capsule; ct = ColliderType::Capsule; mat = e->matPalette[2]; scale = glm::vec3(0.6f); std::snprintf(name, sizeof(name), "Capsula %d", ++counter); break;
        case 3: mesh = e->rock; ct = ColliderType::ConvexMesh; mat = e->matPalette[5]; std::snprintf(name, sizeof(name), "Pedra %d", ++counter); break;
        default: std::snprintf(name, sizeof(name), "Cubo %d", ++counter); break;
    }
    glm::vec3 pos = e->editorCam.target;
    pos.y = std::max(pos.y, eng::mapHeight(e, pos.x, pos.z)) + 2.f;  // nasce acima do foco (cai no Play)
    Scene::Handle h = eng::addMeshObject(e, name, mesh, mat, pos, scale);
    if (withPhysics) eng::addBody(e, h, ct, 1.f);
    eng::selectHandle(e, h);
    GameObject* g = e->scene.get(h);
    return g ? g->id : -1;
}

void engine_delete_selected(Engine* e) {
    if (!e) return;
    GameObject* sel = e->scene.get(e->selected);
    if (!sel) return;
    std::vector<const eng::Transform3D*> gone;
    Scene::forEachInSubtree(*sel, [&](GameObject& g) { gone.push_back(&g.transform); });
    auto isGone = [&](const eng::Transform3D* t) { return t && std::find(gone.begin(), gone.end(), t) != gone.end(); };
    // armas presas a personagens apagados (ou a própria arma apagada) saem da lista
    e->attachments.erase(std::remove_if(e->attachments.begin(), e->attachments.end(), [&](const eng::Attachment& a) {
        GameObject* o = e->scene.get(a.obj);
        return !o || isGone(&o->transform) || (a.anim && isGone(a.anim->space));
    }), e->attachments.end());
    for (Animator* a : e->animators) if (isGone(a->space)) a->space = nullptr;
    for (size_t i = 0; i < e->characters.size(); ++i) {
        eng::CharacterInfo& c = e->characters[i];
        GameObject* r = e->scene.get(c.root);
        if (r && isGone(&r->transform)) {
            c.root = {}; c.anim = nullptr;
            if (e->player == int(i)) e->player = -1;
        }
        GameObject* w = e->scene.get(c.weapon);
        if (w && isGone(&w->transform)) { c.weapon = {}; c.weaponKind = -1; }
    }
    Scene::forEachInSubtree(*sel, [&](GameObject& g) { eng::releaseComponents(e, g); });
    e->scene.destroy(e->selected);
    eng::selectHandle(e, {});
}

void engine_focus_selected(Engine* e) { if (e) eng::focusSceneOrSelection(e); }

// ---------- hierarquia / inspetor ----------
int engine_object_count(Engine* e) { return e ? int(e->objects.size()) : 0; }
int engine_object_valid(Engine* e, int id) { return eng::objectById(e, id) != nullptr; }
const char* engine_object_name(Engine* e, int id) { GameObject* g = eng::objectById(e, id); return g ? g->name : ""; }
int engine_object_parent(Engine* e, int id) {
    GameObject* g = eng::objectById(e, id);
    return g && g->transform.parent && g->transform.parent->owner ? g->transform.parent->owner->id : -1;
}
int engine_object_depth(Engine* e, int id) { GameObject* g = eng::objectById(e, id); return g ? eng::hierarchyDepth(g->transform) : 0; }

void engine_select(Engine* e, int id) {
    if (!e) return;
    eng::selectHandle(e, eng::objectById(e, id) ? e->objects[id] : Scene::Handle{});
}
int engine_selected(Engine* e) { GameObject* g = e ? e->scene.get(e->selected) : nullptr; return g ? g->id : -1; }

int engine_get_transform(Engine* e, int id, float* p, float* r, float* s) {
    GameObject* g = eng::objectById(e, id);
    if (!g) return 0;
    glm::vec3 eu = g->transform.eulerDegrees();
    for (int i = 0; i < 3; ++i) { p[i] = g->transform.position[i]; r[i] = eu[i]; s[i] = g->transform.scale[i]; }
    return 1;
}
void engine_set_transform(Engine* e, int id, const float* p, const float* r, const float* s) {
    GameObject* g = eng::objectById(e, id);
    if (!g) return;
    g->transform.setPosition({p[0], p[1], p[2]});
    g->transform.setEulerDegrees({r[0], r[1], r[2]});
    g->transform.setScale({s[0], s[1], s[2]});
    if (g->rigidBody) e->physics.syncFromTransform(*g->rigidBody);
}
void engine_set_gizmo_mode(Engine* e, int m) {
    if (e) e->gizmo.mode = m == 1 ? Gizmo::Mode::Rotate : m == 2 ? Gizmo::Mode::Scale : Gizmo::Mode::Translate;
}

// ---------- visual ----------
void engine_set_exposure(Engine* e, float x) { if (e) e->env.exposure = std::clamp(x, 0.05f, 8.f); }
float engine_get_exposure(Engine* e) { return e ? e->env.exposure : 1.f; }
void engine_set_sun_intensity(Engine* e, float x) { if (e) e->sun.intensity = std::clamp(x, 0.f, 20.f); }
float engine_get_sun_intensity(Engine* e) { return e ? e->sun.intensity : 0.f; }
void engine_set_grid(Engine* e, int v) { if (e) e->showGrid = v != 0; }

// ---------- animação ----------
static void animatorsOfSelection(Engine* e, std::vector<Animator*>& out) {
    out.clear();
    if (GameObject* sel = e->scene.get(e->selected))
        Scene::forEachInSubtree(*sel, [&](GameObject& g) {
            if (g.animator && std::find(out.begin(), out.end(), g.animator) == out.end()) out.push_back(g.animator);
        });
    if (out.empty()) out = e->animators;
}
int engine_cycle_animation(Engine* e) {
    if (!e) return -1;
    std::vector<Animator*> list;
    animatorsOfSelection(e, list);
    int idx = -1;
    for (Animator* a : list) if (a->clipCount > 0) { a->setClip(a->clipIndex + 1); idx = a->clipIndex; }
    return idx;
}
const char* engine_animation_name(Engine* e) {
    if (!e) return "";
    std::vector<Animator*> list;
    animatorsOfSelection(e, list);
    for (Animator* a : list) if (const AnimationClip* c = a->clip()) return c->name;
    return "";
}

// ---------- estatísticas ----------
void engine_get_stats(Engine* e, float* o) {
    if (!e || !o) return;
    const eng::FrameStats& s = e->renderer.stats();
    o[0] = e->fps; o[1] = e->frameMs; o[2] = e->cpuMs;
    o[3] = float(s.drawCalls); o[4] = float(s.triangles); o[5] = float(e->scene.count());
}

// ---------- criação procedural ----------
int engine_create_character(Engine* e, const float* p) {
    if (!e || !p) return -1;
    eng::CharacterParams c;
    auto col = [&](int i) { return eng::srgb(p[i] * 255.f, p[i + 1] * 255.f, p[i + 2] * 255.f); };
    c.height = std::clamp(p[0], 1.f, 2.4f);
    c.build = std::clamp(int(p[1] + 0.5f), 0, 2);
    c.skin = col(2); c.shirt = col(5); c.pants = col(8); c.shoes = col(11); c.hair = col(14);
    c.hairStyle = std::clamp(int(p[17] + 0.5f), 0, 3);
    c.longSleeves = p[18] > 0.5f;
    glm::vec3 pos = e->editorCam.target;
    pos.y = e->mapKind < 0 ? 0.f : eng::mapHeight(e, pos.x, pos.z) + 0.05f;
    int id = eng::createCharacter(e, c, pos, 0.f, p[19] > 0.5f);
    if (id >= 0) eng::selectHandle(e, e->objects[id]);
    return id;
}

int engine_create_random_character(Engine* e, uint32_t seed, int controllable) {
    if (!e) return -1;
    glm::vec3 pos = e->editorCam.target;
    pos.y = e->mapKind < 0 ? 0.f : eng::mapHeight(e, pos.x, pos.z) + 0.05f;
    int id = eng::createCharacter(e, eng::randomCharacter(seed), pos, 0.f, controllable != 0);
    if (id >= 0) eng::selectHandle(e, e->objects[id]);
    return id;
}

int engine_create_weapon(Engine* e, int kind) {
    if (!e) return -1;
    int ch = eng::characterOfSelection(e);
    if (ch < 0 && engine_has_player(e)) ch = e->player;
    int id = eng::createWeapon(e, kind, ch);
    if (ch < 0 && id >= 0) eng::selectHandle(e, e->objects[id]);
    return id;
}

int engine_create_building(Engine* e, int kind, uint32_t seed) {
    if (!e) return -1;
    glm::vec3 pos = e->editorCam.target;
    Scene::Handle h = eng::createBuilding(e, kind, seed, pos);
    eng::selectHandle(e, h);
    GameObject* g = e->scene.get(h);
    return g ? g->id : -1;
}

void engine_load_map(Engine* e, int kind, uint32_t seed) {
    if (!e) return;
    eng::loadMap(e, std::clamp(kind, 0, int(eng::MAP_Count) - 1), seed ? seed : 1);
}
int engine_map_kind(Engine* e) { return e ? e->mapKind : -1; }
void engine_map_overview(Engine* e) {
    if (!e) return;
    if (e->mapKind >= 0) eng::mapOverview(e);
    else { eng::selectHandle(e, {}); eng::focusSceneOrSelection(e); }
}

// ---------- materiais ----------
int engine_material_count(Engine*) { return int(Mat::Count); }
const char* engine_material_name(Engine*, int i) { return i >= 0 && i < int(Mat::Count) ? eng::matName(Mat(i)) : ""; }

template <class F> static void forSelectedRenderers(Engine* e, F&& f) {
    GameObject* sel = e->scene.get(e->selected);
    if (!sel) return;
    Scene::forEachInSubtree(*sel, [&](GameObject& g) { if (g.meshRenderer) f(*g.meshRenderer); });
}

void engine_set_object_material(Engine* e, int i) {
    if (!e || i < 0 || i >= int(Mat::Count)) return;
    forSelectedRenderers(e, [&](eng::MeshRenderer& mr) { mr.material = e->mats[i]; });
}

void engine_set_object_color(Engine* e, float r, float g, float b) {
    if (!e) return;
    glm::vec3 c(std::clamp(r, 0.f, 1.f), std::clamp(g, 0.f, 1.f), std::clamp(b, 0.f, 1.f));
    forSelectedRenderers(e, [&](eng::MeshRenderer& mr) { mr.material = eng::tintedMaterial(e, mr.material, c); });
}

int engine_set_object_texture(Engine* e, const char* path) {
    if (!e || !path) return 0;
    int w, h, comp;
    unsigned char* px = stbi_load(path, &w, &h, &comp, 4);
    if (!px) { e->lastError = "não consegui ler a imagem"; return 0; }
    GLuint tex = eng::TextureManager::instance().fromPixels(px, w, h, 4, true);
    stbi_image_free(px);
    std::vector<std::pair<eng::MaterialId, eng::MaterialId>> remap;
    forSelectedRenderers(e, [&](eng::MeshRenderer& mr) {
        for (auto& [from, to] : remap) if (from == mr.material) { mr.material = to; return; }
        const eng::Material* m = e->renderer.material(mr.material);
        if (!m) return;
        eng::Material copy = *m;
        copy.albedo = tex;
        copy.albedoFactor = glm::vec4(1.f);
        copy.mode = 0;
        eng::MaterialId id = e->renderer.createMaterial(copy);
        remap.push_back({mr.material, id});
        mr.material = id;
    });
    return 1;
}

// ---------- jogador ----------
int engine_has_player(Engine* e) {
    return e && e->player >= 0 && e->player < int(e->characters.size()) && e->scene.get(e->characters[e->player].root);
}
void engine_set_player(Engine* e) { if (e) { int c = eng::characterOfSelection(e); if (c >= 0) eng::setPlayer(e, c); } }
void engine_player_input(Engine* e, float x, float y, int run) {
    if (!e) return;
    e->moveInput = glm::vec2(std::clamp(x, -1.f, 1.f), std::clamp(y, -1.f, 1.f));
    e->runInput = run != 0;
}
void engine_player_jump(Engine* e) { if (e) e->jumpRequest = true; }
void engine_player_fire(Engine* e) { if (e) e->fireRequest = true; }
int engine_player_weapon(Engine* e) { return engine_has_player(e) ? e->characters[e->player].weaponKind : -1; }

// ---------- toque ----------
static void handleTap(Engine* e, glm::vec2 p) {
    glm::vec3 o, d;
    e->camera.screenRay(p.x, p.y, float(e->width), float(e->height), o, d);
    if (e->playing) {
        // Play: empurra o corpo tocado
        glm::vec3 hit; GameObject* obj = nullptr;
        if (e->physics.raycast(o, o + d * 1000.f, hit, obj) && obj && obj->rigidBody && obj->rigidBody->mass > 0.f) {
            RigidBody* rb = obj->rigidBody;
            rb->addImpulse((d * 4.f + glm::vec3(0, 3.f, 0)) * rb->mass);
        }
        return;
    }
    // Editor: objeto mais próximo (raio x AABB). Cenário de mapa (sem id) não é selecionável.
    float best = 1e30f;
    GameObject* hitGo = nullptr;
    e->scene.forEach([&](GameObject& g, Scene::Handle) {
        if (!g.meshRenderer || !g.active || g.id < 0) return;
        const eng::Mesh* m = e->renderer.mesh(g.meshRenderer->mesh);
        if (!m) return;
        const glm::mat4* pal;
        float t;
        if (eng::rayAABB(o, d, eng::transformAABB(m->bounds, eng::renderMatrix(e, g, &pal)), t) && t < best) { best = t; hitGo = &g; }
    });
    if (!hitGo) { eng::selectHandle(e, {}); return; }
    GameObject* root = hitGo;
    while (root->transform.parent && root->transform.parent->owner && root->transform.parent->owner->id >= 0) root = root->transform.parent->owner;
    // Toque de novo no mesmo objeto já selecionado: desce para a peça tocada
    GameObject* cur = e->scene.get(e->selected);
    eng::selectHandle(e, Scene::handleOf(cur == root ? *hitGo : *root));
}

void engine_touch(Engine* e, int action, int n, const int* ids, const float* xy) {
    if (!e) return;
    TouchPoint t[2];
    int m = n < 2 ? n : 2;
    for (int i = 0; i < m; ++i) t[i] = {ids[i], xy[i * 2], xy[i * 2 + 1]};
    float w = float(e->width), h = float(e->height);
    if (action == 0) {
        if (m == 1) {
            e->tapCandidate = true;
            e->tapStart = {t[0].x, t[0].y};
            e->gizmoGrabbed = !e->playing && e->gizmo.onTouchDown(e->camera, {t[0].x, t[0].y}, w, h);
        } else {
            e->tapCandidate = false;
            e->gizmoGrabbed = false;
            e->gizmo.onTouchUp();
        }
        if (!e->gizmoGrabbed) e->editorCam.onTouchBegin(t, m);
    } else if (action == 1) {
        if (m >= 1 && glm::length(glm::vec2(t[0].x, t[0].y) - e->tapStart) > 14.f) e->tapCandidate = false;
        if (e->gizmoGrabbed && m >= 1) e->gizmo.onTouchMove(e->camera, {t[0].x, t[0].y}, w, h);
        else if (m >= 1) {
            // no Play com jogador, 2 dedos não arrastam a câmera para longe dele (só zoom)
            glm::vec3 keep = e->editorCam.target;
            e->editorCam.onTouchMove(t, m, h, e->camera);
            if (e->playing && e->player >= 0) e->editorCam.target = keep;
        }
    } else {
        if (e->tapCandidate && !e->gizmoGrabbed && m >= 1) handleTap(e, {t[0].x, t[0].y});
        if (e->gizmoGrabbed) {
            if (GameObject* g = e->gizmo.target; g && g->rigidBody) e->physics.syncFromTransform(*g->rigidBody);
        }
        e->tapCandidate = false;
        e->gizmoGrabbed = false;
        e->gizmo.onTouchUp();
        e->editorCam.onTouchEnd(t, m);
    }
}
