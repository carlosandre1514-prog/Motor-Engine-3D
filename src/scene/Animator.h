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
    char name[kMaxJoints][32] = {};    // nome do osso (para reconhecer humanoides)
    // inverse(inverseBind) está no espaço do arquivo (glTF) ou do esqueleto (FBX): isto leva ao espaço do esqueleto
    glm::mat4 bindToSkel{1.f};
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

// Retarget humanoide (como o "Humanoid" da Unity): toca animações feitas para o esqueleto de referência
// do motor num esqueleto importado qualquer, casando os ossos pelo nome e corrigindo a pose de repouso.
struct Retarget {
    const Skeleton* src = nullptr;          // esqueleto de referência (humanoide procedural)
    int map[kMaxJoints];                    // osso do modelo -> osso de referência (ou -1)
    int targetOf[kMaxJoints];               // osso de referência -> osso do modelo (ou -1)
    glm::quat align[kMaxJoints];            // corrige a direção dos ossos (ex.: T-pose -> braços para baixo)
    glm::quat bindRot[kMaxJoints];          // rotação global de repouso do modelo (referencial do mundo)
    glm::quat spaceRot{1, 0, 0, 0};         // rotação do nó acima do esqueleto (ex.: Armature do Blender em Z-up)
    float spaceScale = 1.f;                 // escala desse nó (ex.: 0,01 de modelos em centímetros)
    glm::vec3 restT[kMaxJoints], restS[kMaxJoints];
    glm::quat restR[kMaxJoints];            // pose de repouso local (das matrizes de bind)
    float parentScale[kMaxJoints];          // escala global do pai (para converter a translação do quadril)
    float hipsScale = 1.f;
    int srcHips = 0;
};

struct Animator {
    const Skeleton* skeleton = nullptr;
    const Retarget* retarget = nullptr;     // != null: clips são do esqueleto de referência
    const AnimationClip* clips = nullptr;  // todas as animações do modelo (com retarget: as do motor)
    int clipCount = 0, clipIndex = 0;
    // Com retarget: os primeiros `refCount` clips são do esqueleto de referência; depois vêm os do próprio modelo
    int refCount = 0;
    const AnimationClip* ownClips = nullptr;
    int ownCount = 0;
    const Transform3D* space = nullptr;    // nó pai da raiz do esqueleto (define o espaço de mundo)
    float time = 0.f, speed = 1.f;
    bool loop = true;
    glm::mat4 palette[kMaxJoints];  // enviado ao UBO de skinning
    glm::mat4 global[kMaxJoints];   // pose de cada joint no espaço do esqueleto (para encaixar armas/acessórios)
    // Editor de animação: pose manual (rotação local de cada osso), no lugar do clip
    bool posing = false;
    glm::quat poseR[kMaxJoints];
    const AnimationClip* preview = nullptr;  // prévia de uma animação em edição (toca no lugar do clip)

    // clip pelo índice da lista completa (com retarget: motor primeiro, depois os do modelo)
    const AnimationClip* clipAt(int i) const {
        if (i < 0 || i >= clipCount) return nullptr;
        if (retarget) return i < refCount ? &clips[i] : (i - refCount < ownCount ? &ownClips[i - refCount] : nullptr);
        return &clips[i];
    }

    const AnimationClip* clip() const {
        if (retarget) {
            if (clipIndex < refCount) return &clips[clipIndex];
            int k = clipIndex - refCount;
            return k < ownCount ? &ownClips[k] : nullptr;
        }
        return clipIndex < clipCount ? &clips[clipIndex] : nullptr;
    }

    void setClip(int i) { if (clipCount > 0) { clipIndex = ((i % clipCount) + clipCount) % clipCount; time = 0.f; } }

    // advance = false: só reavalia a pose no tempo atual (modo edição)
    void update(float dt, bool advance) {
        if (!skeleton) return;
        const AnimationClip* c = preview ? preview : clip();
        if (advance && c && c->duration > 0.f) {
            time += dt * speed;
            if (loop) time = std::fmod(time, c->duration);
            else time = std::min(time, c->duration);
        }
        if (retarget && clipIndex < refCount && !posing && !preview) { updateRetarget(c); return; }
        glm::vec3 t[kMaxJoints], s[kMaxJoints];
        glm::quat r[kMaxJoints];
        const int n = skeleton->jointCount;
        for (int i = 0; i < n; ++i) { t[i] = skeleton->bindT[i]; r[i] = skeleton->bindR[i]; s[i] = skeleton->bindS[i]; }

        if (posing) for (int i = 0; i < n; ++i) r[i] = poseR[i];
        else if (c) for (int k = 0; k < c->channelCount; ++k) sample(c->channels[k], t, r, s);

        for (int k = 0; k < n; ++k) {
            int i = skeleton->order[k];
            glm::mat4 local = glm::translate(glm::mat4(1.f), t[i]) * glm::mat4_cast(r[i]) * glm::scale(glm::mat4(1.f), s[i]);
            int p = skeleton->parent[i];
            global[i] = p < 0 ? local : global[p] * local;
            palette[i] = global[i] * skeleton->inverseBind[i];
        }
    }

private:
    void updateRetarget(const AnimationClip* c) {
        const Retarget& R = *retarget;
        const Skeleton& S = *R.src;
        // 1) amostra o clip no esqueleto de referência e acumula as rotações globais
        glm::vec3 st[kMaxJoints], ss[kMaxJoints];
        glm::quat sr[kMaxJoints], sg[kMaxJoints];
        for (int i = 0; i < S.jointCount; ++i) { st[i] = S.bindT[i]; sr[i] = S.bindR[i]; ss[i] = S.bindS[i]; }
        if (c) for (int k = 0; k < c->channelCount; ++k) sample(c->channels[k], st, sr, ss);
        for (int k = 0; k < S.jointCount; ++k) {
            int i = S.order[k], p = S.parent[i];
            sg[i] = p < 0 ? sr[i] : sg[p] * sr[i];
        }
        // deslocamento do quadril: metros no mundo -> unidades do esqueleto
        glm::quat invSpace = glm::inverse(R.spaceRot);
        glm::vec3 hipsDelta = invSpace * ((st[R.srcHips] - S.bindT[R.srcHips]) * R.hipsScale) / R.spaceScale;
        // 2) aplica no esqueleto do modelo: rotação global desejada = anim * alinhamento * repouso
        glm::quat fg[kMaxJoints];
        const int n = skeleton->jointCount;
        for (int k = 0; k < n; ++k) {
            int i = skeleton->order[k], p = skeleton->parent[i];
            glm::quat fp = p < 0 ? glm::quat(1, 0, 0, 0) : fg[p];
            glm::quat lr;
            int m = R.map[i];
            if (m >= 0) {
                fg[i] = glm::normalize(invSpace * sg[m] * R.align[i] * R.bindRot[i]);
                lr = glm::normalize(glm::inverse(fp) * fg[i]);
            } else {
                lr = R.restR[i];
                fg[i] = glm::normalize(fp * lr);
            }
            glm::vec3 lt = R.restT[i];
            if (m == R.srcHips) lt += (glm::inverse(fp) * hipsDelta) / R.parentScale[i];
            glm::mat4 local = glm::translate(glm::mat4(1.f), lt) * glm::mat4_cast(lr) * glm::scale(glm::mat4(1.f), R.restS[i]);
            global[i] = p < 0 ? local : global[p] * local;
            palette[i] = global[i] * skeleton->inverseBind[i];
        }
    }

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
