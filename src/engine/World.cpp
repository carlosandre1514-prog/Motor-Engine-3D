#include <BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include "assets/MeshUtils.h"
#include "engine/Engine.h"
#include "procgen/Noise.h"

namespace eng {

namespace {

constexpr float kChunk = 96.f;   // tamanho do chunk do mundo infinito (m)
constexpr int kRes = 40;          // quads por lado do chunk (2,4 m por quad)
constexpr int kRadius = 2;        // chunks carregados em volta do foco (5 x 5)

using HeightFn = std::function<float(float, float)>;

float smooth01(float a, float b, float x) { float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }

// Mundo infinito: continentes, colinas e cordilheiras; nível do mar em y = 0
float infiniteHeight(float x, float z, uint32_t seed) {
    float continent = fbm(x * 0.0016f + 13.7f, z * 0.0016f - 4.1f, 4, seed);
    float hills = fbm(x * 0.012f, z * 0.012f, 4, seed + 11);
    float mount = ridged(x * 0.0045f, z * 0.0045f, 5, seed + 23);
    float land = (continent - 0.42f) * 70.f;
    float m = smooth01(0.5f, 0.72f, continent) * mount * 85.f;
    float h = land + hills * 9.f + m;
    // área em volta da origem sempre em terra firme (ponto de partida)
    float d = std::sqrt(x * x + z * z);
    float spawn = 1.f - smooth01(60.f, 160.f, d);
    return glm::mix(h, std::max(h, 4.f + hills * 3.f), spawn);
}

// Favela: morro central com encostas irregulares, base plana em volta
float favelaHeight(float x, float z, uint32_t seed) {
    float r = std::sqrt(x * x + z * z);
    float n = fbm(x * 0.02f, z * 0.02f, 4, seed) * 6.f;
    float hill = 46.f * std::exp(-std::pow(r / 88.f, 2.f)) + n * (1.f - smooth01(95.f, 118.f, r));
    float flat = 2.5f;
    return glm::mix(flat + hill, flat, smooth01(108.f, 122.f, r));
}

void terrainMesh(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float x0, float z0, float size, int res, const HeightFn& H,
                 std::vector<float>* heights) {
    v.clear(); idx.clear();
    const float cell = size / res;
    v.reserve((res + 1) * (res + 1));
    if (heights) heights->resize((res + 1) * (res + 1));
    for (int j = 0; j <= res; ++j)
        for (int i = 0; i <= res; ++i) {
            float x = x0 + i * cell, z = z0 + j * cell;
            float h = H(x, z);
            if (heights) (*heights)[j * (res + 1) + i] = h;
            float hx = H(x + cell, z) - H(x - cell, z), hz = H(x, z + cell) - H(x, z - cell);
            glm::vec3 n = glm::normalize(glm::vec3(-hx, 2.f * cell, -hz));
            Vertex vx = makeVertex({x, h, z}, n, {x / 5.f, z / 5.f});
            vx.tangent = glm::vec4(glm::normalize(glm::cross(glm::vec3(0, 0, 1), n)), 1.f);
            v.push_back(vx);
        }
    for (int j = 0; j < res; ++j)
        for (int i = 0; i < res; ++i) {
            uint32_t a = j * (res + 1) + i;
            for (uint32_t k : {a, a + res + 1, a + 1, a + 1, a + res + 1, a + res + 2}) idx.push_back(k);
        }
}

// Heightfield do Bullet com a mesma triangulação da malha. Os dados precisam continuar vivos.
void addHeightfield(Engine* e, TerrainChunk& c, float x0, float z0, float size, int res) {
    float mn = 1e9f, mx = -1e9f;
    for (float h : c.heights) { mn = std::min(mn, h); mx = std::max(mx, h); }
    auto* hf = new btHeightfieldTerrainShape(res + 1, res + 1, c.heights.data(), 1.f, mn, mx, 1, PHY_FLOAT, false);
    float cell = size / res;
    hf->setLocalScaling({cell, 1.f, cell});
    c.hfShape = hf;
    c.hfBody = e->physics.addStatic(hf, {x0 + size * 0.5f, (mn + mx) * 0.5f, z0 + size * 0.5f});
}

Prototype protoFromGeo(Engine* e, Geo& g) {
    Prototype p;
    for (int k = 0; k < int(Mat::Count); ++k) {
        MeshId id = g.mats[k].build(e->renderer);
        if (id != kInvalidId) p.parts.push_back({id, e->mats[k]});
        MeshId d = g.detailMats[k].build(e->renderer);
        if (d != kInvalidId) p.parts.push_back({d, e->mats[k], 0.f, kDetailFar * 0.6f});
    }
    if (!g.colliders.empty()) { p.colliderHalf = g.colliders[0].half; p.colliderY = g.colliders[0].center.y; }
    return p;
}

void spawnProto(Engine* e, const Prototype& p, glm::vec3 pos, float yaw, float scale, std::vector<Scene::Handle>* out,
                std::vector<BoxCollider>* colliders) {
    for (const ProtoPart& pp : p.parts) {
        Scene::Handle h = addMeshObject(e, "", pp.mesh, pp.mat, pos, glm::vec3(scale), {0.f, glm::degrees(yaw), 0.f}, false);
        if (GameObject* g = e->scene.get(h); g && g->meshRenderer) { g->meshRenderer->lodNear = pp.lodNear; g->meshRenderer->lodFar = pp.lodFar; }
        if (out) out->push_back(h);
    }
    if (colliders && p.colliderHalf.x > 0.f)
        colliders->push_back({pos + glm::vec3(0, p.colliderY * scale, 0), p.colliderHalf * scale, yaw});
}

void makeProtos(Engine* e, uint32_t seed) {
    Rng r(seed + 999);
    for (int k = 0; k < 3; ++k) {
        // LOD: versão completa perto, simplificada (mesma forma, menos polígonos) longe, some na neblina
        Rng r2 = r;
        Geo g;
        g.place(glm::vec3(0.f), 0.f);
        buildTree(g[Mat::Painted], r, k);
        g.collider({-0.25f, 0.f, -0.25f}, {0.5f, 4.f, 0.5f});
        Prototype p = protoFromGeo(e, g);
        p.parts[0].lodFar = kTreeLod;
        MeshBuilder low;
        buildTree(low, r2, k, true);
        p.parts.push_back({low.build(e->renderer), e->mats[int(Mat::Painted)], kTreeLod, kTreeFar});
        e->treeProto[k] = p;
    }
    e->rockProto = Prototype{};
    e->rockProto.parts.push_back({e->rock, e->mats[int(Mat::Concrete)], 0.f, 260.f});
    e->rockProto.colliderHalf = glm::vec3(0.45f);
    e->rockProto.colliderY = 0.f;
    for (int k = 0; k < 3; ++k) {
        Geo g;
        g.place(glm::vec3(0.f), 0.f);
        buildHouse(g, r, r.range(7.f, 10.f), r.range(8.f, 10.f), k == 2 ? 2 : 1, housePaint(r), false);
        e->houseProto[k] = protoFromGeo(e, g);
    }
}

// Agrupa construções de um mapa por célula (cada célula vira poucos meshes: 1 por material)
// LOD: a camada de detalhe fica em células pequenas (some longe da câmera); a base é juntada em grupos
// de `group` x `group` células (poucos draw calls, sempre visível).
struct Cells {
    float size;
    int group;
    std::map<int64_t, Geo> m;
    Cells(float s, int grp = 1) : size(s), group(grp) {}
    static int64_t key(int64_t cx, int64_t cz) { return (cx << 32) ^ (cz & 0xffffffff); }
    Geo& at(glm::vec3 p) { return m[key(int64_t(std::floor(p.x / size)), int64_t(std::floor(p.z / size)))]; }
    void flush(Engine* e) {
        std::map<int64_t, Geo> big;
        auto fdiv = [](int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
        for (auto& [k, g] : m) {
            int64_t cx = k >> 32, cz = int64_t(int32_t(uint32_t(k)));
            Geo& b = big[key(fdiv(cx, group), fdiv(cz, group))];
            for (int i = 0; i < int(Mat::Count); ++i) {
                b.mats[i].append(g.mats[i]);
                g.mats[i].clear();
            }
            b.colliders.insert(b.colliders.end(), g.colliders.begin(), g.colliders.end());
            g.colliders.clear();
            bool anyDetail = false;
            for (auto& d : g.detailMats) anyDetail |= !d.empty();
            if (anyDetail) spawnGeo(e, g, "Detalhes", false, glm::vec3(0.f), 0.f, kDetailFar);
        }
        for (auto& [k, g] : big) if (!g.empty() || !g.colliders.empty()) spawnGeo(e, g, "Mapa", false);
        m.clear();
    }
};

void setBounds(Engine* e, glm::vec2 mn, glm::vec2 mx) {
    e->hasBounds = true;
    e->boundsMin = mn; e->boundsMax = mx;
    // barreira invisível: 4 paredes estáticas sem malha
    std::vector<BoxCollider> walls;
    glm::vec2 c = (mn + mx) * 0.5f, h = (mx - mn) * 0.5f;
    const float H = 80.f, T = 1.f;
    walls.push_back({{c.x, H, mn.y - T}, {h.x + 2 * T, H + 20.f, T}, 0.f});
    walls.push_back({{c.x, H, mx.y + T}, {h.x + 2 * T, H + 20.f, T}, 0.f});
    walls.push_back({{mn.x - T, H, c.y}, {T, H + 20.f, h.y + 2 * T}, 0.f});
    walls.push_back({{mx.x + T, H, c.y}, {T, H + 20.f, h.y + 2 * T}, 0.f});
    addStaticBoxes(e, walls);
}

// Faixa de rua de (a) até (b) no plano y, largura w (UV: u atravessa, v ao longo / 6 m)
void roadStrip(MeshBuilder& b, glm::vec2 a, glm::vec2 c, float w, float y) {
    glm::vec2 d = c - a;
    float L = glm::length(d);
    if (L < 0.01f) return;
    glm::vec2 dir = d / L, side(-dir.y, dir.x);
    glm::vec3 p0(a.x - side.x * w * 0.5f, y, a.y - side.y * w * 0.5f), p1(c.x - side.x * w * 0.5f, y, c.y - side.y * w * 0.5f);
    glm::vec3 p2(c.x + side.x * w * 0.5f, y, c.y + side.y * w * 0.5f), p3(a.x + side.x * w * 0.5f, y, a.y + side.y * w * 0.5f);
    glm::vec2 uv[4] = {{0, 0}, {0, L / 6.f}, {1, L / 6.f}, {1, 0}};
    // garante normal para cima
    glm::vec3 n = glm::cross(p1 - p0, p3 - p0);
    if (n.y < 0.f) { std::swap(p1, p3); std::swap(uv[1], uv[3]); }
    b.quad(p0, p1, p2, p3, uv);
}

glm::vec3 findLand(Engine* e) {
    for (float rad = 0.f; rad < 400.f; rad += 6.f)
        for (int k = 0; k < 16; ++k) {
            float a = k * 0.3926991f;
            float x = std::cos(a) * rad, z = std::sin(a) * rad;
            float h = mapHeight(e, x, z);
            float s = std::abs(mapHeight(e, x + 2, z) - mapHeight(e, x - 2, z)) + std::abs(mapHeight(e, x, z + 2) - mapHeight(e, x, z - 2));
            if (h > 3.f && h < 30.f && s < 1.2f) return {x, h, z};
        }
    return {0.f, mapHeight(e, 0, 0), 0.f};
}

// ---------------- mundo infinito ----------------
void releaseChunk(Engine* e, TerrainChunk& c) {
    for (auto h : c.objects) if (GameObject* g = e->scene.get(h)) { releaseComponents(e, *g); e->scene.destroy(h); }
    c.objects.clear();
    if (GameObject* g = e->scene.get(c.ground)) { releaseComponents(e, *g); e->scene.destroy(c.ground); }
    c.ground = {};
    if (c.hfBody) e->physics.removeStatic(c.hfBody);
    delete c.hfShape;
    c.hfBody = nullptr; c.hfShape = nullptr;
    freeStatic(e, c.props);
    c.used = false;
}

void generateChunk(Engine* e, TerrainChunk& c, int cx, int cz) {
    c.cx = cx; c.cz = cz; c.used = true;
    const float x0 = cx * kChunk, z0 = cz * kChunk;
    const uint32_t seed = e->mapSeed;
    HeightFn H = [seed](float x, float z) { return infiniteHeight(x, z, seed); };
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    terrainMesh(v, idx, x0, z0, kChunk, kRes, H, &c.heights);
    if (c.mesh == kInvalidId) c.mesh = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);
    else e->renderer.updateMesh(c.mesh, v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()));
    c.ground = addMeshObject(e, "Terreno", c.mesh, e->mats[int(Mat::Terrain)], glm::vec3(0.f), glm::vec3(1.f), glm::vec3(0.f), false);
    if (GameObject* g = e->scene.get(c.ground)) if (g->meshRenderer) g->meshRenderer->castShadows = false;
    addHeightfield(e, c, x0, z0, kChunk, kRes);

    // Vegetação, pedras e vilas: determinísticos por chunk
    Rng r(hash2(cx, cz, seed + 5));
    std::vector<BoxCollider> boxes;
    auto slopeAt = [&](float x, float z) { return std::abs(H(x + 1, z) - H(x - 1, z)) + std::abs(H(x, z + 1) - H(x, z - 1)); };
    for (int k = 0; k < 26; ++k) {
        float x = x0 + r.f() * kChunk, z = z0 + r.f() * kChunk, h = H(x, z);
        if (h < 1.2f || h > 56.f || slopeAt(x, z) > 1.1f) continue;
        int kind = h > 30.f ? 1 : (h < 5.f && r.chance(0.6f)) ? 2 : (r.chance(0.3f) ? 1 : 0);
        spawnProto(e, e->treeProto[kind], {x, h - 0.15f, z}, r.range(0.f, 6.28f), r.range(0.8f, 1.35f), &c.objects, &boxes);
    }
    for (int k = 0; k < 5; ++k) {
        float x = x0 + r.f() * kChunk, z = z0 + r.f() * kChunk, h = H(x, z);
        if (h < 0.3f) continue;
        spawnProto(e, e->rockProto, {x, h, z}, r.range(0.f, 6.28f), r.range(0.8f, 2.6f), &c.objects, &boxes);
    }
    if (hash2(cx, cz, seed + 77) % 4 == 0) {  // vila
        int n = r.irange(3, 7);
        float vx = x0 + kChunk * 0.5f, vz = z0 + kChunk * 0.5f;
        for (int k = 0; k < n; ++k) {
            float x = vx + r.range(-30.f, 30.f), z = vz + r.range(-30.f, 30.f);
            float h = std::min({H(x - 5, z - 5), H(x + 5, z - 5), H(x - 5, z + 5), H(x + 5, z + 5)});
            if (h < 2.f || h > 32.f || slopeAt(x, z) > 0.8f) continue;
            float yaw = r.irange(0, 3) * 1.5707963f + r.range(-0.2f, 0.2f);
            spawnProto(e, e->houseProto[r.irange(0, 2)], {x, h, z}, yaw, 1.f, &c.objects, &boxes);
        }
    }
    addStaticBoxes(e, boxes, &c.props);
}

void updateInfinite(Engine* e, int budget) {
    glm::vec3 f = e->editorCam.target;
    int ccx = int(std::floor(f.x / kChunk)), ccz = int(std::floor(f.z / kChunk));
    for (auto& c : e->chunks)
        if (c.used && (std::abs(c.cx - ccx) > kRadius + 1 || std::abs(c.cz - ccz) > kRadius + 1)) releaseChunk(e, c);
    while (budget-- > 0) {
        int bx = 0, bz = 0, best = 1 << 30;
        for (int dz = -kRadius; dz <= kRadius; ++dz)
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                int cx = ccx + dx, cz = ccz + dz;
                bool have = false;
                for (auto& c : e->chunks) if (c.used && c.cx == cx && c.cz == cz) { have = true; break; }
                if (!have && dx * dx + dz * dz < best) { best = dx * dx + dz * dz; bx = cx; bz = cz; }
            }
        if (best == (1 << 30)) break;
        TerrainChunk* slot = nullptr;
        for (auto& c : e->chunks) if (!c.used) { slot = &c; break; }
        if (!slot) { e->chunks.emplace_back(); slot = &e->chunks.back(); }
        generateChunk(e, *slot, bx, bz);
    }
    // a água acompanha o foco (plano grande em y = 0)
    if (GameObject* w = e->scene.get(e->water))
        w->transform.setPosition({std::floor(f.x / 50.f) * 50.f, 0.f, std::floor(f.z / 50.f) * 50.f});
}

// ---------------- mapas com limite ----------------
void groundBox(Geo& g, Mat m, glm::vec2 mn, glm::vec2 mx, float top, float tile) {
    MeshBuilder& b = g[m];
    b.uvTile = {tile, tile};
    b.box({mn.x, top - 0.3f, mn.y}, {mx.x - mn.x, 0.3f, mx.y - mn.y}, 4);
    g.collider({mn.x, top - 0.3f, mn.y}, {mx.x - mn.x, 0.3f, mx.y - mn.y});
}

void lotHouse(Engine* e, Cells& cells, Rng& r, glm::vec3 pos, float yaw, float lotW, float lotD) {
    Geo& g = cells.at(pos);
    g.place(pos, yaw);
    int floors = r.chance(0.3f) ? 2 : 1;
    buildHouse(g, r, std::min(lotW - 3.f, r.range(7.f, 10.f)), std::min(lotD - 7.f, r.range(8.f, 11.f)), floors, housePaint(r), r.chance(0.7f));
    (void)e;
}

void genCity(Engine* e, Rng& r, glm::vec3& spawn, float& spawnYaw) {
    const int nb = 6;
    const float block = 56.f, road = 14.f, pitch = block + road;
    const float total = nb * block + (nb + 1) * road, half = total * 0.5f;
    Cells cells(pitch * 2.f, 2);  // detalhe em blocos de 2 x 2 quarteirões, base em grupos de 4 x 4
    // asfalto cobrindo tudo + faixas das ruas
    {
        Geo& g = cells.at({-1e4f, 0, -1e4f});
        g.place(glm::vec3(0.f), 0.f);
        groundBox(g, Mat::Asphalt, {-half - 20, -half - 20}, {half + 20, half + 20}, 0.f, 4.f);
        for (int i = 0; i <= nb; ++i) {
            float c = -half + i * pitch + road * 0.5f;
            for (int j = 0; j < nb; ++j) {
                float a0 = -half + road + j * pitch, a1 = a0 + block;
                roadStrip(g[Mat::Road], {a0, c}, {a1, c}, road, 0.02f);
                roadStrip(g[Mat::Road], {c, a0}, {c, a1}, road, 0.02f);
            }
        }
    }
    std::vector<BoxCollider> treeBoxes;
    for (int i = 0; i < nb; ++i)
        for (int j = 0; j < nb; ++j) {
            float bx0 = -half + road + i * pitch, bz0 = -half + road + j * pitch;
            glm::vec3 c(bx0 + block * 0.5f, 0, bz0 + block * 0.5f);
            Geo& g = cells.at(c);
            g.place(glm::vec3(0.f), 0.f);
            MeshBuilder& sw = g[Mat::Sidewalk];
            sw.uvTile = {2.f, 2.f};
            sw.box({bx0, 0, bz0}, {block, 0.15f, block}, 4);
            g.collider({bx0, 0, bz0}, {block, 0.15f, block});
            float dist = std::max(std::abs(c.x), std::abs(c.z)) / half;
            bool plaza = dist > 0.3f && r.chance(0.1f);
            const float m = 4.f;  // calçada
            if (plaza) {
                MeshBuilder& gr = g[Mat::Grass];
                gr.uvTile = {4.f, 4.f};
                gr.box({bx0 + m, 0.15f, bz0 + m}, {block - 2 * m, 0.05f, block - 2 * m}, 4);
                for (int k = 0; k < 10; ++k)
                    spawnProto(e, e->treeProto[r.chance(0.2f) ? 2 : 0], {bx0 + m + r.f() * (block - 2 * m), 0.2f, bz0 + m + r.f() * (block - 2 * m)},
                               r.range(0, 6.28f), r.range(0.9f, 1.3f), nullptr, &treeBoxes);
            } else if (dist < 0.3f) {
                int n = r.irange(1, 2);
                for (int k = 0; k < n; ++k) {
                    float w = n == 1 ? r.range(26.f, 36.f) : r.range(18.f, 22.f), d = r.range(22.f, 34.f);
                    glm::vec3 p = n == 1 ? c : glm::vec3(bx0 + block * (k ? 0.74f : 0.26f), 0, c.z);
                    g.place(p + glm::vec3(0, 0.15f, 0), 0.f);
                    buildSkyscraper(g, r, w, d, r.irange(18, 50));
                }
            } else if (dist < 0.68f) {
                for (int k = 0; k < 4; ++k) {
                    glm::vec3 p(bx0 + block * ((k & 1) ? 0.73f : 0.27f), 0.15f, bz0 + block * ((k & 2) ? 0.73f : 0.27f));
                    if (r.chance(0.12f)) continue;
                    g.place(p, (k & 2) ? 0.f : 3.14159265f);
                    buildApartment(g, r, r.range(15.f, 21.f), r.range(13.f, 19.f), r.irange(3, 12), buildingPaint(r));
                }
            } else {
                // casas: 3 lotes na frente (+Z) e 3 atrás (-Z), de frente para a rua
                float lotW = (block - 2 * m) / 3.f, lotD = (block - 2 * m) / 2.f;
                for (int k = 0; k < 6; ++k) {
                    int col = k % 3; bool front = k >= 3;
                    glm::vec3 p(bx0 + m + lotW * (col + 0.5f), 0.15f, bz0 + m + lotD * (front ? 1.5f : 0.5f) + (front ? -2.5f : 2.5f));
                    lotHouse(e, cells, r, p, front ? 0.f : 3.14159265f, lotW, lotD);
                }
            }
            // postes, árvores e carros estacionados em volta do quarteirão
            g.place(glm::vec3(0.f), 0.f);
            for (int side = 0; side < 4; ++side)
                for (float t = 6.f; t < block - 4.f; t += 18.f) {
                    glm::vec3 p; float yaw;
                    if (side == 0) { p = {bx0 + t, 0.15f, bz0 + 1.f}; yaw = 3.14159265f; }
                    else if (side == 1) { p = {bx0 + t, 0.15f, bz0 + block - 1.f}; yaw = 0.f; }
                    else if (side == 2) { p = {bx0 + 1.f, 0.15f, bz0 + t}; yaw = -1.5707963f; }
                    else { p = {bx0 + block - 1.f, 0.15f, bz0 + t}; yaw = 1.5707963f; }
                    Geo& gl = cells.at(p);
                    gl.place(p, yaw);
                    buildStreetLight(gl);
                    glm::vec3 tp = p + glm::vec3(std::sin(yaw), 0, std::cos(yaw)) * -0.f + glm::vec3(side < 2 ? 9.f : 0.f, 0, side >= 2 ? 9.f : 0.f);
                    if (dist > 0.3f) spawnProto(e, e->treeProto[0], tp, r.range(0, 6.28f), r.range(0.7f, 0.9f), nullptr, &treeBoxes);
                    if (r.chance(0.45f)) {
                        glm::vec3 cp = p + glm::vec3(std::sin(yaw), 0, std::cos(yaw)) * 3.f + glm::vec3(side < 2 ? 4.f : 0.f, -0.15f, side >= 2 ? 4.f : 0.f);
                        Geo& gc = cells.at(cp);
                        gc.place(cp, yaw + 1.5707963f);
                        buildCar(gc, r);
                    }
                }
        }
    cells.flush(e);
    addStaticBoxes(e, treeBoxes);
    setBounds(e, {-half - 15, -half - 15}, {half + 15, half + 15});
    spawn = {-half + 3 * pitch + road * 0.5f, 0.f, -half + 2 * pitch + road + 10.f};
    spawnYaw = 0.f;
    e->renderer.fogDensity = 0.0016f;
}

void genFavela(Engine* e, Rng& r, glm::vec3& spawn, float& spawnYaw) {
    const float half = 150.f;
    const uint32_t seed = e->mapSeed;
    HeightFn H = [seed](float x, float z) { return favelaHeight(x, z, seed); };
    // chão do morro (malha + heightfield)
    {
        e->chunks.emplace_back();
        TerrainChunk& c = e->chunks.back();
        c.used = true;
        std::vector<Vertex> v; std::vector<uint32_t> idx;
        const int res = 120;
        terrainMesh(v, idx, -half, -half, 2 * half, res, H, &c.heights);
        c.mesh = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);
        c.ground = addMeshObject(e, "Morro", c.mesh, e->mats[int(Mat::Terrain)], glm::vec3(0.f), glm::vec3(1.f), glm::vec3(0.f), false);
        addHeightfield(e, c, -half, -half, 2 * half, res);
        c.cx = 1 << 20;  // fora da grade do mundo infinito
    }
    Cells cells(35.f, 3);
    // rua em volta do morro
    {
        Geo& g = cells.at({-1e4f, 0, -1e4f});
        g.place(glm::vec3(0.f), 0.f);
        const int seg = 48;
        const float rr = 128.f;
        for (int k = 0; k < seg; ++k) {
            float a0 = 6.2831853f * k / seg, a1 = 6.2831853f * (k + 1) / seg;
            roadStrip(g[Mat::Road], {std::cos(a0) * rr, std::sin(a0) * rr}, {std::cos(a1) * rr, std::sin(a1) * rr}, 11.f, 2.62f);
        }
    }
    // barracos
    const float pitch = 6.3f;
    int count = 0;
    for (float z = -104.f; z <= 104.f; z += pitch)
        for (float x = -104.f; x <= 104.f; x += pitch) {
            int ix = int(std::round(x / pitch)), iz = int(std::round(z / pitch));
            float px = x + r.range(-0.7f, 0.7f), pz = z + r.range(-0.7f, 0.7f);
            float rad = std::sqrt(px * px + pz * pz);
            if (rad > 104.f || rad < 3.f) continue;
            if (ix % 5 == 0 || iz % 6 == 0) continue;  // vielas
            if (std::abs(px) < 3.f) continue;            // escadaria principal
            if (r.chance(0.07f)) continue;
            float w = r.range(3.3f, 5.f), d = r.range(3.3f, 5.f);
            float hs[4] = {H(px - w / 2, pz - d / 2), H(px + w / 2, pz - d / 2), H(px - w / 2, pz + d / 2), H(px + w / 2, pz + d / 2)};
            float mn = *std::min_element(hs, hs + 4), mx = *std::max_element(hs, hs + 4);
            int floors = r.chance(0.35f) ? 1 : r.chance(0.6f) ? 2 : 3;
            if (rad > 90.f) floors = std::min(floors, 2);
            glm::vec3 p(px, mx, pz);
            Geo& g = cells.at(p);
            g.place(p, r.range(-0.12f, 0.12f) + (r.chance(0.3f) ? 1.5707963f : 0.f));
            buildBarraco(g, r, w, d, floors, r.chance(0.55f), favelaPaint(r), mx - mn + 0.3f);
            ++count;
        }
    // escadaria principal (rampa de concreto subindo o morro)
    {
        Geo& g = cells.at({0, 0, 0});
        g.place(glm::vec3(0.f), 0.f);
        MeshBuilder& c = g[Mat::Concrete];
        c.uvTile = {2.f, 2.f};
        for (float z = -104.f; z < 104.f; z += 1.f) {
            float h = H(0, z), h2 = H(0, z + 1.f);
            c.quad({-1.5f, h + 0.08f, z}, {-1.5f, h2 + 0.08f, z + 1.f}, {1.5f, h2 + 0.08f, z + 1.f}, {1.5f, h + 0.08f, z});
        }
    }
    // árvores e prédios na baixada
    std::vector<BoxCollider> treeBoxes;
    for (int k = 0; k < 60; ++k) {
        float a = r.range(0, 6.28f), rad = r.range(106.f, 145.f);
        if (std::abs(rad - 128.f) < 7.f) continue;
        float x = std::cos(a) * rad, z = std::sin(a) * rad;
        spawnProto(e, e->treeProto[r.chance(0.4f) ? 2 : 0], {x, H(x, z) - 0.1f, z}, r.range(0, 6.28f), r.range(0.9f, 1.3f), nullptr, &treeBoxes);
    }
    for (int k = 0; k < 4; ++k) {
        float a = 0.785398f + k * 1.5707963f;
        glm::vec3 p(std::cos(a) * 140.f, 2.5f, std::sin(a) * 140.f);
        Geo& g = cells.at(p);
        g.place(p, -a - 1.5707963f);
        buildApartment(g, r, 16.f, 12.f, r.irange(4, 8), buildingPaint(r));
    }
    cells.flush(e);
    addStaticBoxes(e, treeBoxes);
    setBounds(e, {-half + 2, -half + 2}, {half - 2, half - 2});
    spawn = {0.f, H(0, -128.f) + 0.1f, -128.f};
    spawnYaw = 0.f;  // de frente para o morro
    e->renderer.fogDensity = 0.002f;
    (void)count;
}

void genSchool(Engine* e, Rng& r, glm::vec3& spawn, float& spawnYaw) {
    Cells cells(1e5f);
    Geo& g = cells.at({0, 0, 0});
    g.place(glm::vec3(0.f), 0.f);
    groundBox(g, Mat::Grass, {-95, -70}, {95, 62}, 0.f, 4.f);
    groundBox(g, Mat::Asphalt, {-95, 62}, {95, 90}, 0.f, 4.f);
    roadStrip(g[Mat::Road], {-95, 74}, {95, 74}, 12.f, 0.02f);
    // pátio de concreto
    MeshBuilder& c = g[Mat::Concrete];
    c.uvTile = {2.f, 2.f};
    c.box({-58, 0, -46}, {116, 0.06f, 80}, 4);
    // muro com portão
    MeshBuilder& wall = g[Mat::Plaster];
    wall.color = srgb(235, 225, 200); wall.uvTile = {2.f, 2.f};
    const float wh = 2.6f;
    auto wallSeg = [&](glm::vec3 mn, glm::vec3 size) { wall.box(mn, size); g.collider(mn, size); };
    wallSeg({-75, 0, -58}, {150, wh, 0.25f});
    wallSeg({-75, 0, 55}, {71, wh, 0.25f});
    wallSeg({4, 0, 55}, {71, wh, 0.25f});
    wallSeg({-75, 0, -58}, {0.25f, wh, 113.25f});
    wallSeg({74.75f, 0, -58}, {0.25f, wh, 113.25f});
    g[Mat::Metal].box({-4.2f, 2.6f, 54.95f}, {8.4f, 0.35f, 0.35f});      // viga do portão (aberto)
    g[Mat::Metal].box({-4.2f, 0.f, 54.95f}, {0.3f, 2.6f, 0.35f});
    g[Mat::Metal].box({3.9f, 0.f, 54.95f}, {0.3f, 2.6f, 0.35f});
    // prédio em U (2 andares)
    glm::vec4 wallCol = srgb(245, 232, 205), band = srgb(40, 90, 170);
    g.place({0, 0.06f, -40}, 0.f);
    buildSchoolWing(g, 96.f, 12.f, 2, wallCol, band);
    g.place({-42, 0.06f, -10}, 1.5707963f);
    buildSchoolWing(g, 40.f, 12.f, 2, wallCol, band);
    g.place({42, 0.06f, -10}, -1.5707963f);
    buildSchoolWing(g, 40.f, 12.f, 2, wallCol, band);
    // quadra com traves
    g.place(glm::vec3(0.f), 0.f);
    {
        MeshBuilder& q = g[Mat::Court];
        glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        q.quad({-14, 0.08f, 16.5f}, {14, 0.08f, 16.5f}, {14, 0.08f, 1.5f}, {-14, 0.08f, 1.5f}, uv);
        MeshBuilder& m = g[Mat::Painted];
        m.color = srgb(240, 240, 240);
        for (float side : {-1.f, 1.f}) {
            float x = side * 13.f;
            m.box({x - 0.05f, 0, 7.5f}, {0.1f, 2.f, 0.1f});
            m.box({x - 0.05f, 0, 10.4f}, {0.1f, 2.f, 0.1f});
            m.box({x - 0.05f, 1.95f, 7.5f}, {0.1f, 0.1f, 3.f});
        }
        m.color = glm::vec4(1.f);
        // arquibancada
        MeshBuilder& cc = g[Mat::Concrete];
        for (int k = 0; k < 3; ++k) cc.box({-12, 0, 18.f + k * 0.8f}, {24, 0.45f * (k + 1), 0.8f});
        g.collider({-12, 0, 18}, {24, 1.35f, 2.4f});
    }
    // mastro com bandeira
    g[Mat::Metal].cylinder({8, 0, 50}, {8, 9, 50}, 0.07f, 0.05f, 8);
    {
        MeshBuilder& f = g[Mat::Painted];
        f.color = srgb(0, 150, 60);
        f.box({8.05f, 7.2f, 50}, {2.2f, 1.5f, 0.03f});
        f.color = srgb(250, 210, 0);
        f.box({8.55f, 7.55f, 49.99f}, {1.2f, 0.8f, 0.05f});
        f.color = glm::vec4(1.f);
    }
    // carros no estacionamento
    for (int k = 0; k < 7; ++k) {
        glm::vec3 p(-50.f + k * 3.f, 0.06f, 44.f);
        g.place(p, 0.f);
        if (r.chance(0.8f)) buildCar(g, r);
    }
    std::vector<BoxCollider> treeBoxes;
    for (int k = 0; k < 24; ++k) {
        float x = r.chance(0.5f) ? r.range(-72, -62) : r.range(62, 72), z = r.range(-54, 52);
        spawnProto(e, e->treeProto[0], {x, 0, z}, r.range(0, 6.28f), r.range(0.9f, 1.2f), nullptr, &treeBoxes);
    }
    cells.flush(e);
    addStaticBoxes(e, treeBoxes);
    setBounds(e, {-92, -68}, {92, 88});
    spawn = {0.f, 0.f, 66.f};
    spawnYaw = 3.14159265f;  // de frente para o portão
    e->renderer.fogDensity = 0.0015f;
}

void genSuburb(Engine* e, Rng& r, glm::vec3& spawn, float& spawnYaw) {
    const int nb = 5;
    const float block = 64.f, road = 12.f, pitch = block + road;
    const float total = nb * block + (nb + 1) * road, half = total * 0.5f;
    Cells cells(pitch * 2.f, 2);
    {
        Geo& g = cells.at({-1e4f, 0, -1e4f});
        g.place(glm::vec3(0.f), 0.f);
        groundBox(g, Mat::Asphalt, {-half - 20, -half - 20}, {half + 20, half + 20}, 0.f, 4.f);
        for (int i = 0; i <= nb; ++i) {
            float c = -half + i * pitch + road * 0.5f;
            for (int j = 0; j < nb; ++j) {
                float a0 = -half + road + j * pitch, a1 = a0 + block;
                roadStrip(g[Mat::Road], {a0, c}, {a1, c}, road, 0.02f);
                roadStrip(g[Mat::Road], {c, a0}, {c, a1}, road, 0.02f);
            }
        }
    }
    std::vector<BoxCollider> treeBoxes;
    for (int i = 0; i < nb; ++i)
        for (int j = 0; j < nb; ++j) {
            float bx0 = -half + road + i * pitch, bz0 = -half + road + j * pitch;
            glm::vec3 c(bx0 + block * 0.5f, 0, bz0 + block * 0.5f);
            Geo& g = cells.at(c);
            g.place(glm::vec3(0.f), 0.f);
            MeshBuilder& sw = g[Mat::Sidewalk];
            sw.uvTile = {2.f, 2.f};
            sw.box({bx0, 0, bz0}, {block, 0.15f, block}, 4);
            g.collider({bx0, 0, bz0}, {block, 0.15f, block});
            const float m = 3.f;
            MeshBuilder& gr = g[Mat::Grass];
            gr.uvTile = {4.f, 4.f};
            gr.box({bx0 + m, 0.15f, bz0 + m}, {block - 2 * m, 0.04f, block - 2 * m}, 4);
            if (i == nb / 2 && j == nb / 2) {  // praça central com igreja
                for (int k = 0; k < 16; ++k)
                    spawnProto(e, e->treeProto[r.chance(0.3f) ? 2 : 0], {bx0 + m + r.f() * (block - 2 * m), 0.19f, bz0 + m + r.f() * (block - 2 * m)},
                               r.range(0, 6.28f), r.range(0.9f, 1.3f), nullptr, &treeBoxes);
                g.place(c + glm::vec3(0, 0.19f, -14.f), 0.f);
                buildHouse(g, r, 12.f, 20.f, 2, srgb(250, 250, 245), false);
                MeshBuilder& p = g[Mat::Plaster];
                p.color = srgb(250, 250, 245); p.uvTile = {2.f, 2.f};
                p.box({-2.f, 0, 10.f - 4.f}, {4.f, 16.f, 4.f});
                g[Mat::RoofTile].cylinder({0, 16.f, 8.f}, {0, 20.f, 8.f}, 2.9f, 0.f, 4, true);
                g.collider({-2.f, 0, 6.f}, {4.f, 16.f, 4.f});
                continue;
            }
            float lotW = (block - 2 * m) / 4.f, lotD = (block - 2 * m) / 2.f;
            for (int k = 0; k < 8; ++k) {
                int col = k % 4; bool front = k >= 4;
                glm::vec3 p(bx0 + m + lotW * (col + 0.5f), 0.19f, bz0 + m + lotD * (front ? 1.5f : 0.5f) + (front ? 1.f : -1.f));
                lotHouse(e, cells, r, p, front ? 0.f : 3.14159265f, lotW, lotD);
            }
            for (int side = 0; side < 2; ++side)
                for (float t = 8.f; t < block - 4.f; t += 16.f) {
                    glm::vec3 p(bx0 + t, 0.15f, side ? bz0 + block - 1.2f : bz0 + 1.2f);
                    spawnProto(e, e->treeProto[0], p, r.range(0, 6.28f), r.range(0.6f, 0.85f), nullptr, &treeBoxes);
                    if (r.chance(0.4f)) {
                        glm::vec3 cp(bx0 + t + 6.f, 0.f, side ? bz0 + block + 2.2f : bz0 - 2.2f);
                        Geo& gc = cells.at(cp);
                        gc.place(cp, 1.5707963f);
                        buildCar(gc, r);
                    }
                }
        }
    cells.flush(e);
    addStaticBoxes(e, treeBoxes);
    setBounds(e, {-half - 15, -half - 15}, {half + 15, half + 15});
    spawn = {-half + 2 * pitch + road * 0.5f, 0.f, -half + 2 * pitch + road + 8.f};
    spawnYaw = 0.f;
    e->renderer.fogDensity = 0.0018f;
}

}  // namespace

float mapHeight(Engine* e, float x, float z) {
    if (e->mapKind == MAP_Infinite) return infiniteHeight(x, z, e->mapSeed);
    if (e->mapKind == MAP_Favela) return favelaHeight(x, z, e->mapSeed);
    return 0.f;
}

void clearWorld(Engine* e) {
    for (auto& c : e->chunks) if (c.used) releaseChunk(e, c);
    e->chunks.clear();
    for (auto& s : e->statics) freeStatic(e, s);
    e->statics.clear();
    e->mapKind = MAP_None;
    e->hasBounds = false;
    e->water = {};
}

void updateWorld(Engine* e) {
    if (e->mapKind == MAP_Infinite) updateInfinite(e, 1);  // no máximo 1 chunk novo por frame
}

void loadMap(Engine* e, int kind, uint32_t seed) {
    clearScene(e);
    e->mapKind = kind;
    e->mapSeed = seed;
    Rng r(seed);
    makeProtos(e, seed);
    glm::vec3 spawn(0.f);
    float spawnYaw = 0.f;
    switch (kind) {
        case MAP_Infinite: {
            e->renderer.fogDensity = 0.0032f;
            e->chunks.reserve(64);
            MeshBuilder wb;
            wb.quad({-1500, 0, 1500}, {1500, 0, 1500}, {1500, 0, -1500}, {-1500, 0, -1500});
            MeshId wm = wb.build(e->renderer);
            e->water = addMeshObject(e, "Agua", wm, e->mats[int(Mat::Water)], glm::vec3(0.f), glm::vec3(1.f), glm::vec3(0.f), false);
            if (GameObject* w = e->scene.get(e->water)) if (w->meshRenderer) w->meshRenderer->castShadows = false;
            spawn = findLand(e);
            e->editorCam.target = spawn;
            updateInfinite(e, 1000);
            break;
        }
        case MAP_City: genCity(e, r, spawn, spawnYaw); break;
        case MAP_Favela: genFavela(e, r, spawn, spawnYaw); break;
        case MAP_School: genSchool(e, r, spawn, spawnYaw); break;
        default: genSuburb(e, r, spawn, spawnYaw); e->mapKind = MAP_Suburb; break;
    }
    e->mapKind = kind < MAP_Count ? kind : MAP_Suburb;
    // jogador
    int id = createCharacter(e, randomCharacter(seed * 7 + 3), spawn + glm::vec3(0, 0.2f, 0), spawnYaw, true);
    selectHandle(e, objectById(e, id) ? e->objects[id] : Scene::Handle{});
    // câmera do editor começa com a visão geral do mapa
    mapOverview(e);
}

Scene::Handle createBuilding(Engine* e, int kind, uint32_t seed, glm::vec3 pos) {
    Rng r(seed);
    Geo g;
    g.place(glm::vec3(0.f), 0.f);
    const char* name = "Construcao";
    switch (kind) {
        case 0: buildHouse(g, r, r.range(7.f, 10.f), r.range(8.f, 11.f), 1, housePaint(r), r.chance(0.6f)); name = "Casa"; break;
        case 1: buildHouse(g, r, r.range(8.f, 11.f), r.range(9.f, 12.f), 2, housePaint(r), true); name = "Sobrado"; break;
        case 2: buildBarraco(g, r, r.range(3.5f, 5.f), r.range(3.5f, 5.f), r.irange(1, 3), r.chance(0.5f), favelaPaint(r), 0.f); name = "Barraco"; break;
        case 3: buildApartment(g, r, r.range(15.f, 21.f), r.range(13.f, 18.f), r.irange(4, 12), buildingPaint(r)); name = "Predio"; break;
        case 4: buildSkyscraper(g, r, r.range(24.f, 34.f), r.range(22.f, 30.f), r.irange(25, 55)); name = "Arranha-ceu"; break;
        case 5: buildSchoolWing(g, 40.f, 12.f, 2, srgb(245, 232, 205), srgb(40, 90, 170)); name = "Escola"; break;
        case 6: case 7: case 8:
            buildTree(g[Mat::Painted], r, kind - 6);
            g.collider({-0.25f, 0.f, -0.25f}, {0.5f, 4.f, 0.5f});
            name = kind == 6 ? "Arvore" : kind == 7 ? "Pinheiro" : "Palmeira";
            break;
        case 9: buildCar(g, r); name = "Carro"; break;
        case 10: buildStreetLight(g); g.collider({-0.15f, 0, -0.15f}, {0.3f, 7.f, 0.3f}); name = "Poste"; break;
        default: {
            MeshBuilder& w = g[Mat::Brick];
            w.uvTile = {1.f, 1.f};
            w.box({-5.f, 0, -0.1f}, {10.f, 2.2f, 0.2f});
            g.collider({-5.f, 0, -0.1f}, {10.f, 2.2f, 0.2f});
            name = "Muro";
        }
    }
    pos.y = mapHeight(e, pos.x, pos.z);
    return spawnGeo(e, g, name, true, pos, 0.f);
}

void mapOverview(Engine* e) {
    if (e->hasBounds) {
        glm::vec2 c = (e->boundsMin + e->boundsMax) * 0.5f, ext = e->boundsMax - e->boundsMin;
        e->editorCam.target = {c.x, mapHeight(e, c.x, c.y) * 0.5f, c.y};
        e->editorCam.distance = std::max(ext.x, ext.y) * 0.85f;
    } else {
        glm::vec3 t = e->editorCam.target;
        e->editorCam.target = {t.x, mapHeight(e, t.x, t.z), t.z};
        e->editorCam.distance = 220.f;
    }
    e->editorCam.pitch = 0.75f;
    e->editorCam.yaw = 3.14159265f + 0.5f;
}

void drawMapOverlay(Engine* e, const glm::mat4& vp) {
    if (!e->hasBounds) return;
    glm::vec2 a = e->boundsMin, b = e->boundsMax;
    glm::vec4 col(1.f, 0.25f, 0.2f, 0.8f);
    for (float y : {0.3f, 4.f}) {
        e->renderer.line(vp, {a.x, y, a.y}, {b.x, y, a.y}, col, 2.5f, true);
        e->renderer.line(vp, {b.x, y, a.y}, {b.x, y, b.y}, col, 2.5f, true);
        e->renderer.line(vp, {b.x, y, b.y}, {a.x, y, b.y}, col, 2.5f, true);
        e->renderer.line(vp, {a.x, y, b.y}, {a.x, y, a.y}, col, 2.5f, true);
    }
    for (glm::vec2 c : {a, b, glm::vec2(a.x, b.y), glm::vec2(b.x, a.y)}) e->renderer.line(vp, {c.x, 0.f, c.y}, {c.x, 4.f, c.y}, col, 2.5f, true);
}

}  // namespace eng
