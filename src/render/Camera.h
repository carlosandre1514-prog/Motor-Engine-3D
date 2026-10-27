#pragma once
#include <cmath>
#include <utility>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace eng {

struct AABB { glm::vec3 min, max; };

struct Frustum {
    glm::vec4 planes[6];  // normais apontando para dentro

    // Gribb-Hartmann, NDC de OpenGL (z em [-1,1])
    void extract(const glm::mat4& vp) {
        for (int i = 0; i < 3; ++i) {
            for (int s = 0; s < 2; ++s) {
                float sign = s == 0 ? 1.f : -1.f;
                glm::vec4 row(vp[0][3] + sign * vp[0][i], vp[1][3] + sign * vp[1][i],
                              vp[2][3] + sign * vp[2][i], vp[3][3] + sign * vp[3][i]);
                float len = glm::length(glm::vec3(row));
                planes[i * 2 + s] = row / len;
            }
        }
    }

    bool intersects(const AABB& b) const {
        for (const glm::vec4& p : planes) {
            glm::vec3 v(p.x >= 0 ? b.max.x : b.min.x, p.y >= 0 ? b.max.y : b.min.y, p.z >= 0 ? b.max.z : b.min.z);
            if (glm::dot(glm::vec3(p), v) + p.w < 0.f) return false;
        }
        return true;
    }
};

inline AABB transformAABB(const AABB& b, const glm::mat4& m) {
    glm::vec3 c = (b.min + b.max) * 0.5f, e = (b.max - b.min) * 0.5f;
    glm::vec3 nc = glm::vec3(m * glm::vec4(c, 1.f));
    glm::vec3 ne(0.f);
    for (int i = 0; i < 3; ++i)
        ne[i] = std::abs(m[0][i]) * e.x + std::abs(m[1][i]) * e.y + std::abs(m[2][i]) * e.z;
    return {nc - ne, nc + ne};
}

class Camera {
public:
    enum class Projection { Perspective, Orthographic };

    Projection projection = Projection::Perspective;
    float fovYDeg = 60.f, aspect = 16.f / 9.f, nearZ = 0.1f, farZ = 200.f;
    float orthoSize = 5.f;  // meia altura
    glm::vec3 position{0, 2, 6};
    glm::quat rotation{1, 0, 0, 0};

    glm::mat4 view() const { return glm::inverse(glm::translate(glm::mat4(1.f), position) * glm::mat4_cast(rotation)); }
    glm::mat4 proj() const {
        if (projection == Projection::Perspective) return glm::perspective(glm::radians(fovYDeg), aspect, nearZ, farZ);
        float h = orthoSize, w = h * aspect;
        return glm::ortho(-w, w, -h, h, nearZ, farZ);
    }
    glm::mat4 viewProj() const { return proj() * view(); }

    void lookAt(glm::vec3 eye, glm::vec3 target, glm::vec3 up = {0, 1, 0}) {
        position = eye;
        rotation = glm::quat_cast(glm::inverse(glm::lookAt(eye, target, up)));
    }

    Frustum frustum() const { Frustum f; f.extract(viewProj()); return f; }

    // Raio de mundo que passa pelo pixel (x, y) da tela (origem no canto superior esquerdo)
    void screenRay(float x, float y, float w, float h, glm::vec3& origin, glm::vec3& dir) const {
        glm::vec2 ndc(x / w * 2.f - 1.f, 1.f - y / h * 2.f);
        glm::mat4 inv = glm::inverse(viewProj());
        glm::vec4 a = inv * glm::vec4(ndc, -1.f, 1.f), b = inv * glm::vec4(ndc, 1.f, 1.f);
        origin = glm::vec3(a) / a.w;
        dir = glm::normalize(glm::vec3(b) / b.w - origin);
    }
};

// Interseção raio x AABB (método das placas). Retorna a distância de entrada em tHit.
inline bool rayAABB(glm::vec3 o, glm::vec3 d, const AABB& b, float& tHit) {
    float tmin = 0.f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-8f) {
            if (o[i] < b.min[i] || o[i] > b.max[i]) return false;
            continue;
        }
        float inv = 1.f / d[i];
        float t0 = (b.min[i] - o[i]) * inv, t1 = (b.max[i] - o[i]) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmin > tmax) return false;
    }
    tHit = tmin;
    return true;
}

}  // namespace eng
