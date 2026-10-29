#include "engine/Engine.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "assets/MeshUtils.h"
#include <stb_image.h>
#include "assets/TextureManager.h"
#include "scene/Humanoid.h"
#include <glm/gtx/matrix_decompose.hpp>

namespace eng {

// ---------------- utilidades de cena ----------------
GameObject* objectById(Engine* e, int id) {
    if (!e || id < 0 || id >= int(e->objects.size())) return nullptr;
    return e->scene.get(e->objects[id]);
}

int registerObject(Engine* e, Scene::Handle h) {
    GameObject* g = e->scene.get(h);
    if (!g) return -1;
    g->id = int(e->objects.size());
    e->objects.push_back(h);
    return g->id;
}

// Skinned usa o espaço do esqueleto (o transform do nó do mesh é ignorado, como manda o glTF)
glm::mat4 renderMatrix(const Engine* e, const GameObject& go, const glm::mat4** palette) {
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

bool subtreeBounds(Engine* e, GameObject& root, AABB& out) {
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

void focusSceneOrSelection(Engine* e) {
    e->scene.updateTransforms();
    AABB total{glm::vec3(1e30f), glm::vec3(-1e30f)};
    bool any = false;
    if (GameObject* sel = e->scene.get(e->selected)) any = subtreeBounds(e, *sel, total);
    if (!any) {
        e->scene.forEach([&](GameObject& g, Scene::Handle) {
            if (g.transform.parent || g.id < 0) return;          // cenário de mapa não conta
            if (std::strcmp(g.name, "Chao") == 0) return;        // nem o chão
            AABB b;
            if (subtreeBounds(e, g, b)) { total.min = glm::min(total.min, b.min); total.max = glm::max(total.max, b.max); any = true; }
        });
    }
    if (any) focusBounds(e, total);
}

void releaseComponents(Engine* e, GameObject& g) {
    if (g.rigidBody) {
        RigidBody* rb = g.rigidBody;
        e->physics.detach(*rb);
        e->bodies.release(rb);
    }
    if (g.meshRenderer) { e->meshRenderers.release(g.meshRenderer); g.meshRenderer = nullptr; }
    if (g.emitter) { e->emitters.release(g.emitter); g.emitter = nullptr; }
}

void ensureBasics(Engine* e) {
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
    e->matFloor = mat({0.3f, 0.32f, 0.3f, 1.f}, 0.9f, 0.f);
    e->matPalette[0] = mat({0.9f, 0.35f, 0.1f, 1.f}, 0.4f, 0.f);    // laranja plástico
    e->matPalette[1] = mat({0.95f, 0.95f, 0.97f, 1.f}, 0.2f, 1.f);  // aço polido
    e->matPalette[2] = mat({1.f, 0.78f, 0.34f, 1.f}, 0.3f, 1.f);    // ouro
    e->matPalette[3] = mat({0.15f, 0.45f, 0.85f, 1.f}, 0.25f, 0.f); // azul brilhante
    e->matPalette[4] = mat({0.55f, 0.36f, 0.22f, 1.f}, 0.8f, 0.f);  // madeira
    e->matPalette[5] = mat({0.5f, 0.5f, 0.52f, 1.f}, 0.9f, 0.f);    // pedra

    // Biblioteca procedural (texturas geradas uma vez e reenviadas à GPU a cada cena); usa as imagens de shaders/materials
    setMaterialTextureDir((e->renderer.shaderDir() + "/materials").c_str());
    pregenerateTextures();  // todos os núcleos; da 2ª abertura em diante vem do cache
    // celular fraco: metade da resolução (1/4 da memória de vídeo)
    auto half = [](const std::vector<uint8_t>& src, int w, int h) {
        std::vector<uint8_t> out(size_t(w / 2) * (h / 2) * 4);
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w / 2; ++x)
                for (int c = 0; c < 4; ++c) {
                    int a = src[((2 * y) * w + 2 * x) * 4 + c], b = src[((2 * y) * w + 2 * x + 1) * 4 + c];
                    int d = src[((2 * y + 1) * w + 2 * x) * 4 + c], f = src[((2 * y + 1) * w + 2 * x + 1) * 4 + c];
                    out[(size_t(y) * (w / 2) + x) * 4 + c] = uint8_t((a + b + d + f + 2) / 4);
                }
        return out;
    };
    for (int k = 0; k < int(Mat::Count); ++k) {
        const TexData& t0 = proceduralTexture(Mat(k));
        TexData small;
        const bool shrink = e->lowTextures && t0.w >= 256 && t0.h >= 256;
        if (shrink) {
            small.w = t0.w / 2; small.h = t0.h / 2;
            small.albedo = half(t0.albedo, t0.w, t0.h);
            small.orm = half(t0.orm, t0.w, t0.h);
            if (!t0.normal.empty()) small.normal = half(t0.normal, t0.w, t0.h);
        }
        const TexData& t = shrink ? small : t0;
        MatParams mp = matParams(Mat(k));
        Material m;
        m.albedo = tx.fromPixels(t.albedo.data(), t.w, t.h, 4, true);
        m.orm = tx.fromPixels(t.orm.data(), t.w, t.h, 4, false);
        m.normal = t.normal.empty() ? tx.flatNormal() : tx.fromPixels(t.normal.data(), t.w, t.h, 4, false);
        m.emissive = tx.white();
        m.roughness = mp.roughness; m.metallic = mp.metallic; m.mode = mp.mode; m.doubleSided = mp.doubleSided;
        if (Mat(k) == Mat::Water) {
            // água padrão: oceano animado (shader "ocean"); as ondas vêm de waternormals.jpg, junto dos shaders
            int w, h, comp;
            std::string path = e->renderer.shaderDir() + "/waternormals.jpg";
            if (unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4)) {
                m.normal = tx.fromPixels(px, w, h, 4, false);
                stbi_image_free(px);
            }
            m.mode = 2;
            m.albedoFactor = glm::vec4(0.01f, 0.07f, 0.12f, 1.f);  // cor da água funda (azul-esverdeado escuro)
            m.doubleSided = true;
        }
        if (Mat(k) == Mat::ChainLink) m.alphaCutoff = 0.5f;  // alambrado: só o arame aparece
        if (Mat(k) == Mat::Mural) m.alphaCutoff = 0.35f;     // pichação: só a tinta, a parede aparece atrás
        if (Mat(k) == Mat::Lamp) m.emissive_factor = glm::vec3(0.f);  // acende à noite (updateLamps)
        if (Mat(k) == Mat::Terrain) {
            // terreno padrão: camadas de textura reais (shaders/terrain); a ordem é a mesma do pbr.frag
            static const char* kLayers[] = {"grama", "campo", "floresta", "chao", "areia", "terra", "seixos"};
            if (!e->lowTextures && e->renderer.loadTerrainLayers(kLayers, 7)) { m.mode = 3; m.roughness = 0.95f; }  // fraco: terreno simples
        }
        e->mats[k] = e->renderer.createMaterial(m);
    }
    e->tintCache.clear();
}

void selectHandle(Engine* e, Scene::Handle h) {
    GameObject* g = e->scene.get(h);
    e->selected = g ? h : Scene::Handle{};
    e->gizmo.target = g;
}

Scene::Handle addMeshObject(Engine* e, const char* name, MeshId mesh, MaterialId mat, glm::vec3 pos, glm::vec3 scale,
                            glm::vec3 eulerDeg, bool reg) {
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
    if (reg) registerObject(e, h);
    return h;
}

static RigidBody* attachShape(Engine* e, GameObject& go, ColliderType type, btCollisionShape* shape, float mass, glm::vec3 offset) {
    auto bh = e->bodies.acquire();
    RigidBody* rb = e->bodies.get(bh);
    if (!rb) { delete shape; return nullptr; }
    rb->centerOffset = offset;
    if (mass > 0.f) go.noCut = true;  // objeto que se move (caixa, barril): não é cortado ao passar por uma abertura
    e->physics.attach(go, *rb, type, shape, mass);
    e->physics.syncFromTransform(*rb);
    return rb;
}

// Collider no tamanho da malha básica (lado/diâmetro 1); a escala do Transform é aplicada pelo Physics
void addBody(Engine* e, Scene::Handle h, ColliderType type, float mass) {
    GameObject* go = e->scene.get(h);
    if (!go) return;
    btCollisionShape* shape = type == ColliderType::Capsule
        ? Physics::makeShape(type, {1.f, 2.f, 1.f})
        : type == ColliderType::ConvexMesh
        ? Physics::makeShape(type, glm::vec3(1.f), e->rockHull.data(), int(e->rockHull.size()))
        : Physics::makeShape(type, glm::vec3(1.f));
    attachShape(e, *go, type, shape, mass, glm::vec3(0.f));
}

void addBoxCollider(Engine* e, Scene::Handle root, glm::vec3 centerLocal, glm::vec3 half) {
    GameObject* go = e->scene.get(root);
    if (!go || go->rigidBody) return;
    auto* comp = new btCompoundShape(true, 1);
    btTransform t;
    t.setIdentity();
    t.setOrigin({centerLocal.x, centerLocal.y, centerLocal.z});
    comp->addChildShape(t, new btBoxShape({half.x, half.y, half.z}));
    attachShape(e, *go, ColliderType::Compound, comp, 0.f, glm::vec3(0.f));
}

void addFloor(Engine* e, float size) {
    Scene::Handle f = addMeshObject(e, "Chao", e->cube, e->matFloor, {0.f, -0.1f, 0.f}, {size, 0.2f, size});
    addBody(e, f, ColliderType::Box, 0.f);
}

void freeStatic(Engine* e, StaticCollision& s) {
    if (s.body) e->physics.removeStatic(s.body);
    delete s.shape;
    for (auto* c : s.children) delete c;
    s = StaticCollision{};
}

void addStaticBoxes(Engine* e, const std::vector<BoxCollider>& boxes, StaticCollision* into) {
    if (boxes.empty()) return;
    StaticCollision s;
    auto* comp = new btCompoundShape(true, int(boxes.size()));
    for (const BoxCollider& b : boxes) {
        auto* box = new btBoxShape({b.half.x, b.half.y, b.half.z});
        btTransform t;
        t.setIdentity();
        t.setOrigin({b.center.x, b.center.y, b.center.z});
        t.setRotation(btQuaternion(btVector3(0, 1, 0), b.yaw) * btQuaternion(btVector3(1, 0, 0), b.pitch));
        comp->addChildShape(t, box);
        s.children.push_back(box);
    }
    s.shape = comp;
    s.body = e->physics.addStatic(comp, glm::vec3(0.f));
    if (into) *into = s;
    else e->statics.push_back(s);
}

void spawnGeoExtras(Engine* e, Geo& g, Scene::Handle rootH, bool local, float detailFar) {
    GameObject* root = e->scene.get(rootH);
    if (!root) return;
    // portas (filhas da raiz: acompanham a construção) e elevadores
    for (const DoorSpec& ds : g.doors) {
        if (e->doorMesh == kInvalidId) {
            MeshBuilder mb;
            mb.box({0.f, 0.f, -0.5f}, {1.f, 1.f, 1.f});  // dobradiça na origem, folha para +X
            e->doorMesh = mb.build(e->renderer);
        }
        glm::vec3 sc, tr, skew; glm::vec4 persp; glm::quat rot;
        glm::decompose(ds.xf, sc, rot, tr, skew, persp);
        Scene::Handle dh = e->scene.create(ds.style ? "Porta de vidro" : "Porta", root);
        GameObject* d = e->scene.get(dh);
        if (!d) break;
        d->transform.setPosition(tr);
        d->transform.setRotation(rot);
        d->transform.setScale({ds.w, ds.h, ds.style ? 0.04f : 0.06f});
        auto rh = e->meshRenderers.acquire();
        if (MeshRenderer* mr = e->meshRenderers.get(rh)) {
            mr->mesh = e->doorMesh;
            mr->material = e->mats[int(ds.style ? Mat::Glass : Mat::Wood)];
            mr->lodFar = detailFar;
            d->meshRenderer = mr;
        }
        if (local) registerObject(e, dh);
        e->doors.push_back({dh, rot, 0.f, ds.w});
    }
    for (const TeleSpec& ts : g.teles) e->teles.push_back({rootH, ts.a, ts.b, ts.la, ts.lb});
}

Scene::Handle spawnGeo(Engine* e, Geo& g, const char* name, bool local, glm::vec3 pos, float yaw, float detailFar) {
    Scene::Handle rootH = e->scene.create(name);
    GameObject* root = e->scene.get(rootH);
    if (!root) return rootH;
    root->transform.setPosition(pos);
    root->transform.setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
    if (local) registerObject(e, rootH);  // raiz antes dos filhos na hierarquia
    for (int layer = 0; layer < Geo::kLayers; ++layer)
    for (int k = 0; k < int(Mat::Count); ++k) {
        MeshBuilder& mb = g.layer(layer)[k];
        if (mb.empty()) continue;
        MeshId id = mb.build(e->renderer);
        if (id == kInvalidId) continue;
        Scene::Handle ch = e->scene.create(matName(Mat(k)), root);
        GameObject* c = e->scene.get(ch);
        if (!c) break;
        auto rh = e->meshRenderers.acquire();
        if (MeshRenderer* mr = e->meshRenderers.get(rh)) {
            mr->mesh = id; mr->material = e->mats[k]; c->meshRenderer = mr;
            // pisos planos só recebem sombra (economiza draw calls no shadow pass)
            Mat m = Mat(k);
            mr->castShadows = !(m == Mat::Asphalt || m == Mat::Road || m == Mat::Sidewalk || m == Mat::Grass || m == Mat::Court || m == Mat::Terrain ||
                                 m == Mat::ChainLink || m == Mat::Mural || m == Mat::Cement);  // tela/pichação: a sombra sairia cheia
            if (layer == 3) mr->lodNear = detailFar;  // versão simples: só onde o detalhe já sumiu
            else if (layer) mr->lodFar = layer == 2 ? std::min(detailFar, kInteriorFar) : detailFar;  // LOD: detalhe/interior só perto
        }
        if (local) registerObject(e, ch);
    }
    spawnGeoExtras(e, g, rootH, local, detailFar);
    if (local) {
        if (!g.colliders.empty()) {
            auto* comp = new btCompoundShape(true, int(g.colliders.size()));
            for (const BoxCollider& b : g.colliders) {
                btTransform t;
                t.setIdentity();
                t.setOrigin({b.center.x, b.center.y, b.center.z});
                t.setRotation(btQuaternion(btVector3(0, 1, 0), b.yaw) * btQuaternion(btVector3(1, 0, 0), b.pitch));
                comp->addChildShape(t, new btBoxShape({b.half.x, b.half.y, b.half.z}));
            }
            attachShape(e, *root, ColliderType::Compound, comp, 0.f, glm::vec3(0.f));
        }
    } else {
        addStaticBoxes(e, g.colliders);
    }
    return rootH;
}

MaterialId tintedMaterial(Engine* e, MaterialId base, glm::vec3 c) {
    uint64_t key = (uint64_t(base) << 32) | (uint64_t(c.r * 255.f) << 16) | (uint64_t(c.g * 255.f) << 8) | uint64_t(c.b * 255.f);
    auto it = e->tintCache.find(key);
    if (it != e->tintCache.end()) return it->second;
    const Material* m = e->renderer.material(base);
    if (!m) return base;
    Material copy = *m;
    copy.albedoFactor = glm::vec4(std::pow(c.r, 2.2f), std::pow(c.g, 2.2f), std::pow(c.b, 2.2f), copy.albedoFactor.a);
    MaterialId id = e->renderer.createMaterial(copy);
    e->tintCache[key] = id;
    return id;
}

void clearScene(Engine* e) {
    e->playing = false;
    clearWorld(e);
    e->scene.forEach([&](GameObject& g, Scene::Handle) { releaseComponents(e, g); });
    e->scene.clear();
    e->meshRenderers.clear();
    e->emitters.clear();
    e->particles.clear();
    for (auto& f : e->flashes) f.life = 0.f;
    for (auto& p : e->projectiles) p.active = false;
    for (auto& b : e->bolts) b.life = 0.f;
    e->animatorPool.clear();
    e->bodies.clear();
    e->objects.clear();
    e->animators.clear();
    e->characters.clear();
    e->retargets.clear();
    e->veg.clear();
    e->recipes.clear();
    e->doors.clear();
    e->teles.clear();
    e->openings.clear();
    e->mapLamps.clear();
    e->renderer.cutCount = 0;
    e->physics.cuts.clear();
    e->doorMesh = kInvalidId;
    if (e->globeMode) {  // saindo da cena vendo o globo: volta o céu do planeta (senão o próximo mapa fica com céu de espaço)
        e->env = e->globeSavedEnv;
        e->sun.direction = e->globeSavedSun;
    }
    e->globeMode = false;
    e->globe = {};
    e->editorCam.maxDistance = 500.f;
    e->clipStores.clear();
    e->animEdit = AnimEditState{};
    e->lightObjects.clear();
    e->game = GameState{};
    e->rules = GameRules{};
    ++e->revision;
    e->attachments.clear();
    e->models.clear();
    e->snapshot.clear();
    e->player = -1;
    e->renderer.clearAssets();
    e->renderer.fogDensity = 0.f;
    TextureManager::instance().releaseAll();
    e->cube = e->sphere = e->capsule = e->rock = kInvalidId;
    e->selected = {};
    e->gizmo.target = nullptr;
    e->pointCount = 0;
    for (auto& t : e->tracers) t.life = 0.f;
    ensureBasics(e);
}

// ---------------- personagens ----------------
int createCharacter(Engine* e, const CharacterParams& p, glm::vec3 pos, float yaw, bool controllable) {
    if (e->characters.size() >= AnimatorPool::capacity()) { e->lastError = "limite de personagens"; return -1; }
    auto model = std::make_unique<LoadedModel>();
    MeshBuilder mb;
    buildCharacter(p, *model, mb);
    MeshId mesh = mb.build(e->renderer, true);
    if (mesh == kInvalidId) return -1;

    char name[32];
    std::snprintf(name, sizeof(name), "Personagem %d", int(e->characters.size()) + 1);
    Scene::Handle rootH = e->scene.create(name);
    GameObject* root = e->scene.get(rootH);
    if (!root) return -1;
    root->transform.setPosition(pos);
    root->transform.setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
    int rootId = registerObject(e, rootH);

    Scene::Handle bodyH = e->scene.create("Corpo", root);
    GameObject* body = e->scene.get(bodyH);
    auto ah = e->animatorPool.acquire();
    Animator* anim = e->animatorPool.get(ah);
    auto rh = e->meshRenderers.acquire();
    MeshRenderer* mr = e->meshRenderers.get(rh);
    if (!body || !anim || !mr) return -1;
    registerObject(e, bodyH);
    anim->skeleton = &model->skeletons[0];
    anim->clips = model->clips.data();
    anim->clipCount = int(model->clips.size());
    anim->space = &root->transform;
    anim->setClip(CC_Idle);
    anim->time = float(e->characters.size()) * 0.37f;  // personagens não ficam sincronizados
    anim->update(0.f, false);
    mr->mesh = mesh; mr->material = e->mats[int(Mat::Painted)];
    body->meshRenderer = mr;
    body->animator = anim;
    e->animators.push_back(anim);
    e->models.push_back(std::move(model));

    // Cápsula de física com origem nos pés (rotação travada: o controle gira o personagem)
    float diam = 0.38f * p.height / 1.75f * (p.build == 2 ? 1.15f : 1.f);
    btCollisionShape* shape = Physics::makeShape(ColliderType::Capsule, {diam, p.height, diam});
    if (RigidBody* rb = attachShape(e, *root, ColliderType::Capsule, shape, 60.f, {0.f, p.height * 0.5f, 0.f})) {
        rb->lockRotX = rb->lockRotY = rb->lockRotZ = true;
        rb->applyAxisConstraints();
        rb->body->setFriction(0.4f);
        rb->body->setActivationState(DISABLE_DEACTIVATION);
    }
    CharacterInfo info;
    info.root = rootH;
    info.anim = anim;
    info.height = p.height;
    for (int k = 0; k < CC_Count; ++k) info.clipMap[k] = k;
    e->characters.push_back(info);
    if (controllable || e->player < 0) setPlayer(e, int(e->characters.size()) - 1);
    return rootId;
}

int characterOfSelection(Engine* e) {
    GameObject* g = e->scene.get(e->selected);
    while (g && g->transform.parent && g->transform.parent->owner) g = g->transform.parent->owner;
    if (!g) return -1;
    for (size_t i = 0; i < e->characters.size(); ++i)
        if (e->scene.get(e->characters[i].root) == g) return int(i);
    return -1;
}

void setPlayer(Engine* e, int ch) {
    if (ch < 0 || ch >= int(e->characters.size())) { e->player = -1; return; }
    e->player = ch;
    if (GameObject* g = e->scene.get(e->characters[ch].root)) {
        if (g->rigidBody && g->rigidBody->body) g->rigidBody->body->setFriction(0.f);  // não gruda nas paredes
    }
}

static void destroyObject(Engine* e, Scene::Handle h) {
    GameObject* g = e->scene.get(h);
    if (!g) return;
    Scene::forEachInSubtree(*g, [&](GameObject& o) { releaseComponents(e, o); });
    e->scene.destroy(h);
}

static bool isGun(int kind) { return kind == WK_Pistol || kind == WK_Rifle || kind == WK_Shotgun || kind == WK_Sniper; }

// Peças (malha + matriz) viram filhos de um objeto novo; `fix` vai na frente de cada matriz
static Scene::Handle spawnParts(Engine* e, const std::vector<ProtoPart>& parts, const glm::mat4& fix, const char* name, glm::vec3 pos,
                                GameObject* parent = nullptr, MaterialId material = kInvalidId) {
    Scene::Handle rootH = e->scene.create(name, parent);
    GameObject* root = e->scene.get(rootH);
    if (!root) return rootH;
    root->transform.setPosition(pos);
    registerObject(e, rootH);
    for (const ProtoPart& pp : parts) {
        glm::vec3 sc, tr, skew; glm::vec4 persp; glm::quat rot;
        glm::decompose(fix * pp.local, sc, rot, tr, skew, persp);
        Scene::Handle ch = e->scene.create("Peça", root);
        GameObject* c = e->scene.get(ch);
        if (!c) break;
        c->transform.setPosition(tr); c->transform.setRotation(rot); c->transform.setScale(sc);
        auto rh = e->meshRenderers.acquire();
        if (MeshRenderer* mr = e->meshRenderers.get(rh)) {
            mr->mesh = pp.mesh; mr->material = material != kInvalidId ? material : pp.mat; mr->castShadows = true;
            c->meshRenderer = mr;
        }
        registerObject(e, ch);
    }
    return rootH;
}

// Acessórios táticos (Low.fbx: trilho, mira red dot, coronha e empunhadura frontal), encaixados pela caixa da arma.
// wb: caixa da arma no espaço dela (mão na origem, cano +Z).
static void addAccessories(Engine* e, GameObject& weapon, uint32_t acc, const AABB& wb, bool procedural) {
    if (!acc) return;
    const std::string file = e->renderer.shaderDir() + "/kit/armas/acessorios.fbx";
    std::vector<ProtoPart> all;
    std::vector<std::string> names;
    AABB fileBox;
    if (!modelParts(e, file, nullptr, all, fileBox, &names)) return;  // lido uma vez; as peças saem pelo nome
    auto pick = [&](const char* key, AABB& box) {
        std::vector<ProtoPart> out;
        box = AABB{glm::vec3(1e30f), glm::vec3(-1e30f)};
        for (size_t i = 0; i < all.size(); ++i) {
            if (names[i].find(key) == std::string::npos) continue;
            out.push_back(all[i]);
            if (const Mesh* m = e->renderer.mesh(all[i].mesh)) {
                AABB b = transformAABB(m->bounds, all[i].local);
                box.min = glm::min(box.min, b.min); box.max = glm::max(box.max, b.max);
            }
        }
        return out;
    };
    // eixo do arquivo: o lado mais comprido; a frente é o lado da mira (a coronha fica atrás)
    AABB dotBox, stockBox, mountBox;
    std::vector<ProtoPart> dot = pick("red_dot", dotBox), stock = pick("backstock", stockBox), mount = pick("mount", mountBox);
    glm::vec3 ext = fileBox.max - fileBox.min;
    glm::mat4 R(1.f);
    if (ext.x > ext.z) R = glm::rotate(glm::mat4(1.f), -1.5707963f, glm::vec3(0, 1, 0));
    glm::vec3 front = glm::vec3(R * glm::vec4((dotBox.min + dotBox.max) * 0.5f - (stockBox.min + stockBox.max) * 0.5f, 0.f));
    if (!dot.empty() && !stock.empty() && front.z < 0.f) R = glm::rotate(glm::mat4(1.f), 3.14159265f, glm::vec3(0, 1, 0)) * R;
    // escala: a mira red dot tem uns 12 cm
    AABB rd = transformAABB(dot.empty() ? fileBox : dotBox, R);
    const float s = 0.12f / std::max(1e-4f, rd.max.z - rd.min.z);
    const float len = wb.max.z - wb.min.z;
    const MaterialId dark = e->mats[int(Mat::GunMetal)], poly = e->mats[int(Mat::Polymer)];
    auto place = [&](const char* key, const char* name, MaterialId mat, auto&& anchorOf, glm::vec3 at) {
        AABB box;
        std::vector<ProtoPart> parts = pick(key, box);
        if (parts.empty()) return;
        AABB rb = transformAABB(box, R);
        glm::vec3 pivot = anchorOf(rb);
        glm::mat4 fix = glm::translate(glm::mat4(1.f), at) * glm::scale(glm::mat4(1.f), glm::vec3(s)) * glm::translate(glm::mat4(1.f), -pivot) * R;
        spawnParts(e, parts, fix, name, glm::vec3(0.f), &weapon, mat);
    };
    auto midX = [](const AABB& b) { return (b.min.x + b.max.x) * 0.5f; };
    auto midZ = [](const AABB& b) { return (b.min.z + b.max.z) * 0.5f; };
    if (acc & ACC_RedDot) {  // trilho + mira em cima, um pouco à frente da mão
        const float h = wb.max.y - wb.min.y;  // topo do corpo da arma (sem contar a massa de mira): ~80% da altura
        glm::vec3 at(0.f, wb.min.y + h * 0.8f, wb.min.z + len * 0.5f);
        place("mount", "Trilho", dark, [&](const AABB& b) { return glm::vec3(midX(b), b.min.y, midZ(b)); }, at);
        float mountH = mount.empty() ? 0.f : (transformAABB(mountBox, R).max.y - transformAABB(mountBox, R).min.y) * s;
        place("red_dot", "Mira red dot", poly, [&](const AABB& b) { return glm::vec3(midX(b), b.min.y, midZ(b)); },
              at + glm::vec3(0.f, mountH * 0.85f, 0.f));
    }
    if (acc & ACC_Stock)  // coronha atrás da empunhadura
        place("backstock", "Coronha tática", poly, [&](const AABB& b) { return glm::vec3(midX(b), b.max.y * 0.5f + b.min.y * 0.5f, b.max.z); },
              glm::vec3(0.f, 0.045f, -0.06f));
    if (acc & ACC_Foregrip)  // empunhadura frontal embaixo do guarda-mão
        place("front", "Empunhadura frontal", poly, [&](const AABB& b) { return glm::vec3(midX(b), b.max.y, midZ(b)); },
              glm::vec3(0.f, procedural ? 0.024f : wb.min.y + (wb.max.y - wb.min.y) * 0.57f, wb.max.z * 0.55f));  // embaixo do guarda-mão
}

int createWeapon(Engine* e, int kind, int ch, const WeaponLook& look) {
    kind = std::clamp(kind, 0, WK_Count - 1);
    // armas feitas de formas geométricas foram retiradas: sem arquivo, vira o modelo realista (pistola: TT-33;
    // as outras: M4A1); faca e taco saem
    if (look.file.empty()) {
        if (kind == WK_Knife || kind == WK_Bat) { e->lastError = "faca e taco foram retirados (só armas realistas)"; return -1; }
        WeaponLook real = look;
        real.file = kind == WK_Pistol ? "kit:armas/tt33" : "kit:armas/m4a1";
        return createWeapon(e, kind == WK_Pistol ? WK_Pistol : WK_Rifle, ch, real);
    }
    glm::vec3 pos = e->editorCam.target + glm::vec3(0, 0.6f, 0);
    glm::vec3 muzzle;
    Scene::Handle h;
    AABB wb;
    if (!look.file.empty()) {
        // modelo do usuário: deita com o cano em +Z, tamanho do tipo, mão no cabo
        std::vector<ProtoPart> parts;
        AABB b;
        if (!modelParts(e, look.file, nullptr, parts, b)) {
            if (e->lastError.empty()) e->lastError = "o arquivo não tem malha para virar arma";
            return -1;
        }
        glm::vec3 ext = b.max - b.min;
        glm::mat4 R(1.f);
        if (ext.x > ext.z) R = glm::rotate(glm::mat4(1.f), -1.5707963f, glm::vec3(0, 1, 0));
        if (look.flip) R = glm::rotate(glm::mat4(1.f), 3.14159265f, glm::vec3(0, 1, 0)) * R;
        AABB rb = transformAABB(b, R);
        static const float kLen[WK_Count] = {0.25f, 0.95f, 1.f, 1.15f, 0.3f, 0.85f};  // pistola um pouco maior: na mão do personagem a TT-33 ficava pequena
        static const float kGrip[WK_Count] = {0.25f, 0.35f, 0.35f, 0.35f, 0.12f, 0.12f};  // mão a partir de trás
        float s = kLen[kind] * std::clamp(look.size, 0.2f, 5.f) / std::max(1e-4f, rb.max.z - rb.min.z);
        rb.min *= s; rb.max *= s;
        bool gun = kind <= WK_Sniper;
        glm::vec3 grip((rb.min.x + rb.max.x) * 0.5f, gun ? rb.min.y + (rb.max.y - rb.min.y) * 0.3f : (rb.min.y + rb.max.y) * 0.5f,
                       rb.min.z + (rb.max.z - rb.min.z) * kGrip[kind]);
        glm::mat4 fix = glm::translate(glm::mat4(1.f), -grip) * glm::scale(glm::mat4(1.f), glm::vec3(s)) * R;
        muzzle = glm::vec3(0.f, (rb.min.y + rb.max.y) * 0.5f + (rb.max.y - rb.min.y) * 0.2f - grip.y, rb.max.z - grip.z);
        wb = AABB{rb.min - grip, rb.max - grip};
        h = spawnParts(e, parts, fix, weaponName(kind), pos);
    } else {
        Geo g;
        g.place(glm::vec3(0.f), 0.f);
        muzzle = buildWeapon(g, kind, !(look.accessories & ACC_Stock));
        h = spawnGeo(e, g, weaponName(kind), true, pos, 0.f);
        e->scene.updateTransforms();
        GameObject* w = e->scene.get(h);
        if (w && subtreeBounds(e, *w, wb)) { wb.min -= pos; wb.max -= pos; }
        else wb = AABB{glm::vec3(-0.03f, -0.1f, -0.4f), glm::vec3(0.03f, 0.1f, 0.7f)};
    }
    GameObject* w = e->scene.get(h);
    if (!w) return -1;
    addAccessories(e, *w, look.accessories, wb, look.file.empty());
    Scene::forEachInSubtree(*w, [](GameObject& o) { o.noCut = true; });  // passa pela porta sem ser cortada
    if (ch >= 0 && ch < int(e->characters.size())) {
        CharacterInfo& c = e->characters[ch];
        if (e->scene.get(c.weapon)) {  // troca a arma antiga
            e->attachments.erase(std::remove_if(e->attachments.begin(), e->attachments.end(),
                                                [&](const Attachment& a) { return a.obj.index == c.weapon.index && a.obj.gen == c.weapon.gen; }),
                                 e->attachments.end());
            destroyObject(e, c.weapon);
        }
        // empunhadura na palma: desce do pulso e gira para o cano seguir os dedos
        e->attachments.push_back({h, c.anim, c.handJoint, c.handFix * weaponGripOffset(kind, c.height)});
        c.weapon = h;
        c.weaponKind = kind;
        c.muzzle = muzzle;
        // som: cada fuzil do kit tem o seu; os outros pelo tipo
        {
            std::string f = look.file;
            std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch2) { return char(std::tolower(ch2)); });
            static const char* byKind[] = {"tiro_pistola", "tiro_m4", "tiro_escopeta", "tiro_sniper", "golpe_faca", "golpe_taco"};
            c.scope = f.find("m4colt") != std::string::npos;  // modelos com luneta
            c.shotSound = f.find("ak47") != std::string::npos ? "tiro_ak47"
                        : f.find("ak74") != std::string::npos ? "tiro_ak74"
                        : f.find("m4") != std::string::npos ? "tiro_m4" : byKind[std::clamp(kind, 0, WK_Count - 1)];
        }
        // onde a mão esquerda segura: guarda-mão (ou a empunhadura frontal, se tiver)
        c.hasSupport = kind == WK_Rifle || kind == WK_Shotgun || kind == WK_Sniper || kind == WK_Pistol;
        if (c.hasSupport && kind != WK_Pistol) {  // pistola: a mão esquerda segura o pulso direito (veja updateSupportHands)
            const float len = wb.max.z - wb.min.z, hgt = wb.max.y - wb.min.y;
            if (look.accessories & ACC_Foregrip)
                c.support = glm::vec3(0.f, (look.file.empty() ? 0.024f : wb.min.y + hgt * 0.57f) - 0.05f, wb.max.z * 0.55f);
            else if (look.file.empty())
                c.support = kind == WK_Shotgun ? glm::vec3(0.f, 0.01f, 0.43f) : glm::vec3(0.f, 0.03f, kind == WK_Sniper ? 0.4f : 0.38f);
            else
                c.support = glm::vec3(0.f, wb.min.y + hgt * 0.55f, wb.min.z + len * 0.64f);
            // alça de mira: no alto da arma, um terço do comprimento a partir da coronha
            c.sight = glm::vec3(0.f, wb.max.y - hgt * 0.06f, wb.min.z + len * 0.36f);
            c.butt = glm::vec3(0.f, wb.min.y + hgt * 0.62f, wb.min.z + 0.01f);
            c.gripPt = glm::vec3(0.f, wb.min.y + hgt * 0.4f, wb.min.z + len * 0.315f);
            c.guardPt = glm::vec3(0.f, wb.min.y + hgt * 0.68f, wb.min.z + len * 0.63f);
        } else if (kind == WK_Pistol) {  // pistola: alça de mira em cima, atrás; cabo embaixo, atrás
            const float len = wb.max.z - wb.min.z, hgt = wb.max.y - wb.min.y;
            c.sight = glm::vec3(0.f, wb.max.y - hgt * 0.03f, wb.min.z + len * 0.15f);
            c.gripPt = glm::vec3(0.f, wb.min.y + hgt * 0.35f, wb.min.z + len * 0.2f);
        }
        int base = armedClipBase(kind);
        if (c.anim && base >= 0 && c.clipMap[base + AP_Idle] >= 0) c.anim->setClip(c.clipMap[base + AP_Idle]);
        updateAttachments(e);
    }
    return w->id;
}

void updateAttachments(Engine* e) {
    for (const Attachment& a : e->attachments) {
        GameObject* o = e->scene.get(a.obj);
        if (!o || !a.anim) continue;
        glm::mat4 space = a.anim->space ? a.anim->space->world() : glm::mat4(1.f);
        glm::mat4 m = space * a.anim->global[a.joint] * a.offset;
        glm::vec3 x(m[0]), y(m[1]), z(m[2]);
        glm::mat3 rot(glm::normalize(x), glm::normalize(y), glm::normalize(z));
        o->transform.position = glm::vec3(m[3]);
        o->transform.rotation = glm::normalize(glm::quat_cast(rot));
        o->transform.markDirty();
        if (!o->transform.parent) o->transform.updateWorld(glm::mat4(1.f), false);
    }
}

// Gira um osso e tudo o que vem depois dele em volta de `pivot` (espaço do esqueleto)
static void rotateJointSubtree(Animator& a, int root, const glm::quat& r, glm::vec3 pivot) {
    const Skeleton& S = *a.skeleton;
    glm::mat4 M = glm::translate(glm::mat4(1.f), pivot) * glm::mat4_cast(r) * glm::translate(glm::mat4(1.f), -pivot);
    bool in[kMaxJoints] = {};
    for (int k = 0; k < S.jointCount; ++k) {
        int i = S.order[k];
        in[i] = i == root || (S.parent[i] >= 0 && in[S.parent[i]]);
        if (!in[i]) continue;
        a.global[i] = M * a.global[i];
        a.palette[i] = a.global[i] * S.inverseBind[i];
    }
}

static int jointNamed(const Skeleton& S, const char* suffix) {
    size_t n = std::strlen(suffix);
    for (int j = 0; j < S.jointCount; ++j) {
        size_t m = std::strlen(S.name[j]);
        if (m >= n && std::strcmp(S.name[j] + m - n, suffix) == 0) return j;
    }
    return -1;
}

// Gira a mão (com os dedos) para os dedos apontarem em `fingers` e o polegar para o lado de `thumb`;
// depois fecha os dedos em volta de `curlAxis` (segurando o cabo/guarda-mão). Tudo no espaço do esqueleto.
static void poseHand(Animator& a, int hand, bool leftSide, glm::vec3 fingers, glm::vec3 thumb, glm::vec3 curlAxis, float curl, float w) {
    const Skeleton& S = *a.skeleton;
    const char* side = leftSide ? "LeftHand" : "RightHand";
    char buf[48];
    auto j = [&](const char* f, int k) { std::snprintf(buf, sizeof(buf), "%s%s%d", side, f, k); return jointNamed(S, buf); };
    int mid = j("Middle", 1), th = j("Thumb", 1);
    if (hand < 0 || mid < 0 || w <= 0.f) return;
    auto P = [&](int k) { return glm::vec3(a.global[k][3]); };
    glm::vec3 p = P(hand);
    glm::vec3 f = P(mid) - p;
    if (glm::length(f) < 1e-5f) return;
    glm::quat r1 = glm::rotation(glm::normalize(f), glm::normalize(fingers));
    rotateJointSubtree(a, hand, glm::slerp(glm::quat(1, 0, 0, 0), r1, w), p);
    if (th >= 0) {
        glm::vec3 fd = glm::normalize(P(mid) - p);
        glm::vec3 t = P(th) - p, want = thumb;
        t -= fd * glm::dot(t, fd); want -= fd * glm::dot(want, fd);
        if (glm::length(t) > 1e-5f && glm::length(want) > 1e-5f) {
            glm::quat r2 = glm::rotation(glm::normalize(t), glm::normalize(want));
            rotateJointSubtree(a, hand, glm::slerp(glm::quat(1, 0, 0, 0), r2, w), p);
        }
    }
    // dedos fechando (3 falanges de cada dedo)
    for (const char* f : {"Index", "Middle", "Ring", "Pinky"})
        for (int k = 1; k <= 3; ++k) {
            int jj = j(f, k);
            if (jj < 0) continue;
            rotateJointSubtree(a, jj, glm::angleAxis(curl * w, glm::normalize(curlAxis)), P(jj));
        }
}

// IK de dois ossos (ombro-cotovelo-pulso) direto nas matrizes globais da pose: leva o pulso até `target`
// mantendo o plano do cotovelo; gira cada osso (com tudo o que vem depois dele) em volta da própria junta.
// poleHint (opcional): para onde o cotovelo deve apontar; sem ele, mantém o plano do cotovelo da animação
static void twoBoneIK(Animator& a, int up, int fo, int ha, glm::vec3 target, glm::vec3 poleHint = glm::vec3(0.f)) {
    const Skeleton& S = *a.skeleton;
    auto pos = [&](int j) { return glm::vec3(a.global[j][3]); };
    auto rotateSubtree = [&](int root, const glm::quat& r, glm::vec3 pivot) {
        glm::mat4 M = glm::translate(glm::mat4(1.f), pivot) * glm::mat4_cast(r) * glm::translate(glm::mat4(1.f), -pivot);
        bool in[kMaxJoints] = {};
        for (int k = 0; k < S.jointCount; ++k) {
            int i = S.order[k];
            in[i] = i == root || (S.parent[i] >= 0 && in[S.parent[i]]);
            if (!in[i]) continue;
            a.global[i] = M * a.global[i];
            a.palette[i] = a.global[i] * S.inverseBind[i];
        }
    };
    glm::vec3 p0 = pos(up), p1 = pos(fo), p2 = pos(ha);
    float la = glm::length(p1 - p0), lb = glm::length(p2 - p1);
    if (la < 1e-5f || lb < 1e-5f) return;
    glm::vec3 to = target - p0;
    float d = glm::clamp(glm::length(to), std::abs(la - lb) + 1e-4f, la + lb - 1e-4f);
    glm::vec3 u = glm::length(to) > 1e-5f ? glm::normalize(to) : glm::normalize(p2 - p0);
    glm::vec3 pole = glm::length(poleHint) > 1e-5f ? poleHint : (p1 - p0);
    pole -= u * glm::dot(pole, u);
    pole = glm::length(pole) > 1e-5f ? glm::normalize(pole) : glm::vec3(0.f, -1.f, 0.f);
    float cosA = glm::clamp((la * la + d * d - lb * lb) / (2.f * la * d), -1.f, 1.f);
    glm::vec3 elbow = p0 + u * (la * cosA) + pole * (la * std::sqrt(1.f - cosA * cosA));
    rotateSubtree(up, glm::rotation(glm::normalize(p1 - p0), glm::normalize(elbow - p0)), p0);
    glm::vec3 wrist = pos(ha), e1 = pos(fo);
    glm::vec3 want = p0 + u * d;
    if (glm::length(wrist - e1) > 1e-5f && glm::length(want - e1) > 1e-5f)
        rotateSubtree(fo, glm::rotation(glm::normalize(wrist - e1), glm::normalize(want - e1)), e1);
}

void updateSupportHands(Engine* e, float dt) {
    static const bool off = std::getenv("MOTOR_SEM_IK") != nullptr;  // comparação nos testes (antes/depois)
    if (off) return;
    for (CharacterInfo& c : e->characters) {
        Animator* a = c.anim;
        GameObject* w = e->scene.get(c.weapon);
        if (!a || !a->skeleton || !w || !c.hasSupport) { c.supportW = 0.f; continue; }
        // só nas animações com as duas mãos na arma (não ao recarregar nem morrendo)
        // pistola: só correndo e mirando (parado/andando ela fica ao lado da perna, numa mão)
        const bool pistol = c.weaponKind == WK_Pistol;
        bool armed = false;
        if (pistol)
            for (int k : {CC_Pistol + AP_Run, CC_Pistol + AP_Aim, CC_Pistol + AP_AimWalk})
                armed |= c.clipMap[k] >= 0 && a->clipIndex == c.clipMap[k] && !a->posing && !a->preview;
        else
            for (int k : {CC_Rifle + AP_Idle, CC_Rifle + AP_Walk, CC_Rifle + AP_Run, CC_Rifle + AP_Aim, CC_Rifle + AP_AimWalk, int(CC_RifleBack),
                          int(CC_RifleLeft), int(CC_RifleRight), int(CC_RifleFire), int(CC_Jump), int(CC_CrouchWalk)})
                armed |= c.clipMap[k] >= 0 && a->clipIndex == c.clipMap[k] && !a->posing && !a->preview;
        bool want = armed && !c.dying && c.reloadTimer <= 0.f;
        c.supportW = want ? std::min(1.f, c.supportW + dt * 6.f) : std::max(0.f, c.supportW - dt * 6.f);
        // mirando com arma longa: a arma sobe até o olho (bochecha na coronha, coronha no ombro)
        // primeira pessoa: atirar sem mirar só aponta a arma (no ombro); mirar leva a alça de mira ao olho
        const bool running = c.clipMap[CC_Rifle + AP_Run] >= 0 && a->clipIndex == c.clipMap[CC_Rifle + AP_Run];
        bool raise = want && (c.aimHold || c.aimTimer > 0.f) && !running && (!pistol || c.firstPerson);
        bool ads = raise && (c.firstPerson ? c.aimHold : !pistol);
        c.raiseW = raise ? std::min(1.f, c.raiseW + dt * 9.f) : std::max(0.f, c.raiseW - dt * 7.f);
        c.adsW = ads ? std::min(1.f, c.adsW + dt * 7.f) : std::max(0.f, c.adsW - dt * 7.f);
        if (c.supportW <= 0.f) continue;
        int up, fo, ha, rup, rfo, rha, head;
        if (const Retarget* rt = a->retarget) {
            up = rt->targetOf[CJ_LUpper]; fo = rt->targetOf[CJ_LFore]; ha = rt->targetOf[CJ_LHand];
            rup = rt->targetOf[CJ_RUpper]; rfo = rt->targetOf[CJ_RFore]; rha = rt->targetOf[CJ_RHand]; head = rt->targetOf[CJ_Head];
        } else if (a->skeleton->jointCount == CJ_Total) {
            up = CJ_LUpper; fo = CJ_LFore; ha = CJ_LHand; rup = CJ_RUpper; rfo = CJ_RFore; rha = CJ_RHand; head = CJ_Head;
        } else continue;  // esqueleto sem humanoide reconhecido
        if (up < 0 || fo < 0 || ha < 0 || rfo < 0 || rha < 0) continue;
        glm::mat4 space = a->space ? a->space->world() : glm::mat4(1.f);
        auto world = [&](int j) { return glm::vec3(space * a->global[j][3]); };
        // A animação foi feita com as duas mãos na arma: a arma vai da palma direita (cabo) na direção da palma
        // esquerda (guarda-mão), com o topo para cima. Cada arma se acomoda pelo próprio cabo e guarda-mão.
        const float palm = 0.075f * c.height / 1.75f;
        auto palmOf = [&](int fore, int hand) {
            glm::vec3 wr = world(hand), el = world(fore);
            return wr + (glm::length(wr - el) > 1e-4f ? glm::normalize(wr - el) : glm::vec3(0.f)) * palm;
        };
        glm::vec3 R = palmOf(rfo, rha), L = palmOf(fo, ha);
        GameObject* body = e->scene.get(c.root);
        glm::vec3 fwd = body ? body->transform.rotation * glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 0.f, 1.f);
        // Empunhadura de fuzil como no treino de tiro: a soleira da coronha fica no "bolso" do ombro direito (entre a
        // clavícula e o deltoide). Pronto baixo: cano 35° para baixo e um pouco para dentro. Mirando: cano para a
        // frente e a cabeça desce até o olho direito ficar atrás da alça de mira (bochecha apoiada na coronha).
        // A mão direita vai ao cabo e a esquerda ao guarda-mão (IK); pernas e tronco seguem a animação.
        const float scale = c.height / 1.75f;
        glm::vec3 left = body ? body->transform.rotation * glm::vec3(1.f, 0.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
        const glm::vec3 upW(0.f, 1.f, 0.f);
        auto basis = [](glm::vec3 x, glm::vec3 y) {
            x = glm::normalize(x);
            glm::vec3 z = glm::normalize(glm::cross(x, y));
            return glm::mat3(x, glm::cross(z, x), z);
        };
        const bool fpView = c.firstPerson && e->fpEyeValid && !c.dying;
        // pronto baixo: 35° (em primeira pessoa 20°, para a arma aparecer na diagonal embaixo da tela)
        const float down = glm::radians(fpView ? 14.f : 35.f);
        // para onde se olha (o jogador em primeira pessoa olha para cima e para baixo; os robôs, reto)
        const glm::vec3 view = fwd * std::cos(c.viewPitch) + upW * std::sin(c.viewPitch);
        const glm::vec3 viewUp = -fwd * std::sin(c.viewPitch) + upW * std::cos(c.viewPitch);
        glm::vec3 dLow = glm::normalize(view * std::cos(down) - viewUp * std::sin(down) + left * (fpView ? 0.30f : 0.18f));
        glm::vec3 d = glm::normalize(glm::mix(dLow, view, c.firstPerson ? c.raiseW : c.adsW));
        if (fpView) {  // primeira pessoa: baixa na diagonal parado, quase reta atirando sem mirar, no olho mirando
            glm::vec3 dHip = glm::normalize(view * std::cos(0.05f) - viewUp * std::sin(0.05f) + left * 0.03f);
            d = glm::normalize(glm::mix(glm::mix(dLow, dHip, c.raiseW), view, c.adsW));
        }
        glm::mat4 toSkel = glm::inverse(space);
        if (pistol && fpView) {
            // pistola em primeira pessoa: à frente do olho, um pouco à direita e abaixo; mirando, a alça de mira
            // vem para o centro. A mão direita vai ao cabo (IK) e a esquerda envolve a direita.
            glm::vec3 dp = glm::normalize(glm::mix(view * std::cos(0.15f) - viewUp * std::sin(0.15f) + left * 0.06f, view, c.adsW));
            glm::quat qp = glm::normalize(glm::quat_cast(basis(dp, upW) * glm::transpose(basis(glm::vec3(0.f, 0.f, 1.f), upW))));
            glm::quat cur = w->transform.rotation;
            if (glm::dot(cur, qp) < 0.f) qp = -qp;
            glm::vec3 sightAt = e->fpEye + (view * glm::mix(0.34f, 0.30f, c.adsW) - viewUp * glm::mix(0.10f, 0.012f, c.adsW)
                                            - left * glm::mix(0.09f, 0.f, c.adsW)) * scale;
            w->transform.position = glm::mix(w->transform.position, sightAt - qp * c.sight, c.supportW);
            w->transform.rotation = glm::normalize(glm::slerp(cur, qp, c.supportW));
            w->transform.markDirty();
            if (!w->transform.parent) w->transform.updateWorld(glm::mat4(1.f), false);
            glm::vec3 wf = glm::normalize(glm::vec3(toSkel * glm::vec4(w->transform.rotation * glm::vec3(0, 0, 1), 0.f)));
            glm::vec3 wu = glm::normalize(glm::vec3(toSkel * glm::vec4(w->transform.rotation * glm::vec3(0, 1, 0), 0.f)));
            glm::vec3 wl = glm::normalize(glm::cross(wu, wf));
            if (rup >= 0) {
                glm::vec3 g = glm::vec3(toSkel * w->transform.world() * glm::vec4(c.gripPt, 1.f));
                glm::vec3 wristR = g - wf * (0.05f * scale) - wl * (0.02f * scale) + wu * (0.035f * scale);
                glm::vec3 poleR = glm::vec3(toSkel * glm::vec4(-upW - left * 0.6f, 0.f));
                twoBoneIK(*a, rup, rfo, rha, glm::mix(glm::vec3(a->global[rha][3]), wristR, c.supportW), poleR);
                poseHand(*a, rha, false, glm::normalize(wf * 0.6f - wu * 0.6f + wl * 0.2f), glm::normalize(wu + wf * 0.3f), glm::normalize(-wu + wf * 0.3f), -1.2f, c.supportW);
            }
            glm::vec3 wr = world(rha) + (left * 0.035f - upW * 0.04f) * scale;
            glm::vec3 poleL = glm::vec3(toSkel * glm::vec4(-upW + left * 0.5f, 0.f));
            twoBoneIK(*a, up, fo, ha, glm::mix(glm::vec3(a->global[ha][3]), glm::vec3(toSkel * glm::vec4(wr, 1.f)), c.supportW), poleL);
            // mão esquerda fechada por cima da direita (dedos envolvendo os dedos da mão do cabo)
            poseHand(*a, ha, true, glm::normalize(wf * 0.3f - wl * 0.7f - wu * 0.3f), glm::normalize(wu + wf * 0.4f), wf, -1.1f, c.supportW);
            continue;
        }
        if (pistol) {  // reforço para o coice: a mão esquerda envolve o pulso direito, por baixo e do lado de dentro
            glm::vec3 lft = body ? body->transform.rotation * glm::vec3(1.f, 0.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
            glm::vec3 wr = world(rha) + (lft * 0.04f - glm::vec3(0.f, 0.045f, 0.f)) * (c.height / 1.75f);
            glm::vec3 now = glm::vec3(a->global[ha][3]);
            twoBoneIK(*a, up, fo, ha, glm::mix(now, glm::vec3(glm::inverse(space) * glm::vec4(wr, 1.f)), c.supportW));
            continue;
        }
        glm::quat q = glm::normalize(glm::quat_cast(basis(d, upW) * glm::transpose(basis(glm::vec3(0.f, 0.f, 1.f), upW))));
        glm::vec3 shoulder = rup >= 0 ? world(rup) : R;
        glm::vec3 pocket = shoulder + (fwd * glm::mix(0.09f, 0.05f, c.adsW) + left * 0.07f - upW * glm::mix(0.035f, -0.03f, c.adsW)) * scale;
        {
            glm::quat cur = w->transform.rotation;
            if (glm::dot(cur, q) < 0.f) q = -q;
            glm::vec3 place = pocket - q * c.butt;
            if (fpView) {  // primeira pessoa: a arma fica num ponto fixo em relação ao olho (a câmera)
                const glm::vec3 lowAt = e->fpEye + (view * 0.32f - viewUp * 0.115f - left * 0.10f) * scale;
                const glm::vec3 hipAt = e->fpEye + (view * 0.30f - viewUp * 0.105f - left * 0.095f) * scale;
                const glm::vec3 adsAt = e->fpEye + (view * 0.26f - viewUp * 0.012f) * scale;
                place = glm::mix(glm::mix(lowAt, hipAt, c.raiseW), adsAt, c.adsW) - q * c.sight;
            }
            w->transform.position = glm::mix(w->transform.position, place, c.supportW);
            w->transform.rotation = glm::normalize(glm::slerp(cur, q, c.supportW));
            w->transform.markDirty();
            if (!w->transform.parent) w->transform.updateWorld(glm::mat4(1.f), false);
        }
        if (c.adsW > 0.f && head >= 0 && !fpView) {  // cabeça na coronha: o olho direito vai para trás da alça de mira
            glm::vec3 hp = world(head);
            glm::vec3 eye = hp + (upW * 0.075f + fwd * 0.085f - left * 0.032f) * scale;
            glm::vec3 sightW = w->transform.position + w->transform.rotation * c.sight;
            glm::vec3 want = sightW - d * (0.075f * scale) + upW * (0.012f * scale);
            glm::vec3 pivot = glm::vec3(toSkel * glm::vec4(hp, 1.f));
            glm::vec3 from = glm::normalize(glm::vec3(toSkel * glm::vec4(eye - hp, 0.f)));
            glm::vec3 to = glm::normalize(glm::vec3(toSkel * glm::vec4(want - hp, 0.f)));
            glm::quat r = glm::rotation(from, to);
            float ang = glm::angle(r);
            const float maxAng = glm::radians(28.f);
            if (ang > maxAng) r = glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), r, maxAng / ang);
            r = glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), r, c.adsW * c.supportW);
            rotateJointSubtree(*a, head, r, pivot);
        }
        // eixos da arma no esqueleto: frente (cano), cima e esquerda
        glm::vec3 wf = glm::normalize(glm::vec3(toSkel * glm::vec4(w->transform.rotation * glm::vec3(0, 0, 1), 0.f)));
        glm::vec3 wu = glm::normalize(glm::vec3(toSkel * glm::vec4(w->transform.rotation * glm::vec3(0, 1, 0), 0.f)));
        glm::vec3 wl = glm::normalize(glm::cross(wu, wf));
        auto onGun = [&](glm::vec3 local) { return glm::vec3(toSkel * w->transform.world() * glm::vec4(local, 1.f)); };
        if (rup >= 0) {  // mão direita no cabo: palma do lado direito do cabo, dedos dando a volta pela frente
            glm::vec3 g = onGun(c.gripPt);
            glm::vec3 wristR = g - wf * (0.06f * scale) - wl * (0.035f * scale) + wu * (0.03f * scale);
            glm::vec3 nowR = glm::vec3(a->global[rha][3]);
            glm::vec3 poleR = glm::vec3(toSkel * glm::vec4(-upW - left * 0.55f - fwd * 0.1f, 0.f));  // cotovelo direito para baixo e para fora
            twoBoneIK(*a, rup, rfo, rha, glm::mix(nowR, wristR, c.supportW), poleR);
            glm::vec3 gripAxis = glm::normalize(-wu + wf * 0.3f);
            const float cr = fpView ? -1.1f : -0.45f;  // dedos fechando em volta do cabo
            poseHand(*a, rha, false, glm::normalize(wf * 0.8f + wl * 0.35f - wu * 0.35f), glm::normalize(wu + wl * 0.4f), gripAxis, cr, c.supportW);
        }
        // mão esquerda no guarda-mão: palma por baixo, dedos subindo pelo lado direito, polegar pelo lado esquerdo
        {
            glm::vec3 h = onGun(c.guardPt);
            glm::vec3 wristL = h - wu * (0.06f * scale) + wl * (0.03f * scale) - wf * (0.05f * scale);
            glm::vec3 nowL = glm::vec3(a->global[ha][3]);
            glm::vec3 poleL = glm::vec3(toSkel * glm::vec4(-upW + left * 0.35f, 0.f));  // cotovelo esquerdo por baixo da arma
            twoBoneIK(*a, up, fo, ha, glm::mix(nowL, wristL, c.supportW), poleL);
            const float cl = fpView ? -1.0f : -0.4f;   // dedos subindo pelo lado do guarda-mão (de perto: bem fechados)
            poseHand(*a, ha, true, glm::normalize(wf * 0.5f - wl * 0.6f + wu * 0.25f), glm::normalize(wf + wu * 0.5f), wf, cl, c.supportW);
        }
    }
}

// ---------------- primeira pessoa ----------------
static int headJointOf(const Animator& a) {
    if (const Retarget* rt = a.retarget) return rt->targetOf[CJ_Head];
    return a.skeleton && a.skeleton->jointCount == CJ_Total ? int(CJ_Head) : -1;
}

// Olho do jogador: um pouco acima e à frente do osso da cabeça (pose da animação, antes do IK das mãos).
// A altura é suavizada (o balanço da corrida não sacode a tela); na horizontal acompanha o corpo sem atraso.
void updateFirstPersonEye(Engine* e, float dt) {
    if (!e->playing || !e->firstPerson || e->player < 0 || e->player >= int(e->characters.size())) { e->fpEyeValid = false; return; }
    CharacterInfo& c = e->characters[e->player];
    GameObject* body = e->scene.get(c.root);
    Animator* a = c.anim;
    int head = a && a->skeleton ? headJointOf(*a) : -1;
    if (!body || c.dying) { e->fpEyeValid = false; return; }
    const float scale = c.height / 1.75f;
    glm::vec3 fwd = body->transform.rotation * glm::vec3(0.f, 0.f, 1.f);
    glm::vec3 eye;
    if (head >= 0) {
        glm::mat4 space = a->space ? a->space->world() : glm::mat4(1.f);
        eye = glm::vec3(space * a->global[head][3]) + (glm::vec3(0.f, 0.085f, 0.f) + fwd * 0.11f) * scale;
    } else {
        eye = body->transform.position + glm::vec3(0.f, c.height * (c.crouch ? 0.62f : 0.93f), 0.f) + fwd * 0.1f;
    }
    if (e->fpEyeValid) eye.y = glm::mix(e->fpEye.y, eye.y, 1.f - std::exp(-14.f * dt));
    e->fpEye = eye;
    e->fpEyeValid = true;
}

// Câmera no olho olhando para onde o dedo apontou; a cabeça (com capacete/boné) some para não tapar a vista.
// Mirando, o campo de visão fecha um pouco (sniper: luneta, bem mais).
void applyFirstPerson(Engine* e) {
    // arma escondida enquanto se olha pela luneta (volta ao sair da mira)
    auto showWeapon = [&](CharacterInfo* c, bool on) {
        if (!c) return;
        if (GameObject* w = e->scene.get(c->weapon))
            if (w->active != on) Scene::forEachInSubtree(*w, [&](GameObject& o) { o.active = on; });
    };
    CharacterInfo* pc = e->player >= 0 && e->player < int(e->characters.size()) ? &e->characters[e->player] : nullptr;
    if (!e->firstPerson || !e->playing || !e->fpEyeValid || !pc) {
        e->camera.fovYDeg = e->baseFov;
        if (e->fpScoped) { showWeapon(pc, true); e->fpScoped = false; }
        return;
    }
    CharacterInfo& c = *pc;
    glm::vec3 view = e->editorCam.forward();
    const bool scoped = c.scope && c.adsW > 0.85f;
    if (scoped != e->fpScoped) { showWeapon(&c, !scoped); e->fpScoped = scoped; }
    if (scoped) {  // luneta: a mira balança devagar (respiração), e o tiro vai junto
        const float t = e->renderer.time;
        glm::vec3 rt = glm::normalize(glm::cross(view, glm::vec3(0, 1, 0)));
        glm::vec3 up = glm::cross(rt, view);
        view = glm::normalize(view + rt * (0.0045f * std::sin(t * 0.9f)) + up * (0.0035f * std::sin(t * 1.4f + 0.7f)));
    }
    static const bool outside = std::getenv("MOTOR_FP_DE_FORA") != nullptr;  // testes: vê a pose de fora
    if (outside) {
        e->camera.lookAt(e->fpEye + glm::vec3(1.1f, 0.15f, 0.45f), e->fpEye + view * 0.3f - glm::vec3(0, 0.15f, 0));
        return;
    }
    e->camera.lookAt(e->fpEye, e->fpEye + view);
    e->camera.nearZ = 0.035f;
    const float zoom = c.scope ? 0.36f : 0.72f;
    e->camera.fovYDeg = e->baseFov * glm::mix(1.f, zoom, c.adsW);
    e->editorCam.turnSpeed = 0.0042f * e->camera.fovYDeg / e->baseFov;  // com zoom, o dedo gira menos
    // a cabeça (com capacete/boné) some para não tapar a vista; na luneta, os braços também
    if (Animator* a = c.anim; a && a->skeleton) {
        int head = headJointOf(*a);
        if (head < 0) return;
        const Skeleton& S = *a->skeleton;
        int arms[2] = {-1, -1};
        if (scoped && a->retarget) { arms[0] = a->retarget->targetOf[CJ_LUpper]; arms[1] = a->retarget->targetOf[CJ_RUpper]; }
        for (int root : {head, arms[0], arms[1]}) {
            if (root < 0) continue;
            glm::vec3 hp = glm::vec3(a->global[root][3]);
            glm::mat4 M = glm::translate(glm::mat4(1.f), hp) * glm::scale(glm::mat4(1.f), glm::vec3(1e-3f)) * glm::translate(glm::mat4(1.f), -hp);
            bool in[kMaxJoints] = {};
            for (int k = 0; k < S.jointCount; ++k) {
                int i = S.order[k];
                in[i] = i == root || (S.parent[i] >= 0 && in[S.parent[i]]);
                if (in[i]) a->palette[i] = M * a->global[i] * S.inverseBind[i];
            }
        }
    }
}

// ---------------- jogador ----------------
// Mira de terceira pessoa: um pouco acima do centro da câmera (a câmera fica inclinada para baixo)
static glm::vec3 aimDirection(Engine* e) {
    glm::vec3 f = glm::normalize(e->camera.rotation * glm::vec3(0, 0, -1));
    if (e->firstPerson) return f;  // primeira pessoa: o tiro vai para o centro da tela
    return glm::normalize(f + glm::vec3(0.f, 0.28f, 0.f));
}

// Tiro com arma da partida: dano da tabela (cabeça 2,5x), queda com a distância, bagos da escopeta, dispersão
// menor mirando (sniper sem mirar é bem imprecisa)
static void fireGun(Engine* e, CharacterInfo& c, GameObject& root, btRigidBody* self) {
    glm::vec3 aim = aimDirection(e), from = e->camera.position;
    if (!e->firstPerson) {
        glm::quat q = glm::angleAxis(std::atan2(aim.x, aim.z), glm::vec3(0, 1, 0));
        btTransform t = self->getWorldTransform();
        t.setRotation({q.x, q.y, q.z, q.w});
        self->setWorldTransform(t);
        root.transform.setRotation(q);
    }
    e->fireCooldown = c.gunInterval;
    c.aimTimer = 1.5f;
    float spread = c.gunSpread * (c.aimHold ? 0.25f : 1.f);
    if (c.weaponKind == WK_Sniper && !c.aimHold) spread = 0.06f;
    if (c.gunPellets > 1) spread = c.gunSpread;
    glm::vec3 muzzle = from + aim;
    if (GameObject* w = e->scene.get(c.weapon)) muzzle = glm::vec3(w->transform.world() * glm::vec4(c.muzzle, 1.f));
    playSound(e, c.shotSound.empty() ? "tiro_m4" : c.shotSound.c_str(), muzzle, 1.f);
    addFlash(e, fxMuzzleFlash(e->particles, e->fxRng, muzzle, aim), 0.06f);
    for (int k = 0; k < c.gunPellets; ++k) {
        glm::vec3 j(e->fxRng.range(-1.f, 1.f), e->fxRng.range(-1.f, 1.f), e->fxRng.range(-1.f, 1.f));
        glm::vec3 dir = glm::normalize(aim + j * spread);
        glm::vec3 hit; GameObject* obj = nullptr; float frac = 1.f;
        bool any = e->physics.raycastIgnoring(from, from + dir * 300.f, self, hit, obj, frac);
        glm::vec3 end = any ? hit : from + dir * 300.f;
        fxBullet(e->particles, muzzle, end);
        if (!any) continue;
        fxImpact(e->particles, e->fxRng, hit, dir);
        if (k == 0) playSound(e, "impacto", end, 0.5f);
        if (!obj) continue;
        float dist = glm::length(hit - from);
        float dmg = c.gunDmg * (dist > c.gunFalloff ? std::max(0.3f, 1.f - 0.7f * (dist - c.gunFalloff) / (1.5f * c.gunFalloff)) : 1.f);
        GameObject* r = rootOf(e, obj);
        for (const CharacterInfo& o : e->characters)
            if (r && e->scene.get(o.root) == r && &o != &c) {
                bool head = hit.y > r->transform.position.y + o.height * 0.83f;
                if (head) dmg *= 2.5f;
                e->hitMarker = 0.25f; e->hitHead = head;
            }
        damageAt(e, obj, dmg, from);
    }
}

static void fire(Engine* e, CharacterInfo& c, GameObject& root, btRigidBody* self) {
    int kind = c.weaponKind;
    bool melee = !isGun(kind);
    if (!melee && c.gunDmg > 0.f) { fireGun(e, c, root, self); return; }
    glm::vec3 camFwd = aimDirection(e);
    glm::vec3 from, dir;
    float range, power, cooldown;
    if (melee) {
        glm::vec3 fwd = root.transform.rotation * glm::vec3(0, 0, 1);
        from = root.transform.position + glm::vec3(0, c.height * 0.7f, 0);
        dir = glm::normalize(glm::vec3(fwd.x, 0.f, fwd.z));
        range = 1.9f; power = kind == WK_Bat ? 9.f : 5.f; cooldown = 0.45f;
    } else {
        from = e->camera.position;
        dir = camFwd;
        range = 300.f;
        power = kind == WK_Pistol ? 6.f : kind == WK_Rifle ? 8.f : kind == WK_Shotgun ? 14.f : 22.f;
        cooldown = kind == WK_Pistol ? 0.25f : kind == WK_Rifle ? 0.1f : kind == WK_Shotgun ? 0.75f : 1.1f;
        // vira o personagem para onde a câmera mira
        float yaw = std::atan2(dir.x, dir.z);
        glm::quat q = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
        btTransform t = self->getWorldTransform();
        t.setRotation({q.x, q.y, q.z, q.w});
        self->setWorldTransform(t);
        root.transform.setRotation(q);
    }
    e->fireCooldown = cooldown;
    c.aimTimer = melee ? 0.45f : 1.5f;
    if (melee && c.anim) {  // golpe recomeça a cada ataque
        int base = armedClipBase(kind);
        if (base >= 0 && c.clipMap[base + AP_Aim] >= 0) c.anim->setClip(c.clipMap[base + AP_Aim]);
    }
    glm::vec3 hit; GameObject* obj = nullptr; float frac = 1.f;
    bool any = e->physics.raycastIgnoring(from, from + dir * range, self, hit, obj, frac);
    glm::vec3 end = any ? hit : from + dir * range;
    if (melee) playSound(e, c.shotSound.empty() ? "golpe_faca" : c.shotSound.c_str(), from, 0.8f);
    if (!melee) {
        glm::vec3 muzzle = end;
        if (GameObject* w = e->scene.get(c.weapon)) muzzle = glm::vec3(w->transform.world() * glm::vec4(c.muzzle, 1.f));
        glm::vec3 shotDir = glm::normalize(end - muzzle);
        addFlash(e, fxMuzzleFlash(e->particles, e->fxRng, muzzle, shotDir), 0.06f);
        playSound(e, c.shotSound.empty() ? "tiro_m4" : c.shotSound.c_str(), muzzle, 1.f);
        if (any) playSound(e, "impacto", end, 0.5f);
        int pellets = kind == WK_Shotgun ? 6 : 1;
        for (int k = 0; k < pellets; ++k) {
            glm::vec3 jitter = k ? glm::vec3(e->fxRng.range(-1.f, 1.f), e->fxRng.range(-1.f, 1.f), e->fxRng.range(-1.f, 1.f)) * 0.04f * glm::length(end - muzzle) : glm::vec3(0.f);
            fxBullet(e->particles, muzzle, end + jitter);
        }
        if (any) fxImpact(e->particles, e->fxRng, hit, dir);
    } else if (any) {
        fxImpact(e->particles, e->fxRng, hit, dir);
    }
    if (any && obj) {  // dano em inimigos / objetos com vida
        static const float dmg[] = {25.f, 20.f, 60.f, 100.f, 35.f, 30.f};
        damageAt(e, obj, dmg[std::clamp(kind, 0, 5)], from);
    }
    if (any && obj && obj->rigidBody && obj->rigidBody->mass > 0.f) {
        RigidBody* rb = obj->rigidBody;
        glm::vec3 imp = dir * power * rb->mass * 0.4f;
        rb->body->activate(true);
        btVector3 rel = btVector3(hit.x, hit.y, hit.z) - rb->body->getCenterOfMassPosition();
        rb->body->applyImpulse({imp.x, imp.y, imp.z}, rel);
    }
}

// Portas: abrem quando um personagem chega perto (no Play); no editor ficam fechadas
void updateDoors(Engine* e, float dt) {
    for (DoorRT& d : e->doors) {
        GameObject* o = e->scene.get(d.h);
        if (!o) continue;
        float target = 0.f;
        if (e->playing) {
            glm::vec3 c = glm::vec3(o->transform.world() * glm::vec4(0.5f, 0.5f, 0.f, 1.f));
            for (const CharacterInfo& ch : e->characters) {
                GameObject* g = e->scene.get(ch.root);
                if (!g || !g->active) continue;
                glm::vec3 p = g->transform.position;
                if (glm::length(glm::vec2(p.x - c.x, p.z - c.z)) < 1.6f + d.w * 0.5f && std::abs(p.y + ch.height * 0.5f - c.y) < 2.2f) { target = 1.65f; break; }
            }
        }
        float step = 3.5f * dt;
        float na = d.angle + std::clamp(target - d.angle, -step, step);
        if (e->playing ? std::abs(na - d.angle) > 1e-5f : d.angle != 0.f) {
            d.angle = e->playing ? na : 0.f;
            o->transform.setRotation(d.base * glm::angleAxis(d.angle, glm::vec3(0, 1, 0)));
        }
    }
}

// Elevador: perto de uma ponta aparece o botão que leva à outra
std::string interactLabel(Engine* e, int* which) {
    if (!e->playing || e->player < 0 || e->player >= int(e->characters.size())) return "";
    GameObject* pg = e->scene.get(e->characters[e->player].root);
    if (!pg) return "";
    glm::vec3 p = pg->transform.position;
    if (std::string shop = shopLabel(e); !shop.empty()) { if (which) *which = -2; return shop; }  // loja da partida de times
    for (size_t i = 0; i < e->teles.size(); ++i) {
        const TeleRT& t = e->teles[i];
        GameObject* r = e->scene.get(t.root);
        if (!r) continue;
        glm::mat4 W = r->transform.world();
        for (int end = 0; end < 2; ++end) {
            glm::vec3 q = glm::vec3(W * glm::vec4(end ? t.b : t.a, 1.f));
            if (glm::length(glm::vec2(p.x - q.x, p.z - q.z)) < 1.8f && std::abs(p.y - q.y) < 2.f) {
                if (which) *which = int(i) * 2 + end;
                return end ? t.lb : t.la;
            }
        }
    }
    return "";
}

bool interact(Engine* e) {
    if (e->match.active && shopInteract(e)) return true;
    int which = -1;
    if (interactLabel(e, &which).empty() || which < 0) return false;
    const TeleRT& t = e->teles[which / 2];
    GameObject* r = e->scene.get(t.root);
    CharacterInfo& c = e->characters[e->player];
    GameObject* pg = e->scene.get(c.root);
    if (!r || !pg || !pg->rigidBody) return false;
    glm::vec3 dst = glm::vec3(r->transform.world() * glm::vec4(which % 2 ? t.a : t.b, 1.f));
    pg->transform.setPosition(dst + glm::vec3(0, 0.05f, 0));
    e->physics.syncFromTransform(*pg->rigidBody);
    e->editorCam.target = dst + glm::vec3(0, c.height * 0.85f, 0);
    return true;
}

// Move um personagem (jogador ou inimigo): velocidade horizontal, pulo, virar para o movimento e animação
bool driveCharacter(Engine* e, CharacterInfo& c, glm::vec3 move, float speed, bool jump, float dt, glm::vec3 face) {
    GameObject* go = e->scene.get(c.root);
    if (!go || !go->rigidBody || !go->rigidBody->body) return false;
    btRigidBody* b = go->rigidBody->body;
    float mag = std::min(1.f, glm::length(move));
    if (mag > 1e-3f) move = glm::normalize(move);
    btVector3 vel = b->getLinearVelocity();
    glm::vec3 center = go->transform.position + glm::vec3(0, c.height * 0.5f, 0);
    glm::vec3 hit; GameObject* obj = nullptr; float frac;
    bool grounded = e->physics.raycastIgnoring(center, center - glm::vec3(0, c.height * 0.5f + 0.25f, 0), b, hit, obj, frac);
    btVector3 nv(move.x * speed * mag, vel.y(), move.z * speed * mag);
    if (jump && grounded) nv.setY(5.2f);
    // parado no chão (rampa, escada): sem gravidade, senão escorrega para baixo
    const bool anchor = grounded && !jump && mag < 0.05f && vel.y() < 0.5f && frac * (c.height * 0.5f + 0.25f) < c.height * 0.5f + 0.08f;
    b->setGravity(anchor ? btVector3(0, 0, 0) : btVector3(0, -9.81f, 0));
    if (anchor) nv.setY(0.f);
    b->setLinearVelocity(nv);
    b->activate(true);
    c.aimTimer = std::max(0.f, c.aimTimer - dt);
    glm::vec3 look = glm::length(glm::vec2(face.x, face.z)) > 1e-3f ? face : mag > 0.05f ? move : glm::vec3(0.f);
    if (glm::length(glm::vec2(look.x, look.z)) > 1e-3f) {  // mirando: olha para o alvo mesmo andando de lado
        glm::quat target = glm::angleAxis(std::atan2(look.x, look.z), glm::vec3(0, 1, 0));
        glm::quat q = glm::slerp(go->transform.rotation, target, 1.f - std::exp(-12.f * dt));
        btTransform t = b->getWorldTransform();
        t.setRotation({q.x, q.y, q.z, q.w});
        b->setWorldTransform(t);
        go->transform.setRotation(q);
    }
    c.fireAnim = std::max(0.f, c.fireAnim - dt);
    c.reloadTimer = std::max(0.f, c.reloadTimer - dt);
    if (c.anim && !c.dying) {
        float hs = std::sqrt(nv.x() * nv.x() + nv.z() * nv.z());
        int base = armedClipBase(c.weaponKind);
        bool aiming = c.aimTimer > 0.f || c.aimHold;
        int clip = hs < 0.4f ? CC_Idle : hs < 4.f ? CC_Walk : CC_Run;
        if (base >= 0)
            clip = base + (hs < 0.4f ? (aiming ? AP_Aim : AP_Idle) : (aiming ? AP_AimWalk : hs < 4.f ? AP_Walk : AP_Run));
        if (base == CC_Rifle) {
            // mirando com fuzil: anda de costas e de lado olhando para o alvo; parado, o tiro tem coice
            glm::vec3 local = glm::inverse(go->transform.rotation) * glm::vec3(nv.x(), 0.f, nv.z());
            if (aiming && hs >= 0.4f) {
                if (local.z < -0.5f * hs) clip = CC_RifleBack;
                else if (std::abs(local.x) > std::abs(local.z)) clip = local.x > 0.f ? CC_RifleLeft : CC_RifleRight;
            }
            if (aiming && hs < 0.4f && c.fireAnim > 0.f) clip = CC_RifleFire;
            if (c.reloadTimer > 0.f && hs < 4.f) clip = CC_Reload;
            if (c.crouch && c.reloadTimer <= 0.f) clip = CC_CrouchWalk;  // agachado: parado, andando e mirando
        }
        bool airborne = !grounded && std::abs(float(vel.y())) > 1.5f;
        if (airborne) clip = CC_Jump;
        int mapped = c.clipMap[clip];
        if (mapped < 0 && clip >= CC_Death) mapped = c.clipMap[base >= 0 ? base + AP_Aim : CC_Idle];  // modelo sem essa animação
        if ((grounded || airborne) && mapped >= 0 && c.anim->clipIndex != mapped) c.anim->setClip(mapped);
        c.anim->loop = true;
        c.anim->speed = clip == CC_CrouchWalk ? (hs < 0.3f ? 0.f : std::clamp(hs / 1.6f, 0.6f, 1.4f))
                      : clip == CC_Reload ? 1.4f : clip == CC_Jump || clip == CC_RifleFire ? 1.f
                      : hs < 0.4f ? 1.f : hs < 4.f ? std::clamp(hs / 2.6f, 0.6f, 1.6f) : std::clamp(hs / 6.5f, 0.7f, 1.4f);
    }
    return grounded;
}

void playDeath(CharacterInfo& c) {
    c.dying = true;
    if (!c.anim) return;
    int d = c.clipMap[CC_Death];
    if (d >= 0) { c.anim->setClip(d); c.anim->loop = false; c.anim->speed = 1.f; }
    else c.anim->speed = 0.f;
}

void updatePlayer(Engine* e, float dt) {
    for (auto& t : e->tracers) t.life -= dt;
    e->fireCooldown -= dt;
    if (e->player < 0 || e->player >= int(e->characters.size())) return;
    CharacterInfo& c = e->characters[e->player];
    GameObject* go = e->scene.get(c.root);
    if (!go || !go->rigidBody || !go->rigidBody->body) return;
    btRigidBody* b = go->rigidBody->body;
    bool alive = e->game.state == 0 && !c.dying;  // derrota/vitória (ou abatido na partida de times): o jogador para
    if (e->game.state == 2 && !c.dying) playDeath(c);

    float yaw = e->editorCam.yaw;
    glm::vec3 fwd(-std::sin(yaw), 0.f, -std::cos(yaw)), right(std::cos(yaw), 0.f, -std::sin(yaw));
    glm::vec3 move = alive ? right * e->moveInput.x + fwd * e->moveInput.y : glm::vec3(0.f);
    c.aimHold = alive && e->aimInput && c.weaponKind >= 0;
    const bool fp = e->firstPerson && alive;
    c.firstPerson = e->firstPerson;
    c.viewPitch = fp ? -e->editorCam.pitch : 0.f;
    glm::vec3 face = fp || c.aimHold || c.aimTimer > 0.f ? fwd : glm::vec3(0.f);
    c.crouch = alive && e->crouchInput && c.weaponKind >= 0 && c.weaponKind != WK_Pistol;
    float speed = c.crouch ? 1.6f : e->runInput && !c.aimHold ? 6.5f : 2.6f;  // mirando: anda devagar; agachado mais ainda
    if (fp && c.weaponKind == WK_Pistol) c.aimTimer = std::max(c.aimTimer, 0.1f);  // primeira pessoa: pistola sempre à frente
    driveCharacter(e, c, move, speed, alive && e->jumpRequest, dt, face);
    e->jumpRequest = false;
    if (fp) {  // primeira pessoa: o corpo vira junto com o olhar, sem atraso (a arma fica no centro da tela)
        glm::quat q = glm::angleAxis(std::atan2(fwd.x, fwd.z), glm::vec3(0, 1, 0));
        btTransform t = b->getWorldTransform();
        t.setRotation({q.x, q.y, q.z, q.w});
        b->setWorldTransform(t);
        go->transform.setRotation(q);
    }

    e->hitMarker = std::max(0.f, e->hitMarker - dt);
    // armas da partida: pente e reserva de cada arma; recarrega no botão ou quando o pente acaba
    auto startReload = [&]() {
        int n = c.reserve < 0 ? c.magSize - c.ammo : std::min(c.reserve, c.magSize - c.ammo);
        if (n <= 0) return;
        c.ammo += n;
        if (c.reserve > 0) c.reserve -= n;
        const AnimationClip* rc = c.anim && c.clipMap[CC_Reload] >= 0 ? c.anim->clipAt(c.clipMap[CC_Reload]) : nullptr;
        c.reloadTimer = c.weaponKind == WK_Pistol ? 1.3f : rc ? rc->duration / 1.4f : 2.f;
        playSound(e, "recarga", go->transform.position, 0.8f);
        if (c.anim && c.clipMap[CC_Reload] >= 0 && c.weaponKind != WK_Pistol) c.anim->setClip(c.clipMap[CC_Reload]);
    };
    if (c.gunDmg > 0.f) {
        if (alive && e->reloadRequest && c.reloadTimer <= 0.f) startReload();
        if (alive && e->fireRequest && e->fireCooldown <= 0.f && c.reloadTimer <= 0.f) {
            if (c.ammo > 0) {
                fire(e, c, *go, b);
                c.fireAnim = 0.35f;
                if (--c.ammo <= 0) startReload();
            } else startReload();
        }
    }
    e->reloadRequest = false;
    // fuzil, escopeta e sniper: pente de 30 tiros; vazio, recarrega sozinho (com a animação)
    bool longGun = c.weaponKind == WK_Rifle || c.weaponKind == WK_Shotgun || c.weaponKind == WK_Sniper;
    if (c.gunDmg <= 0.f && alive && e->fireRequest && e->fireCooldown <= 0.f && c.reloadTimer <= 0.f) {
        fire(e, c, *go, b);
        if (longGun) {
            c.fireAnim = 0.35f;
            if (--c.ammo <= 0) {
                c.ammo = 30;
                const AnimationClip* rc = c.anim && c.clipMap[CC_Reload] >= 0 ? c.anim->clipAt(c.clipMap[CC_Reload]) : nullptr;
                c.reloadTimer = rc ? rc->duration / 1.4f : 2.f;
                playSound(e, "recarga", go->transform.position, 0.8f);
                if (c.anim && c.clipMap[CC_Reload] >= 0) c.anim->setClip(c.clipMap[CC_Reload]);
                e->game.message = "Recarregando...";
            }
        }
    }
    e->fireRequest = false;

    // câmera em terceira pessoa segue o jogador (em primeira pessoa, só quando ele cai: vê o corpo de fora)
    if (e->firstPerson) e->editorCam.distance = 3.5f;
    glm::vec3 focus = go->transform.position + glm::vec3(0, c.height * 0.85f, 0);
    e->editorCam.target = glm::mix(e->editorCam.target, focus, 1.f - std::exp(-10.f * dt));
}

void deleteObject(Engine* e, Scene::Handle h) {
    GameObject* sel = e->scene.get(h);
    if (!sel) return;
    // personagem apagado leva junto o equipamento preso nele
    std::vector<Scene::Handle> gear;
    for (const CharacterInfo& c : e->characters)
        if (c.root.index == h.index && c.root.gen == h.gen) gear = c.gear;
    std::vector<const Transform3D*> gone;
    Scene::forEachInSubtree(*sel, [&](GameObject& g) { gone.push_back(&g.transform); });
    auto isGone = [&](const Transform3D* t) { return t && std::find(gone.begin(), gone.end(), t) != gone.end(); };
    // armas presas a personagens apagados (ou a própria arma apagada) saem da lista
    e->attachments.erase(std::remove_if(e->attachments.begin(), e->attachments.end(), [&](const Attachment& a) {
        GameObject* o = e->scene.get(a.obj);
        return !o || isGone(&o->transform) || (a.anim && isGone(a.anim->space));
    }), e->attachments.end());
    std::vector<Animator*> ownAnims;  // animators dos nós apagados saem da lista e voltam ao pool
    Scene::forEachInSubtree(*sel, [&](GameObject& g) { if (g.animator) ownAnims.push_back(g.animator); });
    e->animators.erase(std::remove_if(e->animators.begin(), e->animators.end(), [&](Animator* a) {
        return std::find(ownAnims.begin(), ownAnims.end(), a) != ownAnims.end();
    }), e->animators.end());
    for (Animator* a : e->animators) if (isGone(a->space)) a->space = nullptr;
    for (size_t i = e->characters.size(); i-- > 0;) {
        CharacterInfo& c = e->characters[i];
        GameObject* w = e->scene.get(c.weapon);
        if (w && isGone(&w->transform)) { c.weapon = {}; c.weaponKind = -1; }
        GameObject* r = e->scene.get(c.root);
        if (!r || isGone(&r->transform)) {
            if (e->player == int(i)) e->player = -1;
            else if (e->player > int(i)) --e->player;
            e->characters.erase(e->characters.begin() + i);
        }
    }
    e->doors.erase(std::remove_if(e->doors.begin(), e->doors.end(), [&](const DoorRT& d) {
        GameObject* o = e->scene.get(d.h);
        return !o || isGone(&o->transform);
    }), e->doors.end());
    e->teles.erase(std::remove_if(e->teles.begin(), e->teles.end(), [&](const TeleRT& t) {
        GameObject* o = e->scene.get(t.root);
        return !o || isGone(&o->transform);
    }), e->teles.end());
    e->lightObjects.erase(std::remove_if(e->lightObjects.begin(), e->lightObjects.end(), [&](const LightObject& l) {
        GameObject* o = e->scene.get(l.obj);
        return !o || isGone(&o->transform);
    }), e->lightObjects.end());
    if (e->scene.get(e->selected) && isGone(&e->scene.get(e->selected)->transform)) selectHandle(e, {});
    Scene::forEachInSubtree(*sel, [&](GameObject& g) { releaseComponents(e, g); });
    forgetClipStores(e, ownAnims);
    for (Animator* a : ownAnims) e->animatorPool.release(a);
    e->scene.destroy(h);
    ++e->revision;
    for (Scene::Handle g : gear) deleteObject(e, g);
}

// ---------------- efeitos ----------------
int createEffect(Engine* e, int kind, int element) {
    kind = std::clamp(kind, 0, FX_Count - 1);
    char name[32];
    std::snprintf(name, sizeof(name), "%s (%s)", fxName(kind), elementName(std::clamp(element, 0, EL_Count - 1)));
    Scene::Handle h = e->scene.create(name);
    GameObject* go = e->scene.get(h);
    auto eh = e->emitters.acquire();
    Emitter* em = e->emitters.get(eh);
    if (!go || !em) { e->lastError = "limite de efeitos"; return -1; }
    em->kind = kind;
    em->element = std::clamp(element, 0, EL_Count - 1);
    em->owner = go;
    go->emitter = em;
    glm::vec3 pos = e->editorCam.target;
    pos.y = e->mapKind >= 0 ? mapHeight(e, pos.x, pos.z) : 0.f;
    go->transform.setPosition(pos);
    return registerObject(e, h);
}

void addFlash(Engine* e, const FxLight& l, float life) {
    if (l.intensity <= 0.f) return;
    FxFlash* slot = &e->flashes[0];
    for (auto& f : e->flashes) {
        if (f.life <= 0.f) { slot = &f; break; }
        if (f.life < slot->life) slot = &f;
    }
    *slot = {l, life, life};
}

void spawnEffect(Engine* e, int kind, int element, glm::vec3 pos) {
    FxPalette pal = fxPalette(element);
    if (kind == FX_Explosion) { addFlash(e, fxExplosion(e->particles, e->fxRng, pal, pos, 1.f), 0.35f); return; }
    float acc = 0.f, timer = 0.f;
    for (int i = 0; i < 30; ++i) fxContinuous(e->particles, e->fxRng, kind, pal, pos, 1.f, 1.f / 30.f, acc, timer);
}

static void radialImpulse(Engine* e, glm::vec3 c, float radius, float power, const btCollisionObject* ignore) {
    e->bodies.forEach([&](RigidBody& rb, RigidBodyPool::Handle) {
        if (!rb.body || rb.mass <= 0.f || rb.body == ignore) return;
        btVector3 p = rb.body->getCenterOfMassPosition();
        glm::vec3 d = glm::vec3(p.x(), p.y(), p.z()) - c;
        float dist = glm::length(d);
        if (dist > radius) return;
        glm::vec3 dir = dist > 1e-3f ? d / dist : glm::vec3(0, 1, 0);
        dir = glm::normalize(dir + glm::vec3(0, 0.5f, 0));
        float k = power * (1.f - dist / radius) * rb.mass;
        rb.body->activate(true);
        rb.body->applyCentralImpulse({dir.x * k, dir.y * k, dir.z * k});
    });
}

static void lightningBolt(Engine* e, glm::vec3 a, glm::vec3 b, glm::vec3 color) {
    Bolt* slot = &e->bolts[0];
    for (auto& bl : e->bolts) if (bl.life <= 0.f) { slot = &bl; break; }
    Bolt& bl = *slot;
    bl.n = 14;
    glm::vec3 d = b - a;
    float len = glm::length(d);
    glm::vec3 side = glm::normalize(glm::cross(d, glm::vec3(0, 1, 0)) + glm::vec3(1e-4f));
    glm::vec3 up = glm::normalize(glm::cross(side, d));
    for (int i = 0; i < bl.n; ++i) {
        float t = float(i) / (bl.n - 1);
        float jit = (i == 0 || i == bl.n - 1) ? 0.f : len * 0.05f;
        bl.pts[i] = a + d * t + side * e->fxRng.range(-jit, jit) + up * e->fxRng.range(-jit, jit);
    }
    bl.color = color;
    bl.life = 0.18f;
}

void castSpell(Engine* e, int element) {
    if (e->castCooldown > 0.f || e->player < 0 || e->player >= int(e->characters.size())) return;
    CharacterInfo& c = e->characters[e->player];
    GameObject* go = e->scene.get(c.root);
    if (!go || !go->rigidBody || !go->rigidBody->body) return;
    element = std::clamp(element, 0, EL_Count - 1);
    FxPalette pal = fxPalette(element);
    glm::vec3 dir = aimDirection(e);
    // vira o personagem para a mira e lança da altura do peito, à frente do corpo
    float yaw = std::atan2(dir.x, dir.z);
    glm::quat q = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
    btTransform t = go->rigidBody->body->getWorldTransform();
    t.setRotation({q.x, q.y, q.z, q.w});
    go->rigidBody->body->setWorldTransform(t);
    go->transform.setRotation(q);
    glm::vec3 from = go->transform.position + glm::vec3(0, c.height * 0.72f, 0) + glm::normalize(glm::vec3(dir.x, 0, dir.z)) * 0.6f;
    e->castCooldown = 0.45f;
    if (element == EL_Lightning) {  // raio: instantâneo
        glm::vec3 hit; GameObject* obj = nullptr; float frac;
        glm::vec3 aimFrom = e->camera.position;
        bool any = e->physics.raycastIgnoring(aimFrom, aimFrom + dir * 80.f, go->rigidBody->body, hit, obj, frac);
        glm::vec3 end = any ? hit : aimFrom + dir * 80.f;
        lightningBolt(e, from, end, pal.glow);
        addFlash(e, fxExplosion(e->particles, e->fxRng, pal, end, 0.5f), 0.2f);
        addFlash(e, {from, pal.light, 15.f, 10.f}, 0.18f);
        radialImpulse(e, end, 3.f, 7.f, go->rigidBody->body);
        if (any && obj) damageAt(e, obj, 60.f, from);
        return;
    }
    for (auto& p : e->projectiles) {
        if (p.active) continue;
        p.active = true;
        p.pos = from;
        p.vel = dir * 24.f;
        p.pal = pal;
        p.element = element;
        p.life = 3.f;
        p.ignore = go->rigidBody->body;
        break;
    }
}

void updateEffects(Engine* e, float dt) {
    e->castCooldown -= dt;
    // emissores (rodam também no editor, para ver o efeito enquanto monta a cena)
    e->emitters.forEach([&](Emitter& em, EmitterPool::Handle) {
        GameObject* o = em.owner;
        if (!o || !o->active) return;
        glm::vec3 pos = o->transform.worldPosition();
        float s = std::max(0.1f, o->transform.scale.x);
        FxLight l = fxContinuous(e->particles, e->fxRng, em.kind, em.palette(), pos, s, dt, em.accum, em.timer);
        if (em.kind == FX_Explosion && l.intensity > 0.f) addFlash(e, l, 0.35f);
    });
    // projéteis de magia
    for (auto& p : e->projectiles) {
        if (!p.active) continue;
        if (!e->playing) { p.active = false; continue; }
        glm::vec3 next = p.pos + p.vel * dt;
        glm::vec3 hit; GameObject* obj = nullptr; float frac;
        bool any = e->physics.raycastIgnoring(p.pos, next, p.ignore, hit, obj, frac);
        p.life -= dt;
        if (any || p.life <= 0.f) {
            glm::vec3 at = any ? hit : next;
            addFlash(e, fxExplosion(e->particles, e->fxRng, p.pal, at, any ? 0.8f : 0.4f), 0.4f);
            if (any) {
                radialImpulse(e, at, 4.5f, 9.f, p.ignore);
                damageRadius(e, at, 4.5f, 45.f);
            }
            p.active = false;
            continue;
        }
        p.pos = next;
        fxSpellTrail(e->particles, e->fxRng, p.pal, p.pos, p.vel, dt);
    }
    for (auto& f : e->flashes) f.life -= dt;
    for (auto& b : e->bolts) b.life -= dt;
    e->particles.update(dt);
}

int gatherLights(Engine* e, PointLight* out, int max) {
    int n = std::min(e->pointCount, max);
    for (int i = 0; i < n; ++i) out[i] = e->points[i];
    struct Cand { FxLight l; float score; };
    Cand c[64];
    int nc = 0;
    glm::vec3 cam = e->camera.position;
    auto push = [&](const FxLight& l) {
        if (l.intensity <= 0.f || nc >= 64) return;
        glm::vec3 d = l.pos - cam;
        c[nc++] = {l, glm::dot(d, d) / (l.intensity + 1e-3f)};
    };
    e->emitters.forEach([&](Emitter& em, EmitterPool::Handle) {
        GameObject* o = em.owner;
        if (!o || !o->active || em.kind == FX_Smoke || em.kind == FX_Explosion) return;
        float s = std::max(0.1f, o->transform.scale.x);
        float flick = 0.85f + 0.3f * std::sin(em.timer * 23.f) * std::sin(em.timer * 7.3f);
        float inten = (em.kind == FX_Torch ? 2.5f : em.kind == FX_Sparks ? 3.f : 6.f) * s * flick;
        push({o->transform.worldPosition() + glm::vec3(0, 0.8f * s, 0), em.palette().light, inten, 7.f * s});
    });
    for (auto& p : e->projectiles) if (p.active) push({p.pos, p.pal.light, 8.f, 8.f});
    for (const LightObject& lo : e->lightObjects)
        if (GameObject* o = e->scene.get(lo.obj); o && o->active) push({o->transform.worldPosition(), lo.color, lo.intensity, lo.radius});
    for (auto& f : e->flashes)
        if (f.life > 0.f) { FxLight l = f.light; l.intensity *= f.life / f.maxLife; push(l); }
    for (auto& b : e->bolts) if (b.life > 0.f) push({b.pts[b.n / 2], b.color, 20.f, 14.f});
    // postes acesos à noite: só os mais perto da câmera (o shader tem poucas luzes)
    if (e->night > 0.05f && !e->mapLamps.empty()) {
        std::pair<float, int> near[24];
        int nn = 0;
        for (int i = 0; i < int(e->mapLamps.size()); ++i) {
            glm::vec3 d = e->mapLamps[i] - cam;
            float d2 = glm::dot(d, d);
            if (d2 > 90.f * 90.f) continue;
            if (nn < 24) near[nn++] = {d2, i};
            else {
                int worst = 0;
                for (int k = 1; k < 24; ++k) if (near[k].first > near[worst].first) worst = k;
                if (d2 < near[worst].first) near[worst] = {d2, i};
            }
        }
        for (int k = 0; k < nn; ++k) push({e->mapLamps[near[k].second], glm::vec3(1.f, 0.78f, 0.5f), 110.f * e->night, 20.f});
    }
    std::sort(c, c + nc, [](const Cand& a, const Cand& b) { return a.score < b.score; });
    for (int i = 0; i < nc && n < max; ++i) out[n++] = {c[i].l.pos, c[i].l.radius, c[i].l.color, c[i].l.intensity};
    return n;
}

void drawEffectsOverlay(Engine* e, const glm::mat4& vp, bool editor) {
    for (auto& b : e->bolts) {
        if (b.life <= 0.f) continue;
        float a = std::min(1.f, b.life * 10.f);
        for (int i = 0; i + 1 < b.n; ++i) {
            e->renderer.line(vp, b.pts[i], b.pts[i + 1], glm::vec4(b.color, 0.35f * a), 14.f, true);
            e->renderer.line(vp, b.pts[i], b.pts[i + 1], glm::vec4(glm::mix(b.color, glm::vec3(1.f), 0.7f), a), 3.5f, true);
        }
    }
    if (!editor) return;
    // marcador dos emissores (para achar e selecionar no editor)
    e->emitters.forEach([&](Emitter& em, EmitterPool::Handle) {
        if (!em.owner) return;
        glm::vec3 p = em.owner->transform.worldPosition();
        glm::vec4 col(em.palette().glow, 0.9f);
        float s = 0.35f;
        e->renderer.line(vp, p + glm::vec3(-s, 0, 0), p + glm::vec3(s, 0, 0), col, 2.f, false);
        e->renderer.line(vp, p + glm::vec3(0, 0, -s), p + glm::vec3(0, 0, s), col, 2.f, false);
        e->renderer.line(vp, p, p + glm::vec3(0, 2.f * s, 0), col, 2.f, false);
    });
}

bool pickEmitter(Engine* e, glm::vec3 o, glm::vec3 d, float& best, GameObject*& hit) {
    bool any = false;
    e->emitters.forEach([&](Emitter& em, EmitterPool::Handle) {
        if (!em.owner || em.owner->id < 0) return;
        glm::vec3 c = em.owner->transform.worldPosition() + glm::vec3(0, 0.5f, 0);
        float r = 0.8f * std::max(0.3f, em.owner->transform.scale.x);
        glm::vec3 oc = o - c;
        float b = glm::dot(oc, d), cc = glm::dot(oc, oc) - r * r, disc = b * b - cc;
        if (disc < 0.f) return;
        float t = -b - std::sqrt(disc);
        if (t > 0.f && t < best) { best = t; hit = em.owner; any = true; }
    });
    return any;
}

// ---------------- modelo importado -> personagem ----------------
// procura nas animações do próprio modelo (com retarget, elas ficam depois das do motor)
static int findClip(const Animator* a, std::initializer_list<const char*> keys) {
    int from = a->retarget ? a->refCount : 0;
    for (int i = from; i < a->clipCount; ++i) {
        const AnimationClip* c = a->clipAt(i);
        if (!c) continue;
        std::string n = c->name;
        for (auto& ch : n) ch = char(std::tolower((unsigned char)ch));
        if (n.find("arma") != std::string::npos) continue;  // variantes armadas do motor
        for (const char* k : keys) if (n.find(k) != std::string::npos) return i;
    }
    return -1;
}

static int finishModelCharacter(Engine* e, GameObject& root, CharacterInfo& info, const AABB& b, float height, float width, bool controllable);

int makeCharacterFromModel(Engine* e, bool controllable) {
    GameObject* root = e->scene.get(e->selected);
    while (root && root->transform.parent && root->transform.parent->owner && root->transform.parent->owner->id >= 0)
        root = root->transform.parent->owner;
    if (!root) { e->lastError = "toque primeiro no modelo importado (ou use + Criar → Personagem → 📂 Personagem de um arquivo 3D)"; return -1; }
    for (size_t i = 0; i < e->characters.size(); ++i)
        if (e->scene.get(e->characters[i].root) == root) { if (controllable) setPlayer(e, int(i)); return root->id; }
    Animator* anim = animatorOf(*root);
    if (root->rigidBody) { e->lastError = "o modelo já tem física"; return -1; }

    bool humanoid = anim && setupHumanoid(e, *root, anim);  // liga as animações do motor e vira para +Z
    e->scene.updateTransforms();
    AABB b;
    if (!subtreeBounds(e, *root, b)) { e->lastError = "modelo sem malha"; return -1; }
    float height = std::max(0.3f, b.max.y - b.min.y);
    float width = std::max(b.max.x - b.min.x, b.max.z - b.min.z);

    CharacterInfo info;
    info.root = Scene::handleOf(*root);
    info.anim = anim;
    info.height = height;
    for (int k = 0; k < CC_Count; ++k) info.clipMap[k] = -1;
    if (!anim) {  // modelo sem esqueleto (ex.: .obj, estátua, robô rígido): anda e pula, mas sem animação
        info.handJoint = 0;
        e->lastError = "modelo sem esqueleto: o personagem anda, mas sem animação";
        return finishModelCharacter(e, *root, info, b, height, width, controllable);
    }

    int ownIdle = findClip(anim, {"idle", "survey", "stand", "breath", "parado"});
    int ownWalk = findClip(anim, {"walk", "andando", "andar"});
    int ownRun = findClip(anim, {"run", "sprint", "jog", "gallop", "correndo", "correr"});
    if (ownWalk >= 0 || !humanoid) {
        // usa as animações do próprio modelo, casadas pelo nome
        int first = anim->retarget ? anim->refCount : 0;
        int fallback = ownIdle >= 0 ? ownIdle : (anim->clipCount > first ? first : -1);
        info.clipMap[CC_Idle] = fallback;
        info.clipMap[CC_Walk] = ownWalk >= 0 ? ownWalk : fallback;
        info.clipMap[CC_Run] = ownRun >= 0 ? ownRun : info.clipMap[CC_Walk];
        for (int b : {int(CC_Rifle), int(CC_Pistol), int(CC_Melee)}) {  // com arma: as mesmas do modelo
            info.clipMap[b + AP_Idle] = info.clipMap[b + AP_Aim] = info.clipMap[CC_Idle];
            info.clipMap[b + AP_Walk] = info.clipMap[b + AP_AimWalk] = info.clipMap[CC_Walk];
            info.clipMap[b + AP_Run] = info.clipMap[CC_Run];
        }
        info.clipMap[CC_Dance] = findClip(anim, {"dance", "samba", "dan"});
        info.clipMap[CC_Wave] = findClip(anim, {"wave", "greet", "acen"});
        if (fallback >= 0) anim->setClip(fallback);
    } else {
        // humanoide sem andar/correr: as animações do motor (retarget) já estão no começo da lista
        for (int k = 0; k < CC_Count; ++k) info.clipMap[k] = k;
        anim->setClip(CC_Idle);
    }
    glm::mat4 spaceW = anim->space ? anim->space->world() : glm::mat4(1.f);
    if (const Retarget* rt = anim->retarget) {
        int hand = rt->targetOf[CJ_RHand];
        info.handJoint = hand;
        anim->update(0.f, false);
        float ws = glm::length(glm::vec3((spaceW * anim->global[hand])[0]));
        // Com o retarget, a rotação da mão no mundo = anim * alinhamento * bind; desfazendo alinhamento * bind
        // sobra o mesmo referencial da mão do personagem do motor (onde a arma foi desenhada)
        glm::quat rotFix = glm::inverse(rt->align[hand] * rt->bindRot[hand]);
        info.handFix = glm::mat4_cast(rotFix) * glm::scale(glm::mat4(1.f), glm::vec3(1.f / std::max(ws, 1e-6f)));
    } else {
        // esqueleto do próprio motor (personagem exportado em .glb): a mão já tem o referencial certo
        info.handJoint = 0;
        for (int j = 0; j < anim->skeleton->jointCount; ++j) if (humanoidBone(anim->skeleton->name[j]) == CJ_RHand) info.handJoint = j;
        if (!info.handJoint) e->lastError = "ossos humanoides não encontrados: usando as animações do modelo";
    }

    return finishModelCharacter(e, *root, info, b, height, width, controllable);
}

// cápsula de física com a origem do personagem nos pés
static int finishModelCharacter(Engine* e, GameObject& rootRef, CharacterInfo& info, const AABB& b, float height, float width, bool controllable) {
    GameObject* root = &rootRef;
    float diam = std::clamp(width * 0.45f, 0.25f, height * 0.6f);
    btCollisionShape* shape = Physics::makeShape(ColliderType::Capsule, {diam, height, diam});
    glm::vec3 offset(0.f, (b.min.y - root->transform.position.y) + height * 0.5f, 0.f);
    if (RigidBody* rb = attachShape(e, *root, ColliderType::Capsule, shape, 60.f, offset)) {
        rb->lockRotX = rb->lockRotY = rb->lockRotZ = true;
        rb->applyAxisConstraints();
        rb->body->setFriction(0.4f);
        rb->body->setActivationState(DISABLE_DEACTIVATION);
    }
    e->characters.push_back(info);
    if (controllable || e->player < 0) setPlayer(e, int(e->characters.size()) - 1);
    return root->id;
}

}  // namespace eng

namespace eng {
// Aberturas: caixa de cada uma (no mundo) para o shader e para a física
void updateCuts(Engine* e) {
    e->renderer.cutCount = 0;
    e->physics.cuts.clear();
    for (size_t i = 0; i < e->openings.size();) {
        const Opening& o = e->openings[i];
        GameObject* g = e->scene.get(o.root);
        if (!g) { e->openings.erase(e->openings.begin() + long(i)); continue; }  // apagada: a parede fecha de novo
        ++i;
        if (!g->active || e->renderer.cutCount >= Renderer::kMaxCuts) continue;
        glm::mat4 box = g->transform.world() * glm::translate(glm::mat4(1.f), o.center) * glm::scale(glm::mat4(1.f), o.size);
        Renderer::CutBox& c = e->renderer.cuts[e->renderer.cutCount++];
        c.worldToBox = glm::inverse(box);
        c.bounds = {glm::vec3(1e30f), glm::vec3(-1e30f)};
        for (int k = 0; k < 8; ++k) {
            glm::vec3 p = glm::vec3(box * glm::vec4(k & 1 ? 0.5f : -0.5f, k & 2 ? 0.5f : -0.5f, k & 4 ? 0.5f : -0.5f, 1.f));
            c.bounds.min = glm::min(c.bounds.min, p);
            c.bounds.max = glm::max(c.bounds.max, p);
        }
        e->physics.cuts.push_back(c.worldToBox);
    }
}
}  // namespace eng

namespace eng {
void updateLamps(Engine* e) {
    // luz quente de vapor de sódio; de dia o vidro fica apagado (um pouco claro)
    e->renderer.setEmissive(e->mats[int(Mat::Lamp)], glm::vec3(1.f, 0.8f, 0.55f) * (0.05f + 7.f * e->night));
}
}  // namespace eng
