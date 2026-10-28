#include <chrono>
#include <BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include "assets/MeshUtils.h"
#include "engine/Engine.h"
#include "procgen/Noise.h"
#include <glm/gtx/matrix_decompose.hpp>

namespace eng {

namespace {

constexpr float kChunk = 96.f;   // tamanho do chunk do mundo infinito (m)
constexpr int kRes = 40;          // quads por lado do chunk (2,4 m por quad)
constexpr int kRadiusBase = 2;    // chunks carregados em volta do foco (5 x 5)

using HeightFn = std::function<float(float, float)>;

bool gEditable = false;  // gerando mapa editável (peças viram objetos)

// Peça de mapa editável: objeto registrado com receita {"tipo":"peca_mapa","peca":N}
void registerPiece(Engine* e, Scene::Handle h) {
    GameObject* g = e->scene.get(h);
    if (!g) return;
    int idx = int(e->mapPieces.size());
    e->mapPieces.push_back({h, g->transform.position, g->transform.scale, g->transform.rotation, g->name});
    char spec[64];
    std::snprintf(spec, sizeof(spec), "{\"tipo\":\"peca_mapa\",\"peca\":%d}", idx);
    recordRecipe(e, *g, spec);
}

// nome da peça pelo material que mais aparece nela
const char* pieceName(const Geo& g) {
    auto has = [&](Mat m) { return !g.mats[int(m)].empty() || !g.detailMats[int(m)].empty() || !g.interiorMats[int(m)].empty(); };
    if (has(Mat::FacadeOffice)) return "Arranha-ceu";
    if (has(Mat::FacadeApt)) return "Predio";
    if (has(Mat::BrickWindow) || (has(Mat::Brick) && has(Mat::Metal))) return "Barraco";
    if (has(Mat::RoofTile)) return "Casa";
    if (has(Mat::Glass)) return "Carro";
    if (has(Mat::Road) || has(Mat::Asphalt)) return "Ruas";
    if (has(Mat::Court)) return "Quadra";
    if (has(Mat::Sidewalk)) return "Calcada";
    if (has(Mat::FacadeHouse)) return "Escola";
    if (has(Mat::Metal) && !has(Mat::Concrete)) return "Poste";
    if (has(Mat::Concrete)) return "Escadaria";
    return "Peca";
}

// Divide o Geo nas peças marcadas por place() e cria um objeto editável para cada uma
void spawnPieces(Engine* e, Geo& g) {
    std::vector<Geo::Mark> marks = g.marks;
    Geo::Mark m0{};  // geometria antes do 1º place(): peça na origem (vazia é ignorada)
    marks.insert(marks.begin(), m0);
    Geo::Mark end = g.mark();
    for (size_t k = 0; k < marks.size(); ++k) {
        const Geo::Mark& a = marks[k];
        const Geo::Mark& b = k + 1 < marks.size() ? marks[k + 1] : end;
        Geo piece;
        bool any = false;
        glm::mat4 inv = glm::inverse(glm::rotate(glm::translate(glm::mat4(1.f), a.origin), a.yaw, glm::vec3(0, 1, 0)));
        glm::mat3 inv3(inv);
        for (int layer = 0; layer < Geo::kLayers; ++layer)
            for (int m = 0; m < int(Mat::Count); ++m) {
                const MeshBuilder& src = g.layer(layer)[m];
                MeshBuilder& dst = piece.layer(layer)[m];
                uint32_t v0 = a.v[layer][m], v1 = b.v[layer][m], i0 = a.i[layer][m], i1 = b.i[layer][m];
                if (i1 <= i0 || v1 <= v0) continue;
                for (uint32_t v = v0; v < v1; ++v) {
                    Vertex x = src.v[v];
                    x.pos = glm::vec3(inv * glm::vec4(x.pos, 1.f));
                    x.normal = inv3 * x.normal;
                    x.tangent = glm::vec4(inv3 * glm::vec3(x.tangent), x.tangent.w);
                    dst.v.push_back(x);
                }
                for (uint32_t i = i0; i < i1; ++i) dst.idx.push_back(src.idx[i] - v0);
                any = true;
            }
        for (size_t c = a.col; c < b.col; ++c) {
            BoxCollider bc = g.colliders[c];
            bc.center = glm::vec3(inv * glm::vec4(bc.center, 1.f));
            bc.yaw -= a.yaw;
            piece.colliders.push_back(bc);
        }
        for (size_t d = a.door; d < b.door; ++d) {
            DoorSpec ds = g.doors[d];
            ds.xf = inv * ds.xf;
            piece.doors.push_back(ds);
        }
        for (size_t t = a.tele; t < b.tele; ++t) {
            TeleSpec ts = g.teles[t];
            ts.a = glm::vec3(inv * glm::vec4(ts.a, 1.f));
            ts.b = glm::vec3(inv * glm::vec4(ts.b, 1.f));
            piece.teles.push_back(ts);
        }
        if (!any && piece.colliders.empty()) continue;
        Scene::Handle h = spawnGeo(e, piece, pieceName(piece), true, a.origin, a.yaw, kDetailFar);
        registerPiece(e, h);
    }
}

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

// Favela (projeto "Favela Game"): morro em platô com encostas de pedra e mata, subindo do sul (entrada) para o norte
// (campinho e área alta). Áreas planas (patamares) com muro de arrimo: campinho, área alta, praça, entrada e área baixa.
// Norte = -Z. Rua do bairro embaixo, ao sul.
struct FavelaPad { glm::vec2 c, half; float y; };
enum { FP_Field, FP_High, FP_Square, FP_Entrance, FP_Low, FP_Count };

float smoothStepTo(float a, float b, float x) { return smooth01(a, b, x); }

// Distância de p ao segmento ab (t: posição ao longo, 0..1)
float segDist2(glm::vec2 p, glm::vec2 a, glm::vec2 b, float& t) {
    glm::vec2 ab = b - a;
    t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.f, 1.f);
    return glm::length(p - (a + ab * t));
}

// Superelipse do platô: 1 na borda
float favelaEdge(float x, float z) {
    float ax = std::abs(x) / 64.f, az = std::abs(z) / 62.f;
    return std::pow(ax * ax * ax * ax + az * az * az * az, 0.25f);
}

// Morro sem os patamares
float favelaRaw(float x, float z, uint32_t seed) {
    float n = fbm(x * 0.03f, z * 0.03f, 3, seed) - 0.5f;
    float hb = 10.f + 14.f * smooth01(45.f, -50.f, z)
             + 4.f * smooth01(-25.f, -55.f, x) * smooth01(-5.f, -45.f, z)                              // área alta (noroeste)
             - 4.f * smooth01(26.f, 46.f, x) * smooth01(-12.f, 12.f, z) * (1.f - smooth01(40.f, 56.f, z))  // área baixa (leste)
             + n * 2.4f;
    float hv = 4.2f + (fbm(x * 0.02f + 7.f, z * 0.02f, 3, seed + 5) - 0.5f) * 3.f;  // baixada em volta
    float s = favelaEdge(x, z);
    hv = glm::mix(hv, 4.2f, smooth01(1.38f, 1.5f, s));  // rua em volta do morro: plana
    // o morro desce devagar até a rua de baixo (casas por toda a encosta)
    float slope = smooth01(0.72f, 1.46f, s + n * 0.03f);
    float h = glm::mix(hb, hv, slope);
    // rua de acesso (entrada principal): desce do platô até a rua do bairro
    auto ramp = [&](glm::vec2 a, glm::vec2 b, float ya, float yb, float w) {
        float t, d = segDist2({x, z}, a, b, t);
        float k = 1.f - smooth01(w * 0.5f, w * 0.5f + 2.5f, d);
        h = glm::mix(h, glm::mix(ya, yb, t), k);
    };
    ramp({0.f, 56.f}, {0.f, 96.f}, 10.f, 4.2f, 8.f);
    return h;
}

const FavelaPad* favelaPads(uint32_t seed) {
    static FavelaPad pads[FP_Count];
    static uint32_t cached = 0xffffffffu;
    if (cached != seed) {
        cached = seed;
        pads[FP_Field] = {{2.f, -41.f}, {16.f, 11.f}, 0.f};
        pads[FP_High] = {{-46.f, -45.f}, {9.f, 8.f}, 0.f};
        pads[FP_Square] = {{-33.f, 27.f}, {12.f, 9.f}, 0.f};
        pads[FP_Entrance] = {{0.f, 49.f}, {10.f, 7.f}, 0.f};
        pads[FP_Low] = {{45.f, 22.f}, {10.f, 12.f}, 0.f};
        for (FavelaPad& p : pads) p.y = std::round(favelaRaw(p.c.x, p.c.y, seed) * 2.f) * 0.5f;
        pads[FP_Entrance].y = 10.f;
    }
    return pads;
}

// Escadarias principais (rotas amarelas do mapa): saem da entrada, da praça e da área baixa e sobem até o campinho
const std::vector<std::vector<glm::vec2>>& favelaRoutes() {
    static const std::vector<std::vector<glm::vec2>> routes = {
        {{0, 42}, {0, 22}, {-14, 10}, {-14, -14}, {-6, -29}},                // central, da entrada ao campinho
        {{8, 42}, {24, 30}, {26, 6}, {20, -14}, {12, -29}},                  // leste
        {{-21, 33}, {-36, 14}, {-40, -12}, {-44, -36}},                      // oeste, da praça à área alta
        {{35, 18}, {40, -6}, {30, -26}, {19, -31}},                          // da área baixa ao campinho
        {{-21, 22}, {0, 22}}, {{24, 30}, {35, 26}}, {{-40, -12}, {-14, -14}}, {{20, -14}, {40, -6}},
        // acessos pelos 4 lados: escadarias que sobem da rua de baixo
        {{3, -97}, {4, -70}, {2, -53}},                                      // norte -> campinho
        {{99, 20}, {72, 21}, {56, 22}},                                      // leste -> área baixa
        {{-99, 26}, {-72, 27}, {-46, 27}},                                   // oeste -> praça
        {{-38, 98}, {-35, 66}, {-33, 37}},                                   // sudoeste -> praça
        {{52, 96}, {48, 64}, {45, 35}},                                      // sudeste -> área baixa
        {{-92, -58}, {-68, -50}, {-56, -45}},                                // noroeste -> área alta
        {{84, -66}, {50, -40}, {19, -40}},                                   // nordeste -> campinho
    };
    return routes;
}
struct PadEntry { int pad; glm::vec2 a, b; };  // rampa suave de a (na borda do patamar) até b (para fora, ao longo da rota)
const std::vector<PadEntry>& favelaEntries(uint32_t seed);

float padDist(const FavelaPad& p, float x, float z) {
    return std::max(std::abs(x - p.c.x) - p.half.x, std::abs(z - p.c.y) - p.half.y);
}

const std::vector<PadEntry>& favelaEntries(uint32_t seed) {
    static std::vector<PadEntry> list;
    static uint32_t cached = 0xffffffffu;
    if (cached == seed) return list;
    cached = seed;
    list.clear();
    const FavelaPad* pads = favelaPads(seed);
    for (const auto& rt : favelaRoutes())
        for (int end = 0; end < 2; ++end) {
            glm::vec2 e = end ? rt.back() : rt.front(), prev = end ? rt[rt.size() - 2] : rt[1];
            for (int k = 0; k < FP_Count; ++k)
                if (padDist(pads[k], e.x, e.y) < 4.f) list.push_back({k, e, e + glm::normalize(prev - e) * 8.f});
        }
    return list;
}

float favelaHeight(float x, float z, uint32_t seed) {
    float h = favelaRaw(x, z, seed);
    const FavelaPad* pads = favelaPads(seed);
    const auto& entries = favelaEntries(seed);
    for (int k = 0; k < FP_Count; ++k) {
        float d = padDist(pads[k], x, z);
        if (d > 11.f) continue;
        float blend = 1.6f;
        for (const PadEntry& en : entries) {  // onde a escadaria chega, o patamar desce em rampa
            if (en.pad != k) continue;
            float t, ds = segDist2({x, z}, en.a, en.b, t);
            if (ds < 4.f) blend = std::max(blend, glm::mix(1.6f, 9.f, 1.f - smooth01(2.f, 4.f, ds)));
        }
        if (d < blend) h = glm::mix(h, pads[k].y, 1.f - smooth01(0.f, blend, d));
    }
    return h;
}

// hint (opcional): tipo de chão para o shader de terreno, gravado como 1 - cor do vértice (branco = sem dica)
void terrainMesh(std::vector<Vertex>& v, std::vector<uint32_t>& idx, float x0, float z0, float size, int res, const HeightFn& H,
                 std::vector<float>* heights, const std::function<glm::vec3(float, float)>* hint = nullptr) {
    v.clear(); idx.clear();
    const float cell = size / res;
    v.reserve((res + 1) * (res + 1));
    if (heights) heights->resize((res + 1) * (res + 1));
    // alturas numa grade com 1 célula de borda: cada ponto calculado uma vez (as normais saem dos vizinhos)
    const int G = res + 3;
    std::vector<float> hg(G * G);
    for (int j = 0; j < G; ++j)
        for (int i = 0; i < G; ++i) hg[j * G + i] = H(x0 + (i - 1) * cell, z0 + (j - 1) * cell);
    for (int j = 0; j <= res; ++j)
        for (int i = 0; i <= res; ++i) {
            float x = x0 + i * cell, z = z0 + j * cell;
            const float* row = &hg[(j + 1) * G + (i + 1)];
            float h = row[0];
            if (heights) (*heights)[j * (res + 1) + i] = h;
            float hx = row[1] - row[-1], hz = row[G] - row[-G];
            glm::vec3 n = glm::normalize(glm::vec3(-hx, 2.f * cell, -hz));
            Vertex vx = makeVertex({x, h, z}, n, {x / 5.f, z / 5.f});
            vx.tangent = glm::vec4(glm::normalize(glm::cross(glm::vec3(0, 0, 1), n)), 1.f);
            if (hint) vx.color = glm::vec4(1.f - glm::clamp((*hint)(x, z), 0.f, 1.f), 1.f);
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
        MeshId in = g.interiorMats[k].build(e->renderer);
        if (in != kInvalidId) p.parts.push_back({in, e->mats[k], 0.f, kInteriorFar});
    }
    if (!g.colliders.empty()) { p.colliderHalf = g.colliders[0].half; p.colliderY = g.colliders[0].center.y; }
    if (g.colliders.size() > 1) p.boxes = g.colliders;  // construção com paredes: um collider por parede
    return p;
}

void spawnProto(Engine* e, const Prototype& p, glm::vec3 pos, float yaw, float scale, std::vector<Scene::Handle>* out,
                std::vector<BoxCollider>* colliders) {
    if (gEditable && !out) {  // mapa editável: a árvore/pedra vira um objeto com collider próprio
        Scene::Handle rootH = e->scene.create(p.name.empty() ? "Arvore" : p.name.c_str());
        GameObject* root = e->scene.get(rootH);
        if (!root) return;
        root->transform.setPosition(pos);
        root->transform.setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
        root->transform.setScale(glm::vec3(scale));
        registerObject(e, rootH);
        for (const ProtoPart& pp : p.parts) {
            glm::vec3 sc, tr, skew; glm::vec4 persp; glm::quat rot;
            glm::decompose(pp.local, sc, rot, tr, skew, persp);
            Scene::Handle ch = e->scene.create(p.name.empty() ? "Parte" : p.name.c_str(), root);
            GameObject* c = e->scene.get(ch);
            if (!c) break;
            c->transform.setPosition(tr); c->transform.setRotation(rot); c->transform.setScale(sc);
            auto rh = e->meshRenderers.acquire();
            if (MeshRenderer* mr = e->meshRenderers.get(rh)) { mr->mesh = pp.mesh; mr->material = pp.mat; mr->lodNear = pp.lodNear; mr->lodFar = pp.lodFar; c->meshRenderer = mr; }
            registerObject(e, ch);
        }
        if (p.colliderHalf.x > 0.f) addBoxCollider(e, rootH, {0.f, p.colliderY, 0.f}, p.colliderHalf);
        registerPiece(e, rootH);
        return;
    }
    glm::mat4 base = glm::scale(glm::rotate(glm::translate(glm::mat4(1.f), pos), yaw, glm::vec3(0, 1, 0)), glm::vec3(scale));
    for (const ProtoPart& pp : p.parts) {
        glm::vec3 sc(scale), tr = pos, skew; glm::vec4 persp; glm::quat rot = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
        if (pp.local != glm::mat4(1.f)) glm::decompose(base * pp.local, sc, rot, tr, skew, persp);
        Scene::Handle h = addMeshObject(e, "", pp.mesh, pp.mat, tr, sc, glm::degrees(glm::eulerAngles(rot)), false);
        if (GameObject* g = e->scene.get(h); g && g->meshRenderer) { g->meshRenderer->lodNear = pp.lodNear; g->meshRenderer->lodFar = pp.lodFar; }
        if (out) out->push_back(h);
    }
    if (colliders && !p.boxes.empty()) {
        glm::quat q = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
        for (const BoxCollider& b : p.boxes) colliders->push_back({pos + q * (b.center * scale), b.half * scale, yaw + b.yaw, b.pitch});
    } else if (colliders && p.colliderHalf.x > 0.f)
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
        p.name = k == 0 ? "Arvore" : k == 1 ? "Pinheiro" : "Palmeira";
        e->treeProto[k] = p;
    }
    e->rockProto = Prototype{};
    e->rockProto.parts.push_back({e->rock, e->mats[int(Mat::Concrete)], 0.f, 260.f});
    e->rockProto.colliderHalf = glm::vec3(0.45f);
    e->rockProto.colliderY = 0.f;
    e->rockProto.name = "Pedra";
    for (int k = 0; k < 3; ++k) {
        Geo g;
        g.place(glm::vec3(0.f), 0.f);
        buildHouse(g, r, r.range(7.f, 10.f), r.range(8.f, 10.f), k == 2 ? 2 : 1, housePaint(r), false);
        e->houseProto[k] = protoFromGeo(e, g);
        e->houseProto[k].name = "Casa";
    }
}

// Agrupa construções de um mapa por célula (cada célula vira poucos meshes: 1 por material)
// LOD: a camada de detalhe fica em células pequenas (some longe da câmera); a base é juntada em grupos
// de `group` x `group` células (poucos draw calls, sempre visível).
struct Cells {
    float size;
    int group;
    float detailFar = kDetailFar;  // até onde o detalhe aparece (dali em diante, a versão "de longe")
    std::map<int64_t, Geo> m;
    Cells(float s, int grp = 1) : size(s), group(grp) {}
    static int64_t key(int64_t cx, int64_t cz) { return (cx << 32) ^ (cz & 0xffffffff); }
    Geo& at(glm::vec3 p) {
        Geo& g = m[key(int64_t(std::floor(p.x / size)), int64_t(std::floor(p.z / size)))];
        g.recordMarks = gEditable;
        return g;
    }
    void flush(Engine* e) {
        for (auto& [k, g] : m) { e->mapLamps.insert(e->mapLamps.end(), g.lamps.begin(), g.lamps.end()); g.lamps.clear(); }
        if (gEditable) {  // cada construção vira objeto editável
            for (auto& [k, g] : m) spawnPieces(e, g);
            m.clear();
            return;
        }
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
            b.doors.insert(b.doors.end(), g.doors.begin(), g.doors.end());
            g.doors.clear();
            b.teles.insert(b.teles.end(), g.teles.begin(), g.teles.end());
            g.teles.clear();
            bool anyDetail = false;
            for (auto& d : g.detailMats) anyDetail |= !d.empty();
            for (auto& d : g.interiorMats) anyDetail |= !d.empty();
            for (auto& d : g.farMats) anyDetail |= !d.empty();
            if (anyDetail) spawnGeo(e, g, "Detalhes", false, glm::vec3(0.f), 0.f, detailFar);
        }
        for (auto& [k, g] : big) if (!g.empty() || !g.colliders.empty() || !g.doors.empty()) spawnGeo(e, g, "Mapa", false);
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
    for (auto h : c.objects)
        if (GameObject* g = e->scene.get(h)) {
            Scene::forEachInSubtree(*g, [&](GameObject& o) { releaseComponents(e, o); });
            e->scene.destroy(h);
        }
    c.objects.clear();
    e->freeGeoMeshes.insert(e->freeGeoMeshes.end(), c.geoMeshes.begin(), c.geoMeshes.end());
    c.geoMeshes.clear();
    // portas/elevadores de objetos que sumiram
    e->doors.erase(std::remove_if(e->doors.begin(), e->doors.end(), [&](const DoorRT& d) { return !e->scene.get(d.h); }), e->doors.end());
    e->teles.erase(std::remove_if(e->teles.begin(), e->teles.end(), [&](const TeleRT& t) { return !e->scene.get(t.root); }), e->teles.end());
    if (GameObject* g = e->scene.get(c.ground)) { releaseComponents(e, *g); e->scene.destroy(c.ground); }
    c.ground = {};
    if (c.hfBody) e->physics.removeStatic(c.hfBody);
    delete c.hfShape;
    c.hfBody = nullptr; c.hfShape = nullptr;
    freeStatic(e, c.props);
    c.used = false;
}

void generateChunk(Engine* e, TerrainChunk& c, int cx, int cz) {
    auto T0 = std::chrono::steady_clock::now();
    c.cx = cx; c.cz = cz; c.used = true;
    const float x0 = cx * kChunk, z0 = cz * kChunk;
    const uint32_t seed = e->mapSeed;
    const bool planet = e->mapKind == MAP_Planet;
    HeightFn H = planet ? HeightFn([e](float x, float z) { return planetHeight(e, x, z); })
                        : HeightFn([seed](float x, float z) { return infiniteHeight(x, z, seed); });
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    std::function<glm::vec3(float, float)> hint = [e](float x, float z) { return planetGroundHint(e, x, z); };
    terrainMesh(v, idx, x0, z0, kChunk, kRes, H, &c.heights, planet ? &hint : nullptr);
    if (c.mesh == kInvalidId) c.mesh = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);
    else e->renderer.updateMesh(c.mesh, v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()));
    c.ground = addMeshObject(e, "Terreno", c.mesh, e->mats[int(Mat::Terrain)], glm::vec3(0.f), glm::vec3(1.f), glm::vec3(0.f), false);
    if (GameObject* g = e->scene.get(c.ground)) if (g->meshRenderer) g->meshRenderer->castShadows = false;
    addHeightfield(e, c, x0, z0, kChunk, kRes);
    auto T1 = std::chrono::steady_clock::now();

    // Vegetação, pedras e vilas: determinísticos por chunk
    Rng r(hash2(cx, cz, seed + 5));
    std::vector<BoxCollider> boxes;
    auto slopeAt = [&](float x, float z) { return std::abs(H(x + 1, z) - H(x - 1, z)) + std::abs(H(x, z + 1) - H(x, z - 1)); };
    for (int k = 0; k < 26; ++k) {
        float x = x0 + r.f() * kChunk, z = z0 + r.f() * kChunk, h = H(x, z);
        if (h < 1.2f || h > (planet ? 80.f : 56.f) || slopeAt(x, z) > 1.1f) continue;
        if (planet && (planetNoTrees(e, x, z) || planetGroundHint(e, x, z).z > 0.3f)) continue;
        int kind = h > 30.f ? 1 : (h < 5.f && r.chance(0.6f)) ? 2 : (r.chance(0.3f) ? 1 : 0);
        spawnProto(e, e->treeProto[kind], {x, h - 0.15f, z}, r.range(0.f, 6.28f), r.range(0.8f, 1.35f), &c.objects, &boxes);
    }
    if (planet)  // mata fechada: mais árvores onde o chão é de floresta
        for (int k = 0; k < 40; ++k) {
            float x = x0 + r.f() * kChunk, z = z0 + r.f() * kChunk, h = H(x, z);
            if (h < 2.f || h > 85.f || slopeAt(x, z) > 1.1f || planetForest(e, x, z) < 0.5f || planetNoTrees(e, x, z)) continue;
            spawnProto(e, e->treeProto[r.chance(0.35f) ? 1 : 0], {x, h - 0.15f, z}, r.range(0.f, 6.28f), r.range(0.9f, 1.5f), &c.objects, &boxes);
        }
    for (int k = 0; k < 5; ++k) {
        float x = x0 + r.f() * kChunk, z = z0 + r.f() * kChunk, h = H(x, z);
        if (h < 0.3f || (planet && planetNoTrees(e, x, z))) continue;
        spawnProto(e, e->rockProto, {x, h, z}, r.range(0.f, 6.28f), r.range(0.8f, 2.6f), &c.objects, &boxes);
    }
    auto T2 = std::chrono::steady_clock::now();
    if (planet) planetDecorateChunk(e, c, x0, z0, kChunk, boxes);  // estradas e cidades do planeta
    else if (hash2(cx, cz, seed + 77) % 4 == 0) {  // vila
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
    auto T3 = std::chrono::steady_clock::now();
    if (std::getenv("PERF_CHUNK")) {
        auto ms = [](auto a, auto b) { return std::chrono::duration<float, std::milli>(b - a).count(); };
        std::printf("chunk %d,%d terreno %.1f árvores %.1f cidade+colliders %.1f\n", cx, cz, ms(T0, T1), ms(T1, T2), ms(T2, T3));
    }
}

void updateInfinite(Engine* e, int budget) {
    // planeta visto do alto: carrega uma área maior (até 9 x 9 chunks)
    const int kRadius = e->mapKind == MAP_Planet ? std::clamp(int(e->editorCam.distance / 160.f), 2, e->quality == 0 ? 3 : 4) : kRadiusBase;
    glm::vec3 f = e->editorCam.target;
    int ccx = int(std::floor(f.x / kChunk)), ccz = int(std::floor(f.z / kChunk));
    // Para onde a câmera olha (no plano). Olhando para a frente, o que fica atrás (fora do redor imediato)
    // não carrega e é descarregado; volta a carregar quando a câmera vira. Olhando de cima: tudo em volta.
    glm::vec3 look = f - e->camera.position;
    glm::vec2 fw(look.x, look.z);
    bool directional = glm::length(fw) > 0.45f * glm::length(look) && glm::length(fw) > 1e-3f;
    if (directional) fw = glm::normalize(fw);
    auto offset = [&](int cx, int cz) { return glm::vec2((cx + 0.5f) * kChunk - f.x, (cz + 0.5f) * kChunk - f.z) / kChunk; };
    auto behind = [&](glm::vec2 o, float cosLimit) { float l = glm::length(o); return directional && l > 1e-3f && glm::dot(o / l, fw) < cosLimit; };
    for (auto& c : e->chunks) {
        if (!c.used) continue;
        glm::vec2 o = offset(c.cx, c.cz);
        bool far = std::abs(c.cx - ccx) > kRadius + 1 || std::abs(c.cz - ccz) > kRadius + 1;
        if (far || (glm::length(o) > 2.6f && behind(o, -0.45f))) releaseChunk(e, c);
    }
    while (budget-- > 0) {
        int bx = 0, bz = 0;
        float best = 1e30f;
        for (int dz = -kRadius; dz <= kRadius; ++dz)
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                int cx = ccx + dx, cz = ccz + dz;
                glm::vec2 o = offset(cx, cz);
                float d = glm::length(o);
                if (d > 1.6f && behind(o, -0.3f)) continue;     // atrás de quem olha: só o redor imediato
                bool have = false;
                for (auto& c : e->chunks) if (c.used && c.cx == cx && c.cz == cz) { have = true; break; }
                // prioridade: perto e na frente primeiro
                float score = d - (directional && d > 1e-3f ? glm::dot(o / d, fw) * 0.9f : 0.f);
                if (!have && score < best) { best = score; bx = cx; bz = cz; }
            }
        if (best >= 1e29f) break;
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

// Caixa ao longo do segmento a-b (fios, corrimãos, correntes, vergalhões)
void segBox(MeshBuilder& m, glm::vec3 a, glm::vec3 b, float t, float t2 = -1.f) {
    glm::vec3 d = b - a;
    float L = glm::length(d);
    if (L < 1e-4f) return;
    glm::vec3 z = d / L, x = glm::normalize(glm::cross(std::abs(z.y) < 0.95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0), z)), y = glm::cross(z, x);
    glm::mat4 keep = m.xf;
    m.xf = keep * glm::mat4(glm::vec4(x, 0), glm::vec4(y, 0), glm::vec4(z, 0), glm::vec4(a, 1));
    if (t2 < 0.f) t2 = t;
    m.box({-t / 2, -t2 / 2, 0.f}, {t, t2, L});
    m.xf = keep;
}
// Caixa girada em Y (pelo centro da base)
void yawBox(MeshBuilder& m, glm::vec3 base, glm::vec3 size, float yaw) {
    glm::mat4 keep = m.xf;
    m.xf = glm::rotate(glm::translate(keep, base), yaw, glm::vec3(0, 1, 0));
    m.box({-size.x / 2, 0.f, -size.z / 2}, size);
    m.xf = keep;
}
// Fio com barriga entre dois pontos
void wire(MeshBuilder& m, glm::vec3 a, glm::vec3 b, float sag) {
    const int n = 4;
    glm::vec3 prev = a;
    for (int i = 1; i <= n; ++i) {
        float t = float(i) / n;
        glm::vec3 p = glm::mix(a, b, t) - glm::vec3(0, sag * 4.f * t * (1.f - t), 0);
        segBox(m, prev, p, 0.025f);
        prev = p;
    }
}

// Pichação/grafite do atlas (Mat::Mural) colada numa parede: centro, normal para fora (horizontal), largura (altura = metade)
void muralQuad(Geo& g, int idx, glm::vec3 c, glm::vec3 n, float w) {
    MeshBuilder& m = g[Mat::Mural];
    m.color = glm::vec4(1.f); m.uvTile = {0.f, 0.f};
    glm::vec3 right = glm::normalize(glm::cross(-n, glm::vec3(0, 1, 0))), up(0, 1, 0);
    float h = w * 0.5f;
    glm::vec4 uv = muralUV(idx);
    glm::vec2 q[4] = {{uv.x, uv.w}, {uv.z, uv.w}, {uv.z, uv.y}, {uv.x, uv.y}};
    glm::vec3 o = c + n * 0.02f;
    m.quad(o - right * (w / 2) - up * (h / 2), o + right * (w / 2) - up * (h / 2), o + right * (w / 2) + up * (h / 2), o - right * (w / 2) + up * (h / 2), q);
}

// Casa da favela, andar por andar (módulos de verdade: entra, sobe a escada, anda na laje).
// Origem: centro da base no topo da fundação; frente +Z. footLow: quanto o chão desce abaixo da base (fundação).
void favelaHouse(Geo& g, Rng& r, float w, float d, int floors, float footLow, bool frontOpen) {
    glm::vec4 paint = favelaPaint(r);
    const bool brickBase = r.chance(0.3f), graffitiBase = !brickBase && r.chance(0.18f);
    const bool rawUpper = r.chance(0.45f);
    const bool outsideStairs = r.chance(0.5f);
    const bool porchGround = frontOpen && r.chance(0.3f);
    float bw = outsideStairs ? w - 1.05f : w;          // escada externa ocupa 1 m do lado -X
    float bx = outsideStairs ? 0.525f : 0.f;
    float bd = porchGround ? d - 2.f : d;              // varanda coberta na frente
    float bz = porchGround ? -1.f : 0.f;
    // fundação até o chão (terreno inclinado)
    {
        MeshBuilder& f = g[brickBase ? Mat::Brick : Mat::Concrete];
        f.color = glm::vec4(1.f); f.uvTile = {1.f, 1.f};
        if (footLow > 0.05f) {
            f.box({-w / 2, -footLow - 0.3f, -d / 2}, {w, footLow + 0.3f, d});
            g.collider({-w / 2, -footLow - 0.3f, -d / 2}, {w, footLow + 0.3f, d});
        }
    }
    float y = 0.f;
    DetailScope houseDetail(g);  // casa completa só de perto; de longe vale o bloco simples (no fim)
    g.offset({bx, 0.f, bz});
    for (int f = 0; f < floors; ++f) {
        ModuleSpec ms;
        ms.w = bw; ms.d = bd; ms.h = 2.7f;
        ms.floor = f == 0;
        ms.slab = true;
        ms.wallMat = f == 0 ? (brickBase ? Mat::Brick : graffitiBase ? Mat::Graffiti : Mat::Plaster)
                            : (rawUpper ? Mat::Brick : brickBase ? Mat::Brick : Mat::Plaster);
        ms.wallColor = paint;
        ms.floorMat = r.chance(0.6f) ? Mat::Ceramic : Mat::Concrete;
        ms.walls[0] = f == 0 ? MW_Door : (r.chance(0.8f) ? MW_Window : MW_Plain);
        ms.walls[1] = r.chance(0.5f) ? MW_Window : MW_Plain;
        ms.walls[2] = r.chance(0.25f) ? MW_Window : MW_Plain;
        ms.walls[3] = r.chance(0.25f) ? MW_Window : MW_Plain;
        if (bw < 3.2f || bd < 3.2f) ms.walls[2] = ms.walls[3] = MW_Plain;
        // acesso à laje: escada externa em todo andar, ou interna (reta) quando cabe
        if (outsideStairs) ms.stairs = SK_Outside;
        else if (bd >= 4.4f && bw >= 3.4f) ms.stairs = SK_Straight;
        ms.porch = f == 0 ? porchGround : (frontOpen && r.chance(0.3f));
        ms.porchD = f == 0 ? 2.f : 1.1f;
        ms.parapet = f == floors - 1 && r.chance(0.35f);
        g.offset({0.f, y, 0.f});
        buildModule(g, ms);
        g.offset({0.f, -y, 0.f});
        y += moduleTop(ms);
    }
    g.offset({-bx, 0.f, -bz});
    // pichação/grafite numa parede lateral lisa do térreo
    if (r.chance(0.28f) && bd > 3.2f) {
        bool plus = outsideStairs || r.chance(0.5f);  // do lado da escada externa não
        float mw = std::min(bd - 0.6f, 3.2f);
        muralQuad(g, r.irange(0, kMuralCount - 1), {bx + (plus ? bw / 2 : -bw / 2), 0.15f + 0.4f + mw * 0.25f, bz},
                  {plus ? 1.f : -1.f, 0.f, 0.f}, mw);
    }
    // laje: caixa d'água, telhado de fibrocimento, varal, antena, parabólica, vergalhões
    DetailScope ds(g);
    const float top = y;
    MeshBuilder& p = g[Mat::Painted];
    p.uvTile = {0.f, 0.f};
    float tx = r.chance(0.5f) ? bx + bw / 2 - 0.8f : bx - bw / 2 + 0.8f, tz = bz - bd / 2 + 0.8f;
    if (r.chance(0.85f)) {
        MeshBuilder& c = g[Mat::Concrete];
        c.color = glm::vec4(1.f); c.uvTile = {1.f, 1.f};
        c.box({tx - 0.7f, top, tz - 0.7f}, {1.4f, 0.25f, 1.4f});
        buildWaterTank(p, {tx, top + 0.25f, tz});
        g.collider({tx - 0.7f, top, tz - 0.7f}, {1.4f, 1.2f, 1.4f});
    }
    if (r.chance(0.35f)) {  // meia-água de telha ondulada em cima de parte da laje
        MeshBuilder& t = g[Mat::Corrugated];
        t.color = glm::vec4(1.f); t.uvTile = {2.f, 2.f};
        float x0 = bx - bw / 2, x1 = bx + bw / 2, z0 = bz + bd * 0.05f, z1 = bz + bd / 2;
        float y0 = top + 2.2f, y1 = top + 1.9f;
        t.quad({x0, y0, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y0, z0});
        MeshBuilder& m = g[Mat::Wood];
        m.color = glm::vec4(1.f); m.uvTile = {0.f, 0.f};
        for (float x : {x0 + 0.1f, x1 - 0.2f}) m.box({x, top, z1 - 0.2f}, {0.1f, 1.9f, 0.1f});
    }
    if (r.chance(0.35f)) {  // varal com roupa
        MeshBuilder& m = g[Mat::GunMetal];
        m.color = glm::vec4(1.f); m.uvTile = {0.f, 0.f};
        glm::vec3 a(bx - bw / 2 + 0.3f, top, bz + 0.3f), b(bx + bw / 2 - 0.3f, top, bz + 0.3f);
        m.box(a - glm::vec3(0.03f, 0, 0.03f), {0.05f, 1.7f, 0.05f});
        m.box(b - glm::vec3(0.03f, 0, 0.03f), {0.05f, 1.7f, 0.05f});
        segBox(m, a + glm::vec3(0, 1.65f, 0), b + glm::vec3(0, 1.65f, 0), 0.015f);
        for (float x = a.x + 0.3f; x < b.x - 0.3f; x += r.range(0.45f, 0.7f)) {
            p.color = favelaPaint(r);
            p.box({x, top + 1.05f + r.range(0.f, 0.2f), a.z - 0.01f}, {r.range(0.3f, 0.5f), r.range(0.35f, 0.55f), 0.02f});
        }
    }
    if (r.chance(0.3f)) {  // parabólica
        p.color = srgb(220, 220, 215);
        glm::vec3 c(bx + bw / 2 - 0.5f, top + 0.9f, bz + bd / 2 - 0.5f);
        p.cylinder(c, c + glm::vec3(0.2f, 0.12f, 0.25f), 0.45f, 0.05f, 10);
        MeshBuilder& m = g[Mat::GunMetal];
        m.box({c.x - 0.03f, top, c.z - 0.03f}, {0.06f, 0.9f, 0.06f});
    }
    if (r.chance(0.3f)) {  // vergalhões nos cantos (obra que vai continuar)
        MeshBuilder& m = g[Mat::GunMetal];
        for (float sx : {-1.f, 1.f})
            for (float sz : {-1.f, 1.f}) {
                glm::vec3 c(bx + sx * (bw / 2 - 0.1f), top, bz + sz * (bd / 2 - 0.1f));
                for (int k = 0; k < 3; ++k) segBox(m, c + glm::vec3(k * 0.04f, 0, 0), c + glm::vec3(k * 0.04f + r.range(-0.05f, 0.05f), r.range(0.6f, 1.1f), 0), 0.02f);
            }
    }
    if (r.chance(0.25f)) {  // vasos de planta
        for (int k = 0; k < 3; ++k) {
            glm::vec3 c(bx + r.range(-bw / 2 + 0.4f, bw / 2 - 0.4f), top, bz + bd / 2 - 0.35f);
            p.color = srgb(150, 80, 50);
            p.box(c - glm::vec3(0.15f, 0, 0.15f), {0.3f, 0.3f, 0.3f});
            p.color = srgb(60, 130, 50);
            p.boxC(c + glm::vec3(0, 0.5f, 0), {0.45f, 0.4f, 0.45f});
        }
    }
    // de longe: bloco com textura de janelas na mesma cor, laje e caixa d'água
    {
        FarScope fs(g);
        MeshBuilder& w = g[brickBase || rawUpper ? Mat::BrickWindow : Mat::FacadeHouse];
        w.color = brickBase || rawUpper ? glm::vec4(1.f) : paint;
        w.uvTile = brickBase || rawUpper ? glm::vec2(2.5f, 2.7f) : glm::vec2(3.f, 2.85f);
        w.box({bx - bw / 2, 0.f, bz - bd / 2}, {bw, top, bd}, 4);
        MeshBuilder& c = g[Mat::Concrete];
        c.color = glm::vec4(1.f); c.uvTile = {2.f, 2.f};
        c.box({bx - bw / 2 - 0.05f, top - 0.15f, bz - bd / 2 - 0.05f}, {bw + 0.1f, 0.2f, bd + 0.1f}, 4);
        MeshBuilder& t = g[Mat::Painted];
        t.color = srgb(40, 110, 200); t.uvTile = {0.f, 0.f};
        t.box({tx - 0.55f, top, tz - 0.55f}, {1.1f, 1.f, 1.1f}, 4);
    }
}

void genFavela(Engine* e, Rng& r, glm::vec3& spawn, float& spawnYaw) {
    const float half = 130.f;
    const uint32_t seed = e->mapSeed;
    HeightFn H = [seed](float x, float z) { return favelaHeight(x, z, seed); };
    const FavelaPad* pads = favelaPads(seed);
    // chão (malha de 1 m + heightfield). Dica do shader: chão batido no platô e nas ruas, mata nas encostas
    {
        e->chunks.emplace_back();
        TerrainChunk& c = e->chunks.back();
        c.used = true;
        std::vector<Vertex> v; std::vector<uint32_t> idx;
        const int res = 220;
        std::function<glm::vec3(float, float)> hint = [](float x, float z) {
            float s = favelaEdge(x, z);
            float town = 1.f - smooth01(1.4f, 1.5f, s);
            float road = (1.f - smooth01(5.f, 7.f, std::abs(x))) * smooth01(50.f, 56.f, z);
            float forest = smooth01(1.62f, 1.72f, s) * (1.f - road);
            return glm::vec3(std::max(town, road), forest, 0.f);
        };
        terrainMesh(v, idx, -half, -half, 2 * half, res, H, &c.heights, &hint);
        c.mesh = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);
        c.ground = addMeshObject(e, "Morro", c.mesh, e->mats[int(Mat::Terrain)], glm::vec3(0.f), glm::vec3(1.f), glm::vec3(0.f), false);
        if (GameObject* go = e->scene.get(c.ground)) if (go->meshRenderer) go->meshRenderer->castShadows = false;  // só recebe sombra
        addHeightfield(e, c, -half, -half, 2 * half, res);
        c.cx = 1 << 20;  // fora da grade do mundo infinito
    }
    Cells cells(30.f, 2);
    cells.detailFar = 85.f;  // casas completas até 85 m; depois, blocos simples
    std::vector<BoxCollider> treeBoxes;
    // ---- plantas do kit de vegetação (3 níveis de detalhe); sem os arquivos, as árvores feitas por código ----
    const Prototype* trees[6] = {};
    const Prototype* bushes[5] = {};
    const Prototype* grass[4] = {};
    int nTrees = 0, nBushes = 0, nGrass = 0;
    for (const char* n : {"arvore_1", "arvore_2", "arvore_3", "arvore_4", "arvores_dupla", "arvore_5"})
        if (const Prototype* p = vegPrototype(e, n)) trees[nTrees++] = p;
    for (const char* n : {"arbusto_1", "arbusto_2", "arbusto_3", "arbusto_redondo_1", "arbusto_redondo_2"})
        if (const Prototype* p = vegPrototype(e, n)) bushes[nBushes++] = p;
    for (const char* n : {"capim_1", "capim_2", "capim_3", "capim_florido"})
        if (const Prototype* p = vegPrototype(e, n)) grass[nGrass++] = p;

    // ---- rotas principais (escadarias de concreto pelos becos) ----
    const auto& routes = favelaRoutes();
    const auto& entries = favelaEntries(seed);
    auto nearRoute = [&](glm::vec2 p, float w) {
        for (const auto& rt : routes)
            for (size_t i = 0; i + 1 < rt.size(); ++i) { float t; if (segDist2(p, rt[i], rt[i + 1], t) < w) return true; }
        return false;
    };
    {
        Geo& g = cells.at({-1e4f, 0, -1e4f});
        g.place(glm::vec3(0.f), 0.f);
        MeshBuilder& c = g[Mat::Concrete];
        c.color = glm::vec4(1.f); c.uvTile = {1.f, 1.f};
        std::set<Geo*> railStarted;
        for (size_t ri = 0; ri < routes.size(); ++ri)
            for (size_t i = 0; i + 1 < routes[ri].size(); ++i) {
                const auto& rt = routes[ri];
                glm::vec2 a = rt[i], b = rt[i + 1], dir = glm::normalize(b - a);
                float L = glm::length(b - a), yaw = std::atan2(dir.x, dir.y);
                for (float s = 0.f; s < L; s += 0.45f) {
                    glm::vec2 q = a + dir * (s + 0.225f);
                    float h = H(q.x, q.y);
                    yawBox(c, {q.x, h - 0.35f, q.y}, {2.8f, 0.42f, 0.47f}, yaw);  // degrau seguindo o morro
                }
                // corrimão de ferro onde a escadaria é íngreme
                glm::vec2 side(-dir.y, dir.x);
                glm::vec3 prevTop(0.f);
                bool havePrev = false;
                for (float s = 0.5f; s < L - 0.5f; s += 2.f) {
                    glm::vec2 q = a + dir * s, q2 = a + dir * std::min(L, s + 2.f);
                    float steep = std::abs(H(q2.x, q2.y) - H(q.x, q.y)) / 2.f;
                    glm::vec2 pp = q + side * 1.3f;
                    glm::vec3 base(pp.x, H(pp.x, pp.y), pp.y), topP = base + glm::vec3(0, 0.95f, 0);
                    if (steep < 0.25f) { havePrev = false; continue; }
                    Geo& gr = cells.at(base);
                    if (railStarted.insert(&gr).second) gr.place(glm::vec3(0.f), 0.f);
                    DetailScope ds(gr);
                    MeshBuilder& rm = gr[Mat::GunMetal];
                    rm.color = glm::vec4(1.f); rm.uvTile = {0.f, 0.f};
                    rm.box(base - glm::vec3(0.025f, 0.f, 0.025f), {0.05f, 0.95f, 0.05f});
                    if (havePrev) segBox(rm, prevTop, topP, 0.045f);
                    prevTop = topP; havePrev = true;
                }
            }
    }
    // ---- postes com fiação ----
    {
        std::vector<glm::vec3> poles;
        std::vector<glm::vec2> poleSide;  // de que lado da escadaria o poste está
        for (size_t ri = 0; ri < routes.size(); ++ri)
            for (size_t i = 0; i + 1 < routes[ri].size(); ++i) {
                const auto& rt = routes[ri];
                glm::vec2 a = rt[i], b = rt[i + 1], dir = glm::normalize(b - a), side(-dir.y, dir.x);
                float L = glm::length(b - a);
                for (float s = 4.f; s < L; s += 16.f) {
                    glm::vec2 q = a + dir * s + side * 1.7f;
                    poles.push_back({q.x, H(q.x, q.y), q.y});
                    poleSide.push_back(side);
                }
            }
        for (size_t k = 0; k < poles.size(); ++k) {
            glm::vec3 p = poles[k];
            Geo& g = cells.at(p);
            g.place(p, 0.f);
            MeshBuilder& c = g[Mat::Concrete];
            c.color = glm::vec4(1.f); c.uvTile = {1.f, 1.f};
            c.cylinder({0, 0, 0}, {0, 8.f, 0}, 0.13f, 0.09f, 8);
            g.collider({-0.15f, 0, -0.15f}, {0.3f, 8.f, 0.3f});
            DetailScope ds(g);
            MeshBuilder& m = g[Mat::GunMetal];
            m.color = glm::vec4(1.f); m.uvTile = {0.f, 0.f};
            m.box({-0.7f, 7.3f, -0.05f}, {1.4f, 0.1f, 0.1f});
            m.box({-0.25f, 6.2f, -0.2f}, {0.5f, 0.6f, 0.4f});  // transformador / caixa de luz
            if (k % 3 != 2) {  // braço com luminária virada para a escadaria
                glm::vec2 to = glm::normalize(glm::vec2(-poleSide[k].x, -poleSide[k].y));
                glm::vec3 arm(to.x * 1.3f, 6.7f, to.y * 1.3f);
                segBox(m, {0.f, 6.3f, 0.f}, arm, 0.06f);
                m.box(arm + glm::vec3(-0.18f, 0.f, -0.18f), {0.36f, 0.1f, 0.36f});
                MeshBuilder& l = g[Mat::Lamp];
                l.color = glm::vec4(1.f); l.uvTile = {0.f, 0.f};
                l.box(arm + glm::vec3(-0.13f, -0.07f, -0.13f), {0.26f, 0.07f, 0.26f});
                g.lamp(arm - glm::vec3(0, 0.25f, 0));
            }
            g.place(glm::vec3(0.f), 0.f);
            if (k + 1 < poles.size() && glm::length(poles[k + 1] - p) < 22.f)
                for (float o : {-0.6f, 0.f, 0.6f}) wire(g[Mat::Polymer], p + glm::vec3(o, 7.35f, 0), poles[k + 1] + glm::vec3(o, 7.35f, 0), 0.6f);
        }
    }

    // ---- casas: lotes em grade, becos entre eles ----
    std::vector<std::pair<glm::vec2, glm::vec2>> lots;  // centro, meio tamanho (para os testes)
    const float pitchX = 7.6f, pitchZ = 7.2f;
    for (float gz = -94.f; gz < 92.f; gz += pitchZ)
        for (float gx = -98.f; gx < 98.f; gx += pitchX) {
            glm::vec2 c(gx + pitchX * 0.5f + r.range(-0.3f, 0.3f), gz + pitchZ * 0.5f + r.range(-0.3f, 0.3f));
            float gap = r.range(1.2f, 1.6f);
            float lw = pitchX - gap, ld = pitchZ - gap;
            if (r.chance(0.15f)) lw -= r.range(0.5f, 1.5f);
            // o lote encolhe do lado de uma escadaria/patamar em vez de sumir
            auto fits = [&](glm::vec2 cc, glm::vec2 hh) {
                for (int i = -1; i <= 1; ++i)
                    for (int j = -1; j <= 1; ++j) {
                        glm::vec2 q = cc + glm::vec2(i * hh.x, j * hh.y);
                        if (favelaEdge(q.x, q.y) > 1.4f) return false;  // até a rua de baixo
                        for (int k = 0; k < FP_Count; ++k) if (padDist(pads[k], q.x, q.y) < 1.8f) return false;
                        if (nearRoute(q, 1.65f)) return false;
                        if (std::abs(q.x) < 7.f && q.y > 40.f) return false;  // rua da entrada
                    }
                return true;
            };
            glm::vec2 hs(lw * 0.5f, ld * 0.5f);
            bool ok = fits(c, hs);
            for (int tries = 0; !ok && tries < 8; ++tries) {
                glm::vec2 sh = tries < 4 ? glm::vec2(lw * 0.34f, ld * 0.34f) : glm::vec2(lw * 0.5f, ld * 0.5f);
                int side = tries % 4;
                glm::vec2 h2 = side < 2 ? glm::vec2(hs.x - sh.x * 0.5f, hs.y) : glm::vec2(hs.x, hs.y - sh.y * 0.5f);
                glm::vec2 c2 = c + (side == 0 ? glm::vec2(sh.x * 0.5f, 0) : side == 1 ? glm::vec2(-sh.x * 0.5f, 0)
                                  : side == 2 ? glm::vec2(0, sh.y * 0.5f) : glm::vec2(0, -sh.y * 0.5f));
                if (h2.x > 1.4f && h2.y > 1.4f && fits(c2, h2)) { c = c2; hs = h2; lw = hs.x * 2; ld = hs.y * 2; ok = true; }
            }
            if (!ok || r.chance(0.04f)) continue;
            // casa: base no canto mais alto do terreno; a fundação desce até o mais baixo
            float hs4[4] = {H(c.x - hs.x, c.y - hs.y), H(c.x + hs.x, c.y - hs.y), H(c.x - hs.x, c.y + hs.y), H(c.x + hs.x, c.y + hs.y)};
            float mn = *std::min_element(hs4, hs4 + 4), mx = *std::max_element(hs4, hs4 + 4);
            int floors = r.chance(0.2f) ? 1 : r.chance(0.55f) ? 2 : r.chance(0.7f) ? 3 : 4;
            float yaw = 0.f;
            float pick = r.f();
            if (pick > 0.75f) yaw = 1.5707963f; else if (pick > 0.55f) yaw = -1.5707963f;
            float w = yaw == 0.f ? lw : ld, d = yaw == 0.f ? ld : lw;
            bool frontOpen = true;
            glm::vec3 pos(c.x, mx + 0.05f, c.y);
            Geo& g = cells.at(pos);
            g.place(pos, yaw);
            favelaHouse(g, r, w, d, floors, mx - mn + 0.05f, frontOpen);
            lots.push_back({c, hs});
        }

    // ---- chão cimentado (becos, quintais), com alguns cantos de terra; onde é muito inclinado vira escadinha ----
    {
        std::set<Geo*> started;
        const float rise = 0.17f;  // altura do degrau
        auto underHouse = [&](float cx, float cz) {
            for (auto& [lc, lh] : lots) if (std::abs(cx - lc.x) < lh.x - 0.3f && std::abs(cz - lc.y) < lh.y - 0.3f) return true;
            return false;
        };
        for (float z = -96.f; z < 94.f; z += 1.f)
            for (float x = -98.f; x < 98.f; x += 1.f) {
                float cx = x + 0.5f, cz = z + 0.5f;
                if (favelaEdge(cx, cz) > 1.44f) continue;
                bool onPad = false;
                for (int k = 0; k < FP_Count && !onPad; ++k) onPad = padDist(pads[k], cx, cz) < 0.3f;
                if (onPad || (std::abs(cx) < 3.6f && cz > 54.f)) continue;
                if (nearRoute({cx, cz}, 0.6f) || underHouse(cx, cz)) continue;  // escadaria já tem degrau; casa cobre
                if (fbm(cx * 0.07f + 3.f, cz * 0.07f, 3, seed + 41) > 0.63f) continue;  // canto de terra
                Geo& g = cells.at({cx, 0.f, cz});
                if (started.insert(&g).second) g.place(glm::vec3(0.f), 0.f);
                float tone = 0.82f + 0.25f * fbm(cx * 0.2f, cz * 0.2f, 2, seed + 43);
                glm::vec4 col(tone, tone, tone * 0.97f, 1.f);
                float h00 = H(x, z), h10 = H(x + 1, z), h01 = H(x, z + 1), h11 = H(x + 1, z + 1);
                glm::vec2 uv[4] = {{x / 2.f, z / 2.f}, {x / 2.f, (z + 1) / 2.f}, {(x + 1) / 2.f, (z + 1) / 2.f}, {(x + 1) / 2.f, z / 2.f}};
                auto flat = [&](MeshBuilder& m) {
                    m.color = col;
                    m.quad({x, h00 + 0.04f, z}, {x, h01 + 0.04f, z + 1}, {x + 1, h11 + 0.04f, z + 1}, {x + 1, h10 + 0.04f, z}, uv);
                };
                float gx = ((h10 - h00) + (h11 - h01)) * 0.5f, gz = ((h01 - h00) + (h11 - h10)) * 0.5f;
                float slope = std::max(std::abs(gx), std::abs(gz));
                if (slope < 0.2f) { flat(g[Mat::Cement]); continue; }
                { FarScope fs(g); flat(g[Mat::Cement]); }  // de longe: liso
                DetailScope ds(g);
                MeshBuilder& m = g[Mat::Cement];
                m.color = col;
                const bool alongX = std::abs(gx) >= std::abs(gz);
                const float dir = alongX ? (gx > 0 ? 1.f : -1.f) : (gz > 0 ? 1.f : -1.f);  // para onde sobe
                const int n = slope > 0.35f ? 2 : 1;
                auto hAt = [&](float u, float v) {  // bilinear dentro da célula (u em x, v em z, 0..1)
                    return glm::mix(glm::mix(h00, h10, u), glm::mix(h01, h11, u), v);
                };
                for (int i = 0; i < n; ++i) {
                    float a0 = float(i) / n, a1 = float(i + 1) / n;
                    float hs[4] = {alongX ? hAt(a0, 0) : hAt(0, a0), alongX ? hAt(a1, 0) : hAt(1, a0),
                                   alongX ? hAt(a0, 1) : hAt(0, a1), alongX ? hAt(a1, 1) : hAt(1, a1)};
                    float mx = *std::max_element(hs, hs + 4), mn = *std::min_element(hs, hs + 4);
                    float top = std::ceil(mx / rise) * rise + 0.02f;
                    // pisada
                    glm::vec3 p0, p1, p2, p3;
                    if (alongX) { p0 = {x + a0, top, z}; p1 = {x + a0, top, z + 1}; p2 = {x + a1, top, z + 1}; p3 = {x + a1, top, z}; }
                    else { p0 = {x, top, z + a0}; p1 = {x, top, z + a1}; p2 = {x + 1, top, z + a1}; p3 = {x + 1, top, z + a0}; }
                    glm::vec2 tuv[4] = {{p0.x / 2, p0.z / 2}, {p1.x / 2, p1.z / 2}, {p2.x / 2, p2.z / 2}, {p3.x / 2, p3.z / 2}};
                    m.quad(p0, p1, p2, p3, tuv);
                    // espelho do degrau, no lado que desce
                    float e = dir > 0 ? a0 : a1, lo = mn - 0.08f;
                    glm::vec3 r0, r1, r2, r3;
                    if (alongX) { float X = x + e; r0 = {X, lo, z}; r1 = {X, lo, z + 1}; r2 = {X, top, z + 1}; r3 = {X, top, z}; }
                    else { float Z = z + e; r0 = {x + 1, lo, Z}; r1 = {x, lo, Z}; r2 = {x, top, Z}; r3 = {x + 1, top, Z}; }
                    glm::vec3 nrm = glm::cross(r1 - r0, r3 - r0);
                    glm::vec3 want = alongX ? glm::vec3(-dir, 0, 0) : glm::vec3(0, 0, -dir);
                    glm::vec2 ruv[4] = {{0, 0}, {0.5f, 0}, {0.5f, (top - lo) / 2}, {0, (top - lo) / 2}};
                    if (glm::dot(nrm, want) < 0.f) m.quad(r0, r3, r2, r1, ruv); else m.quad(r0, r1, r2, r3, ruv);
                }
            }
    }

    // ---- muros de arrimo dos patamares (grafitados) ----
    for (int k = 0; k < FP_Count; ++k) {
        const FavelaPad& P = pads[k];
        Geo& g = cells.at({P.c.x, 0, P.c.y});
        g.place(glm::vec3(0.f), 0.f);
        for (int side = 0; side < 4; ++side) {
            glm::vec2 n = side == 0 ? glm::vec2(0, 1) : side == 1 ? glm::vec2(0, -1) : side == 2 ? glm::vec2(1, 0) : glm::vec2(-1, 0);
            glm::vec2 t(-n.y, n.x);
            float len = side < 2 ? P.half.x * 2.f : P.half.y * 2.f;
            glm::vec2 edge = P.c + n * (side < 2 ? P.half.y : P.half.x);
            for (float s = -len / 2; s < len / 2 - 0.01f; s += 2.f) {
                float segL = std::min(2.f, len / 2 - s);
                glm::vec2 q = edge + t * (s + segL * 0.5f) + n * 0.2f;
                glm::vec2 out = q + n * 2.2f;
                float ho = favelaRaw(out.x, out.y, seed);
                float diff = ho - P.y;
                if (std::abs(diff) < 0.5f) continue;
                if (k == FP_Entrance && side == 0) continue;  // a rua chega por aqui
                bool entry = false;
                for (const PadEntry& en : entries) { float t; if (en.pad == k && segDist2(q, en.a, en.b, t) < 3.2f) entry = true; }
                if (entry) continue;  // passagem da escadaria
                float y0 = std::min(ho, P.y) - 0.6f, y1 = diff < 0.f ? P.y + 0.9f : ho + 0.1f;  // aterro: guarda-corpo de 90 cm
                Mat wm = r.chance(0.55f) ? Mat::Graffiti : r.chance(0.5f) ? Mat::Concrete : Mat::Stone;
                MeshBuilder& m = g[wm];
                m.color = glm::vec4(1.f);
                m.uvTile = wm == Mat::Graffiti ? glm::vec2(4.f, 2.f) : glm::vec2(2.f, 2.f);
                glm::vec3 size(side < 2 ? segL : 0.4f, y1 - y0, side < 2 ? 0.4f : segL);
                glm::vec3 mnp(q.x - size.x / 2, y0, q.y - size.z / 2);
                m.box(mnp, size);
                g.collider(mnp, size);
                // grafite no lado que aparece (aterro: por fora; corte: virado para o patamar)
                float vis = diff < 0.f ? P.y + 0.9f - ho : ho - P.y;
                auto wallAt = [&](float off) {  // vizinho também tem muro do mesmo tipo?
                    glm::vec2 o2 = q + t * off + n * 2.2f;
                    float d2 = favelaRaw(o2.x, o2.y, seed) - P.y;
                    return std::abs(d2) >= 0.5f && (d2 < 0.f) == (diff < 0.f) && std::abs(s + segL * 0.5f + off) < len / 2;
                };
                if (vis > 1.2f && segL > 1.9f && wallAt(-2.f) && wallAt(2.f) && r.chance(0.4f)) {
                    float mw = std::min(3.6f, (vis - 0.2f) * 2.f);
                    glm::vec3 nn = diff < 0.f ? glm::vec3(n.x, 0, n.y) : glm::vec3(-n.x, 0, -n.y);
                    glm::vec3 face = glm::vec3(q.x, 0, q.y) + nn * 0.2f;
                    float cy = diff < 0.f ? (ho + P.y + 0.9f) * 0.5f : (P.y + ho) * 0.5f;
                    muralQuad(g, r.irange(0, kMuralCount - 1), {face.x, cy, face.z}, nn, std::min(mw, 2.f * 1.95f));
                }
            }
        }
    }

    // ---- campinho de futebol: gramado com linhas, traves com rede e alambrado em volta ----
    {
        const FavelaPad& P = pads[FP_Field];
        glm::vec3 c(P.c.x, P.y, P.c.y);
        Geo& g = cells.at(c);
        g.place(c, 0.f);
        MeshBuilder& b = g[Mat::Concrete];
        b.color = glm::vec4(1.f); b.uvTile = {2.f, 2.f};
        b.box({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        g.collider({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        MeshBuilder& q = g[Mat::Court];
        glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        q.quad({-14, 0.07f, 7.5f}, {14, 0.07f, 7.5f}, {14, 0.07f, -7.5f}, {-14, 0.07f, -7.5f}, uv);
        MeshBuilder& wpost = g[Mat::Painted];
        wpost.color = srgb(240, 240, 240); wpost.uvTile = {0.f, 0.f};
        MeshBuilder& net = g[Mat::ChainLink];
        net.color = glm::vec4(1.f); net.uvTile = {1.f, 1.f};
        for (float sx : {-1.f, 1.f}) {
            float x = sx * 13.f;
            for (float z : {-1.5f, 1.5f}) wpost.box({x - 0.06f, 0.05f, z - 0.06f}, {0.12f, 2.f, 0.12f});
            wpost.box({x - 0.06f, 2.f, -1.56f}, {0.12f, 0.12f, 3.12f});
            float back = x + sx * 1.f;
            net.box({std::min(x, back), 0.05f, -1.5f}, {1.f, 2.05f, 3.f}, sx > 0 ? 1 : 2);  // rede (sem a face da boca)
            g.collider({sx > 0 ? x + 0.9f : back, 0.05f, -1.5f}, {0.12f, 2.f, 3.f});
        }
        // alambrado de 4 m com portões no lado sul (onde chegam as escadarias)
        MeshBuilder& post = g[Mat::GunMetal];
        post.color = glm::vec4(1.f); post.uvTile = {0.f, 0.f};
        const float fh = 4.f, hx = P.half.x - 0.5f, hz = P.half.y - 0.5f;
        auto fence = [&](glm::vec2 a, glm::vec2 b2) {
            glm::vec2 d = b2 - a;
            float L = glm::length(d);
            for (float s = 0.f; s < L - 0.01f; s += 3.f) {
                float l = std::min(3.f, L - s);
                glm::vec2 p0 = a + d / L * s, p1 = a + d / L * (s + l);
                glm::vec2 mn = glm::min(p0, p1), mx = glm::max(p0, p1);
                // gates: aberturas onde chegam as rotas
                glm::vec2 mid = (p0 + p1) * 0.5f;
                if (std::abs(mid.y - hz) < 0.1f && (std::abs(mid.x + 8.f) < 1.6f || std::abs(mid.x - 10.f) < 1.6f)) continue;
                if (std::abs(mid.y + hz) < 0.1f && std::abs(mid.x) < 1.6f) continue;   // portão norte
                if (std::abs(mid.x - hx) < 0.1f && std::abs(mid.y - 1.f) < 1.6f) continue;  // portão leste
                net.box({mn.x - 0.01f, 0.f, mn.y - 0.01f}, {mx.x - mn.x + 0.02f, fh, mx.y - mn.y + 0.02f});
                g.collider({mn.x - 0.03f, 0.f, mn.y - 0.03f}, {mx.x - mn.x + 0.06f, fh, mx.y - mn.y + 0.06f});
                post.box({p0.x - 0.04f, 0.f, p0.y - 0.04f}, {0.08f, fh + 0.1f, 0.08f});
                segBox(post, {p0.x, fh, p0.y}, {p1.x, fh, p1.y}, 0.05f);
            }
        };
        fence({-hx, -hz}, {hx, -hz});
        fence({hx, -hz}, {hx, hz});
        fence({hx, hz}, {-hx, hz});
        fence({-hx, hz}, {-hx, -hz});
        // arquibancada de concreto no lado leste (fora do alambrado não cabe: fica dentro, atrás da linha)
        MeshBuilder& a = g[Mat::Concrete];
        for (int k = 0; k < 2; ++k) {
            a.box({14.6f + k * 0.0f, 0.f, -6.f}, {0.6f, 0.45f * (k + 1), 12.f});
            g.collider({14.6f, 0.f, -6.f}, {0.6f, 0.45f * (k + 1), 12.f});
        }
    }

    // ---- área alta (mirante): laje com guarda-corpo, banco e torre de antena ----
    {
        const FavelaPad& P = pads[FP_High];
        glm::vec3 c(P.c.x, P.y, P.c.y);
        Geo& g = cells.at(c);
        g.place(c, 0.f);
        MeshBuilder& b = g[Mat::Concrete];
        b.color = glm::vec4(1.f); b.uvTile = {2.f, 2.f};
        b.box({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        g.collider({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        MeshBuilder& m = g[Mat::GunMetal];
        m.color = glm::vec4(1.f); m.uvTile = {0.f, 0.f};
        // torre treliçada de 18 m
        glm::vec3 t0(-P.half.x + 2.f, 0.05f, -P.half.y + 2.f);
        for (int k = 0; k < 4; ++k) {
            glm::vec3 a = t0 + glm::vec3(k & 1 ? 1.2f : -1.2f, 0, k & 2 ? 1.2f : -1.2f);
            segBox(m, a, t0 + glm::vec3(k & 1 ? 0.2f : -0.2f, 18.f, k & 2 ? 0.2f : -0.2f), 0.08f);
        }
        for (float h = 1.5f; h < 18.f; h += 1.5f) {
            float s = 1.2f - h / 18.f * 1.0f;
            for (int k = 0; k < 4; ++k) {
                glm::vec3 a = t0 + glm::vec3((k == 0 || k == 3) ? -s : s, h, k < 2 ? -s : s);
                glm::vec3 b2 = t0 + glm::vec3((k == 0 || k == 1) ? s : -s, h, (k == 1 || k == 2) ? s : -s);
                segBox(m, a, b2, 0.04f);
            }
        }
        g.collider(t0 - glm::vec3(1.3f, 0, 1.3f), {2.6f, 18.f, 2.6f});
        MeshBuilder& w = g[Mat::Wood];
        w.color = glm::vec4(1.f); w.uvTile = {0.f, 0.f};
        for (int k = 0; k < 2; ++k) {
            float z = 1.f + k * 3.f;
            w.box({2.f, 0.45f, z}, {2.f, 0.06f, 0.5f});
            m.box({2.1f, 0.f, z + 0.2f}, {0.06f, 0.45f, 0.06f});
            m.box({3.8f, 0.f, z + 0.2f}, {0.06f, 0.45f, 0.06f});
        }
    }

    // ---- praça das crianças: chão de terra, balanço, escorregador, gangorra, gira-gira, bancos e árvores ----
    {
        const FavelaPad& P = pads[FP_Square];
        glm::vec3 c(P.c.x, P.y, P.c.y);
        Geo& g = cells.at(c);
        g.place(c, 0.f);
        MeshBuilder& gr = g[Mat::Dirt];
        gr.color = glm::vec4(1.7f, 1.55f, 1.35f, 1.f); gr.uvTile = {3.f, 3.f};
        gr.box({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        g.collider({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        MeshBuilder& p = g[Mat::Painted];
        p.uvTile = {0.f, 0.f};
        MeshBuilder& m = g[Mat::GunMetal];
        m.color = glm::vec4(1.f); m.uvTile = {0.f, 0.f};
        // balanço (2 lugares)
        {
            glm::vec3 o(-6.f, 0.05f, -3.f);
            p.color = srgb(230, 60, 50);
            for (float x : {-1.6f, 1.6f}) {
                segBox(p, o + glm::vec3(x, 0, -0.9f), o + glm::vec3(x, 2.4f, 0), 0.09f);
                segBox(p, o + glm::vec3(x, 0, 0.9f), o + glm::vec3(x, 2.4f, 0), 0.09f);
            }
            segBox(p, o + glm::vec3(-1.7f, 2.4f, 0), o + glm::vec3(1.7f, 2.4f, 0), 0.1f);
            for (float x : {-0.75f, 0.75f}) {
                segBox(m, o + glm::vec3(x - 0.2f, 2.4f, 0), o + glm::vec3(x - 0.2f, 0.55f, 0), 0.02f);
                segBox(m, o + glm::vec3(x + 0.2f, 2.4f, 0), o + glm::vec3(x + 0.2f, 0.55f, 0), 0.02f);
                p.color = srgb(250, 200, 40);
                p.box(o + glm::vec3(x - 0.25f, 0.5f, -0.12f), {0.5f, 0.05f, 0.24f});
                p.color = srgb(230, 60, 50);
            }
            g.collider(o + glm::vec3(-1.8f, 0, -0.1f), {3.6f, 2.5f, 0.2f});
        }
        // escorregador: escada atrás, plataforma, rampa vermelha
        {
            glm::vec3 o(3.f, 0.05f, -3.f);
            p.color = srgb(40, 120, 220);
            for (float x : {-0.35f, 0.35f}) segBox(p, o + glm::vec3(x, 0, -1.2f), o + glm::vec3(x, 2.f, -0.2f), 0.07f);
            for (float h = 0.35f; h < 2.f; h += 0.35f) p.box(o + glm::vec3(-0.35f, h, -1.2f + h * 0.5f - 0.03f), {0.7f, 0.05f, 0.08f});
            p.color = srgb(250, 200, 40);
            p.box(o + glm::vec3(-0.4f, 1.95f, -0.3f), {0.8f, 0.08f, 0.6f});
            for (float x : {-0.38f, 0.32f}) for (float z : {-0.3f, 0.24f}) p.box(o + glm::vec3(x, 0, z), {0.06f, 1.95f, 0.06f});
            p.color = srgb(225, 50, 45);
            segBox(p, o + glm::vec3(0, 1.98f, 0.3f), o + glm::vec3(0, 0.25f, 2.9f), 0.62f, 0.06f);
            g.collider(o + glm::vec3(-0.45f, 0, -1.25f), {0.9f, 2.f, 1.6f});
        }
        // gangorra
        {
            glm::vec3 o(-6.f, 0.05f, 4.f);
            p.color = srgb(60, 170, 80);
            p.box(o + glm::vec3(-0.15f, 0, -0.15f), {0.3f, 0.45f, 0.3f});
            segBox(p, o + glm::vec3(-1.8f, 0.2f, 0), o + glm::vec3(1.8f, 0.7f, 0), 0.3f, 0.06f);
            g.collider(o + glm::vec3(-1.8f, 0, -0.15f), {3.6f, 0.6f, 0.3f});
        }
        // gira-gira
        {
            glm::vec3 o(3.f, 0.05f, 4.f);
            p.color = srgb(250, 140, 40);
            p.cylinder(o + glm::vec3(0, 0.25f, 0), o + glm::vec3(0, 0.33f, 0), 1.2f, 1.2f, 16);
            m.cylinder(o, o + glm::vec3(0, 0.25f, 0), 0.08f, 0.08f, 8);
            for (int k = 0; k < 4; ++k) {
                float a = k * 1.5707963f;
                segBox(m, o + glm::vec3(std::cos(a) * 1.05f, 0.33f, std::sin(a) * 1.05f), o + glm::vec3(0, 1.f, 0), 0.04f);
            }
            g.collider(o - glm::vec3(1.2f, 0, 1.2f), {2.4f, 0.4f, 2.4f});
        }
        // caixa de areia
        {
            MeshBuilder& w = g[Mat::Wood];
            w.color = glm::vec4(1.f); w.uvTile = {1.f, 1.f};
            glm::vec3 o(8.5f, 0.05f, 4.f);
            w.box(o + glm::vec3(-1.5f, 0, -1.5f), {3.f, 0.3f, 0.12f});
            w.box(o + glm::vec3(-1.5f, 0, 1.38f), {3.f, 0.3f, 0.12f});
            w.box(o + glm::vec3(-1.5f, 0, -1.5f), {0.12f, 0.3f, 3.f});
            w.box(o + glm::vec3(1.38f, 0, -1.5f), {0.12f, 0.3f, 3.f});
            p.color = srgb(220, 200, 150);
            p.box(o + glm::vec3(-1.4f, 0, -1.4f), {2.8f, 0.22f, 2.8f});
        }
        // bancos
        MeshBuilder& w = g[Mat::Wood];
        w.color = glm::vec4(1.f); w.uvTile = {0.f, 0.f};
        for (int k = 0; k < 4; ++k) {
            glm::vec3 o(-9.f + k * 6.f, 0.05f, P.half.y - 1.2f);
            for (int i = 0; i < 3; ++i) w.box(o + glm::vec3(-0.9f, 0.42f, -0.25f + i * 0.17f), {1.8f, 0.04f, 0.14f});
            m.box(o + glm::vec3(-0.8f, 0, -0.2f), {0.05f, 0.42f, 0.4f});
            m.box(o + glm::vec3(0.75f, 0, -0.2f), {0.05f, 0.42f, 0.4f});
            g.collider(o + glm::vec3(-0.9f, 0, -0.3f), {1.8f, 0.46f, 0.6f});
        }
        for (int k = 0; k < 5; ++k) {
            glm::vec3 tp(c.x + (k % 2 ? -1.f : 1.f) * (P.half.x - 1.5f), P.y + 0.05f, c.z - P.half.y + 2.f + k * 3.5f);
            if (trees[0]) spawnProto(e, *trees[k % std::min(nTrees, 4)], tp, r.range(0, 6.28f), r.range(1.7f, 2.1f), nullptr, &treeBoxes);
            else spawnProto(e, e->treeProto[0], tp, r.range(0, 6.28f), r.range(0.8f, 1.1f), nullptr, &treeBoxes);
        }
        // floreiras com plantas no fundo da praça
        if (const Prototype* fl = vegPrototype(e, "floreira_2")) {
            const Prototype* fc[2] = {vegPrototype(e, "forracao_1"), vegPrototype(e, "arbusto_redondo_2")};
            for (int k = 0; k < 3; ++k) {
                glm::vec3 fp(c.x - 7.f + k * 7.f, P.y + 0.05f, c.z - P.half.y + 0.9f);
                spawnProto(e, *fl, fp, 0.f, 1.f, nullptr, &treeBoxes);
                for (int j = -1; j <= 1; ++j)
                    if (const Prototype* pl = fc[(k + j + 3) % 2])
                        spawnProto(e, *pl, fp + glm::vec3(j * 1.f, 0.4f, 0.f), r.range(0, 6.28f), r.range(0.9f, 1.2f), nullptr, nullptr);
            }
        }
        // luz da praça
        g.place(c + glm::vec3(0, 0.05f, 0), 0.f);
        buildStreetLight(g);
    }

    // ---- entrada principal: rua subindo do bairro, largo de concreto, vans estacionadas, boteco ----
    {
        Geo& g = cells.at({0, 0, 70});
        g.place(glm::vec3(0.f), 0.f);
        MeshBuilder& road = g[Mat::Asphalt];
        road.color = glm::vec4(1.f); road.uvTile = {0.f, 0.f};
        for (float z = 55.f; z < 92.f; z += 1.f) {
            float h0 = H(0.f, z) + 0.04f, h1 = H(0.f, z + 1.f) + 0.04f;
            glm::vec2 uv[4] = {{0, z / 4.f}, {0, (z + 1) / 4.f}, {1.5f, (z + 1) / 4.f}, {1.5f, z / 4.f}};
            road.quad({-3.5f, h0, z}, {-3.5f, h1, z + 1.f}, {3.5f, h1, z + 1.f}, {3.5f, h0, z}, uv);
        }
        // postes de rua subindo a entrada (luz de verdade à noite)
        for (int k = 0; k < 3; ++k) {
            float z = 62.f + k * 11.f, x = k % 2 ? 4.4f : -4.4f;
            glm::vec3 p(x, H(x, z), z);
            Geo& gl = cells.at(p);
            gl.place(p, x > 0 ? -1.5707963f : 1.5707963f);
            buildStreetLight(gl);
        }
        g.place(glm::vec3(0.f), 0.f);
        const FavelaPad& P = pads[FP_Entrance];
        glm::vec3 c(P.c.x, P.y, P.c.y);
        Geo& ge = cells.at(c);
        ge.place(c, 0.f);
        MeshBuilder& b = ge[Mat::Sidewalk];
        b.color = glm::vec4(1.f); b.uvTile = {2.f, 2.f};
        b.box({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        ge.collider({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        for (int k = 0; k < 2; ++k) {
            glm::vec3 cp = c + glm::vec3(-6.5f + k * 13.f, 0.05f, 2.5f - k * 1.f);
            Geo& gc = cells.at(cp);
            gc.place(cp, k ? 0.1f : 3.0f);
            buildCar(gc, r);
        }
        // boteco: cômodo com porta larga aberta e toldo de telha ondulada
        glm::vec3 bp = c + glm::vec3(-P.half.x + 3.f, 0.05f, -P.half.y + 2.6f);
        ge.place(bp, 0.f);
        ModuleSpec ms;
        ms.w = 5.f; ms.d = 4.f; ms.h = 2.8f;
        ms.walls[0] = MW_Open; ms.walls[1] = MW_Plain; ms.walls[2] = MW_Window; ms.walls[3] = MW_Plain;
        ms.wallMat = Mat::Plaster; ms.wallColor = srgb(240, 200, 60); ms.floorMat = Mat::Ceramic;
        buildModule(ge, ms);
        MeshBuilder& t = ge[Mat::Corrugated];
        t.color = glm::vec4(1.f); t.uvTile = {2.f, 2.f};
        t.quad({-2.6f, 2.9f, 2.f}, {-2.6f, 2.5f, 4.f}, {2.6f, 2.5f, 4.f}, {2.6f, 2.9f, 2.f});
        MeshBuilder& pw = ge[Mat::Wood];
        for (float x : {-2.5f, 2.4f}) pw.box({x, 0.f, 3.9f}, {0.1f, 2.5f, 0.1f});
        MeshBuilder& tb = ge[Mat::Painted];
        tb.color = srgb(230, 40, 40); tb.uvTile = {0.f, 0.f};
        for (int k = 0; k < 2; ++k) {  // mesas de plástico
            glm::vec3 o(-1.f + k * 2.f, 0.f, 3.f);
            tb.box(o + glm::vec3(-0.4f, 0.7f, -0.4f), {0.8f, 0.04f, 0.8f});
            tb.box(o + glm::vec3(-0.04f, 0.f, -0.04f), {0.08f, 0.7f, 0.08f});
        }
    }

    // ---- área baixa (acesso rápido): terreiro de terra com carros e entulho ----
    {
        const FavelaPad& P = pads[FP_Low];
        glm::vec3 c(P.c.x, P.y, P.c.y);
        Geo& g = cells.at(c);
        g.place(c, 0.f);
        MeshBuilder& b = g[Mat::Concrete];
        b.color = glm::vec4(0.9f, 0.9f, 0.88f, 1.f); b.uvTile = {2.f, 2.f};
        b.box({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        g.collider({-P.half.x, -0.4f, -P.half.y}, {P.half.x * 2, 0.45f, P.half.y * 2});
        MeshBuilder& w = g[Mat::Wood];
        w.color = glm::vec4(1.f); w.uvTile = {1.f, 1.f};
        for (int k = 0; k < 4; ++k) {  // caixotes (cobertura no tiroteio)
            glm::vec3 o(r.range(-6.f, 6.f), 0.05f, r.range(-8.f, 4.f));
            w.box(o, {1.f, 1.f, 1.f});
            g.collider(o, {1.f, 1.f, 1.f});
        }
        for (int k = 0; k < 2; ++k) {
            glm::vec3 cp = c + glm::vec3(-3.f + k * 6.f, 0.05f, 6.f);
            Geo& gc = cells.at(cp);
            gc.place(cp, 1.5707963f + r.range(-0.3f, 0.3f));
            buildCar(gc, r);
        }
    }

    // ---- rua em volta do pé do morro (todas as escadarias chegam nela), prédios do bairro do lado de fora ----
    {
        auto ringAt = [](float t, float k) {  // superelipse do morro aumentada k vezes
            float c = std::cos(t), sn = std::sin(t);
            return glm::vec2(64.f * k * (c < 0 ? -1.f : 1.f) * std::sqrt(std::abs(c)), 62.f * k * (sn < 0 ? -1.f : 1.f) * std::sqrt(std::abs(sn)));
        };
        Geo& g = cells.at({1e4f, 0, -1e4f});
        g.place(glm::vec3(0.f), 0.f);
        MeshBuilder& road = g[Mat::Road];
        road.color = glm::vec4(1.f); road.uvTile = {0.f, 0.f};
        const int seg = 240;
        float along = 0.f;
        for (int k = 0; k < seg; ++k) {
            float t0 = 6.2831853f * k / seg, t1 = 6.2831853f * (k + 1) / seg;
            glm::vec2 a0 = ringAt(t0, 1.5f), a1 = ringAt(t1, 1.5f), b0 = ringAt(t0, 1.62f), b1 = ringAt(t1, 1.62f);
            float L = glm::length((a1 + b1) * 0.5f - (a0 + b0) * 0.5f);
            glm::vec2 uv[4] = {{0, along / 6.f}, {0, (along + L) / 6.f}, {1, (along + L) / 6.f}, {1, along / 6.f}};
            glm::vec3 p0(a0.x, H(a0.x, a0.y) + 0.06f, a0.y), p1(a1.x, H(a1.x, a1.y) + 0.06f, a1.y);
            glm::vec3 p2(b1.x, H(b1.x, b1.y) + 0.06f, b1.y), p3(b0.x, H(b0.x, b0.y) + 0.06f, b0.y);
            glm::vec3 nrm = glm::cross(p1 - p0, p3 - p0);
            if (nrm.y < 0.f) road.quad(p0, p3, p2, p1, uv); else road.quad(p0, p1, p2, p3, uv);
            along += L;
            if (k % 12 == 6) {  // poste do lado de fora da rua
                glm::vec2 o = ringAt(t0, 1.66f), in = ringAt(t0, 1.5f);
                glm::vec2 dir = glm::normalize(in - o);
                glm::vec3 p(o.x, H(o.x, o.y), o.y);
                Geo& gl = cells.at(p);
                gl.place(p, std::atan2(dir.x, dir.y));
                buildStreetLight(gl);
            }
            if (k % 20 == 3 && r.chance(0.6f)) {  // carro parado na beira
                glm::vec2 cp2 = ringAt(t0 + 0.004f, 1.6f), nx = ringAt(t0 + 0.03f, 1.6f);
                glm::vec3 cp(cp2.x, H(cp2.x, cp2.y), cp2.y);
                Geo& gc = cells.at(cp);
                gc.place(cp, std::atan2(nx.x - cp2.x, nx.y - cp2.y) + 1.5707963f);
                buildCar(gc, r);
            }
        }
        // prédios do bairro do lado de fora, só ao sul (o resto é mata)
        for (float x = -110.f; x < 110.f; x += 22.f) {
            glm::vec3 p(x + r.range(-2.f, 2.f), 4.4f, 116.f);
            Geo& gb = cells.at(p);
            gb.place(p, 3.14159265f);
            buildApartment(gb, r, r.range(14.f, 18.f), 12.f, r.irange(3, 6), buildingPaint(r));
        }
    }

    // ---- mata nas encostas e algumas árvores entre as casas ----
    for (int k = 0; k < 260; ++k) {
        float a = r.range(0, 6.2831853f), rr = r.range(1.7f, 2.05f);  // mata do lado de fora da rua de baixo
        float x = std::cos(a) * 64.f * rr, z = std::sin(a) * 62.f * rr;
        if (std::abs(x) > half - 3.f || std::abs(z) > half - 3.f || z > 104.f) continue;
        // mata da encosta: árvores do kit (2 a 3x o tamanho do jardim), arbustos e capim; palmeira de vez em quando
        float pick = r.f();
        if (pick < 0.12f || !trees[0]) {
            spawnProto(e, e->treeProto[2], {x, H(x, z) - 0.2f, z}, r.range(0, 6.28f), r.range(0.9f, 1.4f), nullptr, &treeBoxes);
        } else if (pick < 0.62f) {
            spawnProto(e, *trees[r.irange(0, nTrees - 1)], {x, H(x, z) - 0.15f, z}, r.range(0, 6.28f), r.range(2.f, 2.9f), nullptr, &treeBoxes);
        } else if (pick < 0.85f && bushes[0]) {
            spawnProto(e, *bushes[r.irange(0, nBushes - 1)], {x, H(x, z) - 0.05f, z}, r.range(0, 6.28f), r.range(1.6f, 2.6f), nullptr, &treeBoxes);
        } else if (grass[0]) {
            spawnProto(e, *grass[r.irange(0, nGrass - 1)], {x, H(x, z) - 0.05f, z}, r.range(0, 6.28f), r.range(1.f, 1.6f), nullptr, &treeBoxes);
        }
    }
    for (int k = 0; k < 90; ++k) {
        float x = r.range(-90.f, 90.f), z = r.range(-88.f, 86.f);
        if (favelaEdge(x, z) > 1.42f) continue;
        bool free = !nearRoute({x, z}, 1.6f);
        for (auto& [lc, lh] : lots) if (std::abs(x - lc.x) < lh.x + 1.2f && std::abs(z - lc.y) < lh.y + 1.2f) { free = false; break; }
        for (int p = 0; p < FP_Count && free; ++p) if (padDist(pads[p], x, z) < 1.f) free = false;
        if (!free) continue;
        if (trees[0] && !r.chance(0.3f))
            spawnProto(e, *trees[r.irange(0, nTrees - 1)], {x, H(x, z) - 0.1f, z}, r.range(0, 6.28f), r.range(1.6f, 2.2f), nullptr, &treeBoxes);
        else
            spawnProto(e, e->treeProto[r.chance(0.3f) ? 2 : 0], {x, H(x, z) - 0.1f, z}, r.range(0, 6.28f), r.range(0.6f, 0.9f), nullptr, &treeBoxes);
    }

    // ---- ao fundo: morros com casinhas e o Cristo no alto (sem colisão, fora do limite) ----
    {
        auto far = [seed](float x, float z) {
            float r2 = std::sqrt(x * x + z * z);
            float hills = 6.f + fbm(x * 0.005f, z * 0.005f, 4, seed + 77) * 55.f;
            float cristo = 45.f * std::exp(-glm::dot(glm::vec2(x + 120.f, z + 380.f), glm::vec2(x + 120.f, z + 380.f)) / (2.f * 60.f * 60.f));
            return glm::mix(-30.f, hills + cristo, smooth01(170.f, 260.f, r2));
        };
        e->chunks.emplace_back();
        TerrainChunk& c = e->chunks.back();
        c.used = true;
        std::vector<Vertex> v; std::vector<uint32_t> idx;
        std::function<glm::vec3(float, float)> hint = [](float, float) { return glm::vec3(0.f, 1.f, 0.f); };
        terrainMesh(v, idx, -700.f, -700.f, 1400.f, 120, far, nullptr, &hint);
        c.mesh = e->renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), false);
        c.ground = addMeshObject(e, "Morros ao fundo", c.mesh, e->mats[int(Mat::Terrain)], glm::vec3(0.f), glm::vec3(1.f), glm::vec3(0.f), false);
        if (GameObject* go = e->scene.get(c.ground)) if (go->meshRenderer) go->meshRenderer->castShadows = false;
        c.cx = (1 << 20) + 1;
        // casinhas coloridas nas encostas viradas para cá
        Geo& g = cells.at({1e4f, 0, 1e4f});
        g.place(glm::vec3(0.f), 0.f);
        MeshBuilder& p = g[Mat::Painted];
        p.uvTile = {0.f, 0.f};
        for (int k = 0; k < 1400; ++k) {
            float a = r.range(0, 6.2831853f), d = r.range(230.f, 450.f);
            float x = std::cos(a) * d, z = std::sin(a) * d;
            float h = far(x, z);
            if (h < 6.f || h > 50.f) continue;
            p.color = favelaPaint(r) * r.range(0.75f, 1.f);
            float w = r.range(4.f, 7.f), hh = r.range(3.f, 8.f);
            p.box({x - w / 2, h - 1.f, z - w / 2}, {w, hh + 1.f, w * r.range(0.8f, 1.2f)});
        }
        // Cristo Redentor (silhueta) no morro mais alto ao norte
        glm::vec3 cb(-120.f, far(-120.f, -380.f) - 2.f, -380.f);
        p.color = srgb(235, 232, 225);
        p.box(cb + glm::vec3(-5.f, 0, -5.f), {10.f, 10.f, 10.f});
        p.cylinder(cb + glm::vec3(0, 10.f, 0), cb + glm::vec3(0, 34.f, 0), 3.4f, 2.2f, 10);
        p.box(cb + glm::vec3(-14.f, 29.f, -1.5f), {28.f, 3.f, 3.f});
        p.cylinder(cb + glm::vec3(0, 34.f, 0), cb + glm::vec3(0, 38.f, 0), 1.6f, 1.2f, 10);
    }

    cells.flush(e);
    addStaticBoxes(e, treeBoxes);
    setBounds(e, {-half + 12, -half + 12}, {half - 12, half - 12});
    const FavelaPad& P = pads[FP_Entrance];
    spawn = {P.c.x, P.y + 0.1f, P.c.y + 3.f};
    spawnYaw = 3.14159265f;  // olhando para dentro da favela (norte)
    e->renderer.fogDensity = 0.0018f;
    setTimeOfDay(e, 16.9f);   // fim de tarde, luz dourada
    e->dayCycleMin = 10.f;    // no Play: dia e noite em 10 minutos
    e->rainMode = 1;          // e chove em algum momento
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
    if (e->mapKind == MAP_Planet) return planetHeight(e, x, z);
    return 0.f;
}

void clearWorld(Engine* e) {
    for (auto& c : e->chunks) if (c.used) releaseChunk(e, c);
    e->chunks.clear();
    e->freeGeoMeshes.clear();  // as malhas somem junto com a cena: reaproveitar ids velhos apagava as cidades
    for (auto& s : e->statics) freeStatic(e, s);
    e->statics.clear();
    e->mapKind = MAP_None;
    e->hasBounds = false;
    e->water = {};
    e->mapPieces.clear();
    e->mapEditable = false;
    e->mapLamps.clear();
}

void updateWorld(Engine* e) {
    if (e->globeMode) return;  // vendo o globo: o terreno não precisa carregar
    if (e->mapKind == MAP_Infinite || e->mapKind == MAP_Planet) updateInfinite(e, 1);  // no máximo 1 chunk novo por frame
}

void loadMap(Engine* e, int kind, uint32_t seed, bool withPlayer, bool editable) {
    clearScene(e);
    e->mapKind = kind;
    e->mapSeed = seed;
    e->dayCycleMin = 0.f;  // cada mapa escolhe (a favela liga o ciclo e a chuva)
    e->rainMode = 0;
    Rng r(seed);
    applyMapSwaps(e, true, false);   // texturas trocadas antes (os protótipos usam os materiais)
    makeProtos(e, seed);
    applyMapSwaps(e, false, true);   // modelos trocados (árvores, pedras, casas)
    e->mapEditable = editable && kind != MAP_Infinite && kind != MAP_Planet;
    gEditable = e->mapEditable;
    glm::vec3 spawn(0.f);
    float spawnYaw = 0.f;
    switch (kind) {
        case MAP_Infinite: {
            e->renderer.fogDensity = 0.0032f;
            e->chunks.reserve(160);
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
        case MAP_Planet: {
            planetGenerate(e);
            {   // prédios prontos do kit de cidade (Quaternius) em parte dos lotes das cidades
                const char* kit[3] = {"Building_Small_1", "Building_Medium_2_001", "Building_Large_2"};
                for (int k = 0; k < 3; ++k) if (!kitPrototype(e, kit[k], e->kitBuilding[k], 240.f)) e->kitBuilding[k] = Prototype{};
            }
            e->renderer.fogDensity = 0.0022f;
            e->chunks.reserve(160);
            MeshBuilder wb;
            wb.quad({-1500, 0, 1500}, {1500, 0, 1500}, {1500, 0, -1500}, {-1500, 0, -1500});
            MeshId wm = wb.build(e->renderer);
            e->water = addMeshObject(e, "Agua", wm, e->mats[int(Mat::Water)], glm::vec3(0.f), glm::vec3(1.f), glm::vec3(0.f), false);
            if (GameObject* w = e->scene.get(e->water)) if (w->meshRenderer) w->meshRenderer->castShadows = false;
            spawn = planetSpawn(e, 0, &spawnYaw);
            e->editorCam.maxDistance = 20000.f;
            e->editorCam.target = spawn;
            updateInfinite(e, 1000);
            setBounds(e, glm::vec2(-e->planet.half), glm::vec2(e->planet.half));
            break;
        }
        case MAP_City: genCity(e, r, spawn, spawnYaw); break;
        case MAP_Favela: genFavela(e, r, spawn, spawnYaw); break;
        case MAP_School: genSchool(e, r, spawn, spawnYaw); break;
        default: genSuburb(e, r, spawn, spawnYaw); e->mapKind = MAP_Suburb; break;
    }
    e->mapKind = kind < MAP_Count ? kind : MAP_Suburb;
    gEditable = false;
    // jogador (salvo como receita: ao reabrir o projeto, o mapa é refeito sem ele e ele volta do arquivo)
    if (withPlayer) {
        char spec[160];
        std::snprintf(spec, sizeof(spec), "{\"tipo\":\"personagem\",\"aparencia\":\"aleatorio\",\"semente\":%u,\"jogador\":true,\"nome\":\"Jogador\","
                      "\"pos\":[%.3f,%.3f,%.3f],\"rot_y\":%.1f}", seed * 7 + 3, spawn.x, spawn.y + 0.2f, spawn.z, glm::degrees(spawnYaw));
        int id = createFromSpecJson(e, spec, nullptr);
        selectHandle(e, objectById(e, id) ? e->objects[id] : Scene::Handle{});
    }
    // câmera do editor começa com a visão geral do mapa (planeta: o globo inteiro)
    mapOverview(e);
    if (e->mapKind == MAP_Planet) planetGlobeEnter(e);
}

Scene::Handle createBuilding(Engine* e, int kind, uint32_t seed, glm::vec3 pos, int floors) {
    Rng r(seed);
    Geo g;
    g.place(glm::vec3(0.f), 0.f);
    const char* name = "Construcao";
    switch (kind) {
        case 0: buildHouse(g, r, r.range(7.f, 10.f), r.range(8.f, 11.f), floors > 0 ? std::min(floors, 3) : 1, housePaint(r), r.chance(0.6f)); name = "Casa"; break;
        case 1: buildHouse(g, r, r.range(8.f, 11.f), r.range(9.f, 12.f), floors > 0 ? std::min(floors, 3) : 2, housePaint(r), true); name = "Sobrado"; break;
        case 2: {
            int f = r.irange(1, 3);  // sorteia sempre (mantém o resto do sorteio igual em projetos antigos)
            buildBarraco(g, r, r.range(3.5f, 5.f), r.range(3.5f, 5.f), floors > 0 ? std::min(floors, 6) : f, r.chance(0.5f), favelaPaint(r), 0.f);
            name = "Barraco";
            break;
        }
        case 3: { int f = r.irange(4, 12); buildApartment(g, r, r.range(15.f, 21.f), r.range(13.f, 18.f), floors > 0 ? floors : f, buildingPaint(r)); name = "Predio"; break; }
        case 4: { int f = r.irange(25, 55); buildSkyscraper(g, r, r.range(24.f, 34.f), r.range(22.f, 30.f), floors > 0 ? floors : f); name = "Arranha-ceu"; break; }
        case 5: buildSchoolWing(g, 40.f, 12.f, 2, srgb(245, 232, 205), srgb(40, 90, 170)); name = "Escola"; break;
        case 6: case 7: case 8:
            buildTree(g[Mat::Painted], r, kind - 6);
            g.collider({-0.25f, 0.f, -0.25f}, {0.5f, 4.f, 0.5f});
            name = kind == 6 ? "Arvore" : kind == 7 ? "Pinheiro" : "Palmeira";
            break;
        case 9: buildCar(g, r); name = "Carro"; break;
        case 10: buildStreetLight(g); g.collider({-0.15f, 0, -0.15f}, {0.3f, 7.f, 0.3f}); name = "Poste"; break;
        case 12: case 13: {  // trecho de rua (ao longo de Z) / cruzamento
            MeshBuilder& m = g[kind == 12 ? Mat::Road : Mat::Asphalt];
            float W = 8.f, L = kind == 12 ? 12.f : 8.f;
            glm::vec2 uv[4] = {{0, L / 6.f}, {1, L / 6.f}, {1, 0}, {0, 0}};
            if (kind == 13) { uv[0] = {0, 2}; uv[1] = {2, 2}; uv[2] = {2, 0}; uv[3] = {0, 0}; }
            m.quad({-W / 2, 0.03f, L / 2}, {W / 2, 0.03f, L / 2}, {W / 2, 0.03f, -L / 2}, {-W / 2, 0.03f, -L / 2}, uv);
            g.collider({-W / 2, -0.2f, -L / 2}, {W, 0.23f, L});
            name = kind == 12 ? "Rua" : "Cruzamento";
            break;
        }
        case 14: {  // calçada com meio-fio
            MeshBuilder& m = g[Mat::Sidewalk];
            m.uvTile = {2.f, 2.f};
            m.box({-1.5f, 0, -6.f}, {3.f, 0.15f, 12.f});
            g.collider({-1.5f, -0.2f, -6.f}, {3.f, 0.35f, 12.f});
            name = "Calcada";
            break;
        }
        case 15: case 16: {  // escada / rampa: sobem para +Z (o personagem consegue subir)
            float W = 2.f, H = 3.f, L = kind == 15 ? 5.f : 8.f;
            MeshBuilder& m = g[Mat::Concrete];
            m.uvTile = {2.f, 2.f};
            if (kind == 15) {
                int n = 16;
                for (int i = 0; i < n; ++i) m.box({-W / 2, 0, i * L / n}, {W, H * (i + 1) / n, L / n + 0.001f});
            } else {
                m.quad({W / 2, 0, 0}, {-W / 2, 0, 0}, {-W / 2, H, L}, {W / 2, H, L});
                m.quad({-W / 2, 0, L}, {W / 2, 0, L}, {W / 2, H, L}, {-W / 2, H, L});
                m.tri({-W / 2, 0, 0}, {-W / 2, 0, L}, {-W / 2, H, L}, {0, 0}, {1, 0}, {1, 1});
                m.tri({W / 2, 0, 0}, {W / 2, H, L}, {W / 2, 0, L}, {0, 0}, {1, 1}, {1, 0});
            }
            float th = std::atan2(H, L), len = std::sqrt(H * H + L * L);
            glm::vec3 nrm(0.f, std::cos(th), -std::sin(th));
            g.colliderTilted(glm::vec3(0.f, H * 0.5f, L * 0.5f) - nrm * 0.15f, {W * 0.5f, 0.15f, len * 0.5f}, -th);
            g.collider({-W / 2, 0, L - 0.2f}, {W, H, 0.2f});
            name = kind == 15 ? "Escada" : "Rampa";
            break;
        }
        case 17: {  // piso / laje (plataforma)
            MeshBuilder& m = g[Mat::Concrete];
            m.uvTile = {2.f, 2.f};
            m.box({-3.f, 0, -3.f}, {6.f, 0.3f, 6.f});
            g.collider({-3.f, 0, -3.f}, {6.f, 0.3f, 6.f});
            name = "Piso";
            break;
        }
        case 18: {  // parede de reboco
            MeshBuilder& m = g[Mat::Plaster];
            m.color = housePaint(r);
            m.uvTile = {2.f, 2.f};
            m.box({-2.f, 0, -0.1f}, {4.f, 3.f, 0.2f});
            g.collider({-2.f, 0, -0.1f}, {4.f, 3.f, 0.2f});
            name = "Parede";
            break;
        }
        case 19: {  // cerca de madeira
            MeshBuilder& m = g[Mat::Wood];
            for (int i = 0; i <= 4; ++i) m.box({-2.f + i - 0.05f, 0, -0.05f}, {0.1f, 1.2f, 0.1f});
            m.box({-2.f, 0.35f, -0.03f}, {4.f, 0.1f, 0.06f});
            m.box({-2.f, 0.9f, -0.03f}, {4.f, 0.1f, 0.06f});
            g.collider({-2.f, 0, -0.1f}, {4.f, 1.2f, 0.2f});
            name = "Cerca";
            break;
        }
        case 20: {  // banco de praça
            MeshBuilder& w = g[Mat::Wood];
            for (int i = 0; i < 4; ++i) w.box({-0.9f, 0.42f, -0.25f + i * 0.13f}, {1.8f, 0.04f, 0.1f});
            for (int i = 0; i < 3; ++i) w.box({-0.9f, 0.55f + i * 0.13f, 0.27f}, {1.8f, 0.1f, 0.04f});
            MeshBuilder& m = g[Mat::GunMetal];
            for (float x : {-0.8f, 0.75f}) { m.box({x, 0, -0.25f}, {0.05f, 0.42f, 0.05f}); m.box({x, 0, 0.25f}, {0.05f, 0.95f, 0.05f}); }
            g.collider({-0.9f, 0, -0.3f}, {1.8f, 0.5f, 0.6f});
            name = "Banco";
            break;
        }
        case 21: buildWaterTank(g[Mat::Painted], glm::vec3(0.f)); g.collider({-0.7f, 0, -0.7f}, {1.4f, 1.4f, 1.4f}); name = "Caixa d'agua"; break;
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
    if (e->mapKind == MAP_Planet) {  // planeta: carrega por pedaços; visão de cima da região atual
        glm::vec3 t = e->editorCam.target;
        e->editorCam.target = {t.x, mapHeight(e, t.x, t.z), t.z};
        e->editorCam.distance = 320.f;
    } else if (e->hasBounds) {
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

void spawnPrototype(Engine* e, const Prototype& p, glm::vec3 pos, float yaw, float scale, std::vector<Scene::Handle>* out,
                    std::vector<BoxCollider>* colliders) {
    spawnProto(e, p, pos, yaw, scale, out, colliders);
}

// Grafo de caminhos da favela para os robôs: escadarias, patamares, a rua de acesso e a rua em volta do morro
void favelaNavGraph(Engine* e, std::vector<glm::vec3>& nodes, std::vector<std::vector<int>>& adj) {
    nodes.clear(); adj.clear();
    auto node = [&](glm::vec2 p) {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (glm::length(glm::vec2(nodes[i].x, nodes[i].z) - p) < 2.5f) return int(i);
        nodes.push_back({p.x, mapHeight(e, p.x, p.y), p.y});
        adj.emplace_back();
        return int(nodes.size()) - 1;
    };
    auto link = [&](int a, int b) {
        if (a == b) return;
        if (std::find(adj[a].begin(), adj[a].end(), b) == adj[a].end()) { adj[a].push_back(b); adj[b].push_back(a); }
    };
    auto chain = [&](const std::vector<glm::vec2>& pts) {
        int prev = -1;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            int n = std::max(1, int(glm::length(pts[i + 1] - pts[i]) / 5.f));
            for (int k = 0; k <= n; ++k) {
                if (i > 0 && k == 0) continue;
                int cur = node(glm::mix(pts[i], pts[i + 1], float(k) / float(n)));
                if (prev >= 0) link(prev, cur);
                prev = cur;
            }
        }
    };
    for (const auto& rt : favelaRoutes()) chain(rt);
    chain({{0.f, 49.f}, {0.f, 96.f}});  // rua de acesso principal
    // rua em volta do morro
    std::vector<glm::vec2> ring;
    for (int i = 0; i <= 64; ++i) {
        float a = float(i) / 64.f * 6.2831853f, c = std::cos(a), sn = std::sin(a);
        float r = 1.56f / std::pow(std::pow(std::abs(c) / 64.f, 4.f) + std::pow(std::abs(sn) / 62.f, 4.f), 0.25f);
        ring.push_back({c * r, sn * r});
    }
    chain(ring);
    // patamares: o centro liga nas escadarias que chegam nele
    const FavelaPad* pads = favelaPads(e->mapSeed);
    const size_t routeNodes = nodes.size();
    for (int k = 0; k < FP_Count; ++k) {
        int c = node(pads[k].c);
        for (size_t i = 0; i < routeNodes; ++i)
            if (padDist(pads[k], nodes[i].x, nodes[i].z) < 3.f) link(c, int(i));
    }
    // pontas soltas (fim de escadaria na rua): liga no nó mais perto
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (adj[i].size() > 1) continue;
        int best = -1; float bd = 9.f;
        for (size_t j = 0; j < nodes.size(); ++j) {
            if (j == i || std::find(adj[i].begin(), adj[i].end(), int(j)) != adj[i].end()) continue;
            float d = glm::length(glm::vec2(nodes[j].x - nodes[i].x, nodes[j].z - nodes[i].z));
            if (d < bd) { bd = d; best = int(j); }
        }
        if (best >= 0) link(int(i), best);
    }
    // bases dos times: entram no grafo ligadas ao nó mais perto
    std::vector<glm::vec3> pts;
    for (int t = 0; t < 2; ++t) { pts.push_back(e->match.base[t]); for (const glm::vec3& sp : e->match.spawns[t]) pts.push_back(sp); }
    for (const glm::vec3& b : pts) {
        int best = -1; float bd = 1e9f;
        for (size_t j = 0; j < nodes.size(); ++j) {
            float d = glm::length(glm::vec2(nodes[j].x - b.x, nodes[j].z - b.z));
            if (d < bd) { bd = d; best = int(j); }
        }
        int bn = node({b.x, b.z});
        if (best >= 0) link(bn, best);
    }
}

}  // namespace eng
