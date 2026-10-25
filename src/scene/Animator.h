#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace eng {

constexpr int kMaxJoints = 128;

struct Skeleton {
    int jointCount = 0;
    int parent[kMaxJoints];            // -1 = raiz; pais sempre antes dos filhos
    glm::mat4 inverseBind[kMaxJoints];
    glm::vec3 bindT[kMaxJoints];
    glm::quat bindR[kMaxJoints];
    glm::vec3 bindS[kMaxJoints];
};

enum class ChannelPath : uint8_t { Translation, Rotation, Scale };

struct Channel {
    int joint = 0;
    ChannelPath path = ChannelPath::Translation;
    int keyCount = 0;
    const float* times = nullptr;   // keyCount
    const float* values = nullptr;  // keyCount * (3 ou 4), pertencem ao AnimationClip
};

struct AnimationClip {
    float duration = 0.f;
    int channelCount = 0;
    Channel* channels = nullptr;    // alocados no carregamento, nunca no loop
};

struct Animator {
    const Skeleton* skeleton = nullptr;
    const AnimationClip* clip = nullptr;
    float time = 0.f, speed = 1.f;
    bool loop = true, playing = true;
    glm::mat4 palette[kMaxJoints];  // enviado ao UBO de skinning

    void update(float dt) {
        if (!skeleton || !clip) return;
        if (playing) {
            time += dt * speed;
            if (clip->duration > 0.f) {
                if (loop) time = std::fmod(time, clip->duration);
                else if (time > clip->duration) { time = clip->duration; playing = false; }
            }
        }
        glm::vec3 t[kMaxJoints], s[kMaxJoints];
        glm::quat r[kMaxJoints];
        for (int i = 0; i < skeleton->jointCount; ++i) { t[i] = skeleton->bindT[i]; r[i] = skeleton->bindR[i]; s[i] = skeleton->bindS[i]; }

        for (int c = 0; c < clip->channelCount; ++c) sample(clip->channels[c], t, r, s);

        glm::mat4 global[kMaxJoints];
        for (int i = 0; i < skeleton->jointCount; ++i) {
            glm::mat4 local = glm::translate(glm::mat4(1.f), t[i]) * glm::mat4_cast(r[i]) * glm::scale(glm::mat4(1.f), s[i]);
            global[i] = skeleton->parent[i] < 0 ? local : global[skeleton->parent[i]] * local;
            palette[i] = global[i] * skeleton->inverseBind[i];
        }
    }

private:
    void sample(const Channel& ch, glm::vec3* t, glm::quat* r, glm::vec3* s) const {
        if (ch.keyCount == 0) return;
        int k1 = 0;
        while (k1 < ch.keyCount - 1 && ch.times[k1 + 1] <= time) ++k1;
        int k2 = std::min(k1 + 1, ch.keyCount - 1);
        float span = ch.times[k2] - ch.times[k1];
        float a = span > 1e-6f ? glm::clamp((time - ch.times[k1]) / span, 0.f, 1.f) : 0.f;
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

}  // namespace eng
