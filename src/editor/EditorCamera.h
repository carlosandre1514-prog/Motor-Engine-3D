#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include "render/Camera.h"

namespace eng {

struct TouchPoint { int id; float x, y; };  // pixels

// Orbit (1 dedo), Pan (2 dedos arrastando juntos), Pinch-to-zoom (2 dedos mudando distância).
class EditorCamera {
public:
    glm::vec3 target{0.f};
    float yaw = 0.6f, pitch = 0.4f, distance = 8.f;  // radianos / unidades
    float maxDistance = 500.f;                        // planeta: bem maior (zoom até ver o globo)

    void onTouchBegin(const TouchPoint* t, int n) { reset(t, n); }
    void onTouchEnd(const TouchPoint* t, int n) { reset(t, n); }  // reinicia referências ao mudar nº de dedos

    void onTouchMove(const TouchPoint* t, int n, float viewportH, const Camera& cam) {
        if (n == 1 && prevCount_ == 1) {
            float dx = t[0].x - prev_[0].x, dy = t[0].y - prev_[0].y;
            yaw -= dx * 0.008f;
            pitch = std::clamp(pitch + dy * 0.008f, -1.5f, 1.5f);
        } else if (n == 2 && prevCount_ == 2) {
            glm::vec2 mid{(t[0].x + t[1].x) * .5f, (t[0].y + t[1].y) * .5f};
            glm::vec2 pmid{(prev_[0].x + prev_[1].x) * .5f, (prev_[0].y + prev_[1].y) * .5f};
            float d = glm::length(glm::vec2(t[0].x - t[1].x, t[0].y - t[1].y));
            float pd = glm::length(glm::vec2(prev_[0].x - prev_[1].x, prev_[0].y - prev_[1].y));

            // Pan: converte deslocamento em pixels para unidades de mundo na profundidade do alvo
            float worldPerPixel = 2.f * distance * std::tan(glm::radians(cam.fovYDeg) * .5f) / viewportH;
            glm::vec3 right = glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0)));
            glm::vec3 up = glm::cross(right, forward());
            target += (-right * (mid.x - pmid.x) + up * (mid.y - pmid.y)) * worldPerPixel;

            if (pd > 1.f) distance = std::clamp(distance * (pd / std::max(d, 1.f)), 0.5f, maxDistance);
        }
        reset(t, n);
    }

    void apply(Camera& cam) const {
        glm::vec3 eye = target - forward() * distance;
        cam.lookAt(eye, target);
    }

    glm::vec3 forward() const {  // direção da câmera em direção ao alvo
        return -glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    }

private:
    void reset(const TouchPoint* t, int n) {
        prevCount_ = std::min(n, 2);
        for (int i = 0; i < prevCount_; ++i) prev_[i] = t[i];
    }
    TouchPoint prev_[2];
    int prevCount_ = 0;
};

}  // namespace eng
