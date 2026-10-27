// Exporta a cena (ou um objeto) para .glb (glTF 2.0 binário): abre no Blender, Unity (glTFast), Godot, Unreal,
// visualizadores e no próprio motor. Malhas e texturas são lidas de volta da GPU; personagens levam esqueleto,
// pesos e as animações "assadas" a 30 quadros por segundo (funciona também com retarget humanoide).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <glm/gtx/matrix_decompose.hpp>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include "engine/Engine.h"
#include "json.hpp"

namespace eng {

using json = nlohmann::json;

namespace {

struct GlbWriter {
    std::vector<uint8_t> bin;
    json bufferViews = json::array(), accessors = json::array();

    int view(const void* data, size_t size, int target) {
        while (bin.size() % 4) bin.push_back(0);
        size_t off = bin.size();
        bin.insert(bin.end(), (const uint8_t*)data, (const uint8_t*)data + size);
        json v = {{"buffer", 0}, {"byteOffset", off}, {"byteLength", size}};
        if (target) v["target"] = target;
        bufferViews.push_back(v);
        return int(bufferViews.size()) - 1;
    }
    int accessor(const void* data, size_t count, int compType, const char* type, size_t compSize, int comps, int target,
                 const float* mn = nullptr, const float* mx = nullptr) {
        int bv = view(data, count * compSize * comps, target);
        json a = {{"bufferView", bv}, {"componentType", compType}, {"count", count}, {"type", type}};
        if (mn && mx) {
            json jmn = json::array(), jmx = json::array();
            for (int i = 0; i < comps; ++i) { jmn.push_back(mn[i]); jmx.push_back(mx[i]); }
            a["min"] = jmn; a["max"] = jmx;
        }
        accessors.push_back(a);
        return int(accessors.size()) - 1;
    }
};

json mat4Json(const glm::mat4& m) {
    json a = json::array();
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) a.push_back(m[c][r]);
    return a;
}

void pngToVec(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

// Lê uma textura da GPU (anexando a um FBO). Texturas 1x1 (padrão branco) não são exportadas.
bool readTexture(GLuint tex, std::vector<uint8_t>& png) {
    if (!tex) return false;
    GLint w = 0, h = 0, fmt = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (w <= 1 || h <= 1 || w > 8192 || h > 8192) return false;
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    std::vector<uint8_t> px;
    if (ok) {
        px.resize(size_t(w) * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prevFbo));
    glDeleteFramebuffers(1, &fbo);
    if (!ok) return false;
    if (fmt == GL_SRGB8_ALPHA8 || fmt == GL_SRGB8) {  // a leitura de textura sRGB volta linear: codifica de novo em sRGB
        uint8_t lut[256];
        for (int i = 0; i < 256; ++i) {
            float c = i / 255.f;
            c = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
            lut[i] = uint8_t(std::clamp(c, 0.f, 1.f) * 255.f + 0.5f);
        }
        for (size_t i = 0; i < px.size(); ++i) if (i % 4 != 3) px[i] = lut[px[i]];
    }
    png.clear();
    return stbi_write_png_to_func(pngToVec, &png, w, h, 4, px.data(), w * 4) != 0;
}

}  // namespace

bool exportGlb(Engine* e, int rootId, const std::string& path, std::string* err) {
    GlbWriter W;
    json nodes = json::array(), meshes = json::array(), materials = json::array(), textures = json::array(),
         images = json::array(), skins = json::array(), animations = json::array();
    json sceneNodes = json::array();
    e->scene.updateTransforms();

    GameObject* root = rootId >= 0 ? objectById(e, rootId) : nullptr;
    if (rootId >= 0 && !root) { if (err) *err = "objeto não encontrado"; return false; }
    if (root) root = rootOf(e, root);
    // exportando um objeto: ele vai para a origem (pés no chão, de frente para +Z)
    glm::mat4 toLocal = root ? glm::inverse(root->transform.world()) : glm::mat4(1.f);

    std::map<GLuint, int> texIndex;
    auto textureOf = [&](GLuint tex) -> int {
        auto it = texIndex.find(tex);
        if (it != texIndex.end()) return it->second;
        std::vector<uint8_t> png;
        int idx = -1;
        if (readTexture(tex, png)) {
            int bv = W.view(png.data(), png.size(), 0);
            images.push_back({{"bufferView", bv}, {"mimeType", "image/png"}});
            textures.push_back({{"source", int(images.size()) - 1}});
            idx = int(textures.size()) - 1;
        }
        texIndex[tex] = idx;
        return idx;
    };
    std::map<MaterialId, int> matIndex;
    auto materialOf = [&](MaterialId id) -> int {
        auto it = matIndex.find(id);
        if (it != matIndex.end()) return it->second;
        const Material* m = e->renderer.material(id);
        json pbr = {{"metallicFactor", m ? m->metallic : 0.f}, {"roughnessFactor", m ? m->roughness : 1.f}};
        json jm = {{"name", "Material " + std::to_string(id)}};
        if (m) {
            glm::vec4 f = m->albedoFactor;
            if (m->mode == 1) f = glm::vec4(0.25f, 0.4f, 0.15f, 1.f);  // terreno: o shader mistura por altura; exporta verde
            pbr["baseColorFactor"] = {f.r, f.g, f.b, f.a};
            int t = m->mode == 1 ? -1 : textureOf(m->albedo);
            if (t >= 0) pbr["baseColorTexture"] = {{"index", t}};
            if (m->doubleSided) jm["doubleSided"] = true;
            if (m->alphaCutoff > 0.f) { jm["alphaMode"] = "MASK"; jm["alphaCutoff"] = m->alphaCutoff; }
            if (glm::length(m->emissive_factor) > 0.f) jm["emissiveFactor"] = {m->emissive_factor.r, m->emissive_factor.g, m->emissive_factor.b};
        }
        jm["pbrMetallicRoughness"] = pbr;
        materials.push_back(jm);
        return matIndex[id] = int(materials.size()) - 1;
    };

    // Malha (lida da GPU) + material -> índice em "meshes"
    std::map<uint64_t, int> meshIndex;
    auto meshOf = [&](MeshId id, MaterialId mat, bool skinned) -> int {
        uint64_t key = (uint64_t(id) << 32) | mat;
        auto it = meshIndex.find(key);
        if (it != meshIndex.end()) return it->second;
        const Mesh* m = e->renderer.mesh(id);
        if (!m || !m->vertexCount || !m->indexCount) return meshIndex[key] = -1;
        std::vector<Vertex> v(m->vertexCount);
        std::vector<uint32_t> idx(m->indexCount);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
        if (void* p = glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * v.size(), GL_MAP_READ_BIT)) {
            std::memcpy(v.data(), p, sizeof(Vertex) * v.size());
            glUnmapBuffer(GL_ARRAY_BUFFER);
        } else return meshIndex[key] = -1;
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_COPY_READ_BUFFER, m->ibo);
        if (void* p = glMapBufferRange(GL_COPY_READ_BUFFER, 0, sizeof(uint32_t) * idx.size(), GL_MAP_READ_BIT)) {
            std::memcpy(idx.data(), p, sizeof(uint32_t) * idx.size());
            glUnmapBuffer(GL_COPY_READ_BUFFER);
        } else return meshIndex[key] = -1;
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        size_t n = v.size();
        std::vector<glm::vec3> pos(n), nor(n);
        std::vector<glm::vec2> uv(n);
        std::vector<glm::vec4> col(n), wt(n);
        std::vector<glm::u16vec4> jt(n);
        glm::vec3 mn(1e30f), mx(-1e30f);
        bool hasColor = false;
        for (size_t i = 0; i < n; ++i) {
            pos[i] = v[i].pos; nor[i] = glm::length(v[i].normal) > 1e-6f ? glm::normalize(v[i].normal) : glm::vec3(0, 1, 0);
            uv[i] = v[i].uv; col[i] = v[i].color; jt[i] = v[i].joints;
            float s = v[i].weights.x + v[i].weights.y + v[i].weights.z + v[i].weights.w;
            wt[i] = s > 1e-6f ? v[i].weights / s : glm::vec4(1, 0, 0, 0);
            mn = glm::min(mn, pos[i]); mx = glm::max(mx, pos[i]);
            hasColor |= glm::any(glm::notEqual(col[i], glm::vec4(1.f)));
        }
        json attrs;
        attrs["POSITION"] = W.accessor(pos.data(), n, 5126, "VEC3", 4, 3, 34962, &mn.x, &mx.x);
        attrs["NORMAL"] = W.accessor(nor.data(), n, 5126, "VEC3", 4, 3, 34962);
        attrs["TEXCOORD_0"] = W.accessor(uv.data(), n, 5126, "VEC2", 4, 2, 34962);
        if (hasColor) attrs["COLOR_0"] = W.accessor(col.data(), n, 5126, "VEC4", 4, 4, 34962);
        if (skinned) {
            attrs["JOINTS_0"] = W.accessor(jt.data(), n, 5123, "VEC4", 2, 4, 34962);
            attrs["WEIGHTS_0"] = W.accessor(wt.data(), n, 5126, "VEC4", 4, 4, 34962);
        }
        int ind = W.accessor(idx.data(), idx.size(), 5125, "SCALAR", 4, 1, 34963);
        meshes.push_back({{"primitives", json::array({{{"attributes", attrs}, {"indices", ind}, {"material", materialOf(mat)}}})}});
        return meshIndex[key] = int(meshes.size()) - 1;
    };

    // Esqueleto de um Animator: nós dos ossos sob um nó "espaço", skin e animações assadas
    std::map<const Animator*, int> skinOf;
    auto skinFor = [&](Animator* a) -> int {
        auto it = skinOf.find(a);
        if (it != skinOf.end()) return it->second;
        const Skeleton& S = *a->skeleton;
        glm::mat4 spaceW = toLocal * (a->space ? a->space->world() : glm::mat4(1.f));
        int spaceNode = int(nodes.size());
        nodes.push_back({{"name", "Armature"}, {"matrix", mat4Json(spaceW)}, {"children", json::array()}});
        sceneNodes.push_back(spaceNode);
        int first = int(nodes.size());
        for (int j = 0; j < S.jointCount; ++j) {
            json nd = {{"name", S.name[j][0] ? S.name[j] : ("osso_" + std::to_string(j))},
                       {"translation", {S.bindT[j].x, S.bindT[j].y, S.bindT[j].z}},
                       {"rotation", {S.bindR[j].x, S.bindR[j].y, S.bindR[j].z, S.bindR[j].w}},
                       {"scale", {S.bindS[j].x, S.bindS[j].y, S.bindS[j].z}}};
            nodes.push_back(nd);
        }
        for (int j = 0; j < S.jointCount; ++j) {
            int p = S.parent[j];
            json& parent = p < 0 ? nodes[spaceNode] : nodes[first + p];
            if (!parent.contains("children")) parent["children"] = json::array();
            parent["children"].push_back(first + j);
        }
        std::vector<glm::mat4> ibm(S.inverseBind, S.inverseBind + S.jointCount);
        json joints = json::array();
        for (int j = 0; j < S.jointCount; ++j) joints.push_back(first + j);
        int ibmAcc = W.accessor(ibm.data(), ibm.size(), 5126, "MAT4", 4, 16, 0);
        skins.push_back({{"joints", joints}, {"inverseBindMatrices", ibmAcc}, {"skeleton", spaceNode}});
        int skin = int(skins.size()) - 1;

        // animações: amostra cada clip a 30 fps e grava a pose local de cada osso
        int keepClip = a->clipIndex;
        float keepTime = a->time;
        for (int c = 0; c < a->clipCount; ++c) {
            a->clipIndex = c;
            const AnimationClip* clip = a->clip();
            if (!clip || clip->duration <= 0.f) continue;
            int frames = std::clamp(int(std::ceil(clip->duration * 30.f)) + 1, 2, 3000);
            std::vector<float> times(frames);
            std::vector<glm::vec3> T(size_t(frames) * S.jointCount), Sc(size_t(frames) * S.jointCount);
            std::vector<glm::quat> R(size_t(frames) * S.jointCount);
            for (int f = 0; f < frames; ++f) {
                times[f] = std::min(clip->duration, f / 30.f);
                a->time = times[f];
                a->update(0.f, false);
                for (int j = 0; j < S.jointCount; ++j) {
                    int p = S.parent[j];
                    glm::mat4 local = p < 0 ? a->global[j] : glm::inverse(a->global[p]) * a->global[j];
                    glm::vec3 sc, tr, skew; glm::vec4 persp; glm::quat rot;
                    glm::decompose(local, sc, rot, tr, skew, persp);
                    size_t k = size_t(f) * S.jointCount + j;
                    T[k] = tr; R[k] = glm::normalize(rot); Sc[k] = sc;
                    if (f > 0 && glm::dot(R[k], R[k - S.jointCount]) < 0.f) R[k] = -R[k];  // caminho curto
                }
            }
            float tmin = times.front(), tmax = times.back();
            int timeAcc = W.accessor(times.data(), times.size(), 5126, "SCALAR", 4, 1, 0, &tmin, &tmax);
            json samplers = json::array(), channels = json::array();
            for (int j = 0; j < S.jointCount; ++j) {
                std::vector<glm::vec3> tj(frames), sj(frames);
                std::vector<glm::vec4> rj(frames);
                bool scaleChanges = false;
                for (int f = 0; f < frames; ++f) {
                    size_t k = size_t(f) * S.jointCount + j;
                    tj[f] = T[k]; sj[f] = Sc[k]; rj[f] = {R[k].x, R[k].y, R[k].z, R[k].w};
                    scaleChanges |= glm::length(Sc[k] - S.bindS[j]) > 1e-3f;
                }
                auto add = [&](const char* pathName, int acc) {
                    samplers.push_back({{"input", timeAcc}, {"output", acc}, {"interpolation", "LINEAR"}});
                    channels.push_back({{"sampler", int(samplers.size()) - 1}, {"target", {{"node", first + j}, {"path", pathName}}}});
                };
                add("translation", W.accessor(tj.data(), frames, 5126, "VEC3", 4, 3, 0));
                add("rotation", W.accessor(rj.data(), frames, 5126, "VEC4", 4, 4, 0));
                if (scaleChanges) add("scale", W.accessor(sj.data(), frames, 5126, "VEC3", 4, 3, 0));
            }
            animations.push_back({{"name", clip->name[0] ? clip->name : ("anim_" + std::to_string(c))},
                                  {"samplers", samplers}, {"channels", channels}});
        }
        a->clipIndex = keepClip;
        a->time = keepTime;
        a->update(0.f, false);
        return skinOf[a] = skin;
    };

    int count = 0;
    auto exportObject = [&](GameObject& g) {
        if (!g.active || !g.meshRenderer) return;
        const MeshRenderer* mr = g.meshRenderer;
        if (mr->lodNear > 0.f) return;  // versão simplificada (LOD de longe): exporta só a completa
        const Mesh* m = e->renderer.mesh(mr->mesh);
        if (!m) return;
        bool skinned = m->skinned && g.animator && g.animator->skeleton;
        int mesh = meshOf(mr->mesh, mr->material, skinned);
        if (mesh < 0) return;
        json nd = {{"name", g.name}, {"mesh", mesh}};
        if (skinned) nd["skin"] = skinFor(g.animator);
        else {
            const glm::mat4* pal;
            nd["matrix"] = mat4Json(toLocal * renderMatrix(e, g, &pal));
        }
        nodes.push_back(nd);
        sceneNodes.push_back(int(nodes.size()) - 1);
        ++count;
    };
    if (root) {
        Scene::forEachInSubtree(*root, exportObject);
        // armas na mão do personagem vão junto
        for (const Attachment& at : e->attachments)
            if (GameObject* w = e->scene.get(at.obj); w && at.anim && at.anim->space == &root->transform)
                Scene::forEachInSubtree(*w, exportObject);
    } else {
        e->scene.forEach([&](GameObject& g, Scene::Handle) { exportObject(g); });
    }
    if (!count) { if (err) *err = "nada para exportar (o objeto não tem malha)"; return false; }

    json j;
    j["asset"] = {{"version", "2.0"}, {"generator", "Motor 3D (celular)"}};
    j["scene"] = 0;
    j["scenes"] = json::array({{{"name", root ? std::string(root->name) : e->projectName}, {"nodes", sceneNodes}}});
    j["nodes"] = nodes;
    j["meshes"] = meshes;
    j["materials"] = materials;
    if (!textures.empty()) {
        j["textures"] = textures;
        j["images"] = images;
        j["samplers"] = json::array({{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}}});
        for (auto& t : j["textures"]) t["sampler"] = 0;
    }
    if (!skins.empty()) j["skins"] = skins;
    if (!animations.empty()) j["animations"] = animations;
    j["accessors"] = W.accessors;
    j["bufferViews"] = W.bufferViews;
    while (W.bin.size() % 4) W.bin.push_back(0);
    j["buffers"] = json::array({{{"byteLength", W.bin.size()}}});

    std::string js = j.dump(-1, ' ', false, json::error_handler_t::replace);
    while (js.size() % 4) js += ' ';
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "não consegui criar o arquivo"; return false; }
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    u32(0x46546C67); u32(2); u32(uint32_t(12 + 8 + js.size() + 8 + W.bin.size()));
    u32(uint32_t(js.size())); u32(0x4E4F534A); std::fwrite(js.data(), 1, js.size(), f);
    u32(uint32_t(W.bin.size())); u32(0x004E4942); std::fwrite(W.bin.data(), 1, W.bin.size(), f);
    bool ok = std::fclose(f) == 0;
    if (!ok && err) *err = "erro ao gravar o arquivo";
    return ok;
}

}  // namespace eng
