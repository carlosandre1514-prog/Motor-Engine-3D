#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define STB_IMAGE_IMPLEMENTATION
#include <tiny_gltf.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include "assets/GltfLoader.h"
#include "assets/MeshUtils.h"
#include "assets/TextureManager.h"

namespace eng {

// Lê um accessor como floats (converte inteiros normalizados), `comps` componentes por elemento.
// Accessor sem bufferView (permitido pela spec) vira zeros.
static std::vector<float> readFloats(const tinygltf::Model& m, const tinygltf::Accessor& a, int comps) {
    std::vector<float> out(a.count * comps, 0.f);
    if (a.bufferView < 0 || a.bufferView >= int(m.bufferViews.size())) return out;
    const auto& bv = m.bufferViews[a.bufferView];
    const auto& buf = m.buffers[bv.buffer].data;
    int csize = tinygltf::GetComponentSizeInBytes(a.componentType);
    int ncomp = tinygltf::GetNumComponentsInType(a.type);
    if (csize <= 0 || ncomp <= 0) return out;
    size_t stride = bv.byteStride ? bv.byteStride : size_t(csize * ncomp);
    size_t base = bv.byteOffset + a.byteOffset;
    int n = std::min(comps, ncomp);
    for (size_t i = 0; i < a.count; ++i) {
        size_t off = base + i * stride;
        if (off + size_t(csize * n) > buf.size()) break;
        const uint8_t* e = buf.data() + off;
        for (int k = 0; k < n; ++k) {
            const uint8_t* c = e + k * csize;
            float f = 0.f;
            switch (a.componentType) {
                case TINYGLTF_COMPONENT_TYPE_FLOAT: std::memcpy(&f, c, 4); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: f = a.normalized ? *c / 255.f : float(*c); break;
                case TINYGLTF_COMPONENT_TYPE_BYTE: { int8_t v; std::memcpy(&v, c, 1); f = a.normalized ? std::max(v / 127.f, -1.f) : float(v); } break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, c, 2); f = a.normalized ? v / 65535.f : float(v); } break;
                case TINYGLTF_COMPONENT_TYPE_SHORT: { int16_t v; std::memcpy(&v, c, 2); f = a.normalized ? std::max(v / 32767.f, -1.f) : float(v); } break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: { uint32_t v; std::memcpy(&v, c, 4); f = float(v); } break;
                default: break;
            }
            out[i * comps + k] = f;
        }
    }
    return out;
}

static std::vector<uint32_t> readIndices(const tinygltf::Model& m, const tinygltf::Accessor& a) {
    std::vector<uint32_t> out(a.count, 0);
    if (a.bufferView < 0) return out;
    const auto& bv = m.bufferViews[a.bufferView];
    const auto& buf = m.buffers[bv.buffer].data;
    int csize = tinygltf::GetComponentSizeInBytes(a.componentType);
    size_t stride = bv.byteStride ? bv.byteStride : size_t(csize);
    size_t base = bv.byteOffset + a.byteOffset;
    for (size_t i = 0; i < a.count; ++i) {
        size_t off = base + i * stride;
        if (off + size_t(csize) > buf.size()) break;
        const uint8_t* e = buf.data() + off;
        if (csize == 4) { uint32_t v; std::memcpy(&v, e, 4); out[i] = v; }
        else if (csize == 2) { uint16_t v; std::memcpy(&v, e, 2); out[i] = v; }
        else out[i] = *e;
    }
    return out;
}

static bool endsWithNoCase(const char* s, const char* suf) {
    size_t n = std::strlen(s), k = std::strlen(suf);
    if (n < k) return false;
    for (size_t i = 0; i < k; ++i)
        if (std::tolower((unsigned char)s[n - k + i]) != std::tolower((unsigned char)suf[i])) return false;
    return true;
}

static bool readPrimitive(const tinygltf::Model& m, const tinygltf::Primitive& prim,
                          std::vector<Vertex>& verts, std::vector<uint32_t>& idx, bool& skinned) {
    if (prim.mode != -1 && prim.mode != TINYGLTF_MODE_TRIANGLES) return false;  // linhas/pontos/strips: não suportado
    auto attr = [&](const char* name) -> const tinygltf::Accessor* {
        auto it = prim.attributes.find(name);
        return it == prim.attributes.end() ? nullptr : &m.accessors[it->second];
    };
    const tinygltf::Accessor* pos = attr("POSITION");
    if (!pos || pos->count == 0) return false;
    const size_t n = pos->count;
    verts.assign(n, makeVertex(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec2(0)));

    auto P = readFloats(m, *pos, 3);
    for (size_t i = 0; i < n; ++i) verts[i].pos = glm::make_vec3(&P[i * 3]);

    if (prim.indices >= 0) idx = readIndices(m, m.accessors[prim.indices]);
    else { idx.resize(n); for (size_t i = 0; i < n; ++i) idx[i] = uint32_t(i); }  // primitiva sem índices
    idx.resize(idx.size() / 3 * 3);
    for (uint32_t& i : idx) if (i >= n) i = 0;

    const tinygltf::Accessor* a;
    bool hasNormals = false, hasTangents = false;
    if ((a = attr("NORMAL")) && a->count == n) {
        auto d = readFloats(m, *a, 3);
        for (size_t i = 0; i < n; ++i) verts[i].normal = glm::make_vec3(&d[i * 3]);
        hasNormals = true;
    }
    if ((a = attr("TEXCOORD_0")) && a->count == n) {
        auto d = readFloats(m, *a, 2);
        for (size_t i = 0; i < n; ++i) verts[i].uv = glm::make_vec2(&d[i * 2]);
    }
    if ((a = attr("TANGENT")) && a->count == n) {
        auto d = readFloats(m, *a, 4);
        for (size_t i = 0; i < n; ++i) verts[i].tangent = glm::make_vec4(&d[i * 4]);
        hasTangents = true;
    }
    if (!hasNormals) generateNormals(verts, idx);
    if (!hasTangents) generateTangents(verts, idx);

    if ((a = attr("COLOR_0")) && a->count == n) {
        int comps = tinygltf::GetNumComponentsInType(a->type);
        auto d = readFloats(m, *a, 4);
        for (size_t i = 0; i < n; ++i) verts[i].color = glm::vec4(d[i * 4], d[i * 4 + 1], d[i * 4 + 2], comps == 4 ? d[i * 4 + 3] : 1.f);
    }
    if ((a = attr("JOINTS_0")) && a->count == n) {
        skinned = true;
        auto d = readFloats(m, *a, 4);
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < 4; ++k) {
                int j = int(d[i * 4 + k]);
                verts[i].joints[k] = uint16_t(j >= 0 && j < kMaxJoints ? j : 0);
            }
    }
    if ((a = attr("WEIGHTS_0")) && a->count == n) {
        auto d = readFloats(m, *a, 4);
        for (size_t i = 0; i < n; ++i) {
            glm::vec4 w = glm::make_vec4(&d[i * 4]);
            float sum = w.x + w.y + w.z + w.w;
            verts[i].weights = sum > 1e-6f ? w / sum : glm::vec4(1, 0, 0, 0);
        }
    }
    return !idx.empty();
}

// TRS de um nó (aceita "matrix" ou translation/rotation/scale)
static void nodeTRS(const tinygltf::Node& n, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    t = glm::vec3(0.f); r = glm::quat(1, 0, 0, 0); s = glm::vec3(1.f);
    if (n.matrix.size() == 16) {
        glm::mat4 mtx;
        for (int i = 0; i < 16; ++i) mtx[i / 4][i % 4] = float(n.matrix[i]);
        glm::vec3 skew; glm::vec4 persp;
        glm::decompose(mtx, s, r, t, skew, persp);
        return;
    }
    if (n.translation.size() == 3) t = glm::vec3(n.translation[0], n.translation[1], n.translation[2]);
    if (n.rotation.size() == 4) r = glm::quat(float(n.rotation[3]), float(n.rotation[0]), float(n.rotation[1]), float(n.rotation[2]));
    if (n.scale.size() == 3) s = glm::vec3(n.scale[0], n.scale[1], n.scale[2]);
}

static const char* baseName(const char* path) {
    const char* b = std::strrchr(path, '/');
    return b ? b + 1 : path;
}

bool loadGltf(const char* path, LoadContext& ctx, LoadedModel& out) {
    tinygltf::TinyGLTF loader;
    std::string baseDir = path;
    baseDir = baseDir.substr(0, baseDir.find_last_of("/\\") == std::string::npos ? 0 : baseDir.find_last_of("/\\"));
    tinygltf::Model model;
    std::string err, warn;
    bool ok = endsWithNoCase(path, ".glb") ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                                           : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!ok) { out.error = err.empty() ? "falha ao ler o arquivo" : err; std::fprintf(stderr, "glTF: %s\n", out.error.c_str()); return false; }
    Renderer& renderer = ctx.renderer;
    Scene& scene = ctx.scene;

    // Materiais PBR metallic-roughness. Cache: a mesma imagem vira uma única textura na VRAM.
    TextureManager& tex = TextureManager::instance();
    std::map<std::pair<int, bool>, GLuint> texCache;
    auto texOf = [&](int ti, bool srgb, GLuint fallback) -> GLuint {
        if (ti < 0 || ti >= int(model.textures.size())) return fallback;
        int src = model.textures[ti].source;
        if (src < 0 || src >= int(model.images.size())) return fallback;
        auto key = std::make_pair(src, srgb);
        auto it = texCache.find(key);
        if (it != texCache.end()) return it->second;
        const auto& im = model.images[src];
        // imagem externa: a mesma do disco é enviada uma vez só, mesmo entre arquivos diferentes
        std::string named = im.uri.empty() ? std::string() : baseDir + "/" + im.uri + (srgb ? "#s" : "#l");
        if (GLuint id = named.empty() ? 0 : tex.named(named)) return texCache[key] = id;
        GLuint id = im.image.empty() ? fallback : tex.fromPixels(im.image.data(), im.width, im.height, im.component, srgb);
        if (!named.empty() && !im.image.empty()) tex.setNamed(named, id);
        texCache[key] = id;
        return id;
    };
    for (const auto& gm : model.materials) {
        Material m;
        const auto& pbr = gm.pbrMetallicRoughness;
        m.albedoFactor = glm::vec4(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
        m.roughness = float(pbr.roughnessFactor);
        m.metallic = float(pbr.metallicFactor);
        m.emissive_factor = glm::vec3(gm.emissiveFactor[0], gm.emissiveFactor[1], gm.emissiveFactor[2]);
        m.albedo = texOf(pbr.baseColorTexture.index, true, tex.white());
        m.normal = texOf(gm.normalTexture.index, false, tex.flatNormal());
        m.orm = texOf(pbr.metallicRoughnessTexture.index, false, tex.white());
        m.emissive = texOf(gm.emissiveTexture.index, true, tex.white());
        if (gm.occlusionTexture.index >= 0) {
            m.occlusion = texOf(gm.occlusionTexture.index, false, 0);
            m.occlusionStrength = float(gm.occlusionTexture.strength);
        }
        m.doubleSided = gm.doubleSided;
        if (gm.alphaMode == "MASK") m.alphaCutoff = float(gm.alphaCutoff);
        else if (gm.alphaMode == "BLEND") m.alphaCutoff = 0.1f;  // sem transparência real ainda: recorta o quase invisível
        out.materials.push_back(renderer.createMaterial(m));
    }
    Material def; def.albedo = tex.white(); def.normal = tex.flatNormal(); def.orm = tex.white(); def.emissive = tex.white();
    def.albedoFactor = glm::vec4(0.8f, 0.8f, 0.8f, 1.f); def.roughness = 0.6f; def.metallic = 0.f;
    MaterialId defaultMat = renderer.createMaterial(def);

    // Pai de cada nó (para esqueletos)
    std::vector<int> nodeParent(model.nodes.size(), -1);
    for (size_t ni = 0; ni < model.nodes.size(); ++ni)
        for (int c : model.nodes[ni].children)
            if (c >= 0 && c < int(model.nodes.size())) nodeParent[c] = int(ni);

    // Esqueleto único: a união dos ossos de todas as skins. Personagens com várias malhas (corpo, roupa,
    // capacete) compartilham os ossos e são animados juntos por um único Animator.
    std::vector<int> nodeToJoint(model.nodes.size(), -1);
    std::vector<int> jointNode;
    std::vector<glm::mat4> jointIbm;
    std::vector<std::vector<int>> skinRemap(model.skins.size());  // índice na skin -> osso da união
    for (size_t si = 0; si < model.skins.size(); ++si) {
        const auto& skin = model.skins[si];
        std::vector<float> ib;
        if (skin.inverseBindMatrices >= 0) ib = readFloats(model, model.accessors[skin.inverseBindMatrices], 16);
        for (size_t j = 0; j < skin.joints.size(); ++j) {
            int node = skin.joints[j];
            if (node < 0 || node >= int(model.nodes.size())) { skinRemap[si].push_back(0); continue; }
            if (nodeToJoint[node] < 0 && jointNode.size() < size_t(kMaxJoints)) {
                nodeToJoint[node] = int(jointNode.size());
                jointNode.push_back(node);
                jointIbm.push_back(j * 16 + 15 < ib.size() ? glm::make_mat4(&ib[j * 16]) : glm::mat4(1.f));
            }
            skinRemap[si].push_back(std::max(0, nodeToJoint[node]));
        }
    }
    int spaceNode = -1;  // nó acima da raiz do esqueleto
    if (!jointNode.empty()) {
        Skeleton sk{};
        sk.jointCount = int(jointNode.size());
        int depth[kMaxJoints];
        for (int j = 0; j < sk.jointCount; ++j) {
            int node = jointNode[j];
            nodeTRS(model.nodes[node], sk.bindT[j], sk.bindR[j], sk.bindS[j]);
            std::snprintf(sk.name[j], sizeof(sk.name[j]), "%s", model.nodes[node].name.c_str());
            sk.inverseBind[j] = jointIbm[j];
            int p = nodeParent[node];
            sk.parent[j] = p >= 0 ? nodeToJoint[p] : -1;
            if (sk.parent[j] < 0 && spaceNode < 0) spaceNode = p;
            depth[j] = 0;
            for (int q = p; q >= 0; q = nodeParent[q]) ++depth[j];
            sk.order[j] = j;
        }
        std::stable_sort(sk.order, sk.order + sk.jointCount, [&](int a, int b) { return depth[a] < depth[b]; });
        out.skeletons.push_back(sk);
    }

    // Meshes: uma primitiva = um Mesh do renderer. Índices de osso convertidos para a união.
    std::vector<int> meshSkin(model.meshes.size(), -1);
    for (const auto& n : model.nodes)
        if (n.mesh >= 0 && n.mesh < int(model.meshes.size()) && n.skin >= 0 && n.skin < int(model.skins.size())) meshSkin[n.mesh] = n.skin;
    std::vector<std::vector<std::pair<MeshId, MaterialId>>> meshPrims(model.meshes.size());
    for (size_t mi = 0; mi < model.meshes.size(); ++mi)
        for (const auto& prim : model.meshes[mi].primitives) {
            std::vector<Vertex> v; std::vector<uint32_t> idx; bool skinned = false;
            if (!readPrimitive(model, prim, v, idx, skinned)) continue;
            if (skinned && meshSkin[mi] >= 0) {
                const auto& remap = skinRemap[meshSkin[mi]];
                for (auto& vx : v)
                    for (int k = 0; k < 4; ++k) vx.joints[k] = uint16_t(vx.joints[k] < remap.size() ? remap[vx.joints[k]] : 0);
            }
            MeshId id = renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), skinned);
            if (id == kInvalidId) continue;
            out.meshes.push_back(id);
            MaterialId mat = prim.material >= 0 && prim.material < int(out.materials.size()) ? out.materials[prim.material] : defaultMat;
            meshPrims[mi].push_back({id, mat});
        }

    // Animações dos ossos (todas as skins)
    if (!jointNode.empty()) {
        size_t totalChannels = 0;
        for (const auto& anim : model.animations) totalChannels += anim.channels.size();
        out.channelStorage.reserve(totalChannels);  // exato: ponteiros dos clips não podem ser invalidados
        for (size_t ai = 0; ai < model.animations.size(); ++ai) {
            const auto& anim = model.animations[ai];
            AnimationClip clip;
            if (anim.name.empty()) std::snprintf(clip.name, sizeof(clip.name), "Anim %d", int(ai));
            else std::snprintf(clip.name, sizeof(clip.name), "%s", anim.name.c_str());
            size_t first = out.channelStorage.size();
            for (const auto& ch : anim.channels) {
                if (ch.target_node < 0 || ch.target_node >= int(model.nodes.size())) continue;
                int joint = nodeToJoint[ch.target_node];
                if (joint < 0 || ch.sampler < 0) continue;
                ChannelPath path;
                if (ch.target_path == "rotation") path = ChannelPath::Rotation;
                else if (ch.target_path == "translation") path = ChannelPath::Translation;
                else if (ch.target_path == "scale") path = ChannelPath::Scale;
                else continue;  // morph targets ("weights"): não suportado
                const auto& s = anim.samplers[ch.sampler];
                const auto& tin = model.accessors[s.input];
                int comps = path == ChannelPath::Rotation ? 4 : 3;
                auto times = readFloats(model, tin, 1);
                auto vals = readFloats(model, model.accessors[s.output], comps);
                size_t keys = times.size();
                if (keys == 0) continue;
                bool cubic = s.interpolation == "CUBICSPLINE";  // [in-tangent, valor, out-tangent]: usa só o valor
                if (vals.size() < keys * comps * (cubic ? 3 : 1)) continue;
                out.keyData.emplace_back(keys + keys * comps);
                std::vector<float>& buf = out.keyData.back();
                std::copy(times.begin(), times.end(), buf.begin());
                for (size_t k = 0; k < keys; ++k)
                    for (int c = 0; c < comps; ++c)
                        buf[keys + k * comps + c] = vals[(cubic ? (k * 3 + 1) : k) * comps + c];
                Channel c;
                c.joint = joint;
                c.path = path;
                c.step = s.interpolation == "STEP";
                c.keyCount = int(keys);
                c.times = buf.data();
                c.values = buf.data() + keys;
                clip.duration = std::max(clip.duration, buf[keys - 1]);
                out.channelStorage.push_back(c);
            }
            clip.channels = out.channelStorage.data() + first;
            clip.channelCount = int(out.channelStorage.size() - first);
            out.clips.push_back(clip);
        }
    }

    // Raiz do modelo: um GameObject com o nome do arquivo agrupa toda a hierarquia
    Scene::Handle rootH = scene.create(baseName(path));
    GameObject* root = scene.get(rootH);
    if (!root) { out.error = "cena cheia"; return false; }
    out.roots.push_back(rootH);
    out.allNodes.push_back(rootH);

    // Hierarquia de nós -> GameObjects (primitivas extras viram filhos com MeshRenderer)
    std::vector<Scene::Handle> nodeObj(model.nodes.size());
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        const auto& n = model.nodes[ni];
        char name[32];
        if (!n.name.empty()) std::snprintf(name, sizeof(name), "%s", n.name.c_str());
        else if (n.mesh >= 0 && !model.meshes[n.mesh].name.empty()) std::snprintf(name, sizeof(name), "%s", model.meshes[n.mesh].name.c_str());
        else std::snprintf(name, sizeof(name), "No %d", int(ni));
        nodeObj[ni] = scene.create(name);
        GameObject* go = scene.get(nodeObj[ni]);
        if (!go) { out.error = "cena cheia (limite de objetos)"; return false; }
        out.allNodes.push_back(nodeObj[ni]);
        nodeTRS(n, go->transform.position, go->transform.rotation, go->transform.scale);
        go->transform.markDirty();
    }
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        GameObject* go = scene.get(nodeObj[ni]);
        if (nodeParent[ni] >= 0) go->transform.setParent(&scene.get(nodeObj[nodeParent[ni]])->transform);
        else go->transform.setParent(&root->transform);
    }

    Animator* modelAnim = nullptr;
    if (!out.skeletons.empty()) {
        // glTF: as matrizes de bind estão no espaço da malha com skin; guarda a conversão para o espaço do esqueleto.
        // (a malha pode estar na raiz ou dentro do mesmo nó do esqueleto, ex.: "Character" do Blender com escala 0,01)
        root->transform.updateWorld(glm::mat4(1.f), true);
        glm::mat4 meshWorld = root->transform.world();
        for (size_t ni = 0; ni < model.nodes.size(); ++ni)
            if (model.nodes[ni].skin >= 0 && model.nodes[ni].mesh >= 0) { meshWorld = scene.get(nodeObj[ni])->transform.world(); break; }
        if (spaceNode >= 0) out.skeletons[0].bindToSkel = glm::inverse(scene.get(nodeObj[spaceNode])->transform.world()) * meshWorld;
        auto ah = ctx.animators.acquire();
        modelAnim = ctx.animators.get(ah);
        if (modelAnim) {
            modelAnim->skeleton = &out.skeletons[0];
            if (!out.clips.empty()) { modelAnim->clips = out.clips.data(); modelAnim->clipCount = int(out.clips.size()); }
            modelAnim->space = spaceNode >= 0 ? &scene.get(nodeObj[spaceNode])->transform : &root->transform;
            modelAnim->update(0.f, false);  // pose inicial válida antes do 1º frame
            out.animators.push_back(modelAnim);
        }
    }
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        const auto& n = model.nodes[ni];
        if (n.mesh < 0) continue;
        GameObject* go = scene.get(nodeObj[ni]);
        Animator* anim = n.skin >= 0 ? modelAnim : nullptr;
        bool first = true;
        for (auto [mesh, mat] : meshPrims[n.mesh]) {
            GameObject* target = go;
            if (!first) {
                auto h = scene.create(go->name, go);
                target = scene.get(h);
                if (!target) break;
                out.allNodes.push_back(h);
            }
            auto rh = ctx.meshRenderers.acquire();
            MeshRenderer* mr = ctx.meshRenderers.get(rh);
            if (!mr) break;
            mr->mesh = mesh; mr->material = mat;
            target->meshRenderer = mr;
            target->animator = anim;
            first = false;
        }
    }
    return true;
}

}  // namespace eng
