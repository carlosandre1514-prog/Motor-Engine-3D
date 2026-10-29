// Converte um personagem .fbx com esqueleto (Mixamo/Fuse) para um .glb leve para o celular:
// metros e Y para cima, esqueleto e pesos mantidos, malha reduzida (meshoptimizer), texturas em .jpg.
// Também grava o mapa de regiões do corpo na textura (cabeça, tronco, braço, mão, perna, pé...), usado para
// pintar as roupas por cima da pele.
// Uso: personagem_fbx entrada.fbx saida.glb [opções]
//   --texturas PASTA   onde procurar as imagens (pelo nome do arquivo)
//   --tex N            lado máximo das texturas (padrão 1024)
//   --reduzir R        fração dos triângulos que fica (padrão 0.2; colapso de arestas por quádricas, bordas presas)
//   --regioes ARQ.png  grava o mapa de regiões (R = região, G = lado: 0 meio, 1 esquerdo, 2 direito)
//   --posicoes ARQ.png grava onde cada pixel da textura fica no corpo (RGB = x, y, z na caixa do corpo; .txt com a caixa)
//   --sem-cilios       tira os triângulos que usam a parte transparente da textura (cílios): textura sem alfa, .jpg
//   --roupa PECAS.png COR.jpg  personagem vestido: a pele que fica à mostra (o corpo coberto sai) e a ROUPA 3D: as partes do corpo marcadas na máscara (R = peça*40:
//                      1 camisa, 2 calça, 3 bota/tênis, 4 luva, 5 balaclava) viram uma casca afastada do corpo, com a
//                      borda fechada (espessura nas barras e golas), os mesmos ossos e pesos, e a textura COR.jpg
//   --cor ARQ          usa esta imagem como cor do corpo (roupa já pintada) em vez da textura do FBX
#include <ufbx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <queue>
#include <tuple>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }
static std::string baseName(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    size_t k = p.find_last_of('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}
struct Img { int w = 0, h = 0; std::vector<unsigned char> px; };  // RGBA
static bool loadImg(const std::string& path, Img& out) {
    int c;
    unsigned char* d = stbi_load(path.c_str(), &out.w, &out.h, &c, 4);
    if (!d) return false;
    out.px.assign(d, d + size_t(out.w) * out.h * 4);
    stbi_image_free(d);
    return true;
}
static Img shrink(const Img& in, int maxSide) {
    int f = 1;
    while (std::max(in.w, in.h) / f > maxSide) f *= 2;
    if (f == 1) return in;
    Img o; o.w = in.w / f; o.h = in.h / f; o.px.resize(size_t(o.w) * o.h * 4);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x)
            for (int c = 0; c < 4; ++c) {
                unsigned s = 0;
                for (int j = 0; j < f; ++j)
                    for (int i = 0; i < f; ++i) s += in.px[(size_t(y * f + j) * in.w + (x * f + i)) * 4 + c];
                o.px[(size_t(y) * o.w + x) * 4 + c] = (unsigned char)(s / (f * f));
            }
    return o;
}
static std::vector<unsigned char> encode(const Img& im, bool png, int quality) {
    std::vector<unsigned char> out;
    auto sink = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<unsigned char>*>(ctx);
        v->insert(v->end(), (unsigned char*)data, (unsigned char*)data + size);
    };
    if (png) { stbi_write_png_to_func(sink, &out, im.w, im.h, 4, im.px.data(), im.w * 4); return out; }
    std::vector<unsigned char> rgb(size_t(im.w) * im.h * 3);
    for (size_t i = 0; i < size_t(im.w) * im.h; ++i) for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = im.px[i * 4 + c];
    stbi_write_jpg_to_func(sink, &out, im.w, im.h, 3, rgb.data(), quality);
    return out;
}
static glm::mat4 toGlm(const ufbx_matrix& m) {
    glm::mat4 r(1.f);
    for (int c = 0; c < 4; ++c) for (int k = 0; k < 3; ++k) r[c][k] = float(m.cols[c].v[k]);
    return r;
}

// Região do corpo pelo nome do osso (Mixamo)
enum { R_Head = 1, R_Neck, R_Hips, R_Torso, R_UpperArm, R_ForeArm, R_Hand, R_Thigh, R_Shin, R_Foot };
static int regionOf(const std::string& bone, int& side) {
    std::string n = bone.substr(bone.find(':') == std::string::npos ? 0 : bone.find(':') + 1);
    side = n.rfind("Left", 0) == 0 ? 1 : n.rfind("Right", 0) == 0 ? 2 : 0;
    if (n.find("Head") != std::string::npos || n.find("Eye") != std::string::npos) return R_Head;
    if (n.find("Neck") != std::string::npos) return R_Neck;
    if (n.find("Hand") != std::string::npos) return R_Hand;
    if (n.find("ForeArm") != std::string::npos) return R_ForeArm;
    if (n.find("Arm") != std::string::npos || n.find("Shoulder") != std::string::npos) return R_UpperArm;
    if (n.find("UpLeg") != std::string::npos) return R_Thigh;
    if (n.find("Foot") != std::string::npos || n.find("Toe") != std::string::npos) return R_Foot;
    if (n.find("Leg") != std::string::npos) return R_Shin;
    if (n.find("Hips") != std::string::npos) return R_Hips;
    return R_Torso;
}

struct Vtx { float p[3], n[3], uv[2]; uint16_t j[4]; float w[4]; };
struct Prim { const ufbx_material* mat = nullptr; std::vector<Vtx> v; std::vector<uint32_t> idx; };


#include "simplificar.h"

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "uso: personagem_fbx entrada.fbx saida.glb [opções]\n"); return 1; }
    std::vector<std::string> texDirs;
    int texSize = 1024;
    float keep = 0.2f;
    std::string regionsOut, colorOverride, posOut;
    bool noLashes = false;
    std::string garmentMask, garmentColor;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--texturas" && i + 1 < argc) texDirs.push_back(argv[++i]);
        else if (a == "--tex" && i + 1 < argc) texSize = std::atoi(argv[++i]);
        else if (a == "--reduzir" && i + 1 < argc) keep = float(std::atof(argv[++i]));
        else if (a == "--regioes" && i + 1 < argc) regionsOut = argv[++i];
        else if (a == "--cor" && i + 1 < argc) colorOverride = argv[++i];
        else if (a == "--posicoes" && i + 1 < argc) posOut = argv[++i];
        else if (a == "--sem-cilios") noLashes = true;
        else if (a == "--roupa" && i + 2 < argc) { garmentMask = argv[++i]; garmentColor = argv[++i]; }
    }
    if (texDirs.empty()) texDirs.push_back(fs::path(argv[1]).parent_path().string());
    auto find = [&](const std::string& file) -> std::string {
        std::string want = lower(baseName(file));
        for (auto& d : texDirs) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file() && lower(it->path().filename().string()) == want) return it->path().string();
        }
        return {};
    };

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(argv[1], &opts, &err);
    if (!scene) { std::fprintf(stderr, "erro: %s\n", err.description.data); return 1; }

    // ---- ossos: os que pesam na malha e os pais deles; pose de repouso = a do bind
    std::vector<const ufbx_node*> bones;
    std::unordered_map<const ufbx_node*, int> boneIdx;
    std::unordered_map<const ufbx_node*, glm::mat4> bindWorld;
    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        const ufbx_mesh* mesh = scene->meshes.data[mi];
        if (!mesh->skin_deformers.count) continue;
        const ufbx_skin_deformer* sk = mesh->skin_deformers.data[0];
        for (size_t c = 0; c < sk->clusters.count; ++c) {
            const ufbx_skin_cluster* cl = sk->clusters.data[c];
            if (cl->bone_node) bindWorld[cl->bone_node] = toGlm(cl->bind_to_world);
        }
    }
    if (bindWorld.empty()) { std::fprintf(stderr, "o modelo não tem esqueleto (skin)\n"); return 1; }
    std::function<void(const ufbx_node*)> addBone = [&](const ufbx_node* n) {
        if (!n || n->is_root || boneIdx.count(n)) return;
        addBone(n->parent);
        boneIdx[n] = int(bones.size());
        bones.push_back(n);
    };
    for (auto& [n, m] : bindWorld) addBone(n);
    auto worldOf = [&](const ufbx_node* n) -> glm::mat4 {
        auto it = bindWorld.find(n);
        return it != bindWorld.end() ? it->second : toGlm(n->node_to_world);
    };

    // caixa (em UV) da parte transparente da textura: os triângulos dela saem com --sem-cilios
    float cutBox[4] = {2.f, 2.f, -1.f, -1.f};
    if (noLashes)
        for (size_t mi = 0; mi < scene->materials.count && cutBox[0] > 1.f; ++mi) {
            const ufbx_material* mat = scene->materials.data[mi];
            const ufbx_texture* t = mat->fbx.diffuse_color.texture ? mat->fbx.diffuse_color.texture : mat->pbr.base_color.texture;
            if (!t) continue;
            std::string al = find(t->relative_filename.length ? t->relative_filename.data : t->filename.data);
            size_t k = al.find("_Diffuse");
            if (al.empty() || k == std::string::npos) continue;
            std::string op = al; op.replace(k, 8, "_Opacity");
            Img o;
            if (!loadImg(op, o)) continue;
            for (int y = 0; y < o.h; ++y)
                for (int x = 0; x < o.w; ++x)
                    if (o.px[(size_t(y) * o.w + x) * 4] < 128) {
                        float u = (x + 0.5f) / o.w, v = (y + 0.5f) / o.h;
                        cutBox[0] = std::min(cutBox[0], u); cutBox[1] = std::min(cutBox[1], v);
                        cutBox[2] = std::max(cutBox[2], u); cutBox[3] = std::max(cutBox[3], v);
                    }
        }
    size_t lashTris = 0;
    // ---- malhas: vértices no mundo (pose de repouso), 4 pesos por vértice
    std::map<const ufbx_material*, Prim> prims;
    const int RW = 1024;
    std::vector<unsigned char> regions(size_t(RW) * RW * 4, 0);
    std::vector<float> posMap(size_t(RW) * RW * 3, 0.f);
    std::vector<char> posHas(size_t(RW) * RW, 0);
    size_t trisIn = 0;
    std::vector<uint32_t> tri(64);
    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        const ufbx_mesh* mesh = scene->meshes.data[mi];
        if (!mesh->skin_deformers.count || !mesh->instances.count) continue;
        const ufbx_skin_deformer* sk = mesh->skin_deformers.data[0];
        glm::mat4 g2w = toGlm(mesh->instances.data[0]->geometry_to_world);
        glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(g2w)));
        for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
            ufbx_face face = mesh->faces.data[fi];
            const ufbx_material* mat = nullptr;
            if (mesh->face_material.count && mesh->materials.count) {
                uint32_t k = mesh->face_material.data[fi];
                if (k < mesh->materials.count) mat = mesh->materials.data[k];
            }
            Prim& P = prims[mat];
            P.mat = mat;
            uint32_t nt = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
            for (uint32_t t = 0; t < nt; ++t) {
                Vtx tv[3];
                int reg[3] = {0, 0, 0}, sideOf[3] = {0, 0, 0};
                for (int c = 0; c < 3; ++c) {
                    uint32_t ix = tri[t * 3 + c];
                    Vtx v{};
                    glm::vec3 p = g2w * glm::vec4(float(ufbx_get_vertex_vec3(&mesh->vertex_position, ix).x),
                                                  float(ufbx_get_vertex_vec3(&mesh->vertex_position, ix).y),
                                                  float(ufbx_get_vertex_vec3(&mesh->vertex_position, ix).z), 1.f);
                    glm::vec3 n(0, 1, 0);
                    if (mesh->vertex_normal.exists) {
                        ufbx_vec3 q = ufbx_get_vertex_vec3(&mesh->vertex_normal, ix);
                        n = glm::normalize(nm * glm::vec3(float(q.x), float(q.y), float(q.z)));
                    }
                    if (mesh->vertex_uv.exists) {
                        ufbx_vec2 q = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                        v.uv[0] = float(q.x); v.uv[1] = 1.f - float(q.y);
                    }
                    for (int a = 0; a < 3; ++a) { v.p[a] = p[a]; v.n[a] = n[a]; }
                    // pesos: os 4 maiores, normalizados
                    uint32_t vert = mesh->vertex_indices.data[ix];
                    std::vector<std::pair<float, int>> ws;
                    if (vert < sk->vertices.count) {
                        ufbx_skin_vertex sv = sk->vertices.data[vert];
                        for (uint32_t k = 0; k < sv.num_weights; ++k) {
                            ufbx_skin_weight w = sk->weights.data[sv.weight_begin + k];
                            const ufbx_node* b = sk->clusters.data[w.cluster_index]->bone_node;
                            if (b && boneIdx.count(b)) ws.push_back({float(w.weight), boneIdx[b]});
                        }
                    }
                    std::sort(ws.begin(), ws.end(), [](auto& a, auto& b) { return a.first > b.first; });
                    if (ws.size() > 4) ws.resize(4);
                    float sum = 0.f;
                    for (auto& w : ws) sum += w.first;
                    for (size_t k = 0; k < 4; ++k) {
                        v.j[k] = k < ws.size() ? uint16_t(ws[k].second) : 0;
                        v.w[k] = k < ws.size() && sum > 0.f ? ws[k].first / sum : (k == 0 && ws.empty() ? 1.f : 0.f);
                    }
                    if (!ws.empty()) reg[c] = regionOf(bones[ws[0].second]->name.data, sideOf[c]);
                    tv[c] = v;
                }
                {
                    float cu = (tv[0].uv[0] + tv[1].uv[0] + tv[2].uv[0]) / 3.f, cv = (tv[0].uv[1] + tv[1].uv[1] + tv[2].uv[1]) / 3.f;
                    if (cu >= cutBox[0] && cu <= cutBox[2] && cv >= cutBox[1] && cv <= cutBox[3]) { ++lashTris; continue; }
                }
                for (int c = 0; c < 3; ++c) { P.idx.push_back(uint32_t(P.v.size())); P.v.push_back(tv[c]); }
                ++trisIn;
                // mapa de regiões: a região que mais aparece nos 3 cantos
                int r = reg[0], s = sideOf[0];
                if (reg[1] == reg[2]) { r = reg[1]; s = sideOf[1]; }
                if ((!regionsOut.empty() || !posOut.empty()) && r) {
                    glm::vec2 a(tv[0].uv[0] * RW, tv[0].uv[1] * RW), b(tv[1].uv[0] * RW, tv[1].uv[1] * RW), cc(tv[2].uv[0] * RW, tv[2].uv[1] * RW);
                    int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, cc.x}))) - 1), x1 = std::min(RW - 1, int(std::ceil(std::max({a.x, b.x, cc.x}))) + 1);
                    int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, cc.y}))) - 1), y1 = std::min(RW - 1, int(std::ceil(std::max({a.y, b.y, cc.y}))) + 1);
                    float area = (b.x - a.x) * (cc.y - a.y) - (b.y - a.y) * (cc.x - a.x);
                    if (std::abs(area) < 1e-9f) continue;
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x) {
                            glm::vec2 p(x + 0.5f, y + 0.5f);
                            float w0 = ((b.x - p.x) * (cc.y - p.y) - (b.y - p.y) * (cc.x - p.x)) / area;
                            float w1 = ((cc.x - p.x) * (a.y - p.y) - (cc.y - p.y) * (a.x - p.x)) / area;
                            float w2 = 1.f - w0 - w1;
                            const float e = -0.02f;
                            if (w0 >= e && w1 >= e && w2 >= e) {
                                unsigned char* q = &regions[(size_t(y) * RW + x) * 4];
                                q[0] = (unsigned char)(r * 20); q[1] = (unsigned char)(s * 100); q[2] = 0; q[3] = 255;
                                size_t k = size_t(y) * RW + x;
                                for (int a3 = 0; a3 < 3; ++a3) posMap[k * 3 + a3] = w0 * tv[0].p[a3] + w1 * tv[1].p[a3] + w2 * tv[2].p[a3];
                                posHas[k] = 1;
                            }
                        }
                }
            }
        }
    }

    // ---- roupa 3D: casca afastada do corpo onde a máscara marca uma peça
    if (!garmentMask.empty()) {
        Img mask;
        if (!loadImg(garmentMask, mask)) { std::fprintf(stderr, "não consegui abrir %s\n", garmentMask.c_str()); return 1; }
        auto clsAt = [&](float u, float v) {
            int x = std::clamp(int(u * mask.w), 0, mask.w - 1), y = std::clamp(int(v * mask.h), 0, mask.h - 1);
            return mask.px[(size_t(y) * mask.w + x) * 4] / 40;
        };
        // folga de cada peça (m): calça e bota mais largas; luva e balaclava justas
        const float gap[6] = {0.f, 0.012f, 0.016f, 0.016f, 0.004f, 0.0045f};
        Prim G;
        G.mat = nullptr;
        std::vector<int> triCls;
        Prim skinP;  // pele à mostra (com o material do corpo)
        for (auto& [m, P] : prims)
            for (size_t t = 0; t + 2 < P.idx.size(); t += 3) {
                const Vtx& a = P.v[P.idx[t]], & b = P.v[P.idx[t + 1]], & c = P.v[P.idx[t + 2]];
                int k = 0, cnt[6] = {};
                for (const Vtx* q : {&a, &b, &c}) { int cc = clsAt(q->uv[0], q->uv[1]); if (cc > 0 && cc < 6) ++cnt[cc]; }
                int cc = clsAt((a.uv[0] + b.uv[0] + c.uv[0]) / 3.f, (a.uv[1] + b.uv[1] + c.uv[1]) / 3.f);
                if (cc > 0 && cc < 6) ++cnt[cc];
                for (int q = 1; q < 6; ++q) if (cnt[q] >= 2 && cnt[q] > cnt[k]) k = q;
                int covered = 0;
                for (int q = 1; q < 6; ++q) covered += cnt[q];
                if (covered < 4) {  // pele (ou só um pedaço coberto): o corpo fica
                    skinP.mat = m;
                    for (const Vtx* q : {&a, &b, &c}) { skinP.idx.push_back(uint32_t(skinP.v.size())); skinP.v.push_back(*q); }
                }
                if (!k) continue;
                for (const Vtx* q : {&a, &b, &c}) { G.idx.push_back(uint32_t(G.v.size())); G.v.push_back(*q); }
                triCls.push_back(k);
            }
        // normal suave por posição (os vértices repetidos nas costuras de UV andam juntos: sem rachar a casca)
        auto key = [](const float* p) { return std::make_tuple(std::lround(p[0] * 2e4), std::lround(p[1] * 2e4), std::lround(p[2] * 2e4)); };
        std::map<std::tuple<long, long, long>, glm::vec3> nsum;
        for (auto& v : G.v) nsum[key(v.p)] += glm::make_vec3(v.n);
        std::vector<Vtx> base = G.v;  // na pele (para fechar as bordas)
        for (size_t i = 0; i < G.v.size(); ++i) {
            glm::vec3 n = glm::normalize(nsum[key(G.v[i].p)]);
            float g = gap[triCls[i / 3]];
            for (int a3 = 0; a3 < 3; ++a3) { G.v[i].p[a3] += n[a3] * g; G.v[i].n[a3] = n[a3]; }
        }
        // tecido não marca músculo: alisa a casca (média dos vizinhos), sem deixar entrar no corpo
        {
            std::map<std::tuple<long, long, long>, int> node;
            std::vector<int> nodeOf(G.v.size());
            std::vector<glm::vec3> np, nb, nn;
            std::vector<float> ng;
            for (size_t i = 0; i < G.v.size(); ++i) {
                auto k = key(base[i].p);
                auto it = node.find(k);
                if (it == node.end()) {
                    it = node.emplace(k, int(np.size())).first;
                    np.push_back(glm::make_vec3(G.v[i].p)); nb.push_back(glm::make_vec3(base[i].p)); nn.push_back(glm::make_vec3(G.v[i].n));
                    ng.push_back(gap[triCls[i / 3]]);
                }
                nodeOf[i] = it->second;
                ng[it->second] = std::max(ng[it->second], gap[triCls[i / 3]]);
            }
            std::vector<std::set<int>> nbr(np.size());
            for (size_t t = 0; t < G.idx.size() / 3; ++t)
                for (int e = 0; e < 3; ++e) {
                    int a = nodeOf[t * 3 + e], b = nodeOf[t * 3 + (e + 1) % 3];
                    if (a != b) { nbr[a].insert(b); nbr[b].insert(a); }
                }
            for (int it = 0; it < 8; ++it) {
                std::vector<glm::vec3> nx = np;
                for (size_t i = 0; i < np.size(); ++i) {
                    if (nbr[i].empty()) continue;
                    glm::vec3 avg(0.f);
                    for (int j : nbr[i]) avg += np[j];
                    avg /= float(nbr[i].size());
                    glm::vec3 q = glm::mix(np[i], avg, 0.6f);
                    float out = glm::dot(q - nb[i], nn[i]);  // não entra no corpo (folga mínima de 60%)
                    if (out < ng[i] * 0.6f) q += nn[i] * (ng[i] * 0.6f - out);
                    nx[i] = q;
                }
                np.swap(nx);
            }
            for (size_t i = 0; i < G.v.size(); ++i) for (int a3 = 0; a3 < 3; ++a3) G.v[i].p[a3] = np[nodeOf[i]][a3];
            // normais de novo pela casca alisada
            std::map<std::tuple<long, long, long>, glm::vec3> ns;
            for (size_t t = 0; t < G.idx.size() / 3; ++t) {
                glm::vec3 p0 = glm::make_vec3(G.v[t * 3].p), p1 = glm::make_vec3(G.v[t * 3 + 1].p), p2 = glm::make_vec3(G.v[t * 3 + 2].p);
                glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
                for (int c3 = 0; c3 < 3; ++c3) ns[key(base[t * 3 + c3].p)] += fn;
            }
            for (size_t i = 0; i < G.v.size(); ++i) {
                glm::vec3 n = ns[key(base[i].p)];
                if (glm::length(n) > 1e-12f) { n = glm::normalize(n); G.v[i].n[0] = n.x; G.v[i].n[1] = n.y; G.v[i].n[2] = n.z; }
            }
        }
        // borda de cada peça: aresta que só um triângulo daquela peça usa -> faixa até a pele (espessura)
        std::map<std::tuple<long, long, long, long, long, long, int>, int> edgeUse;
        auto ekey = [&](size_t i, size_t j, int k) {
            auto a = key(base[i].p), b = key(base[j].p);
            if (b < a) std::swap(a, b);
            return std::make_tuple(std::get<0>(a), std::get<1>(a), std::get<2>(a), std::get<0>(b), std::get<1>(b), std::get<2>(b), k);
        };
        size_t tris = G.idx.size() / 3;
        for (size_t t = 0; t < tris; ++t)
            for (int e = 0; e < 3; ++e) ++edgeUse[ekey(t * 3 + e, t * 3 + (e + 1) % 3, triCls[t])];
        size_t rims = 0;
        for (size_t t = 0; t < tris; ++t)
            for (int e = 0; e < 3; ++e) {
                size_t i = t * 3 + e, j = t * 3 + (e + 1) % 3;
                if (edgeUse[ekey(i, j, triCls[t])] != 1) continue;
                Vtx o0 = G.v[i], o1 = G.v[j], b0 = base[i], b1 = base[j];
                // entra 2 mm na pele para não abrir fresta quando o corpo dobra
                for (int a3 = 0; a3 < 3; ++a3) { b0.p[a3] -= b0.n[a3] * 0.002f; b1.p[a3] -= b1.n[a3] * 0.002f; }
                b0.n[0] = o0.n[0]; b0.n[1] = o0.n[1]; b0.n[2] = o0.n[2];
                b1.n[0] = o1.n[0]; b1.n[1] = o1.n[1]; b1.n[2] = o1.n[2];
                uint32_t s0 = uint32_t(G.v.size());
                G.v.push_back(o0); G.v.push_back(o1); G.v.push_back(b1); G.v.push_back(b0);
                for (uint32_t q : {0u, 2u, 1u, 0u, 3u, 2u}) G.idx.push_back(s0 + q);
                ++rims;
            }
        std::printf("  roupa: %zu triângulos de peça, %zu de borda\n", tris, rims * 2);
        std::printf("  pele à mostra: %zu triângulos\n", skinP.idx.size() / 3);
        const ufbx_material* skinMat = skinP.mat;
        prims.clear();
        prims[nullptr] = std::move(G);
        if (!skinP.idx.empty()) prims[skinMat] = std::move(skinP);
        colorOverride = garmentColor;  // a pele também usa a textura do visual (tatuagens, barba)
        keep = 1.f;
    }

    // ---- junta vértices iguais e reduz a malha (as costuras de UV ficam presas: sem rachaduras)
    size_t trisOut = 0;
    for (auto& [m, P] : prims) {
        // cabeça (rosto, olhos, boca) fica com todos os detalhes; o resto do corpo é reduzido
        auto isHead = [&](const Vtx& v) { int sd; int r = regionOf(bones[v.j[0]]->name.data, sd); return r == R_Head; };
        simplify(P.v, P.idx, std::max<size_t>(1, size_t(P.idx.size() / 3 * keep)), isHead);
        trisOut += P.idx.size() / 3;
    }
    if (!regionsOut.empty()) {
        // alarga 4 pixels para cobrir as bordas das ilhas de UV
        for (int pass = 0; pass < 4; ++pass) {
            std::vector<unsigned char> nx = regions;
            for (int y = 1; y < RW - 1; ++y)
                for (int x = 1; x < RW - 1; ++x) {
                    if (regions[(size_t(y) * RW + x) * 4 + 3]) continue;
                    for (int d = 0; d < 4; ++d) {
                        int xx = x + (d == 0) - (d == 1), yy = y + (d == 2) - (d == 3);
                        const unsigned char* q = &regions[(size_t(yy) * RW + xx) * 4];
                        if (q[3]) { std::memcpy(&nx[(size_t(y) * RW + x) * 4], q, 4); break; }
                    }
                }
            regions.swap(nx);
        }
        if (!regionsOut.empty()) stbi_write_png(regionsOut.c_str(), RW, RW, 4, regions.data(), RW * 4);
    }
    if (!posOut.empty()) {
        float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
        for (size_t k = 0; k < posHas.size(); ++k) if (posHas[k]) for (int a = 0; a < 3; ++a) { mn[a] = std::min(mn[a], posMap[k * 3 + a]); mx[a] = std::max(mx[a], posMap[k * 3 + a]); }
        std::vector<unsigned char> px(size_t(RW) * RW * 4, 0);
        for (size_t k = 0; k < posHas.size(); ++k) {
            if (!posHas[k]) continue;
            for (int a = 0; a < 3; ++a) px[k * 4 + a] = (unsigned char)std::lround(255.f * (posMap[k * 3 + a] - mn[a]) / std::max(1e-6f, mx[a] - mn[a]));
            px[k * 4 + 3] = 255;
        }
        stbi_write_png(posOut.c_str(), RW, RW, 4, px.data(), RW * 4);
        if (FILE* f = std::fopen((posOut + ".txt").c_str(), "w")) {
            std::fprintf(f, "%f %f %f\n%f %f %f\n", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
            for (auto* n : bones) { glm::vec3 p = glm::vec3(worldOf(n)[3]); std::fprintf(f, "%s %f %f %f\n", n->name.data, p.x, p.y, p.z); }
            std::fclose(f);
        }
    }

    // ---- glTF
    std::vector<unsigned char> bin;
    json g;
    g["asset"] = {{"version", "2.0"}, {"generator", "Motor 3D personagem_fbx"}};
    g["scene"] = 0;
    g["samplers"] = json::array({{{"magFilter", 9729}, {"minFilter", 9987}}});
    auto view = [&](const void* data, size_t size, int target) {
        while (bin.size() % 4) bin.push_back(0);
        json v = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", size}};
        if (target) v["target"] = target;
        bin.insert(bin.end(), (const unsigned char*)data, (const unsigned char*)data + size);
        g["bufferViews"].push_back(v);
        return int(g["bufferViews"].size() - 1);
    };
    std::map<std::string, int> imageOf;
    auto image = [&](const std::string& key, const Img& im, bool png) {
        auto it = imageOf.find(key);
        if (it != imageOf.end()) return it->second;
        auto data = encode(shrink(im, texSize), png, key.find("normal") != std::string::npos ? 92 : 88);
        g["images"].push_back({{"bufferView", view(data.data(), data.size(), 0)}, {"mimeType", png ? "image/png" : "image/jpeg"}});
        g["textures"].push_back({{"sampler", 0}, {"source", int(g["images"].size() - 1)}});
        return imageOf[key] = int(g["textures"].size() - 1);
    };
    // nós dos ossos (pose de repouso local) + nó da malha
    for (size_t b = 0; b < bones.size(); ++b) {
        const ufbx_node* n = bones[b];
        glm::mat4 local = worldOf(n);
        if (n->parent && boneIdx.count(n->parent)) local = glm::inverse(worldOf(n->parent)) * local;
        glm::vec3 sc, tr, skew; glm::quat rot; glm::vec4 persp;
        glm::decompose(local, sc, rot, tr, skew, persp);
        json node = {{"name", n->name.data}, {"translation", {tr.x, tr.y, tr.z}}, {"rotation", {rot.x, rot.y, rot.z, rot.w}}};
        if (glm::length(sc - glm::vec3(1.f)) > 1e-4f) node["scale"] = {sc.x, sc.y, sc.z};
        g["nodes"].push_back(node);
    }
    for (size_t b = 0; b < bones.size(); ++b) {
        std::vector<int> kids;
        for (size_t c = 0; c < bones.size(); ++c) if (bones[c]->parent == bones[b]) kids.push_back(int(c));
        if (!kids.empty()) g["nodes"][b]["children"] = kids;
    }
    std::vector<float> ibm;
    for (auto* n : bones) { glm::mat4 inv = glm::inverse(worldOf(n)); ibm.insert(ibm.end(), glm::value_ptr(inv), glm::value_ptr(inv) + 16); }
    int ibmAcc = int(g["accessors"].size());
    g["accessors"].push_back({{"bufferView", view(ibm.data(), ibm.size() * 4, 0)}, {"componentType", 5126}, {"count", bones.size()}, {"type", "MAT4"}});
    std::vector<int> joints(bones.size());
    for (size_t b = 0; b < bones.size(); ++b) joints[b] = int(b);
    int rootBone = 0;
    g["skins"] = json::array({{{"joints", joints}, {"inverseBindMatrices", ibmAcc}, {"skeleton", rootBone}}});

    json primsJson = json::array();
    for (auto& [mat, P] : prims) {
        if (P.idx.empty()) continue;
        std::string albedo, normal, opacity;
        if (mat) {
            const ufbx_texture* t = mat->pbr.base_color.texture ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture;
            const ufbx_texture* nt = mat->fbx.normal_map.texture ? mat->fbx.normal_map.texture : mat->pbr.normal_map.texture;
            const ufbx_texture* ot = mat->fbx.transparency_factor.texture ? mat->fbx.transparency_factor.texture : mat->pbr.opacity.texture;
            if (t) albedo = find(t->relative_filename.length ? t->relative_filename.data : t->filename.data);
            if (nt) normal = find(nt->relative_filename.length ? nt->relative_filename.data : nt->filename.data);
            if (ot) opacity = find(ot->relative_filename.length ? ot->relative_filename.data : ot->filename.data);
            if (!albedo.empty() && opacity.empty()) {
                std::string guess = albedo; size_t k = guess.find("_Diffuse");
                if (k != std::string::npos) { guess.replace(k, 8, "_Opacity"); if (fs::exists(guess)) opacity = guess; }
            }
        }
        if (!colorOverride.empty()) { albedo = colorOverride; if (!garmentMask.empty() && !mat) normal.clear(); }
        json m = {{"name", mat ? (garmentMask.empty() ? mat->name.data : "pele") : garmentMask.empty() ? "corpo" : "roupa"}, {"pbrMetallicRoughness", {{"metallicFactor", 0.f}, {"roughnessFactor", garmentMask.empty() || mat ? 0.7f : 0.9f}}}};
        if (!garmentMask.empty() && !mat) m["doubleSided"] = true;
        Img im, op;
        bool cut = false;
        if (!albedo.empty() && loadImg(albedo, im)) {
            if (!noLashes && !opacity.empty() && loadImg(opacity, op) && op.w == im.w && op.h == im.h) {
                size_t holes = 0;
                for (size_t i = 0; i < size_t(im.w) * im.h; ++i) { im.px[i * 4 + 3] = op.px[i * 4]; holes += op.px[i * 4] < 128; }
                cut = holes > 0;
            }
            m["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", image(albedo + (cut ? "#a" : ""), im, cut)}};
        }
        if (cut) { m["alphaMode"] = "MASK"; m["alphaCutoff"] = 0.5; m["doubleSided"] = true; }
        if (!normal.empty() && loadImg(normal, im)) m["normalTexture"] = {{"index", image(normal + "#normal", im, false)}};
        g["materials"].push_back(m);
        std::printf("  material %-28s cor:%s normal:%s recorte:%s  %zu triângulos\n", mat ? mat->name.data : "-",
                    albedo.empty() ? "não" : "sim", normal.empty() ? "não" : "sim", cut ? "sim" : "não", P.idx.size() / 3);
        std::vector<float> pos, nrm, uv, wt;
        std::vector<uint16_t> jt;
        float pmn[3] = {1e30f, 1e30f, 1e30f}, pmx[3] = {-1e30f, -1e30f, -1e30f};
        for (auto& v : P.v) {
            pos.insert(pos.end(), v.p, v.p + 3); nrm.insert(nrm.end(), v.n, v.n + 3); uv.insert(uv.end(), v.uv, v.uv + 2);
            jt.insert(jt.end(), v.j, v.j + 4); wt.insert(wt.end(), v.w, v.w + 4);
            for (int a = 0; a < 3; ++a) { pmn[a] = std::min(pmn[a], v.p[a]); pmx[a] = std::max(pmx[a], v.p[a]); }
        }
        int cnt = int(P.v.size()), ap = int(g["accessors"].size());
        g["accessors"].push_back({{"bufferView", view(pos.data(), pos.size() * 4, 34962)}, {"componentType", 5126}, {"count", cnt}, {"type", "VEC3"},
                                  {"min", {pmn[0], pmn[1], pmn[2]}}, {"max", {pmx[0], pmx[1], pmx[2]}}});
        g["accessors"].push_back({{"bufferView", view(nrm.data(), nrm.size() * 4, 34962)}, {"componentType", 5126}, {"count", cnt}, {"type", "VEC3"}});
        g["accessors"].push_back({{"bufferView", view(uv.data(), uv.size() * 4, 34962)}, {"componentType", 5126}, {"count", cnt}, {"type", "VEC2"}});
        g["accessors"].push_back({{"bufferView", view(jt.data(), jt.size() * 2, 34962)}, {"componentType", 5123}, {"count", cnt}, {"type", "VEC4"}});
        g["accessors"].push_back({{"bufferView", view(wt.data(), wt.size() * 4, 34962)}, {"componentType", 5126}, {"count", cnt}, {"type", "VEC4"}});
        g["accessors"].push_back({{"bufferView", view(P.idx.data(), P.idx.size() * 4, 34963)}, {"componentType", 5125}, {"count", P.idx.size()}, {"type", "SCALAR"}});
        primsJson.push_back({{"attributes", {{"POSITION", ap}, {"NORMAL", ap + 1}, {"TEXCOORD_0", ap + 2}, {"JOINTS_0", ap + 3}, {"WEIGHTS_0", ap + 4}}},
                             {"indices", ap + 5}, {"material", int(g["materials"].size() - 1)}});
    }
    g["meshes"] = json::array({{{"name", "corpo"}, {"primitives", primsJson}}});
    g["nodes"].push_back({{"name", "corpo"}, {"mesh", 0}, {"skin", 0}});
    int meshNode = int(g["nodes"].size() - 1);
    std::vector<int> roots;
    for (size_t b = 0; b < bones.size(); ++b) if (!bones[b]->parent || !boneIdx.count(bones[b]->parent)) roots.push_back(int(b));
    roots.push_back(meshNode);
    g["scenes"] = json::array({{{"nodes", roots}}});
    while (bin.size() % 4) bin.push_back(0);
    g["buffers"] = json::array({{{"byteLength", bin.size()}}});
    std::string js = g.dump();
    while (js.size() % 4) js += ' ';
    FILE* f = std::fopen(argv[2], "wb");
    if (!f) return 1;
    uint32_t hdr[3] = {0x46546C67, 2, uint32_t(12 + 8 + js.size() + 8 + bin.size())};
    uint32_t jh[2] = {uint32_t(js.size()), 0x4E4F534A}, bh[2] = {uint32_t(bin.size()), 0x004E4942};
    std::fwrite(hdr, 4, 3, f); std::fwrite(jh, 4, 2, f); std::fwrite(js.data(), 1, js.size(), f);
    std::fwrite(bh, 4, 2, f); std::fwrite(bin.data(), 1, bin.size(), f);
    std::fclose(f);
    if (lashTris) std::printf("  cílios: %zu triângulos tirados\n", lashTris);
    std::printf("%s: %zu -> %zu triângulos, %zu ossos, %zu KB\n", argv[2], trisIn, trisOut, bones.size(), (12 + 16 + js.size() + bin.size()) / 1024);
    ufbx_free_scene(scene);
    return 0;
}
