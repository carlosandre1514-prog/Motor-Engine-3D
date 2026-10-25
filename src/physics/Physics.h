#pragma once
#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionShapes/btShapeHull.h>
#include <glm/glm.hpp>
#include "core/Pool.h"
#include "scene/Scene.h"

namespace eng {

// Escreve o transform simulado de volta no Transform3D do GameObject.
// (Assume objetos de física na raiz da hierarquia; para filhos, converter world->local.)
class TransformMotionState : public btMotionState {
public:
    TransformMotionState(Transform3D* t) : t_(t) {}
    void getWorldTransform(btTransform& out) const override {
        out.setOrigin(btVector3(t_->position.x, t_->position.y, t_->position.z));
        out.setRotation(btQuaternion(t_->rotation.x, t_->rotation.y, t_->rotation.z, t_->rotation.w));
    }
    void setWorldTransform(const btTransform& in) override {
        btVector3 o = in.getOrigin();
        btQuaternion q = in.getRotation();
        t_->setPosition({o.x(), o.y(), o.z()});
        t_->setRotation(glm::quat(q.w(), q.x(), q.y(), q.z()));
    }
private:
    Transform3D* t_;
};

enum class ColliderType { Box, Sphere, Capsule, ConvexMesh };

struct RigidBody {
    btRigidBody* body = nullptr;
    btCollisionShape* shape = nullptr;
    TransformMotionState* motion = nullptr;
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
        world_->getSolverInfo().m_numIterations = 6;  // menos iterações: mobile
    }
    ~Physics() { delete world_; delete solver_; delete broadphase_; delete dispatcher_; delete config_; }

    // Criação de formas (no carregamento/instanciação, não no loop)
    static btCollisionShape* makeShape(ColliderType type, glm::vec3 size, const glm::vec3* pts = nullptr, int ptCount = 0) {
        switch (type) {
            case ColliderType::Box:     return new btBoxShape({size.x * .5f, size.y * .5f, size.z * .5f});
            case ColliderType::Sphere:  return new btSphereShape(size.x * .5f);
            case ColliderType::Capsule: return new btCapsuleShape(size.x * .5f, size.y);
            case ColliderType::ConvexMesh: {
                // Casco convexo simplificado com btShapeHull (menos vértices = SAT mais barato)
                btConvexHullShape tmp((const btScalar*)pts, ptCount, sizeof(glm::vec3));
                btShapeHull hull(&tmp);
                hull.buildHull(tmp.getMargin());
                return new btConvexHullShape((const btScalar*)hull.getVertexPointer(), hull.numVertices(), sizeof(btVector3));
            }
        }
        return nullptr;
    }

    void attach(GameObject& go, RigidBody& rb, btCollisionShape* shape, float mass) {
        rb.shape = shape;
        rb.mass = mass;
        rb.motion = new TransformMotionState(&go.transform);
        btVector3 inertia(0, 0, 0);
        if (mass > 0.f) shape->calculateLocalInertia(mass, inertia);
        btRigidBody::btRigidBodyConstructionInfo info(mass, rb.motion, shape, inertia);
        rb.body = new btRigidBody(info);
        rb.body->setUserPointer(&go);
        rb.applyAxisConstraints();
        world_->addRigidBody(rb.body);
        go.rigidBody = &rb;
    }

    void detach(RigidBody& rb) {
        if (!rb.body) return;
        world_->removeRigidBody(rb.body);
        delete rb.body; delete rb.motion; delete rb.shape;
        rb = RigidBody{};
    }

    // Passo fixo com acumulador: determinístico e independente do FPS
    void step(float dt) {
        accum_ += dt;
        int steps = 0;
        while (accum_ >= kFixedDt && steps < kMaxSubSteps) {
            world_->stepSimulation(kFixedDt, 0, kFixedDt);
            accum_ -= kFixedDt;
            ++steps;
        }
        if (steps == kMaxSubSteps) accum_ = 0.f;  // evita espiral da morte
    }

    bool raycast(glm::vec3 from, glm::vec3 to, glm::vec3& hit, GameObject*& obj) {
        btCollisionWorld::ClosestRayResultCallback cb({from.x, from.y, from.z}, {to.x, to.y, to.z});
        world_->rayTest(cb.m_rayFromWorld, cb.m_rayToWorld, cb);
        if (!cb.hasHit()) return false;
        hit = {cb.m_hitPointWorld.x(), cb.m_hitPointWorld.y(), cb.m_hitPointWorld.z()};
        obj = static_cast<GameObject*>(cb.m_collisionObject->getUserPointer());
        return true;
    }

    static constexpr float kFixedDt = 1.f / 60.f;
    static constexpr int kMaxSubSteps = 2;

private:
    btDefaultCollisionConfiguration* config_;
    btCollisionDispatcher* dispatcher_;
    btBroadphaseInterface* broadphase_;
    btSequentialImpulseConstraintSolver* solver_;
    btDiscreteDynamicsWorld* world_;
    float accum_ = 0.f;
};

}  // namespace eng
