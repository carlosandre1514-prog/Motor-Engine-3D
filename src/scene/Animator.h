#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "core/Pool.h"

namespace eng {

constexpr int kMaxJoints = 128;

struct Transform3D;  // scene/Scene.h

struct Skeleton {
    int jointCount = 0;
    int parent[kMaxJoints];            // -1 = raiz do esqueleto
    int order[kMaxJoints];             // ordem de avaliação: pais sempre antes dos filhos
    glm::mat4 inverseBind[kMaxJoints];
    glm::vec3 bindT[kMaxJoints];
    glm::quat bindR[kMaxJoints];
    glm::vec3 bindS[kMaxJoints];
};

enum class ChannelPath : uint8_t { Translation, Rotation, Scale };

struct Channel {
    int joint = 0;
    ChannelPath path = ChannelPath::Translation;
    bool step = false;              // interpolação STEP (sem mistura)
    int keyCount = 0;
    const float* times = nullptr;   // keyCount
    const float* values = nullptr;  // keyCount * (3 ou 4), pertencem ao modelo carregado
};

struct AnimationClip {
    char name[32] = {};
    float duration = 0.f;
    int channelCount = 0;
    Channel* channels = nullptr;    // alocados no carregamento, nunca no loop
};

struct Animator {
    const Skeleton* skeleton = nullptr;
    const AnimationClip* clips = nullptr;  // todas as animações do modelo
    int clipCount = 0, clipIndex = 0;
    const Transform3D* space = nullptr;    // nó pai da raiz do esqueleto (define o espaço de mundo)
    float time = 0.f, speed = 1.f;
    bool loop = true;
    glm::mat4 palette[kMaxJoints];  // enviado ao UBO de skinning
    glm::mat4 global[kMaxJoints];   // pose de cada joint no espaço do esqueleto (para encaixar armas/acessórios)

    const AnimationClip* clip() const { return clipIndex < clipCount ? &clips[clipIndex] : nullptr; }

    void setClip(int i) { if (clipCount > 0) { clipIndex = ((i % clipCount) + clipCount) % clipCount; time = 0.f; } }

    // advance = false: só reavalia a pose no tempo atual (modo edição)
    void update(float dt, bool advance) {
        if (!skeleton) return;
        const AnimationClip* c = clip();
        if (advance && c && c->duration > 0.f) {
            time += dt * speed;
            if (loop) time = std::fmod(time, c->duration);
            else time = std::min(time, c->duration);
        }
        glm::vec3 t[kMaxJoints], s[kMaxJoints];
        glm::quat r[kMaxJoints];
        const int n = skeleton->jointCount;
        for (int i = 0; i < n; ++i) { t[i] = skeleton->bindT[i]; r[i] = skeleton->bindR[i]; s[i] = skeleton->bindS[i]; }

        if (c) for (int k = 0; k < c->channelCount; ++k) sample(c->channels[k], t, r, s);

        for (int k = 0; k < n; ++k) {
            int i = skeleton->order[k];
            glm::mat4 local = glm::translate(glm::mat4(1.f), t[i]) * glm::mat4_cast(r[i]) * glm::scale(glm::mat4(1.f), s[i]);
            int p = skeleton->parent[i];
            global[i] = p < 0 ? local : global[p] * local;
            palette[i] = global[i] * skeleton->inverseBind[i];
        }
    }

private:
    void sample(const Channel& ch, glm::vec3* t, glm::quat* r, glm::vec3* s) const {
        if (ch.keyCount == 0 || ch.joint < 0 || ch.joint >= kMaxJoints) return;
        // busca binária do keyframe (clips longos)
        int lo = 0, hi = ch.keyCount - 1;
        if (time <= ch.times[0]) hi = 0;
        else if (time >= ch.times[hi]) lo = hi;
        else while (hi - lo > 1) { int mid = (lo + hi) / 2; (ch.times[mid] <= time ? lo : hi) = mid; }
        int k1 = lo, k2 = hi;
        float span = ch.times[k2] - ch.times[k1];
        float a = (span > 1e-6f && !ch.step) ? glm::clamp((time - ch.times[k1]) / span, 0.f, 1.f) : 0.f;
        if (ch.path == ChannelPath::Rotation) {
            auto q = [&](int k) { const float* v = ch.values + k * 4; return glm::quat(v[3], v[0], v[1], v[2]); };
            r[ch.joint] = glm::normalize(glm::slerp(q(k1), q(k2), a));
        } else {
            auto v3 = [&](int k) { const float* v = ch.values + k * 3; return glm::vec3(v[0], v[1], v[2]); };
            glm::vec3 val = glm::mix(v3(k1), v3(k2), a);
            (ch.path == ChannelPath::Translation ? t : s)[ch.joint] = val;
        }
    }
};

using AnimatorPool = ObjectPool<Animator, 64>;

}  // namespace eng
