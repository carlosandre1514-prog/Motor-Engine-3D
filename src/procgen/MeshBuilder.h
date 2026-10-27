#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "render/Renderer.h"

namespace eng {

// Monta malhas a partir de peças simples (caixas, prismas, cilindros) já transformadas.
// Usado por construções, armas, personagens e mapas. Tudo é juntado em poucas malhas
// (uma por material) para gastar poucos draw calls.
struct MeshBuilder {
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    glm::mat4 xf{1.f};           // transformação aplicada às peças
    glm::vec4 color{1.f};        // cor por vértice
    glm::vec2 uvTile{0.f};       // > 0: UV em metros / tile (fachadas, tijolo); 0: UV 0..1 por face
    int joint = -1;              // >= 0: peça presa 100% a este osso (personagem)

    bool empty() const { return idx.empty(); }
    void clear() { v.clear(); idx.clear(); }

    void pushVertex(glm::vec3 p, glm::vec3 n, glm::vec2 uv, glm::vec3 t) {
        Vertex x{};
        glm::mat3 m3(xf);
        x.pos = glm::vec3(xf * glm::vec4(p, 1.f));
        x.normal = glm::normalize(glm::transpose(glm::inverse(m3)) * n);
        x.tangent = glm::vec4(glm::normalize(m3 * t), 1.f);
        x.uv = uv;
        x.color = color;
        if (joint >= 0) { x.joints = glm::u16vec4(uint16_t(joint), 0, 0, 0); x.weights = glm::vec4(1, 0, 0, 0); }
        else x.weights = glm::vec4(1, 0, 0, 0);
        v.push_back(x);
    }

    // Quad p0..p3 em sentido anti-horário visto de fora. uv opcional por canto.
    void quad(glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec3 p3, const glm::vec2* uv = nullptr) {
        glm::vec3 n = glm::normalize(glm::cross(p1 - p0, p3 - p0));
        glm::vec3 t = glm::normalize(p1 - p0);
        uint32_t b = uint32_t(v.size());
        static const glm::vec2 def[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        const glm::vec2* u = uv ? uv : def;
        pushVertex(p0, n, u[0], t); pushVertex(p1, n, u[1], t); pushVertex(p2, n, u[2], t); pushVertex(p3, n, u[3], t);
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(b + i);
    }

    void tri(glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec2 u0, glm::vec2 u1, glm::vec2 u2) {
        glm::vec3 n = glm::normalize(glm::cross(p1 - p0, p2 - p0));
        glm::vec3 t = glm::normalize(p1 - p0);
        uint32_t b = uint32_t(v.size());
        pushVertex(p0, n, u0, t); pushVertex(p1, n, u1, t); pushVertex(p2, n, u2, t);
        for (uint32_t i : {0u, 1u, 2u}) idx.push_back(b + i);
    }

    // Caixa alinhada aos eixos locais: base em `min`, tamanho `size`. faces: bits para pular
    // (1 = -X, 2 = +X, 4 = -Y, 8 = +Y, 16 = -Z, 32 = +Z).
    void box(glm::vec3 mn, glm::vec3 size, unsigned skipFaces = 0) {
        glm::vec3 mx = mn + size;
        glm::vec3 c[8];
        for (int k = 0; k < 8; ++k) c[k] = {k & 1 ? mx.x : mn.x, k & 2 ? mx.y : mn.y, k & 4 ? mx.z : mn.z};
        auto face = [&](int a, int b, int cc, int d, float w, float h, float u0, float v0) {
            glm::vec2 uv[4];
            if (uvTile.x > 0.f) {
                // UV em metros: u ao longo da largura, v para cima (texturas de fachada alinhadas por andar)
                uv[0] = {u0 / uvTile.x, -v0 / uvTile.y};
                uv[1] = {(u0 + w) / uvTile.x, -v0 / uvTile.y};
                uv[2] = {(u0 + w) / uvTile.x, -(v0 + h) / uvTile.y};
                uv[3] = {u0 / uvTile.x, -(v0 + h) / uvTile.y};
            } else {
                uv[0] = {0, 1}; uv[1] = {1, 1}; uv[2] = {1, 0}; uv[3] = {0, 0};
            }
            quad(c[a], c[b], c[cc], c[d], uv);
        };
        float sx = size.x, sy = size.y, sz = size.z;
        if (!(skipFaces & 32)) face(4, 5, 7, 6, sx, sy, mn.x, mn.y);           // +Z
        if (!(skipFaces & 16)) face(1, 0, 2, 3, sx, sy, -mx.x, mn.y);          // -Z
        if (!(skipFaces & 2))  face(5, 1, 3, 7, sz, sy, -mx.z, mn.y);          // +X
        if (!(skipFaces & 1))  face(0, 4, 6, 2, sz, sy, mn.z, mn.y);           // -X
        if (!(skipFaces & 8))  face(6, 7, 3, 2, sx, sz, mn.x, -mx.z);          // +Y
        if (!(skipFaces & 4))  face(0, 1, 5, 4, sx, sz, mn.x, mn.z);           // -Y
    }
    // Caixa pelo centro
    void boxC(glm::vec3 center, glm::vec3 size, unsigned skip = 0) { box(center - size * 0.5f, size, skip); }

    // Telhado de duas águas sobre o retângulo [0,w]x[0,d] na altura y, cumeeira ao longo de X
    void gableRoof(float w, float d, float y, float height, float overhang, glm::vec2 roofTile) {
        glm::vec3 a(-overhang, y, -overhang), b(w + overhang, y, -overhang);
        glm::vec3 c(w + overhang, y, d + overhang), dd(-overhang, y, d + overhang);
        glm::vec3 r0(-overhang, y + height, d * 0.5f), r1(w + overhang, y + height, d * 0.5f);
        float slope = glm::length(glm::vec2(d * 0.5f + overhang, height));
        float L = w + 2 * overhang;
        glm::vec2 uvA[4] = {{0, slope / roofTile.y}, {L / roofTile.x, slope / roofTile.y}, {L / roofTile.x, 0}, {0, 0}};
        quad(b, a, r0, r1, uvA);   // água da frente (-Z)
        quad(dd, c, r1, r0, uvA);  // água de trás (+Z)
    }
    // Oitões (triângulos das pontas) do telhado de duas águas
    void gableEnds(float w, float d, float y, float height) {
        glm::vec2 u0(0, 0), u1(d, 0), u2(d * 0.5f, height);
        if (uvTile.x > 0.f) { u0 /= uvTile; u1 /= uvTile; u2 /= uvTile; u2.y = -u2.y; }
        tri({0, y, 0}, {0, y, d}, {0, y + height, d * 0.5f}, u0, u1, u2);   // normal -X
        tri({w, y, d}, {w, y, 0}, {w, y + height, d * 0.5f}, u0, u1, u2);   // normal +X
    }

    // Cilindro (ou cone) entre p0 e p1 no espaço local
    void cylinder(glm::vec3 p0, glm::vec3 p1, float r0, float r1, int seg = 12, bool caps = true) {
        glm::vec3 axis = p1 - p0;
        float len = glm::length(axis);
        if (len < 1e-6f) return;
        glm::vec3 a = axis / len;
        glm::vec3 t = glm::normalize(glm::cross(a, std::abs(a.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
        glm::vec3 bt = glm::cross(a, t);
        uint32_t base = uint32_t(v.size());
        float slope = (r0 - r1) / len;
        for (int i = 0; i <= seg; ++i) {
            float ang = 6.2831853f * i / seg;
            glm::vec3 dir = t * std::cos(ang) + bt * std::sin(ang);
            glm::vec3 n = glm::normalize(dir + a * slope);
            glm::vec3 tg = glm::cross(a, dir);
            pushVertex(p0 + dir * r0, n, {float(i) / seg, 1.f}, tg);
            pushVertex(p1 + dir * r1, n, {float(i) / seg, 0.f}, tg);
        }
        for (int i = 0; i < seg; ++i) {
            uint32_t k = base + i * 2;
            for (uint32_t j : {k, k + 2, k + 1, k + 1, k + 2, k + 3}) idx.push_back(j);
        }
        if (!caps) return;
        for (int end = 0; end < 2; ++end) {
            float r = end ? r1 : r0;
            if (r <= 0.f) continue;
            glm::vec3 c = end ? p1 : p0, n = end ? a : -a;
            uint32_t cb = uint32_t(v.size());
            pushVertex(c, n, {0.5f, 0.5f}, t);
            for (int i = 0; i <= seg; ++i) {
                float ang = 6.2831853f * i / seg;
                glm::vec3 dir = t * std::cos(ang) + bt * std::sin(ang);
                pushVertex(c + dir * r, n, {0.5f + 0.5f * std::cos(ang), 0.5f + 0.5f * std::sin(ang)}, t);
            }
            for (int i = 0; i < seg; ++i) {
                if (end) { idx.push_back(cb); idx.push_back(cb + 1 + i); idx.push_back(cb + 2 + i); }
                else { idx.push_back(cb); idx.push_back(cb + 2 + i); idx.push_back(cb + 1 + i); }
            }
        }
    }

    // Esfera low-poly (folhagem, cabeças arredondadas)
    void sphere(glm::vec3 c, glm::vec3 radius, int seg = 10, int rings = 6) {
        uint32_t base = uint32_t(v.size());
        for (int r = 0; r <= rings; ++r) {
            float phi = 3.14159265f * r / rings;
            for (int s = 0; s <= seg; ++s) {
                float th = 6.2831853f * s / seg;
                glm::vec3 n(std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th));
                pushVertex(c + n * radius, glm::normalize(n / radius), {float(s) / seg, float(r) / rings}, {-std::sin(th), 0, std::cos(th)});
            }
        }
        for (int r = 0; r < rings; ++r)
            for (int s = 0; s < seg; ++s) {
                uint32_t a = base + r * (seg + 1) + s, b = a + seg + 1;
                for (uint32_t i : {a, a + 1, b, a + 1, b + 1, b}) idx.push_back(i);
            }
    }

    // Junta outro builder (mesmo material)
    void append(const MeshBuilder& o) {
        uint32_t b = uint32_t(v.size());
        v.insert(v.end(), o.v.begin(), o.v.end());
        for (uint32_t i : o.idx) idx.push_back(b + i);
    }

    MeshId build(Renderer& r, bool skinned = false) const {
        if (idx.empty()) return kInvalidId;
        return r.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), skinned);
    }
};

}  // namespace eng
