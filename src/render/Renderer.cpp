#include "render/Renderer.h"
#include "render/Particles.h"
#include <stb_image.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <glm/gtc/matrix_transform.hpp>

#ifdef __ANDROID__
#include <android/log.h>
#define ENG_LOG(...) __android_log_print(ANDROID_LOG_ERROR, "Mobile3D", __VA_ARGS__)
#else
#define ENG_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif

namespace eng {

// ---------- utilidades de shader (apenas no init; alocação permitida aqui) ----------
static std::string readFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { ENG_LOG("arquivo nao encontrado: %s\n", path.c_str()); return {}; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(size_t(n), '\0');
    size_t got = std::fread(s.data(), 1, size_t(n), f);
    std::fclose(f);
    s.resize(got);
    return s;
}

static std::string g_common;  // shaders/common.glsl

// Monta: linha #version + #define da variante + common.glsl + resto do arquivo
static GLuint compile(GLenum type, const std::string& src, const char* define, const char* name) {
    size_t nl = src.find('\n');
    std::string full = src.substr(0, nl + 1) + "#define " + define + "\n" + g_common + "\n#line 2\n" + src.substr(nl + 1);
    GLuint s = glCreateShader(type);
    const char* p = full.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        ENG_LOG("erro no shader %s (%s): %s\n", name, define, log);
    }
    return s;
}

static GLuint link(const std::string& vs, const std::string& fs, const char* define, const char* name) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, define, name), f = compile(GL_FRAGMENT_SHADER, fs, define, name);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        ENG_LOG("erro de link %s (%s): %s\n", name, define, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

GLuint Renderer::loadProgram(const char* vsFile, const char* fsFile) {
    std::string vs = readFile(shaderDir_ + "/" + vsFile), fs = readFile(shaderDir_ + "/" + fsFile);
    if (vs.empty() || fs.empty()) return 0;
    GLuint p = link(vs, fs, "EXTRA", vsFile);
    if (!p) return 0;
    GLuint bi = glGetUniformBlockIndex(p, "SceneBlock");
    if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 0);
    return p;
}

bool Renderer::init(const char* shaderDir) {
    std::string d = shaderDir;
    shaderDir_ = d;
    g_common = readFile(d + "/common.glsl");
    std::string pv = readFile(d + "/pbr.vert"), pf = readFile(d + "/pbr.frag");
    std::string sv = readFile(d + "/shadow.vert"), sf = readFile(d + "/shadow.frag");
    std::string kv = readFile(d + "/sky.vert"), kf = readFile(d + "/sky.frag");
    std::string lv = readFile(d + "/line.vert"), lf = readFile(d + "/line.frag");
    if (g_common.empty() || pv.empty() || pf.empty() || sv.empty() || sf.empty() || kv.empty() || kf.empty() || lv.empty() || lf.empty())
        return false;

    pbr_.id = link(pv, pf, "STANDARD", "pbr");
    pbrInst_.id = link(pv, pf, "INSTANCED", "pbr");
    pbrSkin_.id = link(pv, pf, "SKINNED", "pbr");
    shadow_.id = link(sv, sf, "STANDARD", "shadow");
    shadowInst_.id = link(sv, sf, "INSTANCED", "shadow");
    shadowSkin_.id = link(sv, sf, "SKINNED", "shadow");
    skyProg_ = link(kv, kf, "SKY", "sky");
    lineProg_ = link(lv, lf, "LINES", "line");
    for (Program* p : {&pbr_, &pbrInst_, &pbrSkin_, &shadow_, &shadowInst_, &shadowSkin_})
        if (!p->id) return false;
    if (!skyProg_ || !lineProg_) return false;

    for (Program* p : {&pbr_, &pbrInst_, &pbrSkin_, &shadow_, &shadowInst_, &shadowSkin_}) {
        p->uModel = glGetUniformLocation(p->id, "uModel");
        p->uLightVP = glGetUniformLocation(p->id, "uLightVP");
        p->uAlbedoF = glGetUniformLocation(p->id, "uAlbedoFactor");
        p->uRough = glGetUniformLocation(p->id, "uRoughnessFactor");
        p->uMetal = glGetUniformLocation(p->id, "uMetallicFactor");
        p->uEmis = glGetUniformLocation(p->id, "uEmissiveFactor");
        p->uAlphaCut = glGetUniformLocation(p->id, "uAlphaCutoff");
        p->uOccStrength = glGetUniformLocation(p->id, "uOcclusionStrength");
        p->uMode = glGetUniformLocation(p->id, "uMode");
        p->uTime = glGetUniformLocation(p->id, "uTime");
        GLuint bi = glGetUniformBlockIndex(p->id, "SceneBlock");
        if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p->id, bi, 0);
        bi = glGetUniformBlockIndex(p->id, "JointBlock");
        if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p->id, bi, 1);
    }
    for (GLuint id : {skyProg_, lineProg_}) {
        GLuint bi = glGetUniformBlockIndex(id, "SceneBlock");
        if (bi != GL_INVALID_INDEX) glUniformBlockBinding(id, bi, 0);
    }
    skyInvVP_ = glGetUniformLocation(skyProg_, "uInvViewProj");
    skyZenith_ = glGetUniformLocation(skyProg_, "uZenith");
    skyHorizon_ = glGetUniformLocation(skyProg_, "uHorizon");
    skyGround_ = glGetUniformLocation(skyProg_, "uGround");
    skyIntensity_ = glGetUniformLocation(skyProg_, "uSkyIntensity");
    skyCapture_ = glGetUniformLocation(skyProg_, "uCapture");

    // Unidades de textura dos samplers (fixas): 0 albedo, 1 normal, 2 metal/rugosidade, 3 emissivo, 4 sombra, 5 ambiente, 6 AO
    for (Program* p : {&pbr_, &pbrInst_, &pbrSkin_}) {
        glUseProgram(p->id);
        glUniform1i(glGetUniformLocation(p->id, "uAlbedoMap"), 0);
        glUniform1i(glGetUniformLocation(p->id, "uNormalMap"), 1);
        glUniform1i(glGetUniformLocation(p->id, "uOrmMap"), 2);
        glUniform1i(glGetUniformLocation(p->id, "uEmissiveMap"), 3);
        glUniform1i(glGetUniformLocation(p->id, "uShadowMap"), 4);
        glUniform1i(glGetUniformLocation(p->id, "uEnvMap"), 5);
        glUniform1i(glGetUniformLocation(p->id, "uOcclusionMap"), 6);
        glUniform1i(glGetUniformLocation(p->id, "uTerrainAlb"), 7);
        glUniform1i(glGetUniformLocation(p->id, "uTerrainNh"), 8);
    }

    glGenBuffers(1, &sceneUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, sceneUbo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(SceneUBO), nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &jointUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, jointUbo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(glm::mat4) * kMaxJoints, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &cutUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, cutUbo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(glm::mat4) * kMaxCuts, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, cutUbo_);
    glGenBuffers(1, &instanceVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::mat4) * kMaxDraws, nullptr, GL_STREAM_DRAW);
    instanceScratch_ = static_cast<glm::mat4*>(std::malloc(sizeof(glm::mat4) * kMaxDraws));
    for (auto& lv2 : lineVerts_) lv2 = static_cast<LineVertex*>(std::malloc(sizeof(LineVertex) * kMaxLineVerts));

    // VAO das linhas do editor
    glGenVertexArrays(1, &lineVao_);
    glBindVertexArray(lineVao_);
    glGenBuffers(1, &lineVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(LineVertex) * kMaxLineVerts, nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex), (void*)offsetof(LineVertex, pos));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex), (void*)offsetof(LineVertex, color));
    glBindVertexArray(0);
    glGenVertexArrays(1, &emptyVao_);  // triângulo de tela cheia (gl_VertexID)

    // Shadow map: array de profundidade com comparação por hardware
    glGenTextures(1, &shadowTex_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowTex_);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, kShadowMapSize, kShadowMapSize, kNumCascades);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &shadowFbo_);

    // Cubemap do ambiente (HDR, com mips = "pré-filtro" barato por rugosidade)
    envMips_ = 1;
    for (int s = kEnvSize; s > 1; s >>= 1) ++envMips_;
    glGenTextures(1, &envCube_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, envCube_);
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, envMips_, GL_RGBA16F, kEnvSize, kEnvSize);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &envFbo_);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    envValid_ = false;
    return true;
}

void Renderer::resize(int w, int h) { width_ = std::max(w, 1); height_ = std::max(h, 1); }

void Renderer::shutdown() {
    clearAssets();
    std::free(instanceScratch_);
    instanceScratch_ = nullptr;
    for (auto& lv : lineVerts_) { std::free(lv); lv = nullptr; }
    for (Program* p : {&pbr_, &pbrInst_, &pbrSkin_, &shadow_, &shadowInst_, &shadowSkin_}) glDeleteProgram(p->id);
    glDeleteProgram(skyProg_);
    glDeleteProgram(lineProg_);
    GLuint bufs[] = {sceneUbo_, jointUbo_, instanceVbo_, lineVbo_, cutUbo_};
    glDeleteBuffers(4, bufs);
    GLuint vaos[] = {lineVao_, emptyVao_};
    glDeleteVertexArrays(2, vaos);
    GLuint tex[] = {shadowTex_, envCube_, terrainAlb_, terrainNh_};
    glDeleteTextures(4, tex);
    terrainAlb_ = terrainNh_ = 0;
    GLuint fbos[] = {shadowFbo_, envFbo_};
    glDeleteFramebuffers(2, fbos);
}

void Renderer::clearAssets() {
    for (uint32_t i = 0; i < meshCount_; ++i) {
        glDeleteVertexArrays(1, &meshes_[i].vao);
        glDeleteBuffers(1, &meshes_[i].vbo);
        glDeleteBuffers(1, &meshes_[i].ibo);
        meshes_[i] = Mesh{};
    }
    meshCount_ = 0;
    materialCount_ = 0;
    drawCount_ = 0;
}

MeshId Renderer::createMesh(const Vertex* v, uint32_t vc, const uint32_t* idx, uint32_t ic, bool skinned) {
    if (meshCount_ >= kMaxMeshes || vc == 0 || ic == 0) { ENG_LOG("createMesh: limite atingido ou mesh vazio\n"); return kInvalidId; }
    Mesh& m = meshes_[meshCount_];
    m.indexCount = ic;
    m.vertexCount = vc;
    m.skinned = skinned;
    m.bounds = {glm::vec3(1e9f), glm::vec3(-1e9f)};
    for (uint32_t i = 0; i < vc; ++i) { m.bounds.min = glm::min(m.bounds.min, v[i].pos); m.bounds.max = glm::max(m.bounds.max, v[i].pos); }

    glGenVertexArrays(1, &m.vao);
    glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vc, v, GL_STATIC_DRAW);
    glGenBuffers(1, &m.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint32_t) * ic, idx, GL_STATIC_DRAW);

    const GLsizei S = sizeof(Vertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, S, (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, S, (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, S, (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, S, (void*)offsetof(Vertex, tangent));
    glEnableVertexAttribArray(4); glVertexAttribIPointer(4, 4, GL_UNSIGNED_SHORT, S, (void*)offsetof(Vertex, joints));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, S, (void*)offsetof(Vertex, weights));
    glEnableVertexAttribArray(10); glVertexAttribPointer(10, 4, GL_FLOAT, GL_FALSE, S, (void*)offsetof(Vertex, color));
    // Matriz por instância (locations 6..9, divisor 1) lendo do buffer compartilhado de instâncias
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
    for (int k = 0; k < 4; ++k) {
        glEnableVertexAttribArray(6 + k);
        glVertexAttribPointer(6 + k, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(sizeof(glm::vec4) * k));
        glVertexAttribDivisor(6 + k, 1);
    }
    glBindVertexArray(0);
    return meshCount_++;
}

void Renderer::updateMesh(MeshId id, const Vertex* v, uint32_t vc, const uint32_t* idx, uint32_t ic) {
    if (id >= meshCount_ || vc == 0 || ic == 0) return;
    Mesh& m = meshes_[id];
    m.indexCount = ic;
    m.vertexCount = vc;
    m.bounds = {glm::vec3(1e9f), glm::vec3(-1e9f)};
    for (uint32_t i = 0; i < vc; ++i) { m.bounds.min = glm::min(m.bounds.min, v[i].pos); m.bounds.max = glm::max(m.bounds.max, v[i].pos); }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vc, v, GL_STATIC_DRAW);
    glBindVertexArray(m.vao);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint32_t) * ic, idx, GL_STATIC_DRAW);
    glBindVertexArray(0);
}

MaterialId Renderer::createMaterial(const Material& m) {
    if (materialCount_ >= kMaxMaterials) { ENG_LOG("createMaterial: limite atingido\n"); return materialCount_ ? 0 : kInvalidId; }
    materials_[materialCount_] = m;
    return materialCount_++;
}

// ---------- ambiente ----------
void Renderer::updateEnvironment(const Environment& env) {
    if (envValid_ && env == envCached_) return;
    envCached_ = env;
    envValid_ = true;
    computeSkySH(env, sh_);

    // Captura o céu nas 6 faces (convenção de cubemap do GL) e gera mips
    static const glm::vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 ups[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    glm::mat4 proj = glm::perspective(glm::radians(90.f), 1.f, 0.1f, 10.f);
    glBindFramebuffer(GL_FRAMEBUFFER, envFbo_);
    glViewport(0, 0, kEnvSize, kEnvSize);
    glDisable(GL_DEPTH_TEST);
    for (int f = 0; f < 6; ++f) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, envCube_, 0);
        glm::mat4 view = glm::lookAt(glm::vec3(0.f), dirs[f], ups[f]);
        drawSky(glm::inverse(proj * view), env, true);
    }
    glEnable(GL_DEPTH_TEST);
    glBindTexture(GL_TEXTURE_CUBE_MAP, envCube_);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
}

void Renderer::drawSky(const glm::mat4& invVP, const Environment& env, bool capture) {
    glUseProgram(skyProg_);
    glUniformMatrix4fv(skyInvVP_, 1, GL_FALSE, &invVP[0][0]);
    glUniform3fv(skyZenith_, 1, &env.zenith[0]);
    glUniform3fv(skyHorizon_, 1, &env.horizon[0]);
    glUniform3fv(skyGround_, 1, &env.ground[0]);
    glUniform1f(skyIntensity_, env.skyIntensity);
    glUniform1i(skyCapture_, capture ? 1 : 0);
    glBindVertexArray(emptyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

// ---------- frame ----------
void Renderer::beginFrame(const Camera& cam, const DirectionalLight& sun) {
    drawCount_ = 0;
    stats_ = {};
    lineCount_[0] = lineCount_[1] = 0;
    camFrustum_ = cam.frustum();
    cascades_ = computeCascades(cam, sun.direction, shadowDistance);
    for (int c = 0; c < kNumCascades; ++c) cascadeFrustum_[c].extract(cascades_.viewProj[c]);
    if (!sun.castShadows) for (auto& f : cascadeFrustum_) f = Frustum{};
}

void Renderer::submit(const GameObject& go, const glm::mat4& model, const glm::mat4* palette) {
    if (!go.active || !go.meshRenderer || drawCount_ >= kMaxDraws) return;
    const MeshRenderer& mr = *go.meshRenderer;
    if (mr.mesh >= meshCount_ || mr.material >= materialCount_) return;
    const Mesh& mesh = meshes_[mr.mesh];
    uint32_t mask = 0;
    glm::mat4 m = model;
    if (palette) {
        // Skinned: a pose muda os limites; sem culling (são poucos objetos)
        mask = 1u | (mr.castShadows ? 0xEu : 0u);
    } else {
        AABB world = transformAABB(mesh.bounds, model);
        if (camFrustum_.intersects(world)) mask |= 1u;            // culling da câmera
        // objetos com LOD (detalhes, árvores) só fazem sombra nas 2 cascatas perto da câmera
        const int shadowCascades = mr.lodFar < 1e29f ? 2 : kNumCascades;
        if (mr.castShadows)
            for (int c = 0; c < shadowCascades; ++c)              // culling por cascata
                if (cascadeFrustum_[c].planes[0] != glm::vec4(0.f) && cascadeFrustum_[c].intersects(world)) mask |= 2u << c;
        // aberturas que encostam no objeto: bits na linha de baixo da matriz (o shader descarta o que fica dentro)
        if (cutCount > 0 && !go.noCut && mask) {
            uint32_t bits = 0;
            for (int i = 0; i < cutCount && i < kMaxCuts; ++i) {
                const AABB& b = cuts[i].bounds;
                if (glm::all(glm::lessThanEqual(b.min, world.max)) && glm::all(glm::lessThanEqual(world.min, b.max))) bits |= 1u << i;
            }
            if (bits) { m[0][3] = float(bits & 0xFFFFu); m[1][3] = float(bits >> 16); }
        }
    }
    if (!mask) return;
    if (mask & 1u) ++stats_.visibleObjects;
    draws_[drawCount_++] = {(uint64_t(mr.material) << 32) | mr.mesh, m, mr.mesh, mr.material, palette, mask};
}

void Renderer::bindMaterial(const Program& p, const Material& m) {
    glUniform4fv(p.uAlbedoF, 1, &m.albedoFactor[0]);
    glUniform1f(p.uRough, m.roughness);
    glUniform1f(p.uMetal, m.metallic);
    glUniform3fv(p.uEmis, 1, &m.emissive_factor[0]);
    glUniform1f(p.uAlphaCut, m.alphaCutoff);
    glUniform1i(p.uMode, m.mode);
    if (p.uTime >= 0) glUniform1f(p.uTime, time);
    glUniform1f(p.uOccStrength, m.occlusion ? m.occlusionStrength : 0.f);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m.albedo);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m.normal);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m.orm);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m.emissive);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, shadowTex_);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_CUBE_MAP, envCube_);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, m.occlusion ? m.occlusion : m.emissive);
    if (m.mode == 3) {
        glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D_ARRAY, terrainAlb_);
        glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D_ARRAY, terrainNh_);
    }
    if (m.doubleSided) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
}

bool Renderer::loadTerrainLayers(const char* const* names, int count) {
    if (terrainAlb_) return true;
    GLuint ids[2] = {0, 0};
    for (int kind = 0; kind < 2; ++kind) {
        const char* suffix = kind == 0 ? "_alb.jpg" : "_nh.jpg";
        int W = 0, H = 0;
        for (int l = 0; l < count; ++l) {
            int w, h, comp;
            std::string path = shaderDir_ + "/terrain/" + names[l] + suffix;
            unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
            if (!px || (l > 0 && (w != W || h != H))) {
                if (px) stbi_image_free(px);
                glDeleteTextures(2, ids);
                return false;
            }
            if (l == 0) {
                W = w; H = h;
                glGenTextures(1, &ids[kind]);
                glBindTexture(GL_TEXTURE_2D_ARRAY, ids[kind]);
                int levels = 1;
                while ((std::max(W, H) >> levels) > 0) ++levels;
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, kind == 0 ? GL_SRGB8_ALPHA8 : GL_RGBA8, W, H, count);
            }
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, l, W, H, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            stbi_image_free(px);
        }
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
        while (glGetError() != GL_NO_ERROR) {}
        GLfloat aniso = 0.f;
        glGetFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &aniso);
        if (glGetError() == GL_NO_ERROR && aniso > 1.f) glTexParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE, std::min(aniso, 4.f));
    }
    terrainAlb_ = ids[0]; terrainNh_ = ids[1];
    return true;
}

void Renderer::render(const Camera& cam, const DirectionalLight& sun, const Environment& env,
                      const PointLight* points, int pointCount) {
    updateEnvironment(env);

    // 1) UBO da cena
    SceneUBO u{};
    const CascadeSet& cs = cascades_;
    u.viewProj = cam.viewProj();
    u.camPos = glm::vec4(cam.position, 1.f);
    u.dirDir = glm::vec4(glm::normalize(sun.direction), sun.castShadows ? 1.f : 0.f);
    u.dirColor = glm::vec4(sun.color * sun.intensity, 1.f);
    for (int i = 0; i < kNumCascades; ++i) u.cascadeVP[i] = cs.viewProj[i];
    u.cascadeSplits = glm::vec4(cs.splitDepth[0], cs.splitDepth[1], cs.splitDepth[2], cs.splitDepth[2]);
    u.cascadeTexel = glm::vec4(cs.texelWorld[0], cs.texelWorld[1], cs.texelWorld[2], 0.f);
    u.envParams = glm::vec4(env.exposure, float(envMips_ - 2), 1.f, 0.f);
    for (int i = 0; i < 9; ++i) u.sh[i] = sh_[i];
    u.fog = glm::vec4(env.horizon * env.skyIntensity, fogDensity);
    u.counts.x = std::min(pointCount, kMaxPointLights);
    for (int i = 0; i < u.counts.x; ++i) {
        u.points[i].posRadius = glm::vec4(points[i].position, points[i].radius);
        u.points[i].colorIntensity = glm::vec4(points[i].color, points[i].intensity);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, sceneUbo_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(u), &u);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, sceneUbo_);

    if (cutCount > 0) {
        glm::mat4 cm[kMaxCuts];
        for (int i = 0; i < cutCount && i < kMaxCuts; ++i) cm[i] = cuts[i].worldToBox;
        glBindBuffer(GL_UNIFORM_BUFFER, cutUbo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(glm::mat4) * std::min(cutCount, kMaxCuts), cm);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, cutUbo_);

    // 2) ordena por (material, mesh) para formar lotes instanciáveis (std::sort não aloca)
    std::sort(draws_, draws_ + drawCount_, [](const DrawItem& a, const DrawItem& b) { return a.key < b.key; });

    // 3) sombras e passe principal
    if (sun.castShadows) shadowPass(cs);

    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, width_, height_);
    glDepthMask(GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawBatches(1u, nullptr);
    glEnable(GL_CULL_FACE);

    // 4) céu por último: só preenche pixels vazios (economiza overdraw)
    glDepthMask(GL_FALSE);
    glm::mat4 viewNoT = cam.view();
    viewNoT[3] = glm::vec4(0, 0, 0, 1);
    drawSky(glm::inverse(cam.proj() * viewNoT), env, false);
    glDepthMask(GL_TRUE);

    // 5) partículas (fogo, fumaça, magia...) e linhas do editor
    if (particles) {
        stats_.drawCalls += particles->draw(cam);
        glEnable(GL_CULL_FACE);
    }
    flushLines();

    // GPUs tiled: não precisamos gravar o depth de volta na memória
    if (targetFbo == 0) {
        const GLenum att[] = {GL_DEPTH, GL_STENCIL};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, att);
    } else {
        const GLenum att[] = {GL_DEPTH_ATTACHMENT};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, att);
    }
}

void Renderer::shadowPass(const CascadeSet& cs) {
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, kShadowMapSize, kShadowMapSize);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glDisable(GL_CULL_FACE);               // pega também planos finos / malhas abertas
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.f);
    for (int c = 0; c < kNumCascades; ++c) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowTex_, 0, c);
        glClear(GL_DEPTH_BUFFER_BIT);
        drawBatches(2u << c, &cs.viewProj[c]);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
}

void Renderer::drawBatches(uint32_t passBit, const glm::mat4* lightVP) {
    const bool shadowOnly = lightVP != nullptr;
    uint32_t i = 0;
    while (i < drawCount_) {
        const DrawItem& first = draws_[i];
        const Mesh& mesh = meshes_[first.mesh];

        if (first.jointPalette) {  // skinned: um draw por objeto
            ++i;
            if (!(first.passMask & passBit)) continue;
            const Program& p = shadowOnly ? shadowSkin_ : pbrSkin_;
            glUseProgram(p.id);
            if (shadowOnly) glUniformMatrix4fv(p.uLightVP, 1, GL_FALSE, &(*lightVP)[0][0]);
            else bindMaterial(p, materials_[first.material]);
            glUniformMatrix4fv(p.uModel, 1, GL_FALSE, &first.model[0][0]);
            glBindBuffer(GL_UNIFORM_BUFFER, jointUbo_);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(glm::mat4) * kMaxJoints, first.jointPalette);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, jointUbo_);
            glBindVertexArray(mesh.vao);
            glDrawElements(GL_TRIANGLES, GLsizei(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
            ++stats_.drawCalls;
            stats_.triangles += mesh.indexCount / 3;
            continue;
        }

        // lote: itens consecutivos com a mesma chave (mesmo mesh + material) visíveis neste passe
        uint32_t j = i, n = 0;
        while (j < drawCount_ && draws_[j].key == first.key && !draws_[j].jointPalette) {
            if (draws_[j].passMask & passBit) instanceScratch_[n++] = draws_[j].model;
            ++j;
        }
        i = j;
        if (n == 0) continue;
        const Program& p = shadowOnly ? shadowInst_ : pbrInst_;
        glUseProgram(p.id);
        if (shadowOnly) glUniformMatrix4fv(p.uLightVP, 1, GL_FALSE, &(*lightVP)[0][0]);
        else bindMaterial(p, materials_[first.material]);

        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(glm::mat4) * kMaxDraws, nullptr, GL_STREAM_DRAW);  // orphaning
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(glm::mat4) * n, instanceScratch_);
        glBindVertexArray(mesh.vao);
        glDrawElementsInstanced(GL_TRIANGLES, GLsizei(mesh.indexCount), GL_UNSIGNED_INT, nullptr, GLsizei(n));
        ++stats_.drawCalls;
        stats_.triangles += mesh.indexCount / 3 * n;
    }
    glBindVertexArray(0);
}

// ---------- linhas do editor ----------
void Renderer::line(const glm::mat4& vp, glm::vec3 a, glm::vec3 b, glm::vec4 color, float thicknessPx, bool depthTest) {
    int list = depthTest ? 0 : 1;
    if (!lineVerts_[list] || lineCount_[list] + 6 > kMaxLineVerts) return;
    glm::vec4 ca = vp * glm::vec4(a, 1.f), cb = vp * glm::vec4(b, 1.f);
    const float eps = 1e-3f;
    if (ca.w < eps && cb.w < eps) return;  // atrás da câmera
    if (ca.w < eps) ca = glm::mix(ca, cb, (eps - ca.w) / (cb.w - ca.w));
    if (cb.w < eps) cb = glm::mix(cb, ca, (eps - cb.w) / (ca.w - cb.w));
    const glm::vec2 vpPx{float(width_), float(height_)};
    glm::vec2 na = glm::vec2(ca) / ca.w, nb = glm::vec2(cb) / cb.w;
    glm::vec2 dirPx = (nb - na) * vpPx * 0.5f;
    float len = glm::length(dirPx);
    if (len < 1e-4f) dirPx = glm::vec2(1.f, 0.f), len = 1.f;
    glm::vec2 perp = glm::vec2(-dirPx.y, dirPx.x) / len * (thicknessPx * 0.5f);
    glm::vec2 offNdc = perp / vpPx * 2.f;
    if (depthTest) { ca.z -= 0.002f * ca.w; cb.z -= 0.002f * cb.w; }  // evita z-fighting com o chão
    glm::vec4 oa(offNdc * ca.w, 0.f, 0.f), ob(offNdc * cb.w, 0.f, 0.f);
    LineVertex* v = lineVerts_[list] + lineCount_[list];
    v[0] = {ca - oa, color}; v[1] = {ca + oa, color}; v[2] = {cb + ob, color};
    v[3] = {ca - oa, color}; v[4] = {cb + ob, color}; v[5] = {cb - ob, color};
    lineCount_[list] += 6;
}

void Renderer::flushLines() {
    if (!lineCount_[0] && !lineCount_[1]) return;
    glUseProgram(lineProg_);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    for (int list = 0; list < 2; ++list) {
        if (!lineCount_[list]) continue;
        if (list == 0) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glBufferData(GL_ARRAY_BUFFER, sizeof(LineVertex) * kMaxLineVerts, nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(LineVertex) * lineCount_[list], lineVerts_[list]);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(lineCount_[list]));
        ++stats_.drawCalls;
    }
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

}  // namespace eng
