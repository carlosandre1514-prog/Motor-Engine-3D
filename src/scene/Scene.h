#pragma once
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include "core/Pool.h"

namespace eng {

using MeshId = uint32_t;
using MaterialId = uint32_t;

struct GameObject;

// Transform hierárquico. Filhos em lista intrusiva (sem alocação).
struct Transform3D {
    GameObject* owner = nullptr;
    glm::vec3 position{0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 scale{1.f};

    Transform3D* parent = nullptr;
    Transform3D* firstChild = nullptr;
    Transform3D* nextSibling = nullptr;

    void setEulerDegrees(glm::vec3 deg) { rotation = glm::quat(glm::radians(deg)); markDirty(); }
    glm::vec3 eulerDegrees() const { return glm::degrees(glm::eulerAngles(rotation)); }
    void setPosition(glm::vec3 p) { position = p; markDirty(); }
    void setRotation(glm::quat q) { rotation = q; markDirty(); }
    void setScale(glm::vec3 s) { scale = s; markDirty(); }

    glm::mat4 local() const {
        return glm::translate(glm::mat4(1.f), position) * glm::mat4_cast(rotation) *
               glm::scale(glm::mat4(1.f), scale);
    }
    const glm::mat4& world() const { return world_; }
    glm::vec3 worldPosition() const { return glm::vec3(world_[3]); }

    void setParent(Transform3D* p) {
        if (parent) detach();
        parent = p;
        if (p) { nextSibling = p->firstChild; p->firstChild = this; }
        markDirty();
    }

    void markDirty() {
        if (dirty_) return;
        dirty_ = true;
        for (Transform3D* c = firstChild; c; c = c->nextSibling) c->markDirty();
    }

    // Propagação top-down (chamado só nas raízes). Recursão limitada pela profundidade da hierarquia.
    void updateWorld(const glm::mat4& parentWorld, bool parentChanged) {
        bool changed = dirty_ || parentChanged;
        if (changed) { world_ = parentWorld * local(); dirty_ = false; }
        for (Transform3D* c = firstChild; c; c = c->nextSibling) c->updateWorld(world_, changed);
    }

private:
    void detach() {
        Transform3D** pp = &parent->firstChild;
        while (*pp && *pp != this) pp = &(*pp)->nextSibling;
        if (*pp) *pp = nextSibling;
        nextSibling = nullptr;
        parent = nullptr;
    }
    glm::mat4 world_{1.f};
    bool dirty_ = true;
};

inline int hierarchyDepth(const Transform3D& t) {
    int d = 0;
    for (const Transform3D* p = t.parent; p; p = p->parent) ++d;
    return d;
}

struct MeshRenderer {
    MeshId mesh = 0;
    MaterialId material = 0;
    bool castShadows = true;
    // LOD por distância: só desenha com a câmera entre lodNear e lodFar (metros) do centro do mesh
    float lodNear = 0.f, lodFar = 1e30f;
    glm::vec3 localAabbMin{-0.5f}, localAabbMax{0.5f};
};

struct Animator;   // scene/Animator.h
struct RigidBody;  // physics/Physics.h
struct Emitter;    // render/Particles.h

struct GameObject {
    char name[32] = {};
    bool active = true;
    int32_t id = -1;                 // índice público usado pela API C (editor/hierarquia)
    int32_t recipe = -1;             // receita (JSON) que recria este objeto ao abrir o projeto; só na raiz
    bool noCut = false;              // aberturas em paredes não cortam este objeto (a própria moldura, porta, arma...)
    uint32_t poolIndex = 0, poolGen = 0;
    Transform3D transform;
    MeshRenderer* meshRenderer = nullptr;
    Animator* animator = nullptr;
    RigidBody* rigidBody = nullptr;
    Emitter* emitter = nullptr;

    // Copia o nome sem cortar caracteres UTF-8 ao meio (o JNI aborta com UTF-8 inválido).
    // Sequências de 4 bytes (emoji) viram '?', pois o JNI usa UTF-8 "modificado".
    void setName(const char* n) {
        size_t o = 0;
        const unsigned char* s = reinterpret_cast<const unsigned char*>(n ? n : "");
        while (*s && o < sizeof(name) - 1) {
            unsigned char c = *s;
            int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            bool ok = len > 0;
            for (int k = 1; ok && k < len; ++k) ok = (s[k] >> 6) == 0x2;
            if (!ok || len == 4) { name[o++] = '?'; s += ok ? len : 1; continue; }
            if (o + size_t(len) > sizeof(name) - 1) break;
            for (int k = 0; k < len; ++k) name[o++] = char(s[k]);
            s += len;
        }
        name[o] = '\0';
    }
};

class Scene {
public:
    static constexpr uint32_t kMaxObjects = 16384;  // mapa editável: cada construção vira objeto
    using Handle = ObjectPool<GameObject, kMaxObjects>::Handle;

    Handle create(const char* name, GameObject* parent = nullptr) {
        Handle h = objects_.acquire();
        if (GameObject* go = objects_.get(h)) {
            go->setName(name && name[0] ? name : "Objeto");
            go->transform.owner = go;
            go->poolIndex = h.index;
            go->poolGen = h.gen;
            if (parent) go->transform.setParent(&parent->transform);
        }
        return h;
    }
    static Handle handleOf(const GameObject& go) { return {go.poolIndex, go.poolGen}; }

    // Visita o objeto e todos os descendentes (filhos antes de serem destruídos, pais primeiro).
    template <class F> static void forEachInSubtree(GameObject& root, F&& f) {
        f(root);
        for (Transform3D* c = root.transform.firstChild; c; c = c->nextSibling) forEachInSubtree(*c->owner, f);
    }

    // Destroi o objeto e toda a sua sub-árvore. Componentes externos (RigidBody...) devem ser
    // liberados antes pelo chamador.
    void destroy(Handle h) {
        GameObject* go = objects_.get(h);
        if (!go) return;
        while (go->transform.firstChild) destroy(handleOf(*go->transform.firstChild->owner));
        go->transform.setParent(nullptr);
        objects_.release(h);
    }
    void clear() {
        objects_.forEach([&](GameObject&, Handle h) { objects_.release(h); });
    }
    GameObject* get(Handle h) { return objects_.get(h); }
    uint32_t count() const { return objects_.count(); }

    void updateTransforms() {
        objects_.forEach([](GameObject& go, Handle) {
            if (!go.transform.parent) go.transform.updateWorld(glm::mat4(1.f), false);
        });
    }
    template <class F> void forEach(F&& f) { objects_.forEach(f); }

private:
    ObjectPool<GameObject, kMaxObjects> objects_;
};

using MeshRendererPool = ObjectPool<MeshRenderer, 16384>;

}  // namespace eng
