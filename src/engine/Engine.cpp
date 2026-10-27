#include "engine/Engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "assets/MeshUtils.h"
#include "assets/TextureManager.h"

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
        t.setRotation(btQuaternion(btVector3(0, 1, 0), b.yaw));
        comp->addChildShape(t, box);
        s.children.push_back(box);
    }
    s.shape = comp;
    s.body = e->physics.addStatic(comp, glm::vec3(0.f));
    if (into) *into = s;
    else e->statics.push_back(s);
}

Scene::Handle spawnGeo(Engine* e, Geo& g, const char* name, bool local, glm::vec3 pos, float yaw) {
    Scene::Handle rootH = e->scene.create(name);
    GameObject* root = e->scene.get(rootH);
    if (!root) return rootH;
    root->transform.setPosition(pos);
    root->transform.setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
    if (local) registerObject(e, rootH);  // raiz antes dos filhos na hierarquia
    for (int k = 0; k < int(Mat::Count); ++k) {
        if (g.mats[k].empty()) continue;
        MeshId id = g.mats[k].build(e->renderer);
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
                t.setRotation(btQuaternion(btVector3(0, 1, 0), b.yaw));
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
    e->animatorPool.clear();
    e->bodies.clear();
    e->objects.clear();
    e->animators.clear();
    e->characters.clear();
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
        // empunhadura na palma: desce do pulso e gira para o cano seguir o antebraço
        glm::mat4 off = glm::translate(glm::mat4(1.f), glm::vec3(0.f, -0.05f * c.height, 0.012f * c.height)) *
                        glm::rotate(glm::mat4(1.f), glm::radians(90.f), glm::vec3(1, 0, 0));
        if (kind == WK_Knife || kind == WK_Bat)
            off = glm::translate(glm::mat4(1.f), glm::vec3(0.f, -0.05f * c.height, 0.f)) *
                  glm::rotate(glm::mat4(1.f), glm::radians(90.f), glm::vec3(1, 0, 0));
        e->attachments.push_back({h, c.anim, CJ_RHand, off});
        c.weapon = h;
        c.weaponKind = kind;
        c.muzzle = muzzle;
        if (c.anim) {
            int base = c.anim->clipIndex % 3;
            if (c.anim->clipIndex < CC_IdleArmed && isGun(kind)) c.anim->setClip(CC_IdleArmed + base);
        }
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
static void fire(Engine* e, CharacterInfo& c, GameObject& root, btRigidBody* self) {
    int kind = c.weaponKind;
    bool melee = !isGun(kind);
    glm::vec3 camFwd = glm::normalize(e->camera.rotation * glm::vec3(0, 0, -1));
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
    glm::vec3 hit; GameObject* obj = nullptr; float frac = 1.f;
    bool any = e->physics.raycastIgnoring(from, from + dir * range, self, hit, obj, frac);
    glm::vec3 end = any ? hit : from + dir * range;
    if (!melee) {
        glm::vec3 muzzle = end;
        if (GameObject* w = e->scene.get(c.weapon)) muzzle = glm::vec3(w->transform.world() * glm::vec4(c.muzzle, 1.f));
        for (auto& t : e->tracers)
            if (t.life <= 0.f) { t = {muzzle, end, 0.08f}; break; }
    }
    if (any && obj && obj->rigidBody && obj->rigidBody->mass > 0.f) {
        RigidBody* rb = obj->rigidBody;
        glm::vec3 imp = dir * power * rb->mass * 0.4f;
        rb->body->activate(true);
        btVector3 rel = btVector3(hit.x, hit.y, hit.z) - rb->body->getCenterOfMassPosition();
        rb->body->applyImpulse({imp.x, imp.y, imp.z}, rel);
    }
}

void updatePlayer(Engine* e, float dt) {
    for (auto& t : e->tracers) t.life -= dt;
    e->fireCooldown -= dt;
    if (e->player < 0 || e->player >= int(e->characters.size())) return;
    CharacterInfo& c = e->characters[e->player];
    GameObject* go = e->scene.get(c.root);
    if (!go || !go->rigidBody || !go->rigidBody->body) return;
    btRigidBody* b = go->rigidBody->body;

    float yaw = e->editorCam.yaw;
    glm::vec3 fwd(-std::sin(yaw), 0.f, -std::cos(yaw)), right(std::cos(yaw), 0.f, -std::sin(yaw));
    glm::vec3 move = right * e->moveInput.x + fwd * e->moveInput.y;
    float mag = std::min(1.f, glm::length(move));
    if (mag > 1e-3f) move = glm::normalize(move);
    float speed = (e->runInput ? 6.5f : 2.6f) * mag;

    btVector3 vel = b->getLinearVelocity();
    glm::vec3 center = go->transform.position + glm::vec3(0, c.height * 0.5f, 0);
    glm::vec3 hit; GameObject* obj = nullptr; float frac;
    bool grounded = e->physics.raycastIgnoring(center, center - glm::vec3(0, c.height * 0.5f + 0.25f, 0), b, hit, obj, frac);
    btVector3 nv(move.x * speed, vel.y(), move.z * speed);
    if (e->jumpRequest && grounded) nv.setY(5.2f);
    e->jumpRequest = false;
    b->setLinearVelocity(nv);
    b->activate(true);

    // vira na direção do movimento
    if (mag > 0.05f) {
        glm::quat target = glm::angleAxis(std::atan2(move.x, move.z), glm::vec3(0, 1, 0));
        glm::quat q = glm::slerp(go->transform.rotation, target, 1.f - std::exp(-12.f * dt));
        btTransform t = b->getWorldTransform();
        t.setRotation({q.x, q.y, q.z, q.w});
        b->setWorldTransform(t);
        go->transform.setRotation(q);
    }

    // animação conforme a velocidade
    if (c.anim) {
        float hs = std::sqrt(nv.x() * nv.x() + nv.z() * nv.z());
        int clip = hs < 0.4f ? CC_Idle : hs < 4.f ? CC_Walk : CC_Run;
        if (isGun(c.weaponKind)) clip += CC_IdleArmed;
        if (grounded && c.anim->clipIndex != clip) c.anim->setClip(clip);
        c.anim->speed = clip % 3 == CC_Walk ? std::clamp(hs / 2.6f, 0.6f, 1.6f) : clip % 3 == CC_Run ? std::clamp(hs / 6.5f, 0.7f, 1.4f) : 1.f;
    }

    if (e->fireRequest && e->fireCooldown <= 0.f) fire(e, c, *go, b);
    e->fireRequest = false;

    // câmera em terceira pessoa segue o jogador
    glm::vec3 focus = go->transform.position + glm::vec3(0, c.height * 0.85f, 0);
    e->editorCam.target = glm::mix(e->editorCam.target, focus, 1.f - std::exp(-10.f * dt));
}

}  // namespace eng
