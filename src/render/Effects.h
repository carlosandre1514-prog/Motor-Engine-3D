#pragma once
#include <glm/glm.hpp>
#include "procgen/Noise.h"
#include "render/Particles.h"

namespace eng {

struct GameObject;

// Elementos (cores e comportamento das magias/efeitos)
enum Element { EL_Fire, EL_Ice, EL_Dark, EL_Light, EL_Poison, EL_Lightning, EL_Arcane, EL_Count };
const char* elementName(int e);

struct FxPalette {
    glm::vec3 core;      // centro (quase branco)
    glm::vec3 glow;      // cor principal (aditiva)
    glm::vec3 smoke;     // fumaça
    bool darkSmoke;      // fumaça opaca escura (trevas) em vez de translúcida
    uint8_t bitShape;    // formato das partículas pequenas (faísca, cristal, estrela...)
    glm::vec3 light;     // cor da luz dinâmica
};
FxPalette fxPalette(int element);
FxPalette fxPaletteFromColor(glm::vec3 color);  // cor livre escolhida pelo usuário

// Efeitos contínuos que podem virar objeto na cena
enum FxKind { FX_Fire, FX_Torch, FX_Smoke, FX_Sparks, FX_Aura, FX_Portal, FX_Explosion, FX_Orb, FX_Pillar, FX_Count };
const char* fxName(int k);

// Componente: emissor preso a um GameObject
struct Emitter {
    int kind = FX_Fire;
    int element = EL_Fire;
    bool customColor = false;
    glm::vec3 color{1.f};
    float scale = 1.f;
    float accum = 0.f, timer = 0.f;
    GameObject* owner = nullptr;
    FxPalette palette() const { return customColor ? fxPaletteFromColor(color) : fxPalette(element); }
};

// Luz dinâmica pedida por um efeito neste frame
struct FxLight { glm::vec3 pos, color; float intensity, radius; };

// Emite as partículas do efeito contínuo durante dt. Retorna a luz do efeito (intensity 0 = sem luz).
FxLight fxContinuous(ParticleSystem& ps, Rng& r, int kind, const FxPalette& pal, glm::vec3 pos, float scale, float dt,
                     float& accum, float& timer);

// Efeitos instantâneos
FxLight fxExplosion(ParticleSystem& ps, Rng& r, const FxPalette& pal, glm::vec3 pos, float scale);
FxLight fxMuzzleFlash(ParticleSystem& ps, Rng& r, glm::vec3 pos, glm::vec3 dir);
void fxBullet(ParticleSystem& ps, glm::vec3 from, glm::vec3 to);
void fxImpact(ParticleSystem& ps, Rng& r, glm::vec3 pos, glm::vec3 dir);
void fxSpellTrail(ParticleSystem& ps, Rng& r, const FxPalette& pal, glm::vec3 pos, glm::vec3 vel, float dt);

}  // namespace eng
