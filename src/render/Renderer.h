#pragma once
#include <GLES3/gl32.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>
#include "core/Pool.h"
#include "render/Camera.h"
#include "render/Lights.h"
#include "scene/Animator.h"
#include "scene/Scene.h"

namespace eng {

struct Vertex {
    glm::vec3 pos, normal;
    glm::vec2 uv;
    glm::vec4 tangent;
    glm::u16vec4 joints;
    glm::vec4 weights;
};

struct Mesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    uint32_t indexCount = 0;
    bool skinned = false;
    AABB bounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
};

struct Material {
    GLuint albedo = 0, normal = 0, orm = 0, emissive = 0;  // texturas padrão 1x1 se ausentes
    glm::vec4 albedoFactor{1.f};
    float roughness = 1.f, metallic = 1.f;
    glm::vec3 emissive_factor{0.f};
};

struct DrawItem {
    uint64_t key;          // (material << 32) | mesh -> agrupa para instancing
    glm::mat4 model;
    MeshId mesh;
    MaterialId material;
    const glm::mat4* jointPalette;  // não-nulo => skinned (sem instancing)
    bool castShadows;
};

class Renderer {
public:
    static constexpr uint32_t kMaxDraws = 4096;
    static constexpr uint32_t kMaxMeshes = 512, kMaxMaterials = 512;

    bool init(const char* shaderDir);
    void resize(int w, int h);
    void shutdown();

    MeshId createMesh(const Vertex* v, uint32_t vcount, const uint32_t* idx, uint32_t icount, bool skinned);
    MaterialId createMaterial(const Material& m);

    // Frame
    void beginFrame();
    void submit(const GameObject& go, const glm::mat4* jointPalette);  // culling feito dentro
    void render(const Camera& cam, const DirectionalLight& sun, const AmbientLight& amb,
                const PointLight* points, int pointCount);

    uint32_t drawCallsLastFrame() const { return drawCalls_; }
    Frustum cullFrustum;

private:
    struct Program {
        GLuint id = 0;
        GLint uModel = -1, uLightVP = -1;
        GLint uAlbedoF = -1, uRough = -1, uMetal = -1, uEmis = -1;
    };

    void shadowPass(const CascadeSet& cs);
    void drawBatches(bool shadowOnly, int cascade, const glm::mat4* lightVP);
    void bindMaterial(const Program& p, const Material& m);

    Program pbr_, pbrInst_, pbrSkin_, shadow_, shadowInst_, shadowSkin_;
    GLuint sceneUbo_ = 0, jointUbo_ = 0, instanceVbo_ = 0;
    GLuint shadowTex_ = 0, shadowFbo_ = 0;
    int width_ = 0, height_ = 0;

    // Capacidades fixas, reservadas no init
    Mesh meshes_[kMaxMeshes];
    Material materials_[kMaxMaterials];
    uint32_t meshCount_ = 0, materialCount_ = 0;
    DrawItem draws_[kMaxDraws];
    uint32_t drawCount_ = 0;
    glm::mat4* instanceScratch_ = nullptr;  // kMaxDraws mat4, alocado uma vez
    uint32_t drawCalls_ = 0;
};

}  // namespace eng
