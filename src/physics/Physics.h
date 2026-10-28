#pragma once
#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionShapes/btShapeHull.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>
#include "core/Pool.h"
#include "scene/Scene.h"

namespace eng {

// Escreve o transform simulado de volta no Transform3D do GameObject.
// (Assume objetos de física na raiz da hierarquia; para filhos, converter world->local.)
// offset: centro do collider em relação à origem do objeto (ex.: personagem com origem nos pés)
class TransformMotionState : public btMotionState {
public:
    TransformMotionState(Transform3D* t, glm::vec3 offset = glm::vec3(0.f)) : t_(t), offset_(offset) {}
    void getWorldTransform(btTransform& out) const override {
        glm::vec3 c = t_->position + t_->rotation * offset_;
        out.setOrigin(btVector3(c.x, c.y, c.z));
        out.setRotation(btQuaternion(t_->rotation.x, t_->rotation.y, t_->rotation.z, t_->rotation.w));
    }
    void setWorldTransform(const btTransform& in) override {
        btVector3 o = in.getOrigin();
        btQuaternion q = in.getRotation();
        glm::quat r(q.w(), q.x(), q.y(), q.z());
        t_->setPosition(glm::vec3(o.x(), o.y(), o.z()) - r * offset_);
        t_->setRotation(r);
    }
private:
    Transform3D* t_;
    glm::vec3 offset_;
};

enum class ColliderType { Box, Sphere, Capsule, ConvexMesh, Compound };

struct RigidBody {
    btRigidBody* body = nullptr;
    btCollisionShape* shape = nullptr;
    TransformMotionState* motion = nullptr;
    GameObject* owner = nullptr;
    ColliderType type = ColliderType::Box;
    glm::vec3 baseScale{1.f};
    glm::vec3 centerOffset{0.f};  // centro do collider em relação à origem do objeto  // collider criado em unidades locais; a escala do Transform multiplica por cima
    float mass = 1.f;  // 0 = estático
    bool lockRotX = false, lockRotY = false, lockRotZ = false;
    bool lockPosX = false, lockPosY = false, lockPosZ = false;

    void addForce(glm::vec3 f) { if (body) { body->activate(); body->applyCentralForce({f.x, f.y, f.z}); } }
    void addImpulse(glm::vec3 i) { if (body) { body->activate(); body->applyCentralImpulse({i.x, i.y, i.z}); } }
    void setVelocity(glm::vec3 v) { if (body) body->setLinearVelocity({v.x, v.y, v.z}); }
    void applyAxisConstraints() {  // fator 0 trava o eixo
        if (!body) return;
        body->setLinearFactor({lockPosX ? 0.f : 1.f, lockPosY ? 0.f : 1.f, lockPosZ ? 0.f : 1.f});
        body->setAngularFactor({lockRotX ? 0.f : 1.f, lockRotY ? 0.f : 1.f, lockRotZ ? 0.f : 1.f});
    }
};

class Physics {
public:
    Physics() {
        config_ = new btDefaultCollisionConfiguration();
        dispatcher_ = new btCollisionDispatcher(config_);
        broadphase_ = new btDbvtBroadphase();
        solver_ = new btSequentialImpulseConstraintSolver();
        world_ = new btDiscreteDynamicsWorld(dispatcher_, broadphase_, solver_, config_);
        world_->setGravity({0, -9.81f, 0});
        world_->getSolverInfo().m_numIterations = 8;  // poucas iterações: mobile
        dispatcher_->setNearCallback(&Physics::cutNearCallback);
    }

    // Aberturas cortadas em paredes/muros (porta, janela, vão): caixas (mundo -> cubo unitário centrado).
    // Contatos e raios em corpos estáticos dentro delas são ignorados: dá para passar e atirar pelo buraco.
    // Corpos com userIndex2 == 1 não são cortados.
    std::vector<glm::mat4> cuts;
    bool insideCut(glm::vec3 p) const {
        for (const glm::mat4& m : cuts) {
            glm::vec3 q = glm::vec3(m * glm::vec4(p, 1.f));
            if (std::abs(q.x) < 0.5f && std::abs(q.y) < 0.5f && std::abs(q.z) < 0.5f) return true;
        }
        return false;
    }
    bool cutHit(const btCollisionObject* o, glm::vec3 p) const {
        return !cuts.empty() && o->isStaticObject() && o->getUserIndex2() != 1 && insideCut(p);
    }
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;
    ~Physics() { delete world_; delete solver_; delete broadphase_; delete dispatcher_; delete config_; }

    // Criação de formas (no carregamento/instanciação, não no loop)
    static btCollisionShape* makeShape(ColliderType type, glm::vec3 size, const glm::vec3* pts = nullptr, int ptCount = 0) {
        switch (type) {
            case ColliderType::Box:     return new btBoxShape({size.x * .5f, size.y * .5f, size.z * .5f});
            case ColliderType::Sphere:  return new btSphereShape(size.x * .5f);
            // Cápsula em Y: size.x = diâmetro, size.y = altura total (com as semiesferas)
            case ColliderType::Capsule: return new btCapsuleShape(size.x * .5f, std::max(size.y - size.x, 0.01f));
            case ColliderType::ConvexMesh: {
                // Casco convexo simplificado com btShapeHull (menos vértices = SAT mais barato)
                btConvexHullShape tmp;
                for (int i = 0; i < ptCount; ++i) tmp.addPoint({pts[i].x * size.x, pts[i].y * size.y, pts[i].z * size.z}, false);
                tmp.recalcLocalAabb();
                btShapeHull hull(&tmp);
                hull.buildHull(tmp.getMargin());
                return new btConvexHullShape((const btScalar*)hull.getVertexPointer(), hull.numVertices(), sizeof(btVector3));
            }
        }
        return nullptr;
    }

    // size = dimensões do collider no espaço local do objeto (antes da escala do Transform)
    void attach(GameObject& go, RigidBody& rb, ColliderType type, btCollisionShape* shape, float mass) {
        rb.shape = shape;
        rb.mass = mass;
        rb.type = type;
        rb.owner = &go;
        shape->setMargin(type == ColliderType::Box || type == ColliderType::ConvexMesh ? 0.02f : shape->getMargin());
        rb.motion = new TransformMotionState(&go.transform, rb.centerOffset);
        btVector3 inertia(0, 0, 0);
        if (mass > 0.f) shape->calculateLocalInertia(mass, inertia);
        btRigidBody::btRigidBodyConstructionInfo info(mass, rb.motion, shape, inertia);
        info.m_friction = 0.6f;
        info.m_restitution = 0.15f;
        info.m_rollingFriction = 0.01f;
        info.m_spinningFriction = 0.01f;
        rb.body = new btRigidBody(info);
        rb.body->setRollingFriction(0.01f);
        rb.body->setUserPointer(&go);
        rb.applyAxisConstraints();
        world_->addRigidBody(rb.body);
        go.rigidBody = &rb;
    }

    void detach(RigidBody& rb) {
        if (!rb.body) return;
        world_->removeRigidBody(rb.body);
        if (rb.owner) rb.owner->rigidBody = nullptr;
        delete rb.body; delete rb.motion; delete rb.shape;
        rb = RigidBody{};
    }

    // Antes do Play: copia o Transform (editado no editor) para o corpo, aplica a escala no
    // collider e zera velocidades. Usado também no Stop para restaurar o snapshot.
    void syncFromTransform(RigidBody& rb) {
        if (!rb.body || !rb.owner) return;
        Transform3D& t = rb.owner->transform;
        glm::vec3 sc = t.scale / glm::max(rb.baseScale, glm::vec3(1e-4f));
        if (rb.type == ColliderType::Sphere) sc = glm::vec3(std::max(sc.x, std::max(sc.y, sc.z)));  // esfera: escala uniforme
        rb.shape->setLocalScaling({sc.x, sc.y, sc.z});
        btVector3 inertia(0, 0, 0);
        if (rb.mass > 0.f) rb.shape->calculateLocalInertia(rb.mass, inertia);
        rb.body->setMassProps(rb.mass, inertia);
        rb.body->updateInertiaTensor();
        btTransform tr;
        glm::vec3 c = t.position + t.rotation * rb.centerOffset;
        tr.setOrigin({c.x, c.y, c.z});
        tr.setRotation({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w});
        rb.body->setWorldTransform(tr);
        rb.body->setInterpolationWorldTransform(tr);
        rb.body->setLinearVelocity({0, 0, 0});
        rb.body->setAngularVelocity({0, 0, 0});
        rb.body->setInterpolationLinearVelocity({0, 0, 0});
        rb.body->setInterpolationAngularVelocity({0, 0, 0});
        rb.body->clearForces();
        rb.body->activate(true);
        world_->updateSingleAabb(rb.body);
    }
    void resetAccumulator() { accum_ = 0.f; }
    // Tira/põe o corpo da simulação (inimigo morto, objeto coletado)
    void setEnabled(RigidBody& rb, bool on) {
        if (!rb.body) return;
        if (on) { if (!rb.body->isInWorld()) world_->addRigidBody(rb.body); }
        else if (rb.body->isInWorld()) world_->removeRigidBody(rb.body);
    }
    // Estático <-> cinemático (plataforma que se move no Play; o corpo segue o Transform)
    void setKinematic(RigidBody& rb, bool on) {
        if (!rb.body) return;
        int f = rb.body->getCollisionFlags();
        if (on) {
            rb.body->setCollisionFlags((f & ~btCollisionObject::CF_STATIC_OBJECT) | btCollisionObject::CF_KINEMATIC_OBJECT);
            rb.body->setActivationState(DISABLE_DEACTIVATION);
        } else {
            rb.body->setCollisionFlags((f & ~btCollisionObject::CF_KINEMATIC_OBJECT) | (rb.mass <= 0.f ? btCollisionObject::CF_STATIC_OBJECT : 0));
            rb.body->forceActivationState(ACTIVE_TAG);
        }
    }

    // Corpos estáticos de cenário (prédios, terreno, barreiras). A forma pertence ao chamador.
    btRigidBody* addStatic(btCollisionShape* shape, glm::vec3 pos, glm::quat rot = glm::quat(1, 0, 0, 0), float friction = 0.8f) {
        btRigidBody::btRigidBodyConstructionInfo info(0.f, nullptr, shape, btVector3(0, 0, 0));
        info.m_friction = friction;
        info.m_startWorldTransform.setOrigin({pos.x, pos.y, pos.z});
        info.m_startWorldTransform.setRotation({rot.x, rot.y, rot.z, rot.w});
        btRigidBody* b = new btRigidBody(info);
        world_->addRigidBody(b);
        return b;
    }
    void removeStatic(btRigidBody* b) {
        if (!b) return;
        world_->removeRigidBody(b);
        delete b;
    }
    // Raio que ignora um corpo (ex.: o próprio jogador)
    bool raycastIgnoring(glm::vec3 from, glm::vec3 to, const btCollisionObject* ignore, glm::vec3& hit, GameObject*& obj, float& fraction) {
        struct Cb : btCollisionWorld::ClosestRayResultCallback {
            const btCollisionObject* ign;
            Cb(const btVector3& a, const btVector3& b, const btCollisionObject* i) : ClosestRayResultCallback(a, b), ign(i) {}
            const Physics* ph;
            btScalar addSingleResult(btCollisionWorld::LocalRayResult& r, bool normalInWorld) override {
                if (r.m_collisionObject == ign) return 1.f;
                btVector3 p = m_rayFromWorld.lerp(m_rayToWorld, r.m_hitFraction);
                if (ph->cutHit(r.m_collisionObject, {p.x(), p.y(), p.z()})) return 1.f;  // passou pela abertura
                return ClosestRayResultCallback::addSingleResult(r, normalInWorld);
            }
        } cb({from.x, from.y, from.z}, {to.x, to.y, to.z}, ignore);
        cb.ph = this;
        world_->rayTest(cb.m_rayFromWorld, cb.m_rayToWorld, cb);
        if (!cb.hasHit()) return false;
        hit = {cb.m_hitPointWorld.x(), cb.m_hitPointWorld.y(), cb.m_hitPointWorld.z()};
        obj = static_cast<GameObject*>(cb.m_collisionObject->getUserPointer());
        fraction = cb.m_closestHitFraction;
        return true;
    }

    // Passo fixo com acumulador: determinístico e independente do FPS
    void step(float dt) {
        active_ = this;
        accum_ += dt;
        int steps = 0;
        while (accum_ >= kFixedDt && steps < kMaxSubSteps) {
            world_->stepSimulation(kFixedDt, 0, kFixedDt);
            accum_ -= kFixedDt;
            ++steps;
        }
        if (steps == kMaxSubSteps && accum_ > kFixedDt) accum_ = 0.f;  // evita espiral da morte
    }

    bool raycast(glm::vec3 from, glm::vec3 to, glm::vec3& hit, GameObject*& obj) {
        float f;
        return raycastIgnoring(from, to, nullptr, hit, obj, f);
    }
    // Raio que também devolve a normal da superfície atingida
    bool raycastNormal(glm::vec3 from, glm::vec3 to, glm::vec3& hit, glm::vec3& normal, const btCollisionObject* ignore = nullptr) {
        struct Cb : btCollisionWorld::ClosestRayResultCallback {
            const btCollisionObject* ign; const Physics* ph;
            Cb(const btVector3& a, const btVector3& b) : ClosestRayResultCallback(a, b) {}
            btScalar addSingleResult(btCollisionWorld::LocalRayResult& r, bool normalInWorld) override {
                if (r.m_collisionObject == ign) return 1.f;
                btVector3 p = m_rayFromWorld.lerp(m_rayToWorld, r.m_hitFraction);
                if (ph->cutHit(r.m_collisionObject, {p.x(), p.y(), p.z()})) return 1.f;
                return ClosestRayResultCallback::addSingleResult(r, normalInWorld);
            }
        } cb({from.x, from.y, from.z}, {to.x, to.y, to.z});
        cb.ign = ignore; cb.ph = this;
        world_->rayTest(cb.m_rayFromWorld, cb.m_rayToWorld, cb);
        if (!cb.hasHit()) return false;
        hit = {cb.m_hitPointWorld.x(), cb.m_hitPointWorld.y(), cb.m_hitPointWorld.z()};
        normal = glm::normalize(glm::vec3(cb.m_hitNormalWorld.x(), cb.m_hitNormalWorld.y(), cb.m_hitNormalWorld.z()));
        return true;
    }

    static constexpr float kFixedDt = 1.f / 60.f;
    static constexpr int kMaxSubSteps = 3;

private:
    // Depois da colisão normal, tira os contatos que caem dentro de uma abertura (só no lado do corpo estático)
    static void cutNearCallback(btBroadphasePair& pair, btCollisionDispatcher& d, const btDispatcherInfo& info) {
        btCollisionDispatcher::defaultNearCallback(pair, d, info);
        const Physics* ph = active_;
        if (!ph || ph->cuts.empty() || !pair.m_algorithm) return;
        btManifoldArray arr;
        pair.m_algorithm->getAllContactManifolds(arr);
        for (int k = 0; k < arr.size(); ++k) {
            btPersistentManifold* m = arr[k];
            const btCollisionObject* a = m->getBody0();
            const btCollisionObject* b = m->getBody1();
            bool sa = a->isStaticObject(), sb = b->isStaticObject();
            if (sa == sb) continue;
            if ((sa ? a : b)->getUserIndex2() == 1) continue;
            for (int i = m->getNumContacts() - 1; i >= 0; --i) {
                const btManifoldPoint& p = m->getContactPoint(i);
                const btVector3& w = sa ? p.getPositionWorldOnA() : p.getPositionWorldOnB();
                if (ph->insideCut({w.x(), w.y(), w.z()})) m->removeContactPoint(i);
            }
        }
    }
    static inline thread_local const Physics* active_ = nullptr;

    btDefaultCollisionConfiguration* config_;
    btCollisionDispatcher* dispatcher_;
    btBroadphaseInterface* broadphase_;
    btSequentialImpulseConstraintSolver* solver_;
    btDiscreteDynamicsWorld* world_;
    float accum_ = 0.f;
};

using RigidBodyPool = ObjectPool<RigidBody, 8192>;

}  // namespace eng
