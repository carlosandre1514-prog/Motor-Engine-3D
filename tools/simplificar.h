// Redução de malha por colapso de arestas (erro quádrico, Garland-Heckbert), usada pelos conversores de personagem.
// V precisa ter `float p[3]`; vértices iguais (byte a byte) são juntados antes. keepVertex(v) == true trava o vértice.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

// ---- redução da malha: colapso de arestas por erro quádrico (Garland-Heckbert). Vértices de borda (costuras de
// UV, buracos) ficam presos, então não abre rachadura entre ilhas; o vértice que fica é um dos dois da aresta
// (mantém UV e pesos sem interpolar).
struct Quadric {
    double a[10] = {};
    void addPlane(double x, double y, double z, double d, double w) {
        double v[4] = {x, y, z, d};
        int k = 0;
        for (int i = 0; i < 4; ++i) for (int j = i; j < 4; ++j) a[k++] += w * v[i] * v[j];
    }
    void add(const Quadric& o) { for (int i = 0; i < 10; ++i) a[i] += o.a[i]; }
    double eval(const float* p) const {
        double x = p[0], y = p[1], z = p[2];
        return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z + 2 * a[6] * y +
               a[7] * z * z + 2 * a[8] * z + a[9];
    }
};
template <class V, class Keep>
static void simplify(std::vector<V>& verts, std::vector<uint32_t>& idx, size_t targetTris, Keep keepVertex) {
    // junta vértices idênticos (mesma posição, normal, UV e pesos)
    {
        std::map<std::vector<unsigned char>, uint32_t> seen;
        std::vector<V> nv;
        std::vector<uint32_t> remap(verts.size());
        for (size_t i = 0; i < verts.size(); ++i) {
            const unsigned char* b = reinterpret_cast<const unsigned char*>(&verts[i]);
            std::vector<unsigned char> key(b, b + sizeof(V));
            auto it = seen.find(key);
            if (it == seen.end()) { remap[i] = uint32_t(nv.size()); seen.emplace(std::move(key), remap[i]); nv.push_back(verts[i]); }
            else remap[i] = it->second;
        }
        for (auto& i : idx) i = remap[i];
        verts.swap(nv);
    }
    const size_t nv = verts.size(), nt = idx.size() / 3;
    std::vector<char> triDead(nt, 0);
    std::vector<std::vector<uint32_t>> vtris(nv);
    for (size_t t = 0; t < nt; ++t) for (int c = 0; c < 3; ++c) vtris[idx[t * 3 + c]].push_back(uint32_t(t));
    // borda: aresta usada por um triângulo só
    std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
    for (size_t t = 0; t < nt; ++t)
        for (int c = 0; c < 3; ++c) {
            uint32_t a = idx[t * 3 + c], b = idx[t * 3 + (c + 1) % 3];
            ++edgeCount[{std::min(a, b), std::max(a, b)}];
        }
    std::vector<char> locked(nv, 0);
    for (auto& [e, n] : edgeCount) if (n == 1) locked[e.first] = locked[e.second] = 1;
    for (size_t i = 0; i < nv; ++i) if (keepVertex(verts[i])) locked[i] = 1;  // rosto: fica inteiro
    std::vector<Quadric> Q(nv);
    auto triNormal = [&](size_t t, glm::vec3& n, float& area) {
        glm::vec3 p0 = glm::make_vec3(verts[idx[t * 3]].p), p1 = glm::make_vec3(verts[idx[t * 3 + 1]].p), p2 = glm::make_vec3(verts[idx[t * 3 + 2]].p);
        glm::vec3 c = glm::cross(p1 - p0, p2 - p0);
        area = glm::length(c);
        n = area > 1e-12f ? c / area : glm::vec3(0.f);
    };
    for (size_t t = 0; t < nt; ++t) {
        glm::vec3 n; float ar;
        triNormal(t, n, ar);
        double d = -glm::dot(n, glm::make_vec3(verts[idx[t * 3]].p));
        for (int c = 0; c < 3; ++c) Q[idx[t * 3 + c]].addPlane(n.x, n.y, n.z, d, ar);
    }
    std::vector<uint32_t> parent(nv);
    for (size_t i = 0; i < nv; ++i) parent[i] = uint32_t(i);
    std::function<uint32_t(uint32_t)> rootOf = [&](uint32_t v) { while (parent[v] != v) v = parent[v] = parent[parent[v]]; return v; };
    using Cand = std::tuple<double, uint32_t, uint32_t>;  // erro, vai (some), fica
    std::priority_queue<Cand, std::vector<Cand>, std::greater<Cand>> pq;
    std::vector<uint32_t> version(nv, 0);
    auto pushEdge = [&](uint32_t a, uint32_t b) {
        if (a == b) return;
        if (locked[a] && locked[b]) return;
        // some o vértice livre; fica o outro (se os dois livres, o de menor erro)
        Quadric q = Q[a]; q.add(Q[b]);
        if (locked[a]) std::swap(a, b);
        if (!locked[b]) { double ea = q.eval(verts[b].p), eb = q.eval(verts[a].p); if (eb < ea) std::swap(a, b); }
        pq.push({q.eval(verts[b].p), a, b});
    };
    for (auto& [e, n] : edgeCount) pushEdge(e.first, e.second);
    size_t alive = nt;
    while (alive > targetTris && !pq.empty()) {
        auto [err, a, b] = pq.top(); pq.pop();
        a = rootOf(a); b = rootOf(b);
        if (a == b || (locked[a])) continue;
        // não colapsa se algum triângulo virar do avesso
        bool flip = false;
        for (uint32_t t : vtris[a]) {
            if (triDead[t]) continue;
            uint32_t v[3];
            bool hasB = false;
            for (int c = 0; c < 3; ++c) { v[c] = rootOf(idx[t * 3 + c]); hasB |= v[c] == b; }
            if (hasB) continue;
            glm::vec3 p[3], q[3];
            for (int c = 0; c < 3; ++c) { p[c] = glm::make_vec3(verts[v[c]].p); q[c] = v[c] == a ? glm::make_vec3(verts[b].p) : p[c]; }
            glm::vec3 n0 = glm::cross(p[1] - p[0], p[2] - p[0]), n1 = glm::cross(q[1] - q[0], q[2] - q[0]);
            float l0 = glm::length(n0), l1 = glm::length(n1);
            // não deixa o triângulo girar mais de ~40° nem ficar degenerado (dobras no peito/ombro)
            if (l1 < l0 * 0.05f || glm::dot(n0, n1) < 0.75f * l0 * l1) { flip = true; break; }
        }
        if (flip) continue;
        // colapsa a -> b
        parent[a] = b;
        Q[b].add(Q[a]);
        for (uint32_t t : vtris[a]) {
            if (triDead[t]) continue;
            uint32_t v[3];
            for (int c = 0; c < 3; ++c) v[c] = rootOf(idx[t * 3 + c]);
            if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) { triDead[t] = 1; --alive; continue; }
            vtris[b].push_back(t);
        }
        vtris[a].clear();
        // novas arestas do vértice que ficou
        std::set<uint32_t> nb;
        for (uint32_t t : vtris[b]) {
            if (triDead[t]) continue;
            for (int c = 0; c < 3; ++c) { uint32_t v = rootOf(idx[t * 3 + c]); if (v != b) nb.insert(v); }
        }
        for (uint32_t v : nb) pushEdge(v, b);
        (void)err;
    }
    // reconstrói índices e vértices usados
    std::vector<uint32_t> outIdx;
    for (size_t t = 0; t < nt; ++t) {
        if (triDead[t]) continue;
        uint32_t v[3];
        for (int c = 0; c < 3; ++c) v[c] = rootOf(idx[t * 3 + c]);
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;
        outIdx.insert(outIdx.end(), v, v + 3);
    }
    std::vector<int64_t> newOf(nv, -1);
    std::vector<V> outV;
    for (auto& i : outIdx) {
        if (newOf[i] < 0) { newOf[i] = int64_t(outV.size()); outV.push_back(verts[i]); }
        i = uint32_t(newOf[i]);
    }
    verts.swap(outV);
    idx.swap(outIdx);
}

