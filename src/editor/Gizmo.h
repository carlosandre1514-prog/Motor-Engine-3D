#pragma once
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "render/Camera.h"
#include "scene/Scene.h"

namespace eng {

// Gizmo de translação/rotação em eixos X/Y/Z, operando em espaço de tela (toque).
// Desenho: linhas/setas com shader unlit sobre o passe principal, sem depth test (omitido aqui).
class Gizmo {
public:
    enum class Mode { Translate, Rotate };
    enum class Axis { None, X, Y, Z };

    Mode mode = Mode::Translate;
    GameObject* target = nullptr;
    float screenSize = 0.15f;       // tamanho do gizmo como fração da altura da tela
    float pickRadiusPx = 36.f;      // alvo de toque confortável para dedos

    Axis active() const { return active_; }

    static glm::vec3 axisDir(Axis a) { return a == Axis::X ? glm::vec3(1, 0, 0) : a == Axis::Y ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1); }

    // Projeta ponto de mundo em pixels
    static glm::vec2 project(const Camera& cam, glm::vec3 p, float w, float h) {
        glm::vec4 c = cam.viewProj() * glm::vec4(p, 1.f);
        glm::vec2 ndc = glm::vec2(c) / c.w;
        return {(ndc.x * .5f + .5f) * w, (1.f - (ndc.y * .5f + .5f)) * h};
    }

    // Comprimento do eixo em unidades de mundo para ter tamanho constante na tela
    float worldLength(const Camera& cam, glm::vec3 pos, float h) const {
        float dist = glm::length(cam.position - pos);
        return screenSize * h * (2.f * dist * std::tan(glm::radians(cam.fovYDeg) * .5f) / h);
    }

    bool onTouchDown(const Camera& cam, glm::vec2 touch, float w, float h) {
        if (!target) return false;
        glm::vec3 origin = target->transform.worldPosition();
        float len = worldLength(cam, origin, h);
        glm::vec2 o = project(cam, origin, w, h);
        float best = pickRadiusPx;
        active_ = Axis::None;
        for (Axis a : {Axis::X, Axis::Y, Axis::Z}) {
            glm::vec2 e = project(cam, origin + axisDir(a) * len, w, h);
            float d = distToSegment(touch, o, e);
            if (d < best) { best = d; active_ = a; }
        }
        last_ = touch;
        return active_ != Axis::None;  // consome o toque: não orbita a câmera
    }

    void onTouchMove(const Camera& cam, glm::vec2 touch, float w, float h) {
        if (!target || active_ == Axis::None) return;
        glm::vec3 origin = target->transform.worldPosition();
        float len = worldLength(cam, origin, h);
        glm::vec2 o = project(cam, origin, w, h);
        glm::vec2 e = project(cam, origin + axisDir(active_) * len, w, h);
        glm::vec2 axisPx = e - o;
        float axisPxLen = glm::length(axisPx);
        if (axisPxLen < 1e-3f) return;
        glm::vec2 dir = axisPx / axisPxLen;
        float movedPx = glm::dot(touch - last_, dir);     // movimento projetado sobre o eixo na tela

        if (mode == Mode::Translate) {
            float worldDelta = movedPx * (len / axisPxLen);  // px -> unidades de mundo
            target->transform.setPosition(target->transform.position + axisDir(active_) * worldDelta);
        } else {
            float angle = movedPx * 0.01f;                 // 100 px ≈ 1 rad
            glm::quat dq = glm::angleAxis(angle, axisDir(active_));
            target->transform.setRotation(glm::normalize(dq * target->transform.rotation));
        }
        last_ = touch;
    }

    void onTouchUp() { active_ = Axis::None; }

private:
    static float distToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
        glm::vec2 ab = b - a;
        float t = glm::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.f, 1.f);
        return glm::length(p - (a + ab * t));
    }
    Axis active_ = Axis::None;
    glm::vec2 last_{0.f};
};

}  // namespace eng
