#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "render/Camera.h"

namespace eng {

constexpr int kNumCascades = 3;
constexpr int kMaxPointLights = 8;
constexpr int kShadowMapSize = 1024;

struct DirectionalLight {
    glm::vec3 direction{0.55f, -1.f, 0.25f};  // direção da luz (aponta para a cena)
    glm::vec3 color{1.f, 0.95f, 0.88f};
    float intensity = 3.2f;
    bool castShadows = true;
};

struct PointLight {
    glm::vec3 position{0.f};
    float radius = 10.f;
    glm::vec3 color{1.f};
    float intensity = 10.f;
};

// Céu procedural: gradiente horizonte->zênite, chão e disco do sol. Também é a fonte do IBL
// (irradiância em SH9 para difuso + cubemap com mips para reflexo especular).
struct Environment {
    glm::vec3 zenith{0.18f, 0.36f, 0.75f};
    glm::vec3 horizon{0.70f, 0.78f, 0.88f};
    glm::vec3 ground{0.22f, 0.20f, 0.18f};
    float skyIntensity = 0.55f;  // brilho do céu (e do IBL)
    float exposure = 1.0f;       // exposição da câmera (antes do tone mapping)

    bool operator==(const Environment& o) const {
        return zenith == o.zenith && horizon == o.horizon && ground == o.ground && skyIntensity == o.skyIntensity;
    }

    // Mesma fórmula de skyColor() em shaders/sky.frag (sem o disco do sol). Manter as duas iguais.
    glm::vec3 sample(glm::vec3 d) const {
        float y = d.y;
        glm::vec3 c;
        if (y >= 0.f) {
            float t = std::pow(std::min(y, 1.f), 0.45f);
            c = glm::mix(horizon, zenith, t);
        } else {
            float t = std::pow(std::min(-y, 1.f), 0.35f);
            c = glm::mix(horizon * 0.6f, ground, t);
        }
        return c * skyIntensity;
    }
};

// Projeta o céu em harmônicos esféricos de ordem 2 (9 coeficientes) já convoluídos com o lobo
// de cosseno e divididos por PI: no shader, irradiance(N) devolve direto o termo difuso / albedo.
inline void computeSkySH(const Environment& env, glm::vec4 out[9]) {
    glm::vec3 c[9] = {};
    const int N = 512;
    const float golden = 2.399963229728653f;  // pi * (3 - sqrt(5))
    for (int i = 0; i < N; ++i) {              // pontos de Fibonacci: amostragem uniforme da esfera
        float y = 1.f - 2.f * (i + 0.5f) / N;
        float r = std::sqrt(std::max(0.f, 1.f - y * y));
        float phi = golden * i;
        glm::vec3 d(std::cos(phi) * r, y, std::sin(phi) * r);
        glm::vec3 L = env.sample(d);
        float b[9] = {0.282095f,
                      0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x,
                      1.092548f * d.x * d.y, 1.092548f * d.y * d.z, 0.315392f * (3.f * d.z * d.z - 1.f),
                      1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y)};
        for (int k = 0; k < 9; ++k) c[k] += L * b[k];
    }
    const float PI = 3.14159265f;
    const float dw = 4.f * PI / N;
    const float A[9] = {PI, 2.f * PI / 3.f, 2.f * PI / 3.f, 2.f * PI / 3.f, PI / 4.f, PI / 4.f, PI / 4.f, PI / 4.f, PI / 4.f};
    for (int k = 0; k < 9; ++k) out[k] = glm::vec4(c[k] * dw * A[k] / PI, 0.f);
}

// UBO std140 (binding 0). Manter alinhamento a vec4 e idêntico ao bloco SceneBlock dos shaders.
struct PointLightGPU { glm::vec4 posRadius; glm::vec4 colorIntensity; };
struct SceneUBO {
    glm::mat4 viewProj;
    glm::vec4 camPos;
    glm::vec4 dirDir;                       // xyz direção, w = castShadows
    glm::vec4 dirColor;                     // rgb * intensidade
    glm::mat4 cascadeVP[kNumCascades];
    glm::vec4 cascadeSplits;                // xyz: profundidade onde cada cascata termina, w: distância total
    glm::vec4 cascadeTexel;                 // tamanho de 1 texel da sombra em unidades de mundo, por cascata
    glm::vec4 envParams;                    // x exposição, y nº de mips do cubemap, z intensidade do IBL
    glm::vec4 sh[9];                        // irradiância do céu (SH9)
    glm::vec4 fog;                          // rgb cor da neblina, w densidade
    glm::ivec4 counts;                      // x = nº de point lights
    PointLightGPU points[kMaxPointLights];
};

struct CascadeSet {
    glm::mat4 viewProj[kNumCascades];
    float splitDepth[kNumCascades];
    float texelWorld[kNumCascades];
};

// Cascaded Shadow Maps: divisão "prática" (mistura log/linear) + ajuste ortográfico ao slice
// do frustum com snapping em texels para evitar tremulação ao mover a câmera.
inline CascadeSet computeCascades(const Camera& cam, const glm::vec3& lightDir, float shadowDistance, float lambda = 0.75f) {
    CascadeSet out{};
    float n = cam.nearZ, f = std::min(cam.farZ, shadowDistance);
    float splits[kNumCascades + 1];
    splits[0] = n;
    for (int i = 1; i <= kNumCascades; ++i) {
        float p = float(i) / kNumCascades;
        float lg = n * std::pow(f / n, p), lin = n + (f - n) * p;
        splits[i] = lambda * lg + (1.f - lambda) * lin;
    }

    glm::mat4 invVP = glm::inverse(cam.proj() * cam.view());
    glm::vec3 L = glm::normalize(lightDir);
    glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);

    // Profundidade de view -> fração entre near e far em NDC perspectiva (para interpolar cantos)
    auto ndcT = [&](float z) { return (z - cam.nearZ) / (cam.farZ - cam.nearZ); };
    glm::vec3 nearC[4], farC[4];
    for (int k = 0; k < 4; ++k) {
        float x = (k & 1) ? 1.f : -1.f, y = (k & 2) ? 1.f : -1.f;
        glm::vec4 a = invVP * glm::vec4(x, y, -1.f, 1.f), b = invVP * glm::vec4(x, y, 1.f, 1.f);
        nearC[k] = glm::vec3(a) / a.w;
        farC[k] = glm::vec3(b) / b.w;
    }

    for (int c = 0; c < kNumCascades; ++c) {
        glm::vec3 corners[8];
        for (int k = 0; k < 4; ++k) {
            corners[k] = glm::mix(nearC[k], farC[k], ndcT(splits[c]));
            corners[k + 4] = glm::mix(nearC[k], farC[k], ndcT(splits[c + 1]));
        }
        glm::vec3 center(0.f);
        for (auto& v : corners) center += v;
        center /= 8.f;
        float radius = 0.f;
        for (auto& v : corners) radius = std::max(radius, glm::length(v - center));
        radius = std::ceil(radius * 16.f) / 16.f;  // esfera envolvente: tamanho estável entre frames

        float texel = (2.f * radius) / kShadowMapSize;
        // Recua bastante a câmera de luz para pegar ocluidores fora do slice (ex.: prédios atrás da câmera)
        float back = std::max(radius * 2.f, 30.f);
        glm::mat4 lightView = glm::lookAt(center - L * back, center, up);
        glm::vec3 lc = glm::vec3(lightView * glm::vec4(center, 1.f));
        lc.x = std::floor(lc.x / texel) * texel;  // snapping
        lc.y = std::floor(lc.y / texel) * texel;
        glm::mat4 proj = glm::ortho(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius, 0.f, back + radius * 2.f);
        out.viewProj[c] = proj * lightView;
        out.splitDepth[c] = splits[c + 1];
        out.texelWorld[c] = texel;
    }
    return out;
}

}  // namespace eng
