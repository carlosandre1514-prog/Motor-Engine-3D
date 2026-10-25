#pragma once
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "render/Camera.h"

namespace eng {

constexpr int kNumCascades = 3;
constexpr int kMaxPointLights = 8;
constexpr int kShadowMapSize = 1024;

struct DirectionalLight {
    glm::vec3 direction{-0.4f, -1.f, -0.3f};  // direção da luz (aponta para a cena)
    glm::vec3 color{1.f, 0.96f, 0.9f};
    float intensity = 3.f;
    bool castShadows = true;
};

struct PointLight {
    glm::vec3 position{0.f};
    float radius = 10.f;
    glm::vec3 color{1.f};
    float intensity = 10.f;
};

struct AmbientLight {
    glm::vec3 skyColor{0.45f, 0.55f, 0.7f};
    glm::vec3 groundColor{0.2f, 0.18f, 0.15f};
    float intensity = 0.6f;
};

// UBO std140 (binding 0). Manter alinhamento a vec4.
struct PointLightGPU { glm::vec4 posRadius; glm::vec4 colorIntensity; };
struct SceneUBO {
    glm::mat4 viewProj;
    glm::vec4 camPos;
    glm::vec4 dirDir;                       // xyz direção, w = castShadows
    glm::vec4 dirColor;                     // rgb * intensidade
    glm::mat4 cascadeVP[kNumCascades];
    glm::vec4 cascadeSplits;                // profundidade de view onde cada cascata termina
    glm::vec4 skyAmbient;
    glm::vec4 groundAmbient;
    glm::ivec4 counts;                      // x = nº de point lights
    PointLightGPU points[kMaxPointLights];
};

struct CascadeSet {
    glm::mat4 viewProj[kNumCascades];
    float splitDepth[kNumCascades];
};

// Cascaded Shadow Maps: divisão "prática" (mistura log/linear) + ajuste ortográfico ao slice
// do frustum com snapping em texels para evitar tremulação ao mover a câmera.
inline CascadeSet computeCascades(const Camera& cam, const glm::vec3& lightDir, float shadowDistance, float lambda = 0.7f) {
    CascadeSet out{};
    float n = cam.nearZ, f = std::min(cam.farZ, shadowDistance);
    float splits[kNumCascades + 1];
    splits[0] = n;
    for (int i = 1; i <= kNumCascades; ++i) {
        float p = float(i) / kNumCascades;
        float log = n * std::pow(f / n, p), lin = n + (f - n) * p;
        splits[i] = lambda * log + (1.f - lambda) * lin;
    }

    glm::mat4 invVP = glm::inverse(cam.proj() * cam.view());
    glm::vec3 L = glm::normalize(lightDir);
    glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);

    for (int c = 0; c < kNumCascades; ++c) {
        // cantos do slice [splits[c], splits[c+1]] interpolando entre plano near e far do frustum
        float t0 = (splits[c] - cam.nearZ) / (cam.farZ - cam.nearZ);
        float t1 = (splits[c + 1] - cam.nearZ) / (cam.farZ - cam.nearZ);
        glm::vec3 corners[8];
        int k = 0;
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) {
                    glm::vec4 nearP = invVP * glm::vec4(x * 2.f - 1.f, y * 2.f - 1.f, -1.f, 1.f);
                    glm::vec4 farP = invVP * glm::vec4(x * 2.f - 1.f, y * 2.f - 1.f, 1.f, 1.f);
                    glm::vec3 a = glm::vec3(nearP) / nearP.w, b = glm::vec3(farP) / farP.w;
                    corners[k++] = glm::mix(a, b, z ? t1 : t0);
                }
        glm::vec3 center(0.f);
        for (auto& v : corners) center += v;
        center /= 8.f;
        float radius = 0.f;
        for (auto& v : corners) radius = std::max(radius, glm::length(v - center));
        radius = std::ceil(radius * 16.f) / 16.f;  // esfera envolvente: tamanho estável entre frames

        float texel = (2.f * radius) / kShadowMapSize;
        glm::mat4 lightView = glm::lookAt(center - L * radius * 2.f, center, up);
        glm::vec3 lc = glm::vec3(lightView * glm::vec4(center, 1.f));
        lc.x = std::floor(lc.x / texel) * texel;  // snapping
        lc.y = std::floor(lc.y / texel) * texel;
        glm::mat4 proj = glm::ortho(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius, 0.f, radius * 4.f);
        out.viewProj[c] = proj * lightView;
        out.splitDepth[c] = splits[c + 1];
    }
    return out;
}

}  // namespace eng
