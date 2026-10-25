#include "platform/engine_c_api.h"
#include <vector>
#include "assets/GltfLoader.h"
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
    AmbientLight ambient;
    PointLight points[kMaxPointLights];
    int pointCount = 0;

    std::vector<LoadedModel> models;       // carregamento: fora do loop
    std::vector<Scene::Handle> objects;    // índice público -> handle
    std::vector<Animator*> animators;

    bool playing = false;
    int width = 1, height = 1;
    Scene::Handle selected;

    // Snapshot para Stop restaurar o estado do editor
    struct Snap { Scene::Handle h; glm::vec3 p, s; glm::quat r; };
    std::vector<Snap> snapshot;
};

static void snapshotScene(Engine* e) {
    e->snapshot.clear();
    for (auto h : e->objects)
        if (GameObject* g = e->scene.get(h)) e->snapshot.push_back({h, g->transform.position, g->transform.scale, g->transform.rotation});
}
static void restoreScene(Engine* e) {
    for (auto& s : e->snapshot)
        if (GameObject* g = e->scene.get(s.h)) {
            g->transform.setPosition(s.p); g->transform.setRotation(s.r); g->transform.setScale(s.s);
            if (g->rigidBody && g->rigidBody->body) { g->rigidBody->setVelocity({0, 0, 0}); }
        }
}

Engine* engine_create(const char* shader_dir) {
    Engine* e = new Engine();
    if (!e->renderer.init(shader_dir)) { delete e; return nullptr; }
    e->objects.reserve(Scene::kMaxObjects);  // sem realocação depois
    e->points[0].position = {3, 4, 3};
    e->pointCount = 1;
    return e;
}
void engine_destroy(Engine* e) { if (e) { e->renderer.shutdown(); delete e; } }
void engine_resize(Engine* e, int w, int h) { e->width = w; e->height = h; e->camera.aspect = float(w) / float(h); e->renderer.resize(w, h); }

void engine_frame(Engine* e, float dt) {
    if (e->playing) {
        e->physics.step(dt);
        for (Animator* a : e->animators) a->update(dt);
    }
    e->editorCam.apply(e->camera);       // no Play, trocar pela câmera do jogo
    e->scene.updateTransforms();

    e->renderer.cullFrustum = e->camera.frustum();
    e->renderer.beginFrame();
    e->scene.forEach([&](GameObject& go, Scene::Handle) {
        e->renderer.submit(go, go.animator ? go.animator->palette : nullptr);
    });
    e->renderer.render(e->camera, e->sun, e->ambient, e->points, e->pointCount);
    // Gizmo desenhado depois (overlay) quando !playing e há seleção.
}

void engine_set_play(Engine* e, int playing) {
    if (playing && !e->playing) snapshotScene(e);
    if (!playing && e->playing) restoreScene(e);
    e->playing = playing != 0;
}
int engine_is_playing(Engine* e) { return e->playing; }

int engine_load_model(Engine* e, const char* path) {
    e->models.emplace_back();
    if (!loadGltf(path, e->renderer, e->scene, e->models.back())) { e->models.pop_back(); return -1; }
    int first = int(e->objects.size());
    for (auto h : e->models.back().roots) e->objects.push_back(h);
    return first;
}
int engine_object_count(Engine* e) { return int(e->objects.size()); }
const char* engine_object_name(Engine* e, int i) {
    GameObject* g = (i >= 0 && i < int(e->objects.size())) ? e->scene.get(e->objects[i]) : nullptr;
    return g ? g->name : "";
}

void engine_select(Engine* e, int id) {
    if (id < 0 || id >= int(e->objects.size())) { e->gizmo.target = nullptr; return; }
    e->selected = e->objects[id];
    e->gizmo.target = e->scene.get(e->selected);
}
void engine_get_transform(Engine* e, int id, float* p, float* r, float* s) {
    GameObject* g = e->scene.get(e->objects[id]);
    if (!g) return;
    glm::vec3 eu = g->transform.eulerDegrees();
    for (int i = 0; i < 3; ++i) { p[i] = g->transform.position[i]; r[i] = eu[i]; s[i] = g->transform.scale[i]; }
}
void engine_set_transform(Engine* e, int id, const float* p, const float* r, const float* s) {
    GameObject* g = e->scene.get(e->objects[id]);
    if (!g) return;
    g->transform.setPosition({p[0], p[1], p[2]});
    g->transform.setEulerDegrees({r[0], r[1], r[2]});
    g->transform.setScale({s[0], s[1], s[2]});
}
void engine_set_gizmo_mode(Engine* e, int m) { e->gizmo.mode = m ? Gizmo::Mode::Rotate : Gizmo::Mode::Translate; }

void engine_touch(Engine* e, int action, int n, const int* ids, const float* xy) {
    TouchPoint t[2];
    int m = n < 2 ? n : 2;
    for (int i = 0; i < m; ++i) t[i] = {ids[i], xy[i * 2], xy[i * 2 + 1]};
    float w = float(e->width), h = float(e->height);
    if (action == 0) {
        bool grabbed = (m == 1) && e->gizmo.onTouchDown(e->camera, {t[0].x, t[0].y}, w, h);
        if (!grabbed) e->editorCam.onTouchBegin(t, m);
    } else if (action == 1) {
        if (e->gizmo.active() != Gizmo::Axis::None) e->gizmo.onTouchMove(e->camera, {t[0].x, t[0].y}, w, h);
        else e->editorCam.onTouchMove(t, m, h, e->camera);
    } else {
        e->gizmo.onTouchUp();
        e->editorCam.onTouchEnd(t, m);
    }
}
