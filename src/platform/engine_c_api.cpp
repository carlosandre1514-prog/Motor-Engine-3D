#include "platform/engine_c_api.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
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

// Cria pelo formato de projeto (receita salva no objeto) e seleciona
static int createSpec(Engine* e, const std::string& spec, bool select = true) {
    std::string err;
    int id = eng::createFromSpecJson(e, spec, &err);
    if (id >= 0 && select) eng::selectHandle(e, e->objects[id]);
    return id;
}
static std::string fmt3(const char* key, glm::vec3 v) {
    char b[96];
    std::snprintf(b, sizeof(b), "\"%s\":[%.3f,%.3f,%.3f]", key, v.x, v.y, v.z);
    return b;
}
// Edição de material/cor/textura vai para a receita (raiz) ou para "pecas" (peça selecionada)
static void recordEdit(Engine* e, const char* key, const std::string& value) {
    GameObject* sel = e->scene.get(e->selected);
    GameObject* root = eng::rootOf(e, sel);
    if (!root) return;
    if (root == sel) {
        if (!std::strcmp(key, "material")) { eng::recipeSet(e, *root, "cor", "null"); eng::recipeSet(e, *root, "textura", "null"); eng::recipeSet(e, *root, "pecas", "null"); }
        if (!std::strcmp(key, "textura")) eng::recipeSet(e, *root, "cor", "null");
        eng::recipeSet(e, *root, key, value);
        return;
    }
    int idx = 0, found = -1;
    Scene::forEachInSubtree(*root, [&](GameObject& g) { if (&g == sel) found = idx; ++idx; });
    if (found < 0) return;
    eng::recipePartSet(e, *root, found, key, value);
}

// ---------- ciclo de vida ----------
Engine* engine_create(const char* shader_dir) {
    eng::TextureManager::instance().forgetAll();  // contexto GL novo: ids antigos não valem
    Engine* e = new Engine();
    if (!e->renderer.init(shader_dir)) { delete e; return nullptr; }
    if (e->particles.init(e->renderer)) e->renderer.particles = &e->particles;
    e->objects.reserve(Scene::kMaxObjects);
    eng::ensureBasics(e);
    return e;
}

void engine_destroy(Engine* e) {
    if (!e) return;
    eng::clearWorld(e);
    e->scene.forEach([&](GameObject& g, Scene::Handle) { eng::releaseComponents(e, g); });
    eng::TextureManager::instance().releaseAll();
    e->particles.shutdown();
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
        eng::gameUpdate(e, dt);
        e->physics.step(dt);
    } else {
        for (auto& t : e->tracers) t.life = 0.f;
    }
    for (Animator* a : e->animators) a->update(dt, e->playing || e->previewAnim);
    eng::updateWorld(e);
    eng::updateEffects(e, dt);

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
    eng::drawEffectsOverlay(e, vp, !e->playing);
    if (!e->playing) eng::drawAnimEditOverlay(e, vp);

    // luzes: as da cena + as mais relevantes dos efeitos (fogo, magia, explosões)
    eng::PointLight lights[eng::kMaxPointLights];
    int nLights = eng::gatherLights(e, lights, eng::kMaxPointLights);
    e->renderer.render(e->camera, e->sun, e->env, lights, nLights);

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
        e->playing = true;  // a lógica de jogo só cria/danifica com o Play ligado
        eng::gameStart(e);
        if (e->player >= 0) {  // câmera atrás do jogador
            if (GameObject* pg = e->scene.get(e->characters[e->player].root)) {
                glm::vec3 f = pg->transform.rotation * glm::vec3(0, 0, 1);
                e->editorCam.yaw = std::atan2(-f.x, -f.z);
                e->editorCam.pitch = 0.3f;
                e->editorCam.distance = 5.5f;
            }
        }
    } else {
        eng::gameStop(e);
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
    createSpec(e, R"({"tipo":"chao","tamanho":40})", false);
    e->editorCam.target = glm::vec3(0.f, 0.5f, 0.f);
    e->editorCam.distance = 8.f;
}

void engine_load_demo(Engine* e) {
    if (!e) return;
    eng::clearScene(e);
    createSpec(e, R"({"tipo":"chao","tamanho":40})", false);
    const char* objs[] = {
        R"({"tipo":"forma","forma":"cubo","paleta":"laranja","nome":"Cubo laranja","pos":[-1.6,0.5,0],"rot":[0,20,0]})",
        R"({"tipo":"forma","forma":"cubo","paleta":"aco","nome":"Cubo metal","pos":[0,0.4,-0.4],"escala":[0.8,0.8,0.8],"rot":[0,35,0]})",
        R"({"tipo":"forma","forma":"esfera","paleta":"ouro","nome":"Esfera ouro","pos":[1.5,0.5,0.3]})",
        R"({"tipo":"forma","forma":"esfera","paleta":"azul","nome":"Esfera azul","pos":[0.3,0.35,1.3],"escala":[0.7,0.7,0.7]})",
        R"({"tipo":"forma","forma":"capsula","paleta":"aco","nome":"Capsula","pos":[-0.4,0.6,1.8],"escala":[0.6,0.6,0.6],"rot":[0,0,90]})",
        R"({"tipo":"forma","forma":"pedra","paleta":"pedra","nome":"Pedra","pos":[2.6,0.45,-1.2],"rot":[10,30,0]})",
        R"({"tipo":"luz","nome":"Luz","pos":[-2.5,2,2],"cor":[1,0.55,0.25],"intensidade":8,"raio":8})"};
    for (const char* o : objs) createSpec(e, o, false);
    e->editorCam.target = glm::vec3(0.f, 0.4f, 0.f);
    e->editorCam.distance = 7.f;
    e->editorCam.yaw = 0.6f; e->editorCam.pitch = 0.45f;
    eng::selectHandle(e, e->objects.size() > 2 ? e->objects[2] : Scene::Handle{});
}

void engine_load_physics_demo(Engine* e) {
    if (!e) return;
    eng::clearScene(e);
    createSpec(e, R"({"tipo":"chao","tamanho":40})", false);
    char spec[320];
    int k = 0;
    for (int row = 0; row < 5; ++row)
        for (int i = 0; i < 5 - row; ++i) {
            std::snprintf(spec, sizeof(spec), R"({"tipo":"forma","forma":"cubo","paleta":"madeira","nome":"Caixa %d","pos":[%.3f,%.3f,0],"escala":[0.8,0.8,0.8]})",
                          ++k, (i - (4 - row) * 0.5f) * 0.82f, 0.4f + row * 0.8f);
            createSpec(e, spec, false);
        }
    static const char* forms[] = {"esfera", "capsula", "pedra"};
    for (int i = 0; i < 12; ++i) {
        float x = -2.5f + (i % 4) * 1.6f, z = -1.5f + (i / 4) * 1.4f, y = 5.f + (i % 3) * 1.2f;
        int kind = i % 3;
        const char* pal = kind == 0 ? (i % 2 ? "ouro" : "azul") : kind == 1 ? "laranja" : "pedra";
        float sc = kind == 0 ? 0.7f : kind == 1 ? 0.5f : 0.9f;
        std::snprintf(spec, sizeof(spec), R"({"tipo":"forma","forma":"%s","paleta":"%s","nome":"%s %d","pos":[%.2f,%.2f,%.2f],"escala":[%.2f,%.2f,%.2f],"rot":[%d,%d,%d],"massa":%d})",
                      forms[kind], pal, kind == 0 ? "Esfera" : kind == 1 ? "Capsula" : "Pedra", i, x, y, z, sc, sc, sc,
                      kind ? i * 23 : 0, kind ? i * 41 : 0, kind ? i * 17 : 0, kind == 2 ? 2 : 1);
        createSpec(e, spec, false);
    }
    e->editorCam.target = glm::vec3(0.f, 1.5f, 0.f);
    e->editorCam.distance = 13.f;
    e->editorCam.yaw = 0.5f; e->editorCam.pitch = 0.35f;
}

int engine_load_model(Engine* e, const char* path) {
    if (!e || !path) return -1;
    std::string spec = "{\"tipo\":\"modelo\",\"arquivo\":" + eng::jsonString(path) + "}";
    int rootId = createSpec(e, spec, false);
    if (rootId < 0) return -1;
    e->lastError.clear();
    // chão cresce para caber o modelo; câmera enquadra
    if (GameObject* root = eng::objectById(e, rootId)) {
        e->scene.updateTransforms();
        AABB b;
        if (eng::subtreeBounds(e, *root, b)) {
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
    static const char* forms[] = {"cubo", "esfera", "capsula", "pedra"};
    char spec[96];
    std::snprintf(spec, sizeof(spec), R"({"tipo":"forma","forma":"%s","fisica":%s})", forms[std::clamp(kind, 0, 3)], withPhysics ? "true" : "false");
    return createSpec(e, spec);
}

void engine_delete_selected(Engine* e) {
    if (!e || !e->scene.get(e->selected)) return;
    eng::deleteObject(e, e->selected);
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
    ++e->revision;
}
void engine_set_gizmo_mode(Engine* e, int m) {
    if (e) e->gizmo.mode = m == 1 ? Gizmo::Mode::Rotate : m == 2 ? Gizmo::Mode::Scale : Gizmo::Mode::Translate;
}

// ---------- visual ----------
void engine_set_exposure(Engine* e, float x) { if (e) { e->env.exposure = std::clamp(x, 0.05f, 8.f); ++e->revision; } }
float engine_get_exposure(Engine* e) { return e ? e->env.exposure : 1.f; }
void engine_set_sun_intensity(Engine* e, float x) { if (e) { e->sun.intensity = std::clamp(x, 0.f, 20.f); ++e->revision; } }
float engine_get_sun_intensity(Engine* e) { return e ? e->sun.intensity : 0.f; }
void engine_set_grid(Engine* e, int v) { if (e) { e->showGrid = v != 0; ++e->revision; } }

// ---------- animação ----------
static void animatorsOfSelection(Engine* e, std::vector<Animator*>& out) {
    out.clear();
    if (GameObject* sel = eng::rootOf(e, e->scene.get(e->selected)))  // toque numa peça vale pelo modelo inteiro
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
const char* engine_animation_list(Engine* e) {
    if (!e) return "";
    std::vector<Animator*> list;
    animatorsOfSelection(e, list);
    e->toolResult.clear();
    for (Animator* a : list) {
        if (a->clipCount <= 0) continue;
        for (int i = 0; i < a->clipCount; ++i) {
            const AnimationClip* c = a->clipAt(i);
            e->toolResult += c && c->name[0] ? std::string(c->name) : "Animação " + std::to_string(i + 1);
            if (a->retarget && i < a->refCount) e->toolResult += " (motor)";
            e->toolResult += '\n';
        }
        break;
    }
    return e->toolResult.c_str();
}
int engine_animation_index(Engine* e) {
    if (!e) return -1;
    std::vector<Animator*> list;
    animatorsOfSelection(e, list);
    for (Animator* a : list) if (a->clipCount > 0) return a->clipIndex;
    return -1;
}
void engine_set_animation(Engine* e, int index) {
    if (!e) return;
    std::vector<Animator*> list;
    animatorsOfSelection(e, list);
    for (Animator* a : list) if (a->clipCount > 0) a->setClip(index);
}
void engine_set_anim_preview(Engine* e, int on) { if (e) e->previewAnim = on != 0; }

// ---------- editor de animação ----------
int engine_anim_edit_begin(Engine* e) { return e ? eng::animEditBegin(e) : -1; }
const char* engine_anim_edit_bones(Engine* e) { if (!e) return ""; e->toolResult = eng::animEditBones(e); return e->toolResult.c_str(); }
void engine_anim_edit_select(Engine* e, int bone) { if (e) eng::animEditSelect(e, bone); }
int engine_anim_edit_selected(Engine* e) { return e ? eng::animEditSelected(e) : -1; }
void engine_player_aim(Engine* e, int on) { if (e) e->aimInput = on != 0; }
void engine_anim_edit_get(Engine* e, int bone, float* euler3) { if (e && euler3) eng::animEditGet(e, bone, euler3); }
void engine_anim_edit_set(Engine* e, int bone, const float* euler3) { if (e && euler3) eng::animEditSet(e, bone, euler3); }
void engine_anim_edit_reset_pose(Engine* e) { if (e) eng::animEditResetPose(e); }
int engine_anim_edit_key(Engine* e, float t) { return e ? eng::animEditKey(e, t) : 0; }
void engine_anim_edit_delete_key(Engine* e, float t) { if (e) eng::animEditDeleteKey(e, t); }
void engine_anim_edit_goto(Engine* e, float t) { if (e) eng::animEditGoto(e, t); }
const char* engine_anim_edit_keys(Engine* e) { if (!e) return ""; e->toolResult = eng::animEditKeys(e); return e->toolResult.c_str(); }
int engine_anim_edit_preview(Engine* e, int on) { return e && eng::animEditPreview(e, on != 0); }
int engine_anim_edit_save(Engine* e, const char* name) { return e ? eng::animEditSave(e, name ? name : "") : -1; }
void engine_anim_edit_end(Engine* e) { if (e) eng::animEditEnd(e); }

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
    char spec[600];
    auto col = [&](int i) { char b[48]; std::snprintf(b, sizeof(b), "[%.3f,%.3f,%.3f]", p[i], p[i + 1], p[i + 2]); return std::string(b); };
    static const char* builds[] = {"magro", "normal", "forte"};
    static const char* hair[] = {"careca", "curto", "comprido", "bone"};
    std::snprintf(spec, sizeof(spec),
                  R"({"tipo":"personagem","jogador":%s,"aparencia":{"altura":%.2f,"corpo":"%s","pele":%s,"camisa":%s,"calca":%s,"tenis":%s,"cabelo":%s,"estilo_cabelo":"%s","manga_comprida":%s}})",
                  p[19] > 0.5f ? "true" : "false", std::clamp(p[0], 1.f, 2.4f), builds[std::clamp(int(p[1] + 0.5f), 0, 2)],
                  col(2).c_str(), col(5).c_str(), col(8).c_str(), col(11).c_str(), col(14).c_str(),
                  hair[std::clamp(int(p[17] + 0.5f), 0, 3)], p[18] > 0.5f ? "true" : "false");
    return createSpec(e, spec);
}

int engine_create_random_character(Engine* e, uint32_t seed, int controllable) {
    if (!e) return -1;
    char spec[120];
    std::snprintf(spec, sizeof(spec), R"({"tipo":"personagem","semente":%u,"jogador":%s})", seed ? seed : 1u, controllable ? "true" : "false");
    return createSpec(e, spec);
}

int engine_create_weapon(Engine* e, int kind) {
    if (!e) return -1;
    static const char* names[] = {"pistola", "fuzil", "escopeta", "sniper", "faca", "taco"};
    int ch = eng::characterOfSelection(e);
    if (ch < 0 && engine_has_player(e)) ch = e->player;
    char spec[120];
    if (ch >= 0) {
        GameObject* owner = e->scene.get(e->characters[ch].root);
        std::snprintf(spec, sizeof(spec), R"({"tipo":"arma","arma":"%s","portador":%d})", names[std::clamp(kind, 0, 5)], owner ? owner->id : -1);
        return createSpec(e, spec, false);
    }
    std::snprintf(spec, sizeof(spec), R"({"tipo":"arma","arma":"%s"})", names[std::clamp(kind, 0, 5)]);
    return createSpec(e, spec);
}

int engine_create_building(Engine* e, int kind, uint32_t seed) {
    if (!e) return -1;
    static const char* names[] = {"casa", "sobrado", "barraco", "predio", "arranha_ceu", "escola", "arvore", "pinheiro", "palmeira", "carro",
                                  "poste", "muro", "rua", "cruzamento", "calcada", "escada", "rampa", "piso", "parede", "cerca", "banco",
                                  "caixa_dagua"};
    char spec[120];
    std::snprintf(spec, sizeof(spec), R"({"tipo":"construcao","modelo":"%s","semente":%u})", names[std::clamp(kind, 0, 21)], seed ? seed : 1u);
    return createSpec(e, spec);
}

void engine_load_map(Engine* e, int kind, uint32_t seed) {
    if (!e) return;
    e->protoSwaps.clear(); e->texSwaps.clear();  // mapa base novo: sem trocas
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
    recordEdit(e, "material", eng::jsonString(eng::matName(Mat(i))));
}

void engine_set_object_color(Engine* e, float r, float g, float b) {
    if (!e) return;
    glm::vec3 c(std::clamp(r, 0.f, 1.f), std::clamp(g, 0.f, 1.f), std::clamp(b, 0.f, 1.f));
    if (GameObject* sel = e->scene.get(e->selected))  // efeito: cor livre
        Scene::forEachInSubtree(*sel, [&](GameObject& g2) {
            if (g2.emitter) { g2.emitter->customColor = true; g2.emitter->color = c; }
        });
    forSelectedRenderers(e, [&](eng::MeshRenderer& mr) { mr.material = eng::tintedMaterial(e, mr.material, c); });
    char v[64];
    std::snprintf(v, sizeof(v), "[%.3f,%.3f,%.3f]", c.r, c.g, c.b);
    recordEdit(e, "cor", v);
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
    recordEdit(e, "textura", eng::jsonString(path));
    return 1;
}

// ---------- jogador ----------
int engine_has_player(Engine* e) {
    return e && e->player >= 0 && e->player < int(e->characters.size()) && e->scene.get(e->characters[e->player].root);
}
void engine_set_player(Engine* e) { if (e) { int c = eng::characterOfSelection(e); if (c >= 0) { eng::setPlayer(e, c); ++e->revision; } } }
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
    // Editor de animação aberto: o toque escolhe o osso (não troca de objeto)
    if (e->animEdit.active && eng::animEditPick(e, p)) return;
    // Editor: objeto mais próximo (raio x AABB). Cenário de mapa (sem id) não é selecionável.
    float best = 1e30f;
    GameObject* hitGo = nullptr;
    eng::pickEmitter(e, o, d, best, hitGo);  // efeitos não têm malha: esfera em volta do emissor
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
            ++e->revision;
        }
        e->tapCandidate = false;
        e->gizmoGrabbed = false;
        e->gizmo.onTouchUp();
        e->editorCam.onTouchEnd(t, m);
    }
}

// ---------- efeitos ----------
static const char* kFxKeys[] = {"fogueira", "tocha", "fumaca", "faiscas", "aura", "portal", "explosao", "orbe", "coluna_luz"};
static const char* kElKeys[] = {"fogo", "gelo", "trevas", "luz", "veneno", "raio", "arcano"};
int engine_create_effect(Engine* e, int kind, int element) {
    if (!e) return -1;
    char spec[120];
    std::snprintf(spec, sizeof(spec), R"({"tipo":"efeito","efeito":"%s","elemento":"%s"})", kFxKeys[std::clamp(kind, 0, 8)], kElKeys[std::clamp(element, 0, 6)]);
    return createSpec(e, spec);
}
void engine_spawn_effect(Engine* e, int kind, int element, float x, float y, float z) {
    if (e) eng::spawnEffect(e, kind, element, {x, y, z});
}
void engine_set_effect_element(Engine* e, int element) {
    if (!e) return;
    GameObject* sel = e->scene.get(e->selected);
    if (!sel) return;
    Scene::forEachInSubtree(*sel, [&](GameObject& g) {
        if (g.emitter) { g.emitter->element = std::clamp(element, 0, int(eng::EL_Count) - 1); g.emitter->customColor = false; }
    });
    if (GameObject* root = eng::rootOf(e, sel)) {
        eng::recipeSet(e, *root, "cor", "null");
        eng::recipeSet(e, *root, "elemento", eng::jsonString(kElKeys[std::clamp(element, 0, 6)]));
    }
}
void engine_player_cast(Engine* e, int element) { if (e && e->playing) eng::castSpell(e, element); }

void engine_set_camera(Engine* e, float tx, float ty, float tz, float yaw, float pitch, float distance) {
    if (!e) return;
    e->editorCam.target = {tx, ty, tz};
    e->editorCam.yaw = yaw;
    e->editorCam.pitch = std::clamp(pitch, -1.5f, 1.5f);
    e->editorCam.distance = std::clamp(distance, 0.3f, 5000.f);
}

int engine_make_character(Engine* e, int controllable) {
    if (!e) return -1;
    e->lastError.clear();
    int id = eng::makeCharacterFromModel(e, controllable != 0);
    if (GameObject* g = eng::objectById(e, id)) eng::recipeSet(e, *g, "personagem", "true");
    return id;
}

// ---------- projeto ----------
const char* engine_export_project(Engine* e) {
    if (!e) return "";
    e->toolResult = eng::exportProject(e, false);
    return e->toolResult.c_str();
}
int engine_import_project(Engine* e, const char* json) {
    if (!e || !json) return 0;
    std::string err;
    bool ok = eng::importProject(e, json, &err);
    if (!ok) e->lastError = err;
    return ok ? 1 : 0;
}
long long engine_project_revision(Engine* e) { return e ? (long long)e->revision : 0; }
void engine_set_project_name(Engine* e, const char* name) { if (e && name) e->projectName = name; }
const char* engine_tool_call(Engine* e, const char* name, const char* input) {
    if (!e || !name) return "{\"erro\":\"motor não iniciado\"}";
    e->toolResult = eng::toolCall(e, name, input ? input : "{}");
    return e->toolResult.c_str();
}
int engine_add_spec(Engine* e, const char* json) { return e && json ? createSpec(e, json) : -1; }
void engine_set_time_of_day(Engine* e, float hour) { if (e) eng::setTimeOfDay(e, hour); }
float engine_get_time_of_day(Engine* e) { return e ? e->timeOfDay : 13.f; }

int engine_map_set_editable(Engine* e, int on) { return e && eng::setMapEditable(e, on != 0); }
int engine_map_editable(Engine* e) { return e && e->mapEditable; }
int engine_map_swap_model(Engine* e, const char* kind, const char* path) {
    return e && kind && eng::setMapSwap(e, false, kind, path ? path : "");
}
int engine_map_swap_texture(Engine* e, int material, const char* path) {
    if (!e || material < 0 || material >= int(Mat::Count)) return 0;
    return eng::setMapSwap(e, true, eng::matName(Mat(material)), path ? path : "");
}

int engine_export_glb(Engine* e, int object_id, const char* path) {
    if (!e || !path) return 0;
    std::string err;
    bool ok = eng::exportGlb(e, object_id, path, &err);
    e->lastError = err;
    return ok ? 1 : 0;
}

// ---------- jogo ----------
void engine_game_status(Engine* e, float* o) {
    if (!e || !o) return;
    const eng::GameState& G = e->game;
    o[0] = float(G.state); o[1] = G.playerHp; o[2] = G.playerMax; o[3] = float(G.score);
    o[4] = G.time; o[5] = e->rules.tempo; o[6] = G.hurtTimer; o[7] = e->rules.objetivo != "livre" ? 1.f : 0.f;
}
const char* engine_game_text(Engine* e, int which) {
    if (!e) return "";
    switch (which) {
        case 0: e->toolResult = eng::gameStatusText(e); break;
        case 1: e->toolResult = e->game.message; break;
        case 2: e->toolResult = e->rules.titulo; break;
        default: e->toolResult = e->rules.descricao; break;
    }
    return e->toolResult.c_str();
}
