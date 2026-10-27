#include "engine/Engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "assets/MeshUtils.h"
#include "assets/TextureManager.h"
#include "scene/Humanoid.h"

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

    // Biblioteca procedural (texturas geradas uma vez e reenviadas à GPU a cada cena)
    for (int k = 0; k < int(Mat::Count); ++k) {
        const TexData& t = proceduralTexture(Mat(k));
        MatParams mp = matParams(Mat(k));
        Material m;
        m.albedo = tx.fromPixels(t.albedo.data(), t.w, t.h, 4, true);
        m.orm = tx.fromPixels(t.orm.data(), t.w, t.h, 4, false);
        m.normal = t.normal.empty() ? tx.flatNormal() : tx.fromPixels(t.normal.data(), t.w, t.h, 4, false);
        m.emissive = tx.white();
        m.roughness = mp.roughness; m.metallic = mp.metallic; m.mode = mp.mode; m.doubleSided = mp.doubleSided;
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

Scene::Handle spawnGeo(Engine* e, Geo& g, const char* name, bool local, glm::vec3 pos, float yaw, float detailFar) {
    Scene::Handle rootH = e->scene.create(name);
    GameObject* root = e->scene.get(rootH);
    if (!root) return rootH;
    root->transform.setPosition(pos);
    root->transform.setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
    if (local) registerObject(e, rootH);  // raiz antes dos filhos na hierarquia
    for (int layer = 0; layer < 2; ++layer)
    for (int k = 0; k < int(Mat::Count); ++k) {
        MeshBuilder& mb = layer ? g.detailMats[k] : g.mats[k];
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
            mr->castShadows = !(m == Mat::Asphalt || m == Mat::Road || m == Mat::Sidewalk || m == Mat::Grass || m == Mat::Court || m == Mat::Terrain);
            if (layer) mr->lodFar = detailFar;  // LOD: detalhe só perto da câmera
        }
        if (local) registerObject(e, ch);
    }
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
    e->recipes.clear();
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

int createWeapon(Engine* e, int kind, int ch) {
    kind = std::clamp(kind, 0, WK_Count - 1);
    Geo g;
    g.place(glm::vec3(0.f), 0.f);
    glm::vec3 muzzle = buildWeapon(g, kind);
    glm::vec3 pos = e->editorCam.target + glm::vec3(0, 0.6f, 0);
    Scene::Handle h = spawnGeo(e, g, weaponName(kind), true, pos, 0.f);
    GameObject* w = e->scene.get(h);
    if (!w) return -1;
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

// ---------------- jogador ----------------
// Mira de terceira pessoa: um pouco acima do centro da câmera (a câmera fica inclinada para baixo)
static glm::vec3 aimDirection(Engine* e) {
    glm::vec3 f = glm::normalize(e->camera.rotation * glm::vec3(0, 0, -1));
    return glm::normalize(f + glm::vec3(0.f, 0.28f, 0.f));
}

static void fire(Engine* e, CharacterInfo& c, GameObject& root, btRigidBody* self) {
    int kind = c.weaponKind;
    bool melee = !isGun(kind);
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
    if (!melee) {
        glm::vec3 muzzle = end;
        if (GameObject* w = e->scene.get(c.weapon)) muzzle = glm::vec3(w->transform.world() * glm::vec4(c.muzzle, 1.f));
        glm::vec3 shotDir = glm::normalize(end - muzzle);
        addFlash(e, fxMuzzleFlash(e->particles, e->fxRng, muzzle, shotDir), 0.06f);
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
    if (c.anim) {
        float hs = std::sqrt(nv.x() * nv.x() + nv.z() * nv.z());
        int base = armedClipBase(c.weaponKind);
        bool aiming = c.aimTimer > 0.f || c.aimHold;
        int clip = hs < 0.4f ? CC_Idle : hs < 4.f ? CC_Walk : CC_Run;
        if (base >= 0)
            clip = base + (hs < 0.4f ? (aiming ? AP_Aim : AP_Idle) : (aiming ? AP_AimWalk : hs < 4.f ? AP_Walk : AP_Run));
        clip = c.clipMap[clip];
        if (grounded && clip >= 0 && c.anim->clipIndex != clip) c.anim->setClip(clip);
        c.anim->speed = hs < 0.4f ? 1.f : hs < 4.f ? std::clamp(hs / 2.6f, 0.6f, 1.6f) : std::clamp(hs / 6.5f, 0.7f, 1.4f);
    }
    return grounded;
}

void updatePlayer(Engine* e, float dt) {
    for (auto& t : e->tracers) t.life -= dt;
    e->fireCooldown -= dt;
    if (e->player < 0 || e->player >= int(e->characters.size())) return;
    CharacterInfo& c = e->characters[e->player];
    GameObject* go = e->scene.get(c.root);
    if (!go || !go->rigidBody || !go->rigidBody->body) return;
    btRigidBody* b = go->rigidBody->body;
    bool alive = e->game.state == 0;  // derrota/vitória: o jogador para

    float yaw = e->editorCam.yaw;
    glm::vec3 fwd(-std::sin(yaw), 0.f, -std::cos(yaw)), right(std::cos(yaw), 0.f, -std::sin(yaw));
    glm::vec3 move = alive ? right * e->moveInput.x + fwd * e->moveInput.y : glm::vec3(0.f);
    c.aimHold = alive && e->aimInput && c.weaponKind >= 0;
    glm::vec3 face = c.aimHold || c.aimTimer > 0.f ? fwd : glm::vec3(0.f);
    float speed = e->runInput && !c.aimHold ? 6.5f : 2.6f;  // mirando: anda devagar
    driveCharacter(e, c, move, speed, alive && e->jumpRequest, dt, face);
    e->jumpRequest = false;

    if (alive && e->fireRequest && e->fireCooldown <= 0.f) fire(e, c, *go, b);
    e->fireRequest = false;

    // câmera em terceira pessoa segue o jogador
    glm::vec3 focus = go->transform.position + glm::vec3(0, c.height * 0.85f, 0);
    e->editorCam.target = glm::mix(e->editorCam.target, focus, 1.f - std::exp(-10.f * dt));
}

void deleteObject(Engine* e, Scene::Handle h) {
    GameObject* sel = e->scene.get(h);
    if (!sel) return;
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
