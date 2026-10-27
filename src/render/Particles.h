#pragma once
#include <GLES3/gl32.h>
#include <cstdint>
#include <glm/glm.hpp>
#include "render/Camera.h"

namespace eng {

class Renderer;

// Formatos desenhados no shader (shaders/particle.frag)
enum ParticleShape : uint8_t { PS_Glow, PS_Smoke, PS_Spark, PS_Ring, PS_Star, PS_Shard, PS_Flame };

struct ParticleSpawn {
    glm::vec3 pos{0.f}, vel{0.f};
    float life = 1.f;
    float size0 = 0.2f, size1 = 0.2f;
    glm::vec4 color0{1.f}, color1{1.f, 1.f, 1.f, 0.f};  // rgb + opacidade (inicio -> fim)
    float rot = 0.f, rotVel = 0.f;
    float drag = 0.f;          // amortecimento da velocidade (1/s)
    float gravity = 0.f;       // m/s² (positivo puxa para baixo; negativo sobe)
    float stretch = 0.f;       // > 0: alonga na direção do movimento (faíscas, balas)
    uint8_t shape = PS_Glow;
    bool additive = true;      // aditivo (brilha) ou alfa (fumaça escura)
    float occlusion = 0.f;     // aditivo que também escurece um pouco o fundo (cor saturada mesmo de dia)
};

// Partículas com capacidade fixa, simuladas na CPU sem alocação e desenhadas num único draw call instanciado.
class ParticleSystem {
public:
    static constexpr int kMax = 6000;

    bool init(Renderer& r);
    void shutdown();
    void clear() { count_ = 0; }
    void emit(const ParticleSpawn& s);
    void update(float dt);
    int draw(const Camera& cam);  // retorna nº de draw calls
    int count() const { return count_; }

private:
    struct P {
        glm::vec3 pos; float age;
        glm::vec3 vel; float life;
        glm::vec4 c0, c1;
        float s0, s1, rot, rotVel, drag, gravity, stretch, seed, occlusion;
        uint8_t shape; bool additive;
    };
    struct GpuInstance { glm::vec4 posSize, color, velStretch, misc; };
    P parts_[kMax];
    GpuInstance gpu_[kMax];
    int count_ = 0;
    uint32_t seed_ = 1;
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLint uRight_ = -1, uUp_ = -1;
};

}  // namespace eng
