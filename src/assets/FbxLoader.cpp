// Leitor de FBX (Mixamo, Unity Asset Store, Blender...) usando a biblioteca ufbx.
// Converte para as mesmas estruturas do glTF: GameObjects, meshes, materiais, esqueleto e animações.
#include <ufbx.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stb_image.h>
#include <glm/gtc/type_ptr.hpp>
#include "assets/GltfLoader.h"
#include "assets/MeshUtils.h"
#include "assets/TextureManager.h"

namespace eng {

namespace {

glm::mat4 toMat4(const ufbx_matrix& m) {
    glm::mat4 r(1.f);
    for (int c = 0; c < 4; ++c) r[c] = glm::vec4(float(m.cols[c].x), float(m.cols[c].y), float(m.cols[c].z), c == 3 ? 1.f : 0.f);
    return r;
}

const char* baseName(const char* path) {
    const char* b = std::strrchr(path, '/');
    return b ? b + 1 : path;
}

std::string dirOf(const char* path) {
    const char* b = std::strrchr(path, '/');
    return b ? std::string(path, b - path + 1) : std::string();
}

// Textura embutida no FBX ou arquivo na mesma pasta do modelo
GLuint loadTexture(const ufbx_texture* t, const std::string& dir, bool srgb, std::map<const ufbx_texture*, GLuint>& cache) {
    if (!t) return 0;
    auto it = cache.find(t);
    if (it != cache.end()) return it->second;
    int w = 0, h = 0, c = 0;
    unsigned char* px = nullptr;
    if (t->content.size > 0) px = stbi_load_from_memory((const unsigned char*)t->content.data, int(t->content.size), &w, &h, &c, 4);
    if (!px) {
        for (const ufbx_string* f : {&t->relative_filename, &t->filename, &t->absolute_filename}) {
            if (!f->length) continue;
            std::string name(f->data, f->length);
            std::replace(name.begin(), name.end(), '\\', '/');
            const char* b = std::strrchr(name.c_str(), '/');
            for (const std::string& cand : {name, dir + name, dir + (b ? b + 1 : name.c_str())}) {
                px = stbi_load(cand.c_str(), &w, &h, &c, 4);
                if (px) break;
            }
            if (px) break;
        }
    }
    GLuint id = 0;
    if (px) {
        id = TextureManager::instance().fromPixels(px, w, h, 4, srgb);
        stbi_image_free(px);
    }
    cache[t] = id;
    return id;
}

}  // namespace

bool loadFbx(const char* path, LoadContext& ctx, LoadedModel& out) {
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(path, &opts, &err);
    if (!scene) {
        char buf[256];
        ufbx_format_error(buf, sizeof(buf), &err);
        out.error = buf;
        return false;
    }
    Renderer& renderer = ctx.renderer;
    Scene& sc = ctx.scene;
    TextureManager& tex = TextureManager::instance();
    const std::string dir = dirOf(path);

    // ---- materiais
    std::map<const ufbx_texture*, GLuint> texCache;
    std::map<const ufbx_material*, MaterialId> matOf;
    Material def; def.albedo = tex.white(); def.normal = tex.flatNormal(); def.orm = tex.white(); def.emissive = tex.white();
    def.albedoFactor = glm::vec4(0.8f, 0.8f, 0.8f, 1.f); def.roughness = 0.6f; def.metallic = 0.f;
    MaterialId defaultMat = renderer.createMaterial(def);
    for (size_t i = 0; i < scene->materials.count; ++i) {
        const ufbx_material* m = scene->materials.data[i];
        Material mt = def;
        const ufbx_texture* albedoTex = m->pbr.base_color.texture ? m->pbr.base_color.texture : m->fbx.diffuse_color.texture;
        if (m->pbr.base_color.has_value) {
            ufbx_vec4 c = m->pbr.base_color.value_vec4;
            float f = m->pbr.base_factor.has_value ? float(m->pbr.base_factor.value_real) : 1.f;
            mt.albedoFactor = glm::vec4(float(c.x) * f, float(c.y) * f, float(c.z) * f, 1.f);
        } else if (m->fbx.diffuse_color.has_value) {
            ufbx_vec4 c = m->fbx.diffuse_color.value_vec4;
            mt.albedoFactor = glm::vec4(float(c.x), float(c.y), float(c.z), 1.f);
        }
        if (GLuint t = loadTexture(albedoTex, dir, true, texCache)) { mt.albedo = t; mt.albedoFactor = glm::vec4(1.f); }
        if (GLuint t = loadTexture(m->fbx.normal_map.texture ? m->fbx.normal_map.texture : m->pbr.normal_map.texture, dir, false, texCache)) mt.normal = t;
        if (m->pbr.roughness.has_value) mt.roughness = std::clamp(float(m->pbr.roughness.value_real), 0.05f, 1.f);
        if (m->pbr.metalness.has_value) mt.metallic = std::clamp(float(m->pbr.metalness.value_real), 0.f, 1.f);
        if (m->features.double_sided.enabled) mt.doubleSided = true;
        out.materials.push_back(renderer.createMaterial(mt));
        matOf[m] = out.materials.back();
    }

    // ---- esqueleto único: todos os ossos usados por clusters de skin
    std::map<const ufbx_node*, int> jointOf;
    std::vector<const ufbx_node*> jointNodes;
    std::vector<glm::mat4> jointBindWorld;  // pose de bind do osso no mundo
    for (size_t s = 0; s < scene->skin_deformers.count; ++s) {
        const ufbx_skin_deformer* skin = scene->skin_deformers.data[s];
        for (size_t c = 0; c < skin->clusters.count; ++c) {
            const ufbx_skin_cluster* cl = skin->clusters.data[c];
            if (!cl->bone_node || jointOf.count(cl->bone_node) || jointNodes.size() >= size_t(kMaxJoints)) continue;
            jointOf[cl->bone_node] = int(jointNodes.size());
            jointNodes.push_back(cl->bone_node);
            jointBindWorld.push_back(toMat4(cl->bind_to_world));
        }
    }
    // inclui pais intermediários sem skin (ex.: Hips sem peso) para a cadeia ficar contínua
    for (size_t k = 0; k < jointNodes.size() && jointNodes.size() < size_t(kMaxJoints); ++k)
        for (const ufbx_node* p = jointNodes[k]->parent; p && !p->is_root; p = p->parent) {
            if (jointOf.count(p)) break;
            bool hasJointAncestor = false;
            for (const ufbx_node* q = p->parent; q && !q->is_root; q = q->parent) if (jointOf.count(q)) { hasJointAncestor = true; break; }
            if (!hasJointAncestor) break;
            jointOf[p] = int(jointNodes.size());
            jointNodes.push_back(p);
            jointBindWorld.push_back(toMat4(p->node_to_world));
            if (jointNodes.size() >= size_t(kMaxJoints)) break;
        }
    const bool skinned = !jointNodes.empty();
    const ufbx_node* spaceNode = nullptr;  // nó acima da raiz do esqueleto
    if (skinned) {
        Skeleton sk{};
        sk.jointCount = int(jointNodes.size());
        int depth[kMaxJoints];
        for (int j = 0; j < sk.jointCount; ++j) {
            const ufbx_node* n = jointNodes[j];
            const ufbx_transform& t = n->local_transform;
            sk.bindT[j] = glm::vec3(float(t.translation.x), float(t.translation.y), float(t.translation.z));
            sk.bindR[j] = glm::quat(float(t.rotation.w), float(t.rotation.x), float(t.rotation.y), float(t.rotation.z));
            sk.bindS[j] = glm::vec3(float(t.scale.x), float(t.scale.y), float(t.scale.z));
            std::snprintf(sk.name[j], sizeof(sk.name[j]), "%.*s", int(n->name.length), n->name.data);
            auto pit = n->parent ? jointOf.find(n->parent) : jointOf.end();
            sk.parent[j] = pit != jointOf.end() ? pit->second : -1;
            if (sk.parent[j] < 0 && !spaceNode) spaceNode = n->parent;
            depth[j] = 0;
            for (const ufbx_node* p = n->parent; p; p = p->parent) ++depth[j];
            sk.order[j] = j;
        }
        std::stable_sort(sk.order, sk.order + sk.jointCount, [&](int a, int b) { return depth[a] < depth[b]; });
        // inverseBind do FBX é geometria->osso; os ossos sem cluster usam o mundo. Como a malha usa o espaço do
        // esqueleto (nó acima da raiz), converte tudo para esse espaço.
        glm::mat4 spaceW = spaceNode ? toMat4(spaceNode->node_to_world) : glm::mat4(1.f);
        for (int j = 0; j < sk.jointCount; ++j)  // bind do osso no espaço do esqueleto
            sk.inverseBind[j] = glm::inverse(glm::inverse(spaceW) * jointBindWorld[j]);
        out.skeletons.push_back(sk);

        // ---- animações (cada "take" do FBX vira um clip)
        size_t totalCh = 0;
        std::vector<ufbx_baked_anim*> baked;
        for (size_t a = 0; a < scene->anim_stacks.count; ++a) {
            ufbx_bake_opts bo = {};
            bo.resample_rate = 30.0;
            ufbx_error be;
            ufbx_baked_anim* b = ufbx_bake_anim(scene, scene->anim_stacks.data[a]->anim, &bo, &be);
            baked.push_back(b);
            if (b) totalCh += b->nodes.count * 3;
        }
        out.channelStorage.reserve(totalCh);
        for (size_t a = 0; a < baked.size(); ++a) {
            ufbx_baked_anim* b = baked[a];
            if (!b) continue;
            AnimationClip clip;
            const ufbx_anim_stack* st = scene->anim_stacks.data[a];
            if (st->name.length && std::strcmp(st->name.data, "mixamo.com") != 0)
                std::snprintf(clip.name, sizeof(clip.name), "%.*s", int(st->name.length), st->name.data);
            else
                std::snprintf(clip.name, sizeof(clip.name), "%s", baseName(path));
            size_t first = out.channelStorage.size();
            double t0 = b->playback_time_begin;
            for (size_t k = 0; k < b->nodes.count; ++k) {
                const ufbx_baked_node& bn = b->nodes.data[k];
                const ufbx_node* node = scene->nodes.data[bn.typed_id];
                auto jt = jointOf.find(node);
                if (jt == jointOf.end()) continue;
                auto addVec = [&](const ufbx_baked_vec3_list& keys, ChannelPath path) {
                    if (!keys.count) return;
                    size_t n = keys.count;
                    out.keyData.emplace_back(n * 4);
                    std::vector<float>& buf = out.keyData.back();
                    for (size_t i = 0; i < n; ++i) {
                        buf[i] = float(keys.data[i].time - t0);
                        buf[n + i * 3] = float(keys.data[i].value.x); buf[n + i * 3 + 1] = float(keys.data[i].value.y); buf[n + i * 3 + 2] = float(keys.data[i].value.z);
                    }
                    Channel c; c.joint = jt->second; c.path = path; c.keyCount = int(n); c.times = buf.data(); c.values = buf.data() + n;
                    clip.duration = std::max(clip.duration, buf[n - 1]);
                    out.channelStorage.push_back(c);
                };
                addVec(bn.translation_keys, ChannelPath::Translation);
                addVec(bn.scale_keys, ChannelPath::Scale);
                if (bn.rotation_keys.count) {
                    size_t n = bn.rotation_keys.count;
                    out.keyData.emplace_back(n * 5);
                    std::vector<float>& buf = out.keyData.back();
                    for (size_t i = 0; i < n; ++i) {
                        const ufbx_quat& q = bn.rotation_keys.data[i].value;
                        buf[i] = float(bn.rotation_keys.data[i].time - t0);
                        buf[n + i * 4] = float(q.x); buf[n + i * 4 + 1] = float(q.y); buf[n + i * 4 + 2] = float(q.z); buf[n + i * 4 + 3] = float(q.w);
                    }
                    Channel c; c.joint = jt->second; c.path = ChannelPath::Rotation; c.keyCount = int(n); c.times = buf.data(); c.values = buf.data() + n;
                    clip.duration = std::max(clip.duration, buf[n - 1]);
                    out.channelStorage.push_back(c);
                }
            }
            clip.channels = out.channelStorage.data() + first;
            clip.channelCount = int(out.channelStorage.size() - first);
            if (clip.channelCount > 0 && clip.duration > 0.f) out.clips.push_back(clip);
            ufbx_free_baked_anim(b);
        }
    }

    // ---- GameObjects (hierarquia de nós)
    Scene::Handle rootH = sc.create(baseName(path));
    GameObject* root = sc.get(rootH);
    if (!root) { ufbx_free_scene(scene); out.error = "cena cheia"; return false; }
    out.roots.push_back(rootH);
    out.allNodes.push_back(rootH);
    std::map<const ufbx_node*, Scene::Handle> objOf;
    for (size_t i = 0; i < scene->nodes.count; ++i) {
        const ufbx_node* n = scene->nodes.data[i];
        if (n->is_root) continue;
        char name[32];
        std::snprintf(name, sizeof(name), "%.*s", int(n->name.length), n->name.data);
        Scene::Handle h = sc.create(name[0] ? name : "No");
        GameObject* go = sc.get(h);
        if (!go) { ufbx_free_scene(scene); out.error = "cena cheia (limite de objetos)"; return false; }
        const ufbx_transform& t = n->local_transform;
        go->transform.position = glm::vec3(float(t.translation.x), float(t.translation.y), float(t.translation.z));
        go->transform.rotation = glm::quat(float(t.rotation.w), float(t.rotation.x), float(t.rotation.y), float(t.rotation.z));
        go->transform.scale = glm::vec3(float(t.scale.x), float(t.scale.y), float(t.scale.z));
        go->transform.markDirty();
        objOf[n] = h;
        out.allNodes.push_back(h);
    }
    for (auto& [n, h] : objOf) {
        GameObject* go = sc.get(h);
        auto pit = n->parent ? objOf.find(n->parent) : objOf.end();
        go->transform.setParent(pit != objOf.end() ? &sc.get(pit->second)->transform : &root->transform);
    }

    // ---- Animator (um para o modelo todo)
    Animator* anim = nullptr;
    if (skinned) {
        auto ah = ctx.animators.acquire();
        anim = ctx.animators.get(ah);
        if (anim) {
            anim->skeleton = &out.skeletons[0];
            if (!out.clips.empty()) { anim->clips = out.clips.data(); anim->clipCount = int(out.clips.size()); }
            auto sp = spaceNode ? objOf.find(spaceNode) : objOf.end();
            anim->space = sp != objOf.end() ? &sc.get(sp->second)->transform : &root->transform;
            anim->update(0.f, false);
            out.animators.push_back(anim);
        }
    }

    // ---- malhas: uma por material de cada mesh
    std::vector<uint32_t> triIdx;
    for (size_t i = 0; i < scene->nodes.count; ++i) {
        const ufbx_node* node = scene->nodes.data[i];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh || !objOf.count(node)) continue;
        GameObject* go = sc.get(objOf[node]);
        const ufbx_skin_deformer* skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
        // skinned: vértices no espaço do esqueleto; estático: espaço local do nó (geometry_to_node)
        glm::mat4 geomXf = skin ? glm::inverse(spaceNode ? toMat4(spaceNode->node_to_world) : glm::mat4(1.f)) * toMat4(node->geometry_to_world)
                                : toMat4(node->geometry_to_node);
        glm::mat3 nrmXf = glm::transpose(glm::inverse(glm::mat3(geomXf)));
        triIdx.resize(mesh->max_face_triangles * 3);
        bool first = true;
        for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
            const ufbx_mesh_part& part = mesh->material_parts.data[pi];
            if (part.num_triangles == 0) continue;
            std::vector<Vertex> verts;
            std::vector<uint32_t> idx;
            verts.reserve(part.num_triangles * 3);
            for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                ufbx_face face = mesh->faces.data[part.face_indices.data[fi]];
                uint32_t nt = ufbx_triangulate_face(triIdx.data(), triIdx.size(), mesh, face);
                for (uint32_t k = 0; k < nt * 3; ++k) {
                    uint32_t ix = triIdx[k];
                    ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
                    ufbx_vec3 nn = mesh->vertex_normal.exists ? ufbx_get_vertex_vec3(&mesh->vertex_normal, ix) : ufbx_vec3{0, 1, 0};
                    ufbx_vec2 uv = mesh->vertex_uv.exists ? ufbx_get_vertex_vec2(&mesh->vertex_uv, ix) : ufbx_vec2{0, 0};
                    Vertex v = makeVertex(glm::vec3(geomXf * glm::vec4(float(p.x), float(p.y), float(p.z), 1.f)),
                                          glm::normalize(nrmXf * glm::vec3(float(nn.x), float(nn.y), float(nn.z))),
                                          {float(uv.x), 1.f - float(uv.y)});  // FBX: v para cima
                    if (mesh->vertex_color.exists) {
                        ufbx_vec4 c = ufbx_get_vertex_vec4(&mesh->vertex_color, ix);
                        v.color = glm::vec4(float(c.x), float(c.y), float(c.z), float(c.w));
                    }
                    if (skin) {
                        uint32_t vi = mesh->vertex_indices.data[ix];
                        const ufbx_skin_vertex& sv = skin->vertices.data[vi];
                        float sum = 0.f;
                        int used = 0;
                        for (uint32_t w = 0; w < sv.num_weights && used < 4; ++w) {
                            const ufbx_skin_weight& sw = skin->weights.data[sv.weight_begin + w];
                            const ufbx_skin_cluster* cl = skin->clusters.data[sw.cluster_index];
                            auto jt = cl->bone_node ? jointOf.find(cl->bone_node) : jointOf.end();
                            if (jt == jointOf.end()) continue;
                            v.joints[used] = uint16_t(jt->second);
                            v.weights[used] = float(sw.weight);
                            sum += float(sw.weight);
                            ++used;
                        }
                        for (int u = used; u < 4; ++u) { v.joints[u] = 0; v.weights[u] = 0.f; }
                        if (sum > 1e-6f) v.weights /= sum; else v.weights = glm::vec4(1, 0, 0, 0);
                    }
                    idx.push_back(uint32_t(verts.size()));
                    verts.push_back(v);
                }
            }
            if (idx.empty()) continue;
            generateTangents(verts, idx);
            MeshId id = renderer.createMesh(verts.data(), uint32_t(verts.size()), idx.data(), uint32_t(idx.size()), skin != nullptr);
            if (id == kInvalidId) continue;
            out.meshes.push_back(id);
            const ufbx_material* m = part.index < mesh->materials.count ? mesh->materials.data[part.index] : nullptr;
            MaterialId mat = m && matOf.count(m) ? matOf[m] : defaultMat;
            GameObject* target = go;
            if (!first) {
                Scene::Handle h = sc.create(go->name, go);
                target = sc.get(h);
                if (!target) break;
                out.allNodes.push_back(h);
            }
            auto rh = ctx.meshRenderers.acquire();
            MeshRenderer* mr = ctx.meshRenderers.get(rh);
            if (!mr) break;
            mr->mesh = id; mr->material = mat;
            target->meshRenderer = mr;
            if (skin) target->animator = anim;
            first = false;
        }
    }
    ufbx_free_scene(scene);
    return true;
}

}  // namespace eng
