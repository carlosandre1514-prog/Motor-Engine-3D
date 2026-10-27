#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "render/Renderer.h"

namespace eng {

// Normais suaves por média das faces (para glTF sem NORMAL)
inline void generateNormals(std::vector<Vertex>& v, const std::vector<uint32_t>& idx) {
    for (auto& x : v) x.normal = glm::vec3(0.f);
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        Vertex &a = v[idx[i]], &b = v[idx[i + 1]], &c = v[idx[i + 2]];
        glm::vec3 n = glm::cross(b.pos - a.pos, c.pos - a.pos);  // ponderado pela área
        a.normal += n; b.normal += n; c.normal += n;
    }
    for (auto& x : v) x.normal = glm::length(x.normal) > 1e-12f ? glm::normalize(x.normal) : glm::vec3(0, 1, 0);
}

// Tangentes por triângulo acumuladas por vértice (aproximação de MikkTSpace, suficiente para normal maps comuns)
inline void generateTangents(std::vector<Vertex>& v, const std::vector<uint32_t>& idx) {
    std::vector<glm::vec3> tan(v.size(), glm::vec3(0.f)), bit(v.size(), glm::vec3(0.f));
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        uint32_t i0 = idx[i], i1 = idx[i + 1], i2 = idx[i + 2];
        glm::vec3 e1 = v[i1].pos - v[i0].pos, e2 = v[i2].pos - v[i0].pos;
        glm::vec2 d1 = v[i1].uv - v[i0].uv, d2 = v[i2].uv - v[i0].uv;
        float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) continue;
        float r = 1.f / det;
        glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r, b = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t k : {i0, i1, i2}) { tan[k] += t; bit[k] += b; }
    }
    for (size_t i = 0; i < v.size(); ++i) {
        glm::vec3 n = v[i].normal, t = tan[i] - n * glm::dot(n, tan[i]);
        if (glm::length(t) < 1e-8f) {  // sem UV: qualquer tangente perpendicular
            t = glm::cross(n, std::abs(n.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
        }
        t = glm::normalize(t);
        float w = glm::dot(glm::cross(n, t), bit[i]) < 0.f ? -1.f : 1.f;
        v[i].tangent = glm::vec4(t, w);
    }
}

inline Vertex makeVertex(glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
    Vertex x{};
    x.pos = p; x.normal = n; x.uv = uv;
    x.tangent = glm::vec4(1, 0, 0, 1);
    x.weights = glm::vec4(1, 0, 0, 0);
    return x;
}

// Cubo de lado 1 centrado na origem
inline void buildCube(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    const glm::vec3 N[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const glm::vec3 T[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    const glm::vec2 corner[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int f = 0; f < 6; ++f) {
        glm::vec3 n = N[f], t = T[f], b = glm::cross(n, t);
        uint32_t base = uint32_t(v.size());
        for (int k = 0; k < 4; ++k) {
            Vertex vx = makeVertex((n + t * corner[k].x + b * corner[k].y) * 0.5f, n, uv[k]);
            vx.tangent = glm::vec4(t, 1.f);
            v.push_back(vx);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(base + i);
    }
}

// Esfera UV de diâmetro 1
inline void buildSphere(std::vector<Vertex>& v, std::vector<uint32_t>& idx, int seg = 32, int rings = 16) {
    uint32_t base = uint32_t(v.size());
    for (int r = 0; r <= rings; ++r) {
        float phi = 3.14159265f * r / rings;
        for (int s = 0; s <= seg; ++s) {
            float th = 6.2831853f * s / seg;
            glm::vec3 n(std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th));
            v.push_back(makeVertex(n * 0.5f, n, {float(s) / seg, float(r) / rings}));
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < seg; ++s) {
            uint32_t a = base + r * (seg + 1) + s, b = a + seg + 1;
            for (uint32_t i : {a, a + 1, b, a + 1, b + 1, b}) idx.push_back(i);
        }
    generateTangents(v, idx);
}

// Cápsula em Y: diâmetro 1, altura total `height` (>= 1)
inline void buildCapsule(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float height = 2.f, int seg = 24, int capRings = 8) {
    uint32_t base = uint32_t(v.size());
    float half = std::max(height - 1.f, 0.f) * 0.5f;
    int rows = 0;
    auto ring = [&](float phi, float yOff) {
        for (int s = 0; s <= seg; ++s) {
            float th = 6.2831853f * s / seg;
            glm::vec3 n(std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th));
            glm::vec3 p = n * 0.5f + glm::vec3(0, yOff, 0);
            v.push_back(makeVertex(p, n, {float(s) / seg, 0.5f - p.y / height}));
        }
        ++rows;
    };
    for (int r = 0; r <= capRings; ++r) ring(1.5707963f * r / capRings, half);                  // topo
    for (int r = 0; r <= capRings; ++r) ring(1.5707963f + 1.5707963f * r / capRings, -half);  // base
    for (int r = 0; r < rows - 1; ++r)
        for (int s = 0; s < seg; ++s) {
            uint32_t a = base + r * (seg + 1) + s, b = a + seg + 1;
            for (uint32_t i : {a, a + 1, b, a + 1, b + 1, b}) idx.push_back(i);
        }
    generateTangents(v, idx);
}

// "Pedra": icosaedro irregular de faces planas (demo de collider ConvexMesh). Devolve os cantos em `hull`.
inline void buildRock(std::vector<Vertex>& v, std::vector<uint32_t>& idx, std::vector<glm::vec3>& hull) {
    const float t = 1.618034f;
    glm::vec3 p[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                       {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    const float jitter[12] = {1.0f, 0.85f, 0.95f, 1.1f, 0.9f, 1.05f, 0.8f, 1.0f, 0.92f, 1.08f, 0.88f, 1.0f};
    for (int i = 0; i < 12; ++i) p[i] = glm::normalize(p[i]) * 0.5f * jitter[i];
    const int F[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                          {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (auto& f : F) {
        glm::vec3 a = p[f[0]], b = p[f[1]], c = p[f[2]];
        glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        uint32_t base = uint32_t(v.size());
        v.push_back(makeVertex(a, n, {0, 0})); v.push_back(makeVertex(b, n, {1, 0})); v.push_back(makeVertex(c, n, {0.5f, 1}));
        for (uint32_t k = 0; k < 3; ++k) idx.push_back(base + k);
    }
    generateTangents(v, idx);
    hull.assign(p, p + 12);
}

}  // namespace eng
