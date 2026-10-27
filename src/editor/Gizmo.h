#pragma once
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "render/Camera.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

namespace eng {

// Gizmo de translação / rotação / escala em X/Y/Z, operado por toque em espaço de tela.
// Desenhado com linhas grossas sem depth test (sempre visível por cima da cena).
class Gizmo {
public:
    enum class Mode { Translate, Rotate, Scale };
    enum class Axis { None, X, Y, Z };

    Mode mode = Mode::Translate;
    GameObject* target = nullptr;
    float screenSize = 0.16f;       // tamanho do gizmo como fração da altura da tela
    float pickRadiusPx = 40.f;      // alvo de toque confortável para dedos

    Axis active() const { return active_; }

    static glm::vec3 axisDir(Axis a) { return a == Axis::X ? glm::vec3(1, 0, 0) : a == Axis::Y ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1); }
    static glm::vec4 axisColor(Axis a) {
        return a == Axis::X ? glm::vec4(0.95f, 0.25f, 0.25f, 1) : a == Axis::Y ? glm::vec4(0.35f, 0.9f, 0.3f, 1) : glm::vec4(0.3f, 0.5f, 1.f, 1);
    }

    // Projeta ponto de mundo em pixels
    static glm::vec2 project(const Camera& cam, glm::vec3 p, float w, float h) {
        glm::vec4 c = cam.viewProj() * glm::vec4(p, 1.f);
        glm::vec2 ndc = glm::vec2(c) / std::max(c.w, 1e-4f);
        return {(ndc.x * .5f + .5f) * w, (1.f - (ndc.y * .5f + .5f)) * h};
    }

    // Comprimento do eixo em unidades de mundo para ter tamanho constante na tela
    float worldLength(const Camera& cam, glm::vec3 pos) const {
        if (cam.projection == Camera::Projection::Orthographic) return screenSize * 2.f * cam.orthoSize;
        float dist = glm::length(cam.position - pos);
        return screenSize * 2.f * dist * std::tan(glm::radians(cam.fovYDeg) * .5f);
    }

    bool onTouchDown(const Camera& cam, glm::vec2 touch, float w, float h) {
        active_ = Axis::None;
        if (!target) return false;
        glm::vec3 origin = target->transform.worldPosition();
        float len = worldLength(cam, origin);
        glm::vec2 o = project(cam, origin, w, h);
        float best = pickRadiusPx;
        for (Axis a : {Axis::X, Axis::Y, Axis::Z}) {
            float d;
            if (mode == Mode::Rotate) {
                d = 1e9f;  // distância até o círculo projetado
                glm::vec2 prev = project(cam, origin + circlePoint(a, 0.f) * len, w, h);
                for (int k = 1; k <= kCircleSeg; ++k) {
                    glm::vec2 p = project(cam, origin + circlePoint(a, float(k) / kCircleSeg) * len, w, h);
                    d = std::min(d, distToSegment(touch, prev, p));
                    prev = p;
                }
            } else {
                d = distToSegment(touch, o, project(cam, origin + axisDir(a) * len, w, h));
            }
            if (d < best) { best = d; active_ = a; }
        }
        last_ = touch;
        return active_ != Axis::None;  // consome o toque: não orbita a câmera
    }

    void onTouchMove(const Camera& cam, glm::vec2 touch, float w, float h) {
        if (!target || active_ == Axis::None) return;
        Transform3D& tr = target->transform;
        glm::vec3 origin = tr.worldPosition();
        float len = worldLength(cam, origin);
        glm::vec2 o = project(cam, origin, w, h);

        if (mode == Mode::Rotate) {
            // ângulo varrido pelo dedo em volta do centro do gizmo na tela
            glm::vec2 a = last_ - o, b = touch - o;
            if (glm::length(a) > 4.f && glm::length(b) > 4.f) {
                float ang = std::atan2(a.x * b.y - a.y * b.x, glm::dot(a, b));  // + = horário na tela (y para baixo)
                glm::vec3 toCam = glm::normalize(cam.position - origin);
                float sign = glm::dot(axisDir(active_), toCam) > 0.f ? -1.f : 1.f;
                glm::quat dq = glm::angleAxis(ang * sign, axisDir(active_));
                tr.setRotation(glm::normalize(dq * tr.rotation));
            }
        } else {
            glm::vec2 e = project(cam, origin + axisDir(active_) * len, w, h);
            glm::vec2 axisPx = e - o;
            float axisPxLen = glm::length(axisPx);
            if (axisPxLen > 1e-3f) {
                float movedPx = glm::dot(touch - last_, axisPx / axisPxLen);  // movimento projetado no eixo
                if (mode == Mode::Translate) {
                    tr.setPosition(tr.position + axisDir(active_) * (movedPx * (len / axisPxLen)));
                } else {
                    glm::vec3 s = tr.scale;
                    int i = active_ == Axis::X ? 0 : active_ == Axis::Y ? 1 : 2;
                    s[i] = std::max(0.01f, s[i] * (1.f + movedPx / axisPxLen));
                    tr.setScale(s);
                }
            }
        }
        last_ = touch;
    }

    void onTouchUp() { active_ = Axis::None; }

    void draw(Renderer& r, const Camera& cam) const {
        if (!target) return;
        glm::mat4 vp = cam.viewProj();
        glm::vec3 o = target->transform.worldPosition();
        float len = worldLength(cam, o);
        for (Axis a : {Axis::X, Axis::Y, Axis::Z}) {
            glm::vec4 col = active_ == a ? glm::vec4(1.f, 0.9f, 0.2f, 1.f) : axisColor(a);
            glm::vec3 d = axisDir(a);
            if (mode == Mode::Rotate) {
                glm::vec3 prev = o + circlePoint(a, 0.f) * len;
                for (int k = 1; k <= kCircleSeg; ++k) {
                    glm::vec3 p = o + circlePoint(a, float(k) / kCircleSeg) * len;
                    r.line(vp, prev, p, col, 5.f, false);
                    prev = p;
                }
            } else {
                glm::vec3 tip = o + d * len;
                r.line(vp, o, tip, col, 6.f, false);
                // ponta: seta (translação) ou quadrado (escala)
                glm::vec3 side = glm::normalize(glm::cross(d, std::abs(d.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0)));
                glm::vec3 side2 = glm::cross(d, side);
                float hs = len * 0.08f;
                if (mode == Mode::Translate) {
                    glm::vec3 base = tip - d * hs * 2.f;
                    for (glm::vec3 s : {side, -side, side2, -side2}) r.line(vp, tip, base + s * hs, col, 5.f, false);
                } else {
                    glm::vec3 c[4] = {tip + side * hs + side2 * hs, tip - side * hs + side2 * hs, tip - side * hs - side2 * hs, tip + side * hs - side2 * hs};
                    for (int k = 0; k < 4; ++k) r.line(vp, c[k], c[(k + 1) % 4], col, 5.f, false);
                }
            }
        }
    }

private:
    static constexpr int kCircleSeg = 40;
    static glm::vec3 circlePoint(Axis a, float t) {
        float ang = t * 6.2831853f, c = std::cos(ang), s = std::sin(ang);
        return a == Axis::X ? glm::vec3(0, c, s) : a == Axis::Y ? glm::vec3(c, 0, s) : glm::vec3(c, s, 0);
    }
    static float distToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
        glm::vec2 ab = b - a;
        float t = glm::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.f, 1.f);
        return glm::length(p - (a + ab * t));
    }
    Axis active_ = Axis::None;
    glm::vec2 last_{0.f};
};

}  // namespace eng
