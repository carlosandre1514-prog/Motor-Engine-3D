#include "render/Renderer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace eng {

// ---------- utilidades de shader (apenas no init; alocação permitida aqui) ----------
static std::string readFile(const std::string& path) {
    // Android: trocar por AAssetManager_open
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(n, '\0');
    std::fread(s.data(), 1, n, f);
    std::fclose(f);
    return s;
}

static GLuint compile(GLenum type, const std::string& src, const char* define) {
    // injeta #define logo após a linha #version
    size_t nl = src.find('\n');
    std::string full = src.substr(0, nl + 1) + "#define " + define + "\n" + src.substr(nl + 1);
    GLuint s = glCreateShader(type);
    const char* p = full.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); std::fprintf(stderr, "shader: %s\n", log); }
    return s;
}

static GLuint link(const std::string& vs, const std::string& fs, const char* define) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, define), f = compile(GL_FRAGMENT_SHADER, fs, define);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

bool Renderer::init(const char* shaderDir) {
    std::string d = shaderDir;
    std::string pv = readFile(d + "/pbr.vert"), pf = readFile(d + "/pbr.frag");
    std::string sv = readFile(d + "/shadow.vert"), sf = readFile(d + "/shadow.frag");
    if (pv.empty() || pf.empty() || sv.empty() || sf.empty()) return false;

    pbr_.id = link(pv, pf, "STANDARD");
    pbrInst_.id = link(pv, pf, "INSTANCED");
    pbrSkin_.id = link(pv, pf, "SKINNED");
    shadow_.id = link(sv, sf, "STANDARD");
    shadowInst_.id = link(sv, sf, "INSTANCED");
    shadowSkin_.id = link(sv, sf, "SKINNED");
    for (Program* p : {&pbr_, &pbrInst_, &pbrSkin_, &shadow_, &shadowInst_, &shadowSkin_}) {
        p->uModel = glGetUniformLocation(p->id, "uModel");
        p->uLightVP = glGetUniformLocation(p->id, "uLightVP");
        p->uAlbedoF = glGetUniformLocation(p->id, "uAlbedoFactor");
        p->uRough = glGetUniformLocation(p->id, "uRoughnessFactor");
        p->uMetal = glGetUniformLocation(p->id, "uMetallicFactor");
        p->uEmis = glGetUniformLocation(p->id, "uEmissiveFactor");
        GLuint bi = glGetUniformBlockIndex(p->id, "SceneBlock");
        if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p->id, bi, 0);
        bi = glGetUniformBlockIndex(p->id, "JointBlock");
        if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p->id, bi, 1);
    }

    // Unidades de textura dos samplers (fixas): 0 albedo, 1 normal, 2 ORM, 3 emissivo, 4 sombra
    for (Program* p : {&pbr_, &pbrInst_, &pbrSkin_}) {
        glUseProgram(p->id);
        glUniform1i(glGetUniformLocation(p->id, "uAlbedoMap"), 0);
        glUniform1i(glGetUniformLocation(p->id, "uNormalMap"), 1);
        glUniform1i(glGetUniformLocation(p->id, "uOrmMap"), 2);
        glUniform1i(glGetUniformLocation(p->id, "uEmissiveMap"), 3);
        glUniform1i(glGetUniformLocation(p->id, "uShadowMap"), 4);
    }

    glGenBuffers(1, &sceneUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, sceneUbo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(SceneUBO), nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &jointUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, jointUbo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(glm::mat4) * kMaxJoints, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &instanceVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::mat4) * kMaxDraws, nullptr, GL_STREAM_DRAW);
    instanceScratch_ = static_cast<glm::mat4*>(std::malloc(sizeof(glm::mat4) * kMaxDraws));

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

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    return true;
}

void Renderer::resize(int w, int h) { width_ = w; height_ = h; }

void Renderer::shutdown() {
    std::free(instanceScratch_);
    instanceScratch_ = nullptr;
    // glDelete* de programas, buffers e texturas omitido por brevidade
}

MeshId Renderer::createMesh(const Vertex* v, uint32_t vc, const uint32_t* idx, uint32_t ic, bool skinned) {
    Mesh& m = meshes_[meshCount_];
    m.indexCount = ic;
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
    glBindVertexArray(0);
    return meshCount_++;
}

MaterialId Renderer::createMaterial(const Material& m) { materials_[materialCount_] = m; return materialCount_++; }

// ---------- frame ----------
void Renderer::beginFrame() { drawCount_ = 0; drawCalls_ = 0; }

void Renderer::submit(const GameObject& go, const glm::mat4* palette) {
    if (!go.active || !go.meshRenderer || drawCount_ >= kMaxDraws) return;
    const MeshRenderer& mr = *go.meshRenderer;
    const Mesh& mesh = meshes_[mr.mesh];
    AABB world = transformAABB(mesh.bounds, go.transform.world());
    if (!cullFrustum.intersects(world)) return;  // frustum culling da câmera
    draws_[drawCount_++] = {(uint64_t(mr.material) << 32) | mr.mesh, go.transform.world(), mr.mesh, mr.material,
                            palette, mr.castShadows};
}

void Renderer::bindMaterial(const Program& p, const Material& m) {
    glUniform4fv(p.uAlbedoF, 1, &m.albedoFactor[0]);
    glUniform1f(p.uRough, m.roughness);
    glUniform1f(p.uMetal, m.metallic);
    glUniform3fv(p.uEmis, 1, &m.emissive_factor[0]);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m.albedo);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m.normal);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m.orm);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m.emissive);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, shadowTex_);
}

void Renderer::render(const Camera& cam, const DirectionalLight& sun, const AmbientLight& amb,
                      const PointLight* points, int pointCount) {
    // 1) UBO da cena
    SceneUBO u{};
    CascadeSet cs = computeCascades(cam, sun.direction, 60.f);
    u.viewProj = cam.viewProj();
    u.camPos = glm::vec4(cam.position, 1.f);
    u.dirDir = glm::vec4(glm::normalize(sun.direction), sun.castShadows ? 1.f : 0.f);
    u.dirColor = glm::vec4(sun.color * sun.intensity, 1.f);
    for (int i = 0; i < kNumCascades; ++i) u.cascadeVP[i] = cs.viewProj[i];
    u.cascadeSplits = glm::vec4(cs.splitDepth[0], cs.splitDepth[1], cs.splitDepth[2], 0.f);
    u.skyAmbient = glm::vec4(amb.skyColor, amb.intensity);
    u.groundAmbient = glm::vec4(amb.groundColor, 1.f);
    u.counts.x = std::min(pointCount, kMaxPointLights);
    for (int i = 0; i < u.counts.x; ++i) {
        u.points[i].posRadius = glm::vec4(points[i].position, points[i].radius);
        u.points[i].colorIntensity = glm::vec4(points[i].color, points[i].intensity);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, sceneUbo_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(u), &u);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, sceneUbo_);

    // 2) ordena por (material, mesh) para formar lotes instanciáveis (std::sort não aloca)
    std::sort(draws_, draws_ + drawCount_, [](const DrawItem& a, const DrawItem& b) { return a.key < b.key; });

    // 3) sombras e passe principal
    if (sun.castShadows) shadowPass(cs);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glClearColor(0.1f, 0.12f, 0.15f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawBatches(false, 0, nullptr);
}

void Renderer::shadowPass(const CascadeSet& cs) {
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, kShadowMapSize, kShadowMapSize);
    glDrawBuffers(0, nullptr);
    glCullFace(GL_FRONT);  // reduz acne em superfícies iluminadas
    for (int c = 0; c < kNumCascades; ++c) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowTex_, 0, c);
        glClear(GL_DEPTH_BUFFER_BIT);
        drawBatches(true, c, &cs.viewProj[c]);
        // TODO: culling por cascata (Frustum::extract(cs.viewProj[c])) antes de enfileirar
    }
    glCullFace(GL_BACK);
}

void Renderer::drawBatches(bool shadowOnly, int, const glm::mat4* lightVP) {
    uint32_t i = 0;
    while (i < drawCount_) {
        const DrawItem& first = draws_[i];
        if (shadowOnly && !first.castShadows) { ++i; continue; }
        const Mesh& mesh = meshes_[first.mesh];

        if (first.jointPalette) {  // skinned: um draw por objeto
            const Program& p = shadowOnly ? shadowSkin_ : pbrSkin_;
            glUseProgram(p.id);
            if (shadowOnly) glUniformMatrix4fv(p.uLightVP, 1, GL_FALSE, &(*lightVP)[0][0]);
            else bindMaterial(p, materials_[first.material]);
            glUniformMatrix4fv(p.uModel, 1, GL_FALSE, &first.model[0][0]);
            glBindBuffer(GL_UNIFORM_BUFFER, jointUbo_);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(glm::mat4) * kMaxJoints, first.jointPalette);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, jointUbo_);
            glBindVertexArray(mesh.vao);
            glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
            ++drawCalls_;
            ++i;
            continue;
        }

        // lote: todos os itens consecutivos com a mesma chave (mesmo mesh + material)
        uint32_t j = i, n = 0;
        while (j < drawCount_ && draws_[j].key == first.key && !draws_[j].jointPalette) {
            instanceScratch_[n++] = draws_[j].model;
            ++j;
        }
        const Program& p = shadowOnly ? shadowInst_ : pbrInst_;
        glUseProgram(p.id);
        if (shadowOnly) glUniformMatrix4fv(p.uLightVP, 1, GL_FALSE, &(*lightVP)[0][0]);
        else bindMaterial(p, materials_[first.material]);

        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(glm::mat4) * kMaxDraws, nullptr, GL_STREAM_DRAW);  // orphaning
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(glm::mat4) * n, instanceScratch_);
        glBindVertexArray(mesh.vao);
        for (int k = 0; k < 4; ++k) {  // mat4 = 4 atributos vec4 com divisor 1
            glEnableVertexAttribArray(6 + k);
            glVertexAttribPointer(6 + k, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(sizeof(glm::vec4) * k));
            glVertexAttribDivisor(6 + k, 1);
        }
        glDrawElementsInstanced(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr, n);
        ++drawCalls_;
        i = j;
    }
    glBindVertexArray(0);
}

}  // namespace eng
