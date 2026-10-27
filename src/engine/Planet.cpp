// Mundo-planeta: um mundo grande (6 x 6 km) com continentes e ilhas no meio do oceano, montanhas, florestas,
// cidades e vilas com nomes inventados e estradas ligando as cidades. Carrega por pedaços (chunks) em volta
// do foco, como o mundo infinito; o terreno é achatado nas cidades e ao longo das estradas.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "engine/Engine.h"
#include "procgen/Noise.h"

namespace eng {

namespace {

constexpr float kPitch = 70.f, kBlock = 56.f, kRoad = 14.f;  // quarteirão de cidade (igual ao mapa Cidade)

float smooth01(float a, float b, float x) { float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }

// Relevo natural (sem cidades/estradas): continentes no meio, oceano nas bordas
float baseHeight(uint32_t seed, float half, float x, float z) {
    float c = fbm(x * 0.00042f + 3.1f, z * 0.00042f - 7.7f, 5, seed);
    float hills = fbm(x * 0.006f, z * 0.006f, 4, seed + 5);
    float mount = ridged(x * 0.0011f, z * 0.0011f, 5, seed + 9);
    float h = (c - 0.45f) * 130.f + hills * 10.f + smooth01(0.55f, 0.75f, c) * mount * 150.f;
    float edge = smooth01(half * 0.62f, half * 0.97f, std::max(std::abs(x), std::abs(z)));
    return glm::mix(h, -45.f, edge);
}

float segDist(glm::vec2 p, glm::vec2 a, glm::vec2 b, float& t) {
    glm::vec2 ab = b - a;
    t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.f, 1.f);
    return glm::length(p - (a + ab * t));
}

// Relevo com as cidades achatadas (sem as estradas)
float cityHeight(const PlanetData& P, uint32_t seed, float x, float z) {
    float h = baseHeight(seed, P.half, x, z);
    for (const PlanetCity& c : P.cities) {
        float d = glm::length(glm::vec2(x, z) - c.pos);
        if (d < c.radius * 1.7f) h = glm::mix(h, c.h, 1.f - smooth01(c.radius * 1.05f, c.radius * 1.7f, d));
    }
    return h;
}

std::string makeName(Rng& r, int kind) {
    static const char* pre[] = {"Nova", "Porto", "Vila", "Alto", "Campo", "Serra", "Monte", "Lagoa", "Bela", "Vale", "Ponta", "Barra"};
    static const char* syl[] = {"ar", "bel", "cor", "dan", "es", "ita", "jur", "lu", "mar", "nor", "ori", "pa", "quei", "ros", "sol", "tau",
                                "ver", "za", "ca", "mi", "ra", "ti", "go", "la", "ne", "su", "vi", "bra"};
    static const char* end[] = {"é", "ã", "ira", "ana", "ópolis", "ia", "ema", "ó", "ar", "ina", "eto", "aba"};
    std::string base;
    int n = r.irange(1, 2);
    for (int k = 0; k < n; ++k) base += syl[r.irange(0, 27)];
    base += end[r.irange(0, 11)];
    base[0] = char(std::toupper((unsigned char)base[0]));
    if (kind == 0 || r.chance(0.35f)) return base;           // capital: nome simples
    return std::string(pre[r.irange(0, 11)]) + " " + base;
}

uint32_t blockSeed(uint32_t seed, int city, int i, int j) { return hash2(i * 7 + city * 131, j * 13 - city * 17, seed + 404); }

}  // namespace

void planetGenerate(Engine* e) {
    PlanetData& P = e->planet;
    P = PlanetData{};
    const uint32_t seed = e->mapSeed;
    Rng r(seed * 31 + 7);
    // lugares planos em terra firme, bem separados
    struct Cand { glm::vec2 p; float h, score; };
    std::vector<Cand> cands;
    for (float z = -P.half * 0.6f; z <= P.half * 0.6f; z += 150.f)
        for (float x = -P.half * 0.6f; x <= P.half * 0.6f; x += 150.f) {
            float jx = x + r.range(-50.f, 50.f), jz = z + r.range(-50.f, 50.f);
            float h = baseHeight(seed, P.half, jx, jz);
            if (h < 5.f || h > 45.f) continue;
            float slope = 0.f;
            for (int k = 0; k < 8; ++k) {
                float a = k * 0.785398f;
                slope += std::abs(baseHeight(seed, P.half, jx + std::cos(a) * 120.f, jz + std::sin(a) * 120.f) - h);
            }
            cands.push_back({{jx, jz}, h, slope + r.f() * 20.f});
        }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    for (const Cand& c : cands) {
        if (P.cities.size() >= 9) break;
        bool far = true;
        for (const PlanetCity& o : P.cities) far &= glm::length(o.pos - c.p) > 950.f;
        if (!far) continue;
        int kind = P.cities.empty() ? 0 : P.cities.size() < 5 ? 1 : 2;
        float radius = kind == 0 ? 380.f : kind == 1 ? 240.f : 110.f;
        P.cities.push_back({makeName(r, kind), c.p, radius, std::max(4.f, c.h), kind});
    }
    if (P.cities.empty()) P.cities.push_back({"Aurora", {0.f, 0.f}, 240.f, 8.f, 0});
    // estradas: árvore geradora mínima (sem atravessar o mar), mais alguns atalhos
    const int n = int(P.cities.size());
    auto dryPath = [&](int a, int b) {
        glm::vec2 A = P.cities[a].pos, B = P.cities[b].pos;
        int steps = int(glm::length(B - A) / 40.f) + 1;
        for (int k = 1; k < steps; ++k) {
            glm::vec2 q = glm::mix(A, B, float(k) / steps);
            if (cityHeight(P, seed, q.x, q.y) < 1.5f) return false;
        }
        return true;
    };
    std::vector<int> comp(n);
    for (int i = 0; i < n; ++i) comp[i] = i;
    struct Edge { int a, b; float d; };
    std::vector<Edge> edges;
    for (int a = 0; a < n; ++a)
        for (int b = a + 1; b < n; ++b) edges.push_back({a, b, glm::length(P.cities[a].pos - P.cities[b].pos)});
    std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) { return x.d < y.d; });
    int extra = 0;
    for (const Edge& ed : edges) {
        if (ed.d > 2600.f || !dryPath(ed.a, ed.b)) continue;
        int ca = comp[ed.a], cb = comp[ed.b];
        bool join = ca != cb;
        if (!join && (extra >= 2 || ed.d > 1600.f)) continue;
        if (!join) ++extra;
        for (int& c : comp) if (c == cb) c = ca;
        P.roads.push_back({P.cities[ed.a].pos, P.cities[ed.b].pos, ed.a, ed.b});
    }
}

float planetHeight(const Engine* e, float x, float z) {
    const PlanetData& P = e->planet;
    const uint32_t seed = e->mapSeed;
    float h = cityHeight(P, seed, x, z);
    glm::vec2 p(x, z);
    for (const PlanetRoad& rd : P.roads) {
        float t, d = segDist(p, rd.a, rd.b, t);
        if (d > 16.f) continue;
        glm::vec2 c = rd.a + (rd.b - rd.a) * t;
        // a estrada segue o relevo suavizado e fica plana na largura dela
        float along = 0.f;
        for (int k = -2; k <= 2; ++k) {
            glm::vec2 q = c + glm::normalize(rd.b - rd.a) * (k * 18.f);
            along += cityHeight(P, seed, q.x, q.y);
        }
        along = std::max(along / 5.f, 1.2f);
        h = glm::mix(h, along, 1.f - smooth01(5.f, 16.f, d));
    }
    return h;
}

bool planetNoTrees(const Engine* e, float x, float z) {
    glm::vec2 p(x, z);
    for (const PlanetCity& c : e->planet.cities) if (glm::length(p - c.pos) < c.radius * 1.1f) return true;
    for (const PlanetRoad& rd : e->planet.roads) { float t; if (segDist(p, rd.a, rd.b, t) < 10.f) return true; }
    return false;
}

void planetDecorateChunk(Engine* e, TerrainChunk& chunk, float x0, float z0, float size, std::vector<BoxCollider>& boxes) {
    const PlanetData& P = e->planet;
    Geo g;
    g.place(glm::vec3(0.f), 0.f);
    auto inChunk = [&](glm::vec2 q) { return q.x >= x0 && q.x < x0 + size && q.y >= z0 && q.y < z0 + size; };
    // ---- estradas entre cidades (fora das cidades)
    for (const PlanetRoad& rd : P.roads) {
        glm::vec2 dir = glm::normalize(rd.b - rd.a);
        float L = glm::length(rd.b - rd.a);
        const float step = 8.f;
        for (float s = 0.f; s < L; s += step) {
            glm::vec2 a = rd.a + dir * s, b = rd.a + dir * std::min(L, s + step);
            glm::vec2 mid = (a + b) * 0.5f;
            if (!inChunk(mid)) continue;
            bool inCity = false;
            for (const PlanetCity& c : P.cities) inCity |= glm::length(mid - c.pos) < c.radius * 0.98f;
            if (inCity) continue;
            float ha = planetHeight(e, a.x, a.y) + 0.12f, hb = planetHeight(e, b.x, b.y) + 0.12f;
            MeshBuilder& m = g[Mat::Road];
            glm::vec2 side(-dir.y, dir.x);
            const float w = 9.f;
            glm::vec3 p0(a.x - side.x * w * 0.5f, ha, a.y - side.y * w * 0.5f), p1(b.x - side.x * w * 0.5f, hb, b.y - side.y * w * 0.5f);
            glm::vec3 p2(b.x + side.x * w * 0.5f, hb, b.y + side.y * w * 0.5f), p3(a.x + side.x * w * 0.5f, ha, a.y + side.y * w * 0.5f);
            float v0 = s / 6.f, v1 = (s + step) / 6.f;
            glm::vec2 uv[4] = {{0, v0}, {0, v1}, {1, v1}, {1, v0}};
            if (glm::cross(p1 - p0, p3 - p0).y < 0.f) { std::swap(p1, p3); std::swap(uv[1], uv[3]); }
            m.quad(p0, p1, p2, p3, uv);
        }
    }
    // ---- quarteirões das cidades cujo centro cai neste chunk
    for (int ci = 0; ci < int(P.cities.size()); ++ci) {
        const PlanetCity& c = P.cities[ci];
        if (c.pos.x + c.radius < x0 || c.pos.x - c.radius > x0 + size || c.pos.y + c.radius < z0 || c.pos.y - c.radius > z0 + size) continue;
        int nb = int(c.radius / kPitch) + 1;
        for (int j = -nb; j < nb; ++j)
            for (int i = -nb; i < nb; ++i) {
                glm::vec2 bc = c.pos + glm::vec2((i + 0.5f) * kPitch, (j + 0.5f) * kPitch);
                if (!inChunk(bc) || glm::length(bc - c.pos) > c.radius) continue;
                Rng r(blockSeed(e->mapSeed, ci, i, j));
                float y = c.h;
                float dist = glm::length(bc - c.pos) / c.radius;
                // asfalto (quarteirão + metade das ruas em volta) e faixas das ruas do lado -X e -Z
                g.place(glm::vec3(0.f), 0.f);
                MeshBuilder& as = g[Mat::Asphalt];
                as.uvTile = {4.f, 4.f};
                as.box({bc.x - kPitch / 2, y - 0.3f, bc.y - kPitch / 2}, {kPitch, 0.32f, kPitch}, 4);
                boxes.push_back({{bc.x, y - 0.14f, bc.y}, {kPitch / 2, 0.16f, kPitch / 2}, 0.f});
                float rx = bc.x - kPitch / 2, rz = bc.y - kPitch / 2;
                auto strip = [&](glm::vec2 a, glm::vec2 b) {
                    glm::vec2 d = b - a, dir = glm::normalize(d), side(-dir.y, dir.x);
                    float L = glm::length(d), w = kRoad;
                    glm::vec3 p0(a.x - side.x * w / 2, y + 0.04f, a.y - side.y * w / 2), p1(b.x - side.x * w / 2, y + 0.04f, b.y - side.y * w / 2);
                    glm::vec3 p2(b.x + side.x * w / 2, y + 0.04f, b.y + side.y * w / 2), p3(a.x + side.x * w / 2, y + 0.04f, a.y + side.y * w / 2);
                    glm::vec2 uv[4] = {{0, 0}, {0, L / 6.f}, {1, L / 6.f}, {1, 0}};
                    if (glm::cross(p1 - p0, p3 - p0).y < 0.f) { std::swap(p1, p3); std::swap(uv[1], uv[3]); }
                    g[Mat::Road].quad(p0, p1, p2, p3, uv);
                };
                strip({rx, rz + kRoad / 2}, {rx, rz + kPitch - kRoad / 2});
                strip({rx + kRoad / 2, rz}, {rx + kPitch - kRoad / 2, rz});
                // calçada do quarteirão
                float bx0 = bc.x - kBlock / 2, bz0 = bc.y - kBlock / 2;
                MeshBuilder& sw = g[Mat::Sidewalk];
                sw.uvTile = {2.f, 2.f};
                sw.box({bx0, y, bz0}, {kBlock, 0.15f, kBlock}, 4);
                boxes.push_back({{bc.x, y + 0.075f, bc.y}, {kBlock / 2, 0.075f, kBlock / 2}, 0.f});
                const float top = y + 0.15f, m = 4.f;
                // construções: centro da capital com arranha-céus, depois prédios, casas na periferia
                if (r.chance(0.08f)) {  // praça
                    MeshBuilder& gr = g[Mat::Grass];
                    gr.uvTile = {4.f, 4.f};
                    gr.box({bx0 + m, top, bz0 + m}, {kBlock - 2 * m, 0.05f, kBlock - 2 * m}, 4);
                    for (int k = 0; k < 6; ++k) {
                        glm::vec3 tp(bx0 + m + r.f() * (kBlock - 2 * m), top + 0.05f, bz0 + m + r.f() * (kBlock - 2 * m));
                        g.place(tp, r.range(0.f, 6.28f));
                        buildTree(g[Mat::Painted], r, r.chance(0.3f) ? 2 : 0, true);
                        g.collider({-0.25f, 0.f, -0.25f}, {0.5f, 4.f, 0.5f});
                    }
                } else if (c.kind == 0 && dist < 0.35f) {
                    g.place({bc.x, top, bc.y}, 0.f);
                    buildSkyscraper(g, r, r.range(26.f, 36.f), r.range(24.f, 34.f), r.irange(18, 45));
                } else if (c.kind < 2 && dist < 0.7f) {
                    for (int k = 0; k < 4; ++k) {
                        if (r.chance(0.12f)) continue;
                        glm::vec3 p(bx0 + kBlock * ((k & 1) ? 0.73f : 0.27f), top, bz0 + kBlock * ((k & 2) ? 0.73f : 0.27f));
                        g.place(p, (k & 2) ? 0.f : 3.14159265f);
                        buildApartment(g, r, r.range(15.f, 20.f), r.range(13.f, 18.f), r.irange(3, c.kind == 0 ? 12 : 7), buildingPaint(r));
                    }
                } else {
                    float lotW = (kBlock - 2 * m) / 3.f, lotD = (kBlock - 2 * m) / 2.f;
                    for (int k = 0; k < 6; ++k) {
                        if (c.kind == 2 && r.chance(0.3f)) continue;
                        int col = k % 3; bool front = k >= 3;
                        glm::vec3 p(bx0 + m + lotW * (col + 0.5f), top, bz0 + m + lotD * (front ? 1.5f : 0.5f) + (front ? -2.5f : 2.5f));
                        g.place(p, front ? 0.f : 3.14159265f);
                        buildHouse(g, r, std::min(lotW - 3.f, r.range(7.f, 10.f)), std::min(lotD - 7.f, r.range(8.f, 11.f)),
                                   r.chance(0.3f) ? 2 : 1, housePaint(r), r.chance(0.6f));
                    }
                }
                // postes e carros
                for (int k = 0; k < 4; ++k) {
                    glm::vec3 p(bx0 + (k & 1 ? kBlock - 1.f : 1.f), top, bz0 + (k & 2 ? kBlock - 1.f : 1.f));
                    g.place(p, (k & 2) ? 0.f : 3.14159265f);
                    buildStreetLight(g);
                }
                if (r.chance(0.5f)) {
                    glm::vec3 cp(bx0 + r.range(8.f, kBlock - 8.f), y, bz0 - 3.f);
                    g.place(cp, 1.5707963f);
                    buildCar(g, r);
                }
            }
    }
    if (g.empty() && g.doors.empty()) return;
    // malhas: reaproveita as de chunks já descarregados (o mundo é carregado e descarregado enquanto anda)
    Scene::Handle rootH = e->scene.create("Cidade");
    chunk.objects.push_back(rootH);
    for (int layer = 0; layer < 3; ++layer)
        for (int k = 0; k < int(Mat::Count); ++k) {
            MeshBuilder& mb = g.layer(layer)[k];
            if (mb.empty()) continue;
            MeshId id;
            if (!e->freeGeoMeshes.empty()) {
                id = e->freeGeoMeshes.back();
                e->freeGeoMeshes.pop_back();
                e->renderer.updateMesh(id, mb.v.data(), uint32_t(mb.v.size()), mb.idx.data(), uint32_t(mb.idx.size()));
            } else {
                id = mb.build(e->renderer);
            }
            if (id == kInvalidId) continue;
            chunk.geoMeshes.push_back(id);
            Scene::Handle ch = e->scene.create(matName(Mat(k)), e->scene.get(rootH));
            GameObject* go = e->scene.get(ch);
            if (!go) break;
            auto rh = e->meshRenderers.acquire();
            if (MeshRenderer* mr = e->meshRenderers.get(rh)) {
                mr->mesh = id; mr->material = e->mats[k]; go->meshRenderer = mr;
                Mat mm = Mat(k);
                mr->castShadows = !(mm == Mat::Asphalt || mm == Mat::Road || mm == Mat::Sidewalk || mm == Mat::Grass);
                if (layer) mr->lodFar = layer == 2 ? kInteriorFar : kDetailFar;
            }
        }
    spawnGeoExtras(e, g, rootH, false, kDetailFar);
    boxes.insert(boxes.end(), g.colliders.begin(), g.colliders.end());
}

glm::vec3 planetSpawn(Engine* e, int city, float* yaw) {
    const PlanetData& P = e->planet;
    if (P.cities.empty()) return glm::vec3(0.f, 10.f, 0.f);
    const PlanetCity& c = P.cities[std::clamp(city, 0, int(P.cities.size()) - 1)];
    if (yaw) *yaw = 0.f;
    return {c.pos.x, c.h + 0.2f, c.pos.y + 4.f};  // cruzamento central (as ruas passam pelo centro)
}

std::string planetRegion(Engine* e, glm::vec3 p) {
    const PlanetData& P = e->planet;
    glm::vec2 q(p.x, p.z);
    for (const PlanetCity& c : P.cities)
        if (glm::length(q - c.pos) < c.radius * 1.15f) return c.name;
    for (const PlanetRoad& rd : P.roads) {
        float t;
        if (segDist(q, rd.a, rd.b, t) < 12.f) return "Estrada " + P.cities[rd.ca].name + " - " + P.cities[rd.cb].name;
    }
    float h = planetHeight(e, p.x, p.z);
    if (h < 0.5f) return "Oceano";
    if (h < 3.f) return "Praia";
    if (h > 75.f) return "Montanhas";
    return "Campo";
}

void planetMapImage(Engine* e, int size, uint8_t* out) {
    const PlanetData& P = e->planet;
    const uint32_t seed = e->mapSeed;
    for (int py = 0; py < size; ++py)
        for (int px = 0; px < size; ++px) {
            float x = (px + 0.5f) / size * 2.f * P.half - P.half, z = (py + 0.5f) / size * 2.f * P.half - P.half;
            float h = baseHeight(seed, P.half, x, z);
            glm::vec3 c;
            if (h < 0.f) c = glm::mix(glm::vec3(0.05f, 0.15f, 0.35f), glm::vec3(0.15f, 0.45f, 0.7f), std::clamp(1.f + h / 40.f, 0.f, 1.f));
            else if (h < 3.f) c = {0.86f, 0.8f, 0.58f};
            else if (h < 40.f) c = glm::mix(glm::vec3(0.35f, 0.62f, 0.3f), glm::vec3(0.2f, 0.45f, 0.22f), h / 40.f);
            else if (h < 85.f) c = glm::mix(glm::vec3(0.45f, 0.42f, 0.35f), glm::vec3(0.55f, 0.53f, 0.5f), (h - 40.f) / 45.f);
            else c = {0.95f, 0.96f, 0.98f};
            glm::vec2 q(x, z);
            for (const PlanetRoad& rd : P.roads) { float t; if (segDist(q, rd.a, rd.b, t) < P.half / size * 2.f) c = {0.25f, 0.22f, 0.2f}; }
            for (const PlanetCity& ct : P.cities) if (glm::length(q - ct.pos) < ct.radius) c = glm::mix(c, glm::vec3(0.75f, 0.73f, 0.7f), 0.8f);
            uint8_t* o = out + (size_t(py) * size + px) * 4;
            o[0] = uint8_t(c.r * 255.f); o[1] = uint8_t(c.g * 255.f); o[2] = uint8_t(c.b * 255.f); o[3] = 255;
        }
}

}  // namespace eng
