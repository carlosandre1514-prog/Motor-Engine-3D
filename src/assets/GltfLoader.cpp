#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>
#include <cstdio>
#include <cstring>
#include "assets/GltfLoader.h"
#include "assets/TextureManager.h"

namespace eng {

template <class T>
static const T* accessorData(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t& stride) {
    const auto& bv = m.bufferViews[a.bufferView];
    stride = bv.byteStride ? bv.byteStride : tinygltf::GetComponentSizeInBytes(a.componentType) * tinygltf::GetNumComponentsInType(a.type);
    return reinterpret_cast<const T*>(m.buffers[bv.buffer].data.data() + bv.byteOffset + a.byteOffset);
}

static bool endsWith(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

static void readPrimitive(const tinygltf::Model& m, const tinygltf::Primitive& prim,
                          std::vector<Vertex>& verts, std::vector<uint32_t>& idx, bool& skinned) {
    auto attr = [&](const char* name) -> const tinygltf::Accessor* {
        auto it = prim.attributes.find(name);
        return it == prim.attributes.end() ? nullptr : &m.accessors[it->second];
    };
    const tinygltf::Accessor* pos = attr("POSITION");
    if (!pos) return;
    verts.assign(pos->count, Vertex{glm::vec3(0), glm::vec3(0, 1, 0), glm::vec2(0), glm::vec4(1, 0, 0, 1), glm::u16vec4(0), glm::vec4(1, 0, 0, 0)});

    size_t st;
    const uint8_t* pp = accessorData<uint8_t>(m, *pos, st);
    for (size_t i = 0; i < pos->count; ++i) std::memcpy(&verts[i].pos, pp + i * st, 12);

    if (auto* a = attr("NORMAL")) { const uint8_t* d = accessorData<uint8_t>(m, *a, st); for (size_t i = 0; i < a->count; ++i) std::memcpy(&verts[i].normal, d + i * st, 12); }
    if (auto* a = attr("TEXCOORD_0")) { const uint8_t* d = accessorData<uint8_t>(m, *a, st); for (size_t i = 0; i < a->count; ++i) std::memcpy(&verts[i].uv, d + i * st, 8); }
    if (auto* a = attr("TANGENT")) { const uint8_t* d = accessorData<uint8_t>(m, *a, st); for (size_t i = 0; i < a->count; ++i) std::memcpy(&verts[i].tangent, d + i * st, 16); }
    // Sem TANGENT: gerar com MikkTSpace em produção (omitido aqui).
    if (auto* a = attr("JOINTS_0")) {
        skinned = true;
        const uint8_t* d = accessorData<uint8_t>(m, *a, st);
        for (size_t i = 0; i < a->count; ++i) {
            if (a->componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) { for (int k = 0; k < 4; ++k) verts[i].joints[k] = d[i * st + k]; }
            else std::memcpy(&verts[i].joints, d + i * st, 8);
        }
    }
    if (auto* a = attr("WEIGHTS_0")) { const uint8_t* d = accessorData<uint8_t>(m, *a, st); for (size_t i = 0; i < a->count; ++i) std::memcpy(&verts[i].weights, d + i * st, 16); }

    if (prim.indices >= 0) {
        const auto& a = m.accessors[prim.indices];
        const uint8_t* d = accessorData<uint8_t>(m, a, st);
        idx.resize(a.count);
        for (size_t i = 0; i < a.count; ++i) {
            const uint8_t* e = d + i * st;
            idx[i] = a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT ? *reinterpret_cast<const uint32_t*>(e)
                   : a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? *reinterpret_cast<const uint16_t*>(e) : *e;
        }
    }
}

bool loadGltf(const char* path, Renderer& renderer, Scene& scene, LoadedModel& out) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    bool ok = endsWith(path, ".glb") ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                                     : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!ok) { std::fprintf(stderr, "glTF: %s\n", err.c_str()); return false; }

    // Materiais PBR metallic-roughness (texturas via TextureManager: ASTC/ETC2 quando houver .ktx)
    TextureManager& tex = TextureManager::instance();
    for (const auto& gm : model.materials) {
        Material m;
        const auto& pbr = gm.pbrMetallicRoughness;
        m.albedoFactor = glm::vec4(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
        m.roughness = float(pbr.roughnessFactor);
        m.metallic = float(pbr.metallicFactor);
        m.emissive_factor = glm::vec3(gm.emissiveFactor[0], gm.emissiveFactor[1], gm.emissiveFactor[2]);
        auto texOf = [&](int ti, bool srgb, GLuint fallback) {
            return ti >= 0 ? tex.fromImage(model.images[model.textures[ti].source], srgb) : fallback;
        };
        m.albedo = texOf(pbr.baseColorTexture.index, true, tex.white());
        m.normal = texOf(gm.normalTexture.index, false, tex.flatNormal());
        m.orm = texOf(pbr.metallicRoughnessTexture.index, false, tex.white());
        m.emissive = texOf(gm.emissiveTexture.index, true, tex.white());
        out.materials.push_back(renderer.createMaterial(m));
    }
    if (out.materials.empty()) {
        Material def; def.albedo = tex.white(); def.normal = tex.flatNormal(); def.orm = tex.white(); def.emissive = tex.white();
        out.materials.push_back(renderer.createMaterial(def));
    }

    // Meshes: uma primitiva = um Mesh do renderer. meshPrims[i] = índices em out.meshes
    std::vector<std::vector<std::pair<MeshId, MaterialId>>> meshPrims(model.meshes.size());
    for (size_t mi = 0; mi < model.meshes.size(); ++mi)
        for (const auto& prim : model.meshes[mi].primitives) {
            std::vector<Vertex> v; std::vector<uint32_t> idx; bool skinned = false;
            readPrimitive(model, prim, v, idx, skinned);
            if (v.empty()) continue;
            MeshId id = renderer.createMesh(v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()), skinned);
            out.meshes.push_back(id);
            meshPrims[mi].push_back({id, prim.material >= 0 ? out.materials[prim.material] : out.materials[0]});
        }

    // Esqueletos
    for (const auto& skin : model.skins) {
        Skeleton sk{};
        sk.jointCount = std::min<int>(int(skin.joints.size()), kMaxJoints);
        std::vector<int> nodeToJoint(model.nodes.size(), -1);
        for (int j = 0; j < sk.jointCount; ++j) nodeToJoint[skin.joints[j]] = j;
        for (int j = 0; j < sk.jointCount; ++j) {
            const auto& n = model.nodes[skin.joints[j]];
            sk.parent[j] = -1;
            sk.bindT[j] = n.translation.size() ? glm::vec3(n.translation[0], n.translation[1], n.translation[2]) : glm::vec3(0);
            sk.bindR[j] = n.rotation.size() ? glm::quat(float(n.rotation[3]), float(n.rotation[0]), float(n.rotation[1]), float(n.rotation[2])) : glm::quat(1, 0, 0, 0);
            sk.bindS[j] = n.scale.size() ? glm::vec3(n.scale[0], n.scale[1], n.scale[2]) : glm::vec3(1);
            sk.inverseBind[j] = glm::mat4(1.f);
        }
        for (size_t ni = 0; ni < model.nodes.size(); ++ni)
            for (int c : model.nodes[ni].children)
                if (nodeToJoint[c] >= 0 && nodeToJoint[ni] >= 0) sk.parent[nodeToJoint[c]] = nodeToJoint[ni];
        if (skin.inverseBindMatrices >= 0) {
            size_t st;
            const float* ib = accessorData<float>(model, model.accessors[skin.inverseBindMatrices], st);
            std::memcpy(sk.inverseBind, ib, sizeof(glm::mat4) * sk.jointCount);
        }
        out.skeletons.push_back(sk);
    }

    // Animações (assume um skin; mapeamento nó->joint do primeiro esqueleto)
    if (!out.skeletons.empty()) {
        const auto& skin0 = model.skins[0];
        std::vector<int> nodeToJoint(model.nodes.size(), -1);
        for (size_t j = 0; j < skin0.joints.size() && j < size_t(kMaxJoints); ++j) nodeToJoint[skin0.joints[j]] = int(j);
        out.channelStorage.reserve(4096);  // evita realocação invalidando ponteiros
        for (const auto& anim : model.animations) {
            AnimationClip clip;
            size_t first = out.channelStorage.size();
            for (const auto& ch : anim.channels) {
                int joint = nodeToJoint[ch.target_node];
                if (joint < 0) continue;
                const auto& s = anim.samplers[ch.sampler];
                const auto& tin = model.accessors[s.input];
                const auto& vout = model.accessors[s.output];
                size_t st;
                int comps = tinygltf::GetNumComponentsInType(vout.type);
                out.keyData.emplace_back(tin.count + tin.count * comps);
                std::vector<float>& buf = out.keyData.back();
                const float* t = accessorData<float>(model, tin, st);
                std::memcpy(buf.data(), t, sizeof(float) * tin.count);
                const float* v = accessorData<float>(model, vout, st);
                std::memcpy(buf.data() + tin.count, v, sizeof(float) * tin.count * comps);
                Channel c;
                c.joint = joint;
                c.path = ch.target_path == "rotation" ? ChannelPath::Rotation : ch.target_path == "scale" ? ChannelPath::Scale : ChannelPath::Translation;
                c.keyCount = int(tin.count);
                c.times = buf.data();
                c.values = buf.data() + tin.count;
                clip.duration = std::max(clip.duration, buf[tin.count - 1]);
                out.channelStorage.push_back(c);
            }
            clip.channels = out.channelStorage.data() + first;
            clip.channelCount = int(out.channelStorage.size() - first);
            out.clips.push_back(clip);
        }
    }

    // Hierarquia de nós -> GameObjects (cada primitiva vira um GameObject filho com MeshRenderer)
    // MeshRenderers vivem em um pool simples do loader; produção: pool de componentes na Scene.
    static ObjectPool<MeshRenderer, 8192> rendererPool;
    std::vector<Scene::Handle> nodeObj(model.nodes.size());
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        const auto& n = model.nodes[ni];
        nodeObj[ni] = scene.create(n.name.c_str());
        GameObject* go = scene.get(nodeObj[ni]);
        if (n.translation.size()) go->transform.position = glm::vec3(n.translation[0], n.translation[1], n.translation[2]);
        if (n.rotation.size()) go->transform.rotation = glm::quat(float(n.rotation[3]), float(n.rotation[0]), float(n.rotation[1]), float(n.rotation[2]));
        if (n.scale.size()) go->transform.scale = glm::vec3(n.scale[0], n.scale[1], n.scale[2]);
        if (n.mesh >= 0) {
            bool first = true;
            for (auto [mesh, mat] : meshPrims[n.mesh]) {
                GameObject* target = go;
                if (!first) { auto h = scene.create("prim", go); target = scene.get(h); }
                auto rh = rendererPool.acquire();
                MeshRenderer* mr = rendererPool.get(rh);
                mr->mesh = mesh; mr->material = mat;
                target->meshRenderer = mr;
                first = false;
            }
        }
    }
    for (size_t ni = 0; ni < model.nodes.size(); ++ni)
        for (int c : model.nodes[ni].children) scene.get(nodeObj[c])->transform.setParent(&scene.get(nodeObj[ni])->transform);

    std::vector<bool> isChild(model.nodes.size(), false);
    for (const auto& n : model.nodes) for (int c : n.children) isChild[c] = true;
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) if (!isChild[ni]) out.roots.push_back(nodeObj[ni]);
    return true;
}

}  // namespace eng
