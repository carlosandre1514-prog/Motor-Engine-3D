#include "platform/engine_c_api.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <memory>
#include <vector>
#include "assets/GltfLoader.h"
#include "assets/MeshUtils.h"
#include "assets/TextureManager.h"
#include "editor/EditorCamera.h"
#include "editor/Gizmo.h"
#include "physics/Physics.h"
#include "render/Renderer.h"
#include "scene/Animator.h"
#include "scene/Scene.h"

using namespace eng;

struct Engine {
    Renderer renderer;
    Scene scene;
    Physics physics;
    Camera camera;
    EditorCamera editorCam;
    Gizmo gizmo;
    DirectionalLight sun;
    Environment env;
    PointLight points[kMaxPointLights];
    int pointCount = 0;

    // Componentes (pools de capacidade fixa)
    MeshRendererPool meshRenderers;
    AnimatorPool animatorPool;
    RigidBodyPool bodies;

    std::vector<std::unique_ptr<LoadedModel>> models;  // carregamento: fora do loop
    std::vector<Scene::Handle> objects;                // id público -> handle
    std::vector<Animator*> animators;

    // Malhas e materiais básicos da cena (recriados a cada cena nova)
    MeshId cube = kInvalidId, sphere = kInvalidId, capsule = kInvalidId, rock = kInvalidId;
    std::vector<glm::vec3> rockHull;
    MaterialId matDefault = kInvalidId, matFloor = kInvalidId;
    MaterialId matPalette[6] = {};

    bool playing = false;
    bool showGrid = true;
    int width = 1, height = 1;
    Scene::Handle selected;
    std::string lastError;

    // Snapshot para o Stop restaurar o estado do editor
    struct Snap { Scene::Handle h; glm::vec3 p, s; glm::quat r; };
    std::vector<Snap> snapshot;
    std::vector<float> animTimes;

    // Toque: detecção de "tap" (toque curto sem arrastar)
    bool tapCandidate = false;
    bool gizmoGrabbed = false;
    glm::vec2 tapStart{0.f};

    // Estatísticas
    float statTime = 0.f, fps = 0.f, frameMs = 0.f, cpuMs = 0.f, cpuAccum = 0.f;
    int statFrames = 0;
};

// ---------- utilidades ----------
static GameObject* objectById(Engine* e, int id) {
    if (!e || id < 0 || id >= int(e->objects.size())) return nullptr;
    return e->scene.get(e->objects[id]);
}

static int registerObject(Engine* e, Scene::Handle h) {
    GameObject* g = e->scene.get(h);
    if (!g) return -1;
    g->id = int(e->objects.size());
    e->objects.push_back(h);
    return g->id;
}

// Matriz usada para desenhar: skinned usa o espaço do esqueleto (o transform do nó do mesh é ignorado, como manda o glTF)
static glm::mat4 renderMatrix(const Engine* e, const GameObject& go, const glm::mat4** palette) {
    *palette = nullptr;
    if (go.animator && go.meshRenderer) {
        const Mesh* m = e->renderer.mesh(go.meshRenderer->mesh);
        if (m && m->skinned) {
            *palette = go.animator->palette;
            return go.animator->space ? go.animator->space->world() : glm::mat4(1.f);
        }
    }
    return go.transform.world();
}

static bool subtreeBounds(Engine* e, GameObject& root, AABB& out) {
    bool any = false;
    out = {glm::vec3(1e30f), glm::vec3(-1e30f)};
    Scene::forEachInSubtree(root, [&](GameObject& g) {
        if (!g.meshRenderer) return;
        const Mesh* m = e->renderer.mesh(g.meshRenderer->mesh);
        if (!m) return;
        const glm::mat4* pal;
        AABB b = transformAABB(m->bounds, renderMatrix(e, g, &pal));
        out.min = glm::min(out.min, b.min);
        out.max = glm::max(out.max, b.max);
        any = true;
    });
    return any;
}

static void focusBounds(Engine* e, const AABB& b) {
    glm::vec3 c = (b.min + b.max) * 0.5f;
    float radius = std::max(glm::length(b.max - b.min) * 0.5f, 0.05f);
    e->editorCam.target = c;
    e->editorCam.distance = std::clamp(radius / std::tan(glm::radians(e->camera.fovYDeg) * 0.5f) * 1.1f, 0.3f, 5000.f);
}

static void focusSceneOrSelection(Engine* e) {
    e->scene.updateTransforms();
    AABB total{glm::vec3(1e30f), glm::vec3(-1e30f)};
    bool any = false;
    if (GameObject* sel = e->scene.get(e->selected)) any = subtreeBounds(e, *sel, total);
    if (!any) {
        e->scene.forEach([&](GameObject& g, Scene::Handle) {
            if (g.transform.parent) return;
            AABB b;
            if (std::strcmp(g.name, "Chao") == 0) return;  // o chão não conta para o enquadramento
            if (subtreeBounds(e, g, b)) { total.min = glm::min(total.min, b.min); total.max = glm::max(total.max, b.max); any = true; }
        });
    }
    if (any) focusBounds(e, total);
}

static void releaseComponents(Engine* e, GameObject& g) {
    if (g.rigidBody) {
        RigidBody* rb = g.rigidBody;
        e->physics.detach(*rb);
        e->bodies.release(rb);
    }
    if (g.meshRenderer) { e->meshRenderers.release(g.meshRenderer); g.meshRenderer = nullptr; }
}

static void ensureBasics(Engine* e) {
    if (e->cube != kInvalidId) return;
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    buildCube(v, idx);
    e->cube = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);
    v.clear(); idx.clear(); buildSphere(v, idx);
    e->sphere = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);
    v.clear(); idx.clear(); buildCapsule(v, idx, 2.f);
    e->capsule = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);
    v.clear(); idx.clear(); buildRock(v, idx, e->rockHull);
    e->rock = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);

    TextureManager& tx = TextureManager::instance();
    auto mat = [&](glm::vec4 albedo, float rough, float metal) {
        Material m;
        m.albedo = tx.white(); m.normal = tx.flatNormal(); m.orm = tx.white(); m.emissive = tx.white();
        m.albedoFactor = albedo; m.roughness = rough; m.metallic = metal;
        return e->renderer.createMaterial(m);
    };
    e->matDefault = mat({0.8f, 0.8f, 0.8f, 1.f}, 0.5f, 0.f);
    e->matFloor = mat({0.3f, 0.32f, 0.3f, 1.f}, 0.9f, 0.f);
    e->matPalette[0] = mat({0.9f, 0.35f, 0.1f, 1.f}, 0.4f, 0.f);    // laranja plástico
    e->matPalette[1] = mat({0.95f, 0.95f, 0.97f, 1.f}, 0.2f, 1.f);  // aço polido
    e->matPalette[2] = mat({1.f, 0.78f, 0.34f, 1.f}, 0.3f, 1.f);    // ouro
    e->matPalette[3] = mat({0.15f, 0.45f, 0.85f, 1.f}, 0.25f, 0.f); // azul brilhante
    e->matPalette[4] = mat({0.55f, 0.36f, 0.22f, 1.f}, 0.8f, 0.f);  // madeira
    e->matPalette[5] = mat({0.5f, 0.5f, 0.52f, 1.f}, 0.9f, 0.f);    // pedra
}

static Scene::Handle addMeshObject(Engine* e, const char* name, MeshId mesh, MaterialId mat,
                                   glm::vec3 pos, glm::vec3 scale, glm::vec3 eulerDeg = glm::vec3(0.f)) {
    Scene::Handle h = e->scene.create(name);
    GameObject* go = e->scene.get(h);
    if (!go) return h;
    go->transform.setPosition(pos);
    go->transform.setScale(scale);
    go->transform.setEulerDegrees(eulerDeg);
    auto rh = e->meshRenderers.acquire();
    if (MeshRenderer* mr = e->meshRenderers.get(rh)) {
        mr->mesh = mesh; mr->material = mat;
        go->meshRenderer = mr;
    }
    registerObject(e, h);
    return h;
}

// Collider no tamanho da malha básica (lado/diâmetro 1); a escala do Transform é aplicada pelo Physics
static void addBody(Engine* e, Scene::Handle h, ColliderType type, float mass) {
    GameObject* go = e->scene.get(h);
    if (!go) return;
    auto bh = e->bodies.acquire();
    RigidBody* rb = e->bodies.get(bh);
    if (!rb) return;
    btCollisionShape* shape = type == ColliderType::Capsule
        ? Physics::makeShape(type, {1.f, 2.f, 1.f})
        : type == ColliderType::ConvexMesh
        ? Physics::makeShape(type, glm::vec3(1.f), e->rockHull.data(), int(e->rockHull.size()))
        : Physics::makeShape(type, glm::vec3(1.f));
    e->physics.attach(*go, *rb, type, shape, mass);
    e->physics.syncFromTransform(*rb);
}

static void clearScene(Engine* e) {
    if (e->playing) e->playing = false;
    e->scene.forEach([&](GameObject& g, Scene::Handle) { releaseComponents(e, g); });
    e->scene.clear();
    e->meshRenderers.clear();
    e->animatorPool.clear();
    e->bodies.clear();
    e->objects.clear();
    e->animators.clear();
    e->models.clear();
    e->snapshot.clear();
    e->renderer.clearAssets();
    TextureManager::instance().releaseAll();
    e->cube = e->sphere = e->capsule = e->rock = kInvalidId;
    e->selected = {};
    e->gizmo.target = nullptr;
    e->pointCount = 0;
    ensureBasics(e);
}

static void addFloor(Engine* e) {
    Scene::Handle f = addMeshObject(e, "Chao", e->cube, e->matFloor, {0.f, -0.1f, 0.f}, {40.f, 0.2f, 40.f});
    addBody(e, f, ColliderType::Box, 0.f);
}

static void selectHandle(Engine* e, Scene::Handle h) {
    GameObject* g = e->scene.get(h);
    e->selected = g ? h : Scene::Handle{};
    e->gizmo.target = g;
}

// ---------- ciclo de vida ----------
Engine* engine_create(const char* shader_dir) {
    TextureManager::instance().forgetAll();  // contexto GL novo: ids antigos não valem
    Engine* e = new Engine();
    if (!e->renderer.init(shader_dir)) { delete e; return nullptr; }
    e->objects.reserve(Scene::kMaxObjects);
    ensureBasics(e);
    return e;
}

void engine_destroy(Engine* e) {
    if (!e) return;
    e->scene.forEach([&](GameObject& g, Scene::Handle) { releaseComponents(e, g); });
    TextureManager::instance().releaseAll();
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

    if (e->playing) e->physics.step(dt);
    for (Animator* a : e->animators) a->update(dt, e->playing);

    // Câmera: planos near/far e distância de sombra acompanham o zoom (modelos minúsculos ou enormes)
    float d = e->editorCam.distance;
    e->camera.nearZ = std::clamp(d * 0.01f, 0.01f, 5.f);
    e->camera.farZ = std::max(200.f, d * 40.f);
    e->renderer.shadowDistance = std::clamp(d * 3.5f, 12.f, 1500.f);
    e->editorCam.apply(e->camera);
    e->scene.updateTransforms();

    e->renderer.beginFrame(e->camera, e->sun);
    e->scene.forEach([&](GameObject& go, Scene::Handle) {
        if (!go.meshRenderer) return;
        const glm::mat4* palette;
        glm::mat4 m = renderMatrix(e, go, &palette);
        e->renderer.submit(go, m, palette);
    });

    if (!e->playing) {  // overlay do editor
        glm::mat4 vp = e->camera.viewProj();
        if (e->showGrid) {
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
        if (GameObject* sel = e->scene.get(e->selected)) {
            AABB b;
            if (subtreeBounds(e, *sel, b)) {
                glm::vec3 c[8];
                for (int k = 0; k < 8; ++k) c[k] = {k & 1 ? b.max.x : b.min.x, k & 2 ? b.max.y : b.min.y, k & 4 ? b.max.z : b.min.z};
                const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (auto& ed : E) e->renderer.line(vp, c[ed[0]], c[ed[1]], {1.f, 0.6f, 0.1f, 0.9f}, 2.f, false);
            }
            e->gizmo.draw(e->renderer, e->camera);
        }
    }

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
            e->snapshot.push_back({h, g.transform.position, g.transform.scale, g.transform.rotation});
        });
        e->animTimes.clear();
        for (Animator* a : e->animators) e->animTimes.push_back(a->time);
        e->bodies.forEach([&](RigidBody& rb, RigidBodyPool::Handle) { e->physics.syncFromTransform(rb); });
        e->physics.resetAccumulator();
        e->gizmo.onTouchUp();
    } else {
        for (auto& s : e->snapshot)
            if (GameObject* g = e->scene.get(s.h)) {
                g->transform.setPosition(s.p); g->transform.setRotation(s.r); g->transform.setScale(s.s);
            }
        for (size_t i = 0; i < e->animators.size() && i < e->animTimes.size(); ++i) e->animators[i]->time = e->animTimes[i];
        e->bodies.forEach([&](RigidBody& rb, RigidBodyPool::Handle) { e->physics.syncFromTransform(rb); });
    }
    e->playing = p;
}
int engine_is_playing(Engine* e) { return e && e->playing; }

// ---------- cenas ----------
void engine_new_scene(Engine* e) {
    if (!e) return;
    clearScene(e);
    addFloor(e);
    e->editorCam.target = glm::vec3(0.f, 0.5f, 0.f);
    e->editorCam.distance = 8.f;
}

void engine_load_demo(Engine* e) {
    if (!e) return;
    clearScene(e);
    addFloor(e);
    auto add = [&](const char* name, MeshId mesh, MaterialId m, glm::vec3 pos, glm::vec3 scale, glm::vec3 eul, ColliderType ct) {
        Scene::Handle h = addMeshObject(e, name, mesh, m, pos, scale, eul);
        addBody(e, h, ct, 1.f);
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
    selectHandle(e, e->objects.size() > 2 ? e->objects[2] : Scene::Handle{});
}

void engine_load_physics_demo(Engine* e) {
    if (!e) return;
    clearScene(e);
    addFloor(e);
    int k = 0;
    // pirâmide de caixas
    for (int row = 0; row < 5; ++row)
        for (int i = 0; i < 5 - row; ++i) {
            char name[32]; std::snprintf(name, sizeof(name), "Caixa %d", ++k);
            Scene::Handle h = addMeshObject(e, name, e->cube, e->matPalette[4],
                                            {(i - (4 - row) * 0.5f) * 0.82f, 0.4f + row * 0.8f, 0.f}, glm::vec3(0.8f));
            addBody(e, h, ColliderType::Box, 1.f);
        }
    // chuva de objetos variados
    for (int i = 0; i < 12; ++i) {
        char name[32];
        float x = -2.5f + (i % 4) * 1.6f, z = -1.5f + (i / 4) * 1.4f, y = 5.f + (i % 3) * 1.2f;
        glm::vec3 eul(i * 23.f, i * 41.f, i * 17.f);
        int kind = i % 3;
        Scene::Handle h;
        if (kind == 0) {
            std::snprintf(name, sizeof(name), "Esfera %d", i);
            h = addMeshObject(e, name, e->sphere, e->matPalette[i % 2 ? 2 : 3], {x, y, z}, glm::vec3(0.7f));
            addBody(e, h, ColliderType::Sphere, 1.f);
        } else if (kind == 1) {
            std::snprintf(name, sizeof(name), "Capsula %d", i);
            h = addMeshObject(e, name, e->capsule, e->matPalette[0], {x, y, z}, glm::vec3(0.5f), eul);
            addBody(e, h, ColliderType::Capsule, 1.f);
        } else {
            std::snprintf(name, sizeof(name), "Pedra %d", i);
            h = addMeshObject(e, name, e->rock, e->matPalette[5], {x, y, z}, glm::vec3(0.9f), eul);
            addBody(e, h, ColliderType::ConvexMesh, 2.f);
        }
    }
    e->editorCam.target = glm::vec3(0.f, 1.5f, 0.f);
    e->editorCam.distance = 13.f;
    e->editorCam.yaw = 0.5f; e->editorCam.pitch = 0.35f;
}

int engine_load_model(Engine* e, const char* path) {
    if (!e || !path) return -1;
    auto model = std::make_unique<LoadedModel>();
    LoadContext ctx{e->renderer, e->scene, e->meshRenderers, e->animatorPool};
    if (!loadGltf(path, ctx, *model)) {
        e->lastError = model->error;
        for (auto h : model->allNodes) if (GameObject* g = e->scene.get(h)) releaseComponents(e, *g);
        for (auto h : model->roots) e->scene.destroy(h);
        return -1;
    }
    int rootId = -1;
    for (auto h : model->allNodes) {
        int id = registerObject(e, h);
        if (rootId < 0) rootId = id;
    }
    for (Animator* a : model->animators) e->animators.push_back(a);
    e->models.push_back(std::move(model));
    e->lastError.clear();

    // Modelo pousado no chão (y = 0) e centrado no XZ; enquadra a câmera nele
    if (GameObject* root = objectById(e, rootId)) {
        e->scene.updateTransforms();
        AABB b;
        if (subtreeBounds(e, *root, b)) {
            glm::vec3 c = (b.min + b.max) * 0.5f;
            root->transform.setPosition(root->transform.position + glm::vec3(-c.x, -b.min.y, -c.z));
            // Chão acompanha modelos grandes (ex.: Fox mede ~100 unidades)
            float extent = std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 4.f;
            e->scene.forEach([&](GameObject& g, Scene::Handle) {
                if (std::strcmp(g.name, "Chao") != 0 || g.transform.scale.x >= extent) return;
                g.transform.setScale({extent, g.transform.scale.y, extent});
                if (g.rigidBody) e->physics.syncFromTransform(*g.rigidBody);
            });
        }
        selectHandle(e, e->objects[rootId]);
        focusSceneOrSelection(e);
    }
    return rootId;
}

const char* engine_last_error(Engine* e) { return e ? e->lastError.c_str() : ""; }

int engine_add_object(Engine* e, int kind, int withPhysics) {
    if (!e) return -1;
    ensureBasics(e);
    static int counter = 0;
    char name[32];
    MeshId mesh = e->cube; ColliderType ct = ColliderType::Box; MaterialId mat = e->matPalette[0];
    glm::vec3 scale(1.f);
    switch (kind) {
        case 1: mesh = e->sphere; ct = ColliderType::Sphere; mat = e->matPalette[3]; std::snprintf(name, sizeof(name), "Esfera %d", ++counter); break;
        case 2: mesh = e->capsule; ct = ColliderType::Capsule; mat = e->matPalette[2]; scale = glm::vec3(0.6f); std::snprintf(name, sizeof(name), "Capsula %d", ++counter); break;
        case 3: mesh = e->rock; ct = ColliderType::ConvexMesh; mat = e->matPalette[5]; std::snprintf(name, sizeof(name), "Pedra %d", ++counter); break;
        default: std::snprintf(name, sizeof(name), "Cubo %d", ++counter); break;
    }
    glm::vec3 pos = e->editorCam.target;
    pos.y = std::max(pos.y, 0.f) + 2.f;  // nasce acima do ponto de foco (cai no Play)
    Scene::Handle h = addMeshObject(e, name, mesh, mat, pos, scale);
    if (withPhysics) addBody(e, h, ct, 1.f);
    selectHandle(e, h);
    GameObject* g = e->scene.get(h);
    return g ? g->id : -1;
}

void engine_delete_selected(Engine* e) {
    if (!e) return;
    GameObject* sel = e->scene.get(e->selected);
    if (!sel) return;
    // Animators que dependem de nós apagados passam a usar o espaço de mundo
    Scene::forEachInSubtree(*sel, [&](GameObject& g) {
        for (Animator* a : e->animators) if (a->space == &g.transform) a->space = nullptr;
        releaseComponents(e, g);
    });
    e->scene.destroy(e->selected);
    selectHandle(e, {});
}

void engine_focus_selected(Engine* e) { if (e) focusSceneOrSelection(e); }

// ---------- hierarquia / inspetor ----------
int engine_object_count(Engine* e) { return e ? int(e->objects.size()) : 0; }
int engine_object_valid(Engine* e, int id) { return objectById(e, id) != nullptr; }
const char* engine_object_name(Engine* e, int id) { GameObject* g = objectById(e, id); return g ? g->name : ""; }
int engine_object_parent(Engine* e, int id) {
    GameObject* g = objectById(e, id);
    return g && g->transform.parent && g->transform.parent->owner ? g->transform.parent->owner->id : -1;
}
int engine_object_depth(Engine* e, int id) { GameObject* g = objectById(e, id); return g ? hierarchyDepth(g->transform) : 0; }

void engine_select(Engine* e, int id) {
    if (!e) return;
    selectHandle(e, objectById(e, id) ? e->objects[id] : Scene::Handle{});
}
int engine_selected(Engine* e) { GameObject* g = e ? e->scene.get(e->selected) : nullptr; return g ? g->id : -1; }

int engine_get_transform(Engine* e, int id, float* p, float* r, float* s) {
    GameObject* g = objectById(e, id);
    if (!g) return 0;
    glm::vec3 eu = g->transform.eulerDegrees();
    for (int i = 0; i < 3; ++i) { p[i] = g->transform.position[i]; r[i] = eu[i]; s[i] = g->transform.scale[i]; }
    return 1;
}
void engine_set_transform(Engine* e, int id, const float* p, const float* r, const float* s) {
    GameObject* g = objectById(e, id);
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
    const FrameStats& s = e->renderer.stats();
    o[0] = e->fps; o[1] = e->frameMs; o[2] = e->cpuMs;
    o[3] = float(s.drawCalls); o[4] = float(s.triangles); o[5] = float(e->scene.count());
}

// ---------- toque ----------
static void handleTap(Engine* e, glm::vec2 p) {
    glm::vec3 o, d;
    e->camera.screenRay(p.x, p.y, float(e->width), float(e->height), o, d);
    if (e->playing) {
        // Play: empurra o corpo tocado
        glm::vec3 hit; GameObject* obj = nullptr;
        if (e->physics.raycast(o, o + d * 1000.f, hit, obj) && obj && obj->rigidBody && obj->rigidBody->mass > 0.f) {
            obj->rigidBody->addImpulse((d * 4.f + glm::vec3(0, 3.f, 0)) * obj->rigidBody->mass);
        }
        return;
    }
    // Editor: seleciona o objeto mais próximo (raio x AABB); seleciona a raiz do modelo
    float best = 1e30f;
    GameObject* hitGo = nullptr;
    e->scene.forEach([&](GameObject& g, Scene::Handle) {
        if (!g.meshRenderer || !g.active) return;
        const Mesh* m = e->renderer.mesh(g.meshRenderer->mesh);
        if (!m) return;
        const glm::mat4* pal;
        float t;
        if (rayAABB(o, d, transformAABB(m->bounds, renderMatrix(e, g, &pal)), t) && t < best) { best = t; hitGo = &g; }
    });
    if (!hitGo) { selectHandle(e, {}); return; }
    GameObject* root = hitGo;
    while (root->transform.parent && root->transform.parent->owner) root = root->transform.parent->owner;
    // Toque de novo no mesmo modelo já selecionado: desce para a peça tocada
    GameObject* cur = e->scene.get(e->selected);
    selectHandle(e, Scene::handleOf(cur == root ? *hitGo : *root));
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
        else if (m >= 1) e->editorCam.onTouchMove(t, m, h, e->camera);
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
