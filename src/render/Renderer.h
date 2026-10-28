#pragma once
#include <GLES3/gl32.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>
#include <string>
#include "core/Pool.h"
#include "render/Camera.h"
#include "render/Lights.h"
#include "scene/Animator.h"
#include "scene/Scene.h"

namespace eng {

class ParticleSystem;  // render/Particles.h

constexpr uint32_t kInvalidId = 0xFFFFFFFFu;

struct Vertex {
    glm::vec3 pos, normal;
    glm::vec2 uv;
    glm::vec4 tangent;
    glm::u16vec4 joints;
    glm::vec4 weights;
    glm::vec4 color{1.f};  // cor por vértice (multiplica o albedo): permite juntar objetos de cores diferentes num draw
};

struct Mesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    uint32_t indexCount = 0, vertexCount = 0;
    bool skinned = false;
    AABB bounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
};

struct Material {
    GLuint albedo = 0, normal = 0, orm = 0, emissive = 0;  // texturas padrão 1x1 se ausentes
    GLuint occlusion = 0;  // AO separado (glTF occlusionTexture, canal R); 0 = sem AO
    glm::vec4 albedoFactor{1.f};
    float roughness = 1.f, metallic = 1.f;
    glm::vec3 emissive_factor{0.f};
    float alphaCutoff = 0.f;   // > 0: alpha test (glTF alphaMode MASK)
    float occlusionStrength = 1.f;
    bool doubleSided = false;
    int mode = 0;              // 0 padrão, 1 terreno simples, 2 água, 3 terreno com as camadas de textura reais
};

struct DrawItem {
    uint64_t key;          // (material << 32) | mesh -> agrupa para instancing
    glm::mat4 model;
    MeshId mesh;
    MaterialId material;
    const glm::mat4* jointPalette;  // não-nulo => skinned (sem instancing)
    uint32_t passMask;     // bit 0: câmera, bits 1..3: cascatas de sombra
};

// Linhas grossas do editor, montadas na CPU em clip space (sem alocação: capacidade fixa)
struct LineVertex { glm::vec4 pos; glm::vec4 color; };

struct FrameStats {
    uint32_t drawCalls = 0, triangles = 0, visibleObjects = 0;
};

class Renderer {
public:
    static constexpr uint32_t kMaxDraws = 16384;
    static constexpr uint32_t kMaxMeshes = 12288, kMaxMaterials = 2048;
    static constexpr uint32_t kMaxLineVerts = 6 * 4096;
    static constexpr int kEnvSize = 128;

    bool init(const char* shaderDir);
    void resize(int w, int h);
    void shutdown();

    MeshId createMesh(const Vertex* v, uint32_t vcount, const uint32_t* idx, uint32_t icount, bool skinned);
    MaterialId createMaterial(const Material& m);
    // Reenvia os vértices/índices de um mesh existente (ex.: chunk de terreno reciclado)
    void updateMesh(MeshId id, const Vertex* v, uint32_t vcount, const uint32_t* idx, uint32_t icount);
    const Material* material(MaterialId id) const { return id < materialCount_ ? &materials_[id] : nullptr; }
    void setEmissive(MaterialId id, glm::vec3 f) { if (id < materialCount_) materials_[id].emissive_factor = f; }
    uint32_t materialCount() const { return materialCount_; }
    const Mesh* mesh(MeshId id) const { return id < meshCount_ ? &meshes_[id] : nullptr; }
    // Libera todos os meshes/materiais (nova cena). Texturas ficam a cargo do TextureManager.
    void clearAssets();

    // Frame: beginFrame -> submit* -> (linhas) -> render
    void beginFrame(const Camera& cam, const DirectionalLight& sun);
    void submit(const GameObject& go, const glm::mat4& model, const glm::mat4* jointPalette);
    void render(const Camera& cam, const DirectionalLight& sun, const Environment& env,
                const PointLight* points, int pointCount);

    // Linhas do editor. depthTest=false: desenhada por cima de tudo (gizmo).
    void line(const glm::mat4& viewProj, glm::vec3 a, glm::vec3 b, glm::vec4 color, float thicknessPx, bool depthTest);

    const FrameStats& stats() const { return stats_; }
    float time = 0.f;             // segundos desde o início (ondas da água)
    float fogDensity = 0.f;       // neblina exponencial (cor = horizonte do céu); 0 = desligada
    // Compila "<dir>/<vs>" + "<dir>/<fs>" com o common.glsl (para outros sistemas, ex.: partículas)
    GLuint loadProgram(const char* vsFile, const char* fsFile);
    ParticleSystem* particles = nullptr;  // desenhado depois do céu, antes das linhas do editor
    GLuint targetFbo = 0;         // 0 = tela. Outro valor: renderiza em FBO (ex.: textura do Flutter)
    float shadowDistance = 40.f;
    // Aberturas (porta/janela/vão cortados em paredes): o que estiver dentro da caixa some nos objetos
    // marcados (não personagens, não objetos com noCut). Preenchidas pelo motor a cada frame.
    static constexpr int kMaxCuts = 32;
    struct CutBox { glm::mat4 worldToBox; AABB bounds; };
    CutBox cuts[kMaxCuts];
    int cutCount = 0;

private:
    struct Program {
        GLuint id = 0;
        GLint uModel = -1, uLightVP = -1;
        GLint uAlbedoF = -1, uRough = -1, uMetal = -1, uEmis = -1, uAlphaCut = -1, uOccStrength = -1, uMode = -1, uTime = -1;
    };

    void updateEnvironment(const Environment& env);
    void drawSky(const glm::mat4& invViewProjNoTranslation, const Environment& env, bool capture);
    void shadowPass(const CascadeSet& cs);
    void drawBatches(uint32_t passBit, const glm::mat4* lightVP);
    void bindMaterial(const Program& p, const Material& m);
    void flushLines();

    Program pbr_, pbrInst_, pbrSkin_, shadow_, shadowInst_, shadowSkin_;
    GLuint skyProg_ = 0, lineProg_ = 0;
    GLint skyInvVP_ = -1, skyZenith_ = -1, skyHorizon_ = -1, skyGround_ = -1, skyIntensity_ = -1, skyCapture_ = -1;
    GLuint emptyVao_ = 0, lineVao_ = 0, lineVbo_ = 0;
    GLuint sceneUbo_ = 0, jointUbo_ = 0, instanceVbo_ = 0, cutUbo_ = 0;
    GLuint shadowTex_ = 0, shadowFbo_ = 0;
    GLuint envCube_ = 0, envFbo_ = 0;
    int envMips_ = 1;
    Environment envCached_{};
    bool envValid_ = false;
    glm::vec4 sh_[9] = {};
    int width_ = 1, height_ = 1;
    std::string shaderDir_;
public:
    const std::string& shaderDir() const { return shaderDir_; }
    // Camadas do terreno (texture arrays): <shaderDir>/terrain/<nome>_alb.jpg (cor) e _nh.jpg (normal XY + altura).
    // Carregadas uma vez por contexto GL; retorna false se faltar algum arquivo.
    bool loadTerrainLayers(const char* const* names, int count);
    bool terrainLayersReady() const { return terrainAlb_ != 0; }
private:
    GLuint terrainAlb_ = 0, terrainNh_ = 0;

    // Estado do frame
    CascadeSet cascades_{};
    Frustum camFrustum_{}, cascadeFrustum_[kNumCascades]{};

    // Capacidades fixas, reservadas no init
    Mesh meshes_[kMaxMeshes];
    Material materials_[kMaxMaterials];
    uint32_t meshCount_ = 0, materialCount_ = 0;
    DrawItem draws_[kMaxDraws];
    uint32_t drawCount_ = 0;
    glm::mat4* instanceScratch_ = nullptr;  // kMaxDraws mat4, alocado uma vez
    LineVertex* lineVerts_[2] = {nullptr, nullptr};  // [0] com depth test, [1] overlay
    uint32_t lineCount_[2] = {0, 0};
    FrameStats stats_{};
};

}  // namespace eng
