// Converte um modelo .fbx/.obj (com as texturas numa pasta à parte) para um .glb leve para o celular:
// texturas reduzidas em .jpg, rugosidade e metal juntos (ORM do glTF), malhas fixas assadas no lugar.
// Uso: fbx_para_glb entrada.fbx saida.glb [opções]
//   --texturas PASTA   onde procurar as imagens pelo nome do arquivo (procura nas subpastas; pode repetir)
//   --tex N            lado máximo das texturas (padrão 1024)
//   --so x0 x1 z0 z1   só as malhas cujo centro cai nesta área (metros, Y para cima)
//   --sem TEXTO        tira as malhas cujo nome contém TEXTO (pode repetir)
//   --girar GRAUS      gira em volta do eixo vertical (ex.: 180 para o cano ir para +Z)
//   --unico COR NORMAL RUGOSIDADE METAL   mesmas texturas para todos os materiais (FBX sem a ligação)
#include <ufbx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
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

// reduz pela média de blocos (lado máximo `maxSide`)
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

static std::vector<unsigned char> jpg(const Img& im, int quality) {
    std::vector<unsigned char> out;
    std::vector<unsigned char> rgb(size_t(im.w) * im.h * 3);
    for (size_t i = 0; i < size_t(im.w) * im.h; ++i) for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = im.px[i * 4 + c];
    stbi_write_jpg_to_func([](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<unsigned char>*>(ctx);
        v->insert(v->end(), (unsigned char*)data, (unsigned char*)data + size);
    }, &out, im.w, im.h, 3, rgb.data(), quality);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "uso: fbx_para_glb entrada.fbx saida.glb [opções]\n"); return 1; }
    std::vector<std::string> texDirs, without;
    int texSize = 1024;
    bool box = false;
    float bx0 = 0, bx1 = 0, bz0 = 0, bz1 = 0, turn = 0;
    std::string single[4];
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--texturas" && i + 1 < argc) texDirs.push_back(argv[++i]);
        else if (a == "--tex" && i + 1 < argc) texSize = std::atoi(argv[++i]);
        else if (a == "--so" && i + 4 < argc) { box = true; bx0 = std::atof(argv[++i]); bx1 = std::atof(argv[++i]); bz0 = std::atof(argv[++i]); bz1 = std::atof(argv[++i]); }
        else if (a == "--sem" && i + 1 < argc) without.push_back(lower(argv[++i]));
        else if (a == "--girar" && i + 1 < argc) turn = float(std::atof(argv[++i]) * 3.14159265358979 / 180.0);
        else if (a == "--unico" && i + 4 < argc) for (auto& s : single) s = argv[++i];
    }
    // índice das imagens disponíveis (nome do arquivo em minúsculas -> caminho)
    std::map<std::string, std::string> files;
    for (const auto& d : texDirs)
        for (const auto& e : fs::recursive_directory_iterator(d))
            if (e.is_regular_file()) files.emplace(lower(e.path().filename().string()), e.path().string());
    auto find = [&](const std::string& name) -> std::string {
        auto it = files.find(lower(baseName(name)));
        return it == files.end() ? std::string() : it->second;
    };
    // rugosidade/metal com o mesmo começo do nome da cor (…_Albedo / …_BaseColor)
    auto sibling = [&](const std::string& albedo, std::initializer_list<const char*> kinds) -> std::string {
        std::string b = baseName(albedo);
        for (const char* tag : {"_Albedo", "_BaseColor", "_Base_Color", "_Diffuse", "_Color"}) {
            size_t k = lower(b).find(lower(tag));
            if (k == std::string::npos) continue;
            for (const char* kind : kinds)
                for (const char* ext : {".png", ".jpg", ".jpeg", ".tga"}) {
                    std::string p = find(b.substr(0, k) + kind + ext);
                    if (!p.empty()) return p;
                }
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

    // ---- malhas escolhidas: vértices no mundo, girados e depois centralizados
    struct V { float p[3], n[3], uv[2]; };
    struct Prim { std::vector<V> v; std::vector<uint32_t> idx; };
    std::map<const ufbx_material*, Prim> prims;  // uma primitiva por material
    float c = std::cos(turn), s = std::sin(turn);
    auto rot = [&](ufbx_vec3 p) { return std::array<float, 3>{float(c * p.x + s * p.z), float(p.y), float(-s * p.x + c * p.z)}; };
    for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
        const ufbx_node* node = scene->nodes.data[ni];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh) continue;
        std::string nm = lower(node->name.data);
        bool skip = false;
        for (const auto& w : without) skip |= nm.find(w) != std::string::npos;
        if (skip) continue;
        if (box) {
            double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
            for (size_t k = 0; k < mesh->vertex_position.values.count; ++k) {
                ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_world, mesh->vertex_position.values.data[k]);
                double q[3] = {p.x, p.y, p.z};
                for (int a = 0; a < 3; ++a) { mn[a] = std::min(mn[a], q[a]); mx[a] = std::max(mx[a], q[a]); }
            }
            double cx = (mn[0] + mx[0]) / 2, cz = (mn[2] + mx[2]) / 2;
            if (cx < bx0 || cx > bx1 || cz < bz0 || cz > bz1) continue;
        }
        ufbx_matrix nrmM = ufbx_matrix_for_normals(&node->geometry_to_world);
        std::vector<uint32_t> tri(mesh->max_face_triangles * 3);
        for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
            const ufbx_mesh_part& part = mesh->material_parts.data[pi];
            const ufbx_material* mat = pi < mesh->materials.count ? mesh->materials.data[pi] : nullptr;
            if (mat) {  // --sem também tira as partes pelo nome do material (ex.: a bala solta junto da pistola)
                bool skipMat = false;
                for (const auto& w : without) skipMat |= lower(mat->name.data).find(w) != std::string::npos;
                if (skipMat) continue;
            }
            Prim& P = prims[mat];
            std::unordered_map<std::string, uint32_t> seen;
            for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                ufbx_face face = mesh->faces.data[part.face_indices.data[fi]];
                uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                for (uint32_t k = 0; k < n * 3; ++k) {
                    uint32_t ix = tri[k];
                    V v{};
                    auto p = rot(ufbx_transform_position(&node->geometry_to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, ix)));
                    std::memcpy(v.p, p.data(), 12);
                    if (mesh->vertex_normal.exists) {
                        ufbx_vec3 nn = ufbx_vec3_normalize(ufbx_transform_direction(&nrmM, ufbx_get_vertex_vec3(&mesh->vertex_normal, ix)));
                        auto r = rot(nn);
                        std::memcpy(v.n, r.data(), 12);
                    }
                    if (mesh->vertex_uv.exists) {
                        ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                        v.uv[0] = float(uv.x); v.uv[1] = 1.f - float(uv.y);  // glTF: v para baixo
                    }
                    std::string key((const char*)&v, sizeof(V));
                    auto it = seen.find(key);
                    if (it == seen.end()) { it = seen.emplace(key, uint32_t(P.v.size())).first; P.v.push_back(v); }
                    P.idx.push_back(it->second);
                }
            }
        }
    }
    // centraliza (a arma é deitada e encaixada na mão pelo motor)
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    size_t tris = 0;
    for (auto& [m, P] : prims) {
        tris += P.idx.size() / 3;
        for (auto& v : P.v) for (int a = 0; a < 3; ++a) { mn[a] = std::min(mn[a], v.p[a]); mx[a] = std::max(mx[a], v.p[a]); }
    }
    if (!tris) { std::fprintf(stderr, "nenhuma malha escolhida\n"); return 1; }
    for (auto& [m, P] : prims) for (auto& v : P.v) for (int a = 0; a < 3; ++a) v.p[a] -= (mn[a] + mx[a]) * 0.5f;

    // ---- glTF
    std::vector<unsigned char> bin;
    json g;
    g["asset"] = {{"version", "2.0"}, {"generator", "Motor 3D fbx_para_glb"}};
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
    auto image = [&](const std::string& key, const Img& im) {
        auto it = imageOf.find(key);
        if (it != imageOf.end()) return it->second;
        auto data = jpg(shrink(im, texSize), key.find("normal") != std::string::npos ? 92 : 86);
        g["images"].push_back({{"bufferView", view(data.data(), data.size(), 0)}, {"mimeType", "image/jpeg"}});
        g["textures"].push_back({{"sampler", 0}, {"source", int(g["images"].size() - 1)}});
        return imageOf[key] = int(g["textures"].size() - 1);
    };
    int nodeIdx = 0;
    for (auto& [mat, P] : prims) {
        // material: cor, normal e ORM (G rugosidade, B metal)
        std::string albedo, normal, rough, metal;
        if (!single[0].empty()) { albedo = single[0]; normal = single[1]; rough = single[2]; metal = single[3]; }
        else if (mat) {
            const ufbx_texture* t = mat->pbr.base_color.texture ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture;
            const ufbx_texture* nt = mat->fbx.normal_map.texture ? mat->fbx.normal_map.texture : mat->pbr.normal_map.texture;
            if (t) albedo = find(t->relative_filename.length ? t->relative_filename.data : t->filename.data);
            if (nt) normal = find(nt->relative_filename.length ? nt->relative_filename.data : nt->filename.data);
            if (!albedo.empty()) {
                rough = sibling(albedo, {"_Roughness", "_Rough"});
                metal = sibling(albedo, {"_Metalness", "_Metallic", "_Metal"});
                if (normal.empty()) normal = sibling(albedo, {"_Normal", "_Normal_OpenGL", "_NormalGL"});
            }
        }
        json m = {{"name", mat ? mat->name.data : "material"}, {"pbrMetallicRoughness", {{"metallicFactor", 0.f}, {"roughnessFactor", 0.6f}}}};
        Img im;
        if (!albedo.empty() && loadImg(albedo, im)) m["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", image(albedo, im)}};
        else if (mat && mat->pbr.base_color.has_value) {
            ufbx_vec4 cc = mat->pbr.base_color.value_vec4;
            m["pbrMetallicRoughness"]["baseColorFactor"] = {cc.x, cc.y, cc.z, 1.0};
        }
        if (!normal.empty() && loadImg(normal, im)) m["normalTexture"] = {{"index", image(normal + "#normal", im)}};
        Img r, mt;
        bool hr = !rough.empty() && loadImg(rough, r), hm = !metal.empty() && loadImg(metal, mt);
        if (hr || hm) {
            Img base = hr ? r : mt;
            Img orm; orm.w = base.w; orm.h = base.h; orm.px.assign(size_t(orm.w) * orm.h * 4, 255);
            for (int y = 0; y < orm.h; ++y)
                for (int x = 0; x < orm.w; ++x) {
                    size_t i = (size_t(y) * orm.w + x) * 4;
                    auto at = [&](const Img& s) { return s.px[(size_t(y * s.h / orm.h) * s.w + x * s.w / orm.w) * 4]; };
                    orm.px[i + 1] = hr ? at(r) : 150;
                    orm.px[i + 2] = hm ? at(mt) : 0;
                }
            m["pbrMetallicRoughness"]["metallicRoughnessTexture"] = {{"index", image(rough + metal + "#orm", orm)}};
            m["pbrMetallicRoughness"]["metallicFactor"] = 1.f;
            m["pbrMetallicRoughness"]["roughnessFactor"] = 1.f;
        }
        g["materials"].push_back(m);
        std::printf("  material %-28s cor:%s normal:%s rug:%s metal:%s  %zu triângulos\n", mat ? mat->name.data : "-",
                    albedo.empty() ? "não" : "sim", normal.empty() ? "não" : "sim", hr ? "sim" : "não", hm ? "sim" : "não", P.idx.size() / 3);
        // geometria
        std::vector<float> pos, nrm, uv;
        float pmn[3] = {1e30f, 1e30f, 1e30f}, pmx[3] = {-1e30f, -1e30f, -1e30f};
        for (auto& v : P.v) {
            pos.insert(pos.end(), v.p, v.p + 3); nrm.insert(nrm.end(), v.n, v.n + 3); uv.insert(uv.end(), v.uv, v.uv + 2);
            for (int a = 0; a < 3; ++a) { pmn[a] = std::min(pmn[a], v.p[a]); pmx[a] = std::max(pmx[a], v.p[a]); }
        }
        int cnt = int(P.v.size());
        int ap = int(g["accessors"].size());
        g["accessors"].push_back({{"bufferView", view(pos.data(), pos.size() * 4, 34962)}, {"componentType", 5126}, {"count", cnt}, {"type", "VEC3"},
                                  {"min", {pmn[0], pmn[1], pmn[2]}}, {"max", {pmx[0], pmx[1], pmx[2]}}});
        g["accessors"].push_back({{"bufferView", view(nrm.data(), nrm.size() * 4, 34962)}, {"componentType", 5126}, {"count", cnt}, {"type", "VEC3"}});
        g["accessors"].push_back({{"bufferView", view(uv.data(), uv.size() * 4, 34962)}, {"componentType", 5126}, {"count", cnt}, {"type", "VEC2"}});
        g["accessors"].push_back({{"bufferView", view(P.idx.data(), P.idx.size() * 4, 34963)}, {"componentType", 5125}, {"count", P.idx.size()}, {"type", "SCALAR"}});
        g["meshes"].push_back({{"primitives", json::array({{{"attributes", {{"POSITION", ap}, {"NORMAL", ap + 1}, {"TEXCOORD_0", ap + 2}}},
                                                            {"indices", ap + 3}, {"material", int(g["materials"].size() - 1)}}})}});
        g["nodes"].push_back({{"name", mat ? mat->name.data : "parte"}, {"mesh", int(g["meshes"].size() - 1)}});
        g["scenes"][0]["nodes"].push_back(nodeIdx++);
    }
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
    std::printf("%s: %zu triângulos, tamanho %.1f x %.1f x %.1f m, %zu KB\n", argv[2], tris, mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2],
                (12 + 16 + js.size() + bin.size()) / 1024);
    ufbx_free_scene(scene);
    return 0;
}
