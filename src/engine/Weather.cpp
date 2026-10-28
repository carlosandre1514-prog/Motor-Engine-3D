// Ciclo de dia e noite, chuva (gotas, respingos, céu fechado, raios e trovão) e fila de sons para o app tocar.
#include <algorithm>
#include <cmath>
#include "engine/Engine.h"
#include "render/Particles.h"

namespace eng {

namespace {
float cycleSeconds(const Engine* e) { return e->dayCycleMin > 0.f ? e->dayCycleMin * 60.f : 600.f; }

// Altura do que cobre cada ponto em volta da câmera (telhado, laje, chão): a gota some ali e respinga
void updateRainCover(Engine* e, glm::vec3 c) {
    const int N = Engine::kRainGrid;
    const float step = 1.2f;
    glm::vec2 origin(std::floor(c.x / step) * step - N / 2 * step, std::floor(c.z / step) * step - N / 2 * step);
    e->rainCover.resize(N * N);
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            glm::vec3 top(origin.x + i * step, c.y + 25.f, origin.y + j * step), hit;
            GameObject* obj = nullptr;
            e->rainCover[j * N + i] = e->physics.raycast(top, top - glm::vec3(0, 80.f, 0), hit, obj) ? hit.y : c.y - 60.f;
        }
    e->rainCoverOrigin = origin;
}
float coverAt(const Engine* e, float x, float z) {
    const int N = Engine::kRainGrid;
    int i = int(std::floor((x - e->rainCoverOrigin.x) / 1.2f + 0.5f)), j = int(std::floor((z - e->rainCoverOrigin.y) / 1.2f + 0.5f));
    if (e->rainCover.empty() || i < 0 || j < 0 || i >= N || j >= N) return -1e9f;
    return e->rainCover[j * N + i];
}
}  // namespace

void weatherStart(Engine* e) {
    e->rain = e->rainMode == 2 ? 1.f : 0.f;
    e->rainTarget = e->rain;
    e->rainClock = e->fxRng.range(0.12f, 0.45f) * cycleSeconds(e);  // vai chover em algum momento do ciclo
    e->lightning = 0.f;
    e->lightningNext = e->fxRng.range(4.f, 12.f);
    e->thunderIn = -1.f;
    e->rainCoverTimer = 0.f;
    e->cycleAccum = 0.f;
}

void weatherUpdate(Engine* e, float dt) {
    // dia e noite: um dia inteiro em dayCycleMin minutos (o céu é recalculado a cada ~0,02 h)
    if (e->dayCycleMin > 0.f) {
        e->cycleAccum += dt * 24.f / (e->dayCycleMin * 60.f);
        if (e->cycleAccum >= 0.02f) { setTimeOfDay(e, e->timeOfDay + e->cycleAccum); e->cycleAccum = 0.f; }
    }
    // quando chove e quanto tempo dura
    if (e->rainMode == 0) e->rainTarget = 0.f;
    else if (e->rainMode == 2) e->rainTarget = 1.f;
    else if ((e->rainClock -= dt) <= 0.f) {
        if (e->rainTarget > 0.f) {
            e->rainTarget = 0.f;
            e->rainClock = e->fxRng.range(0.3f, 0.6f) * cycleSeconds(e);
        } else {
            e->rainTarget = e->fxRng.range(0.55f, 1.f);
            e->rainClock = e->fxRng.range(0.12f, 0.28f) * cycleSeconds(e);
        }
    }
    e->rain += std::clamp(e->rainTarget - e->rain, -dt / 8.f, dt / 8.f);
    // raios e trovões na chuva forte
    e->lightning = std::max(0.f, e->lightning - dt * 5.f);
    if (e->rain > 0.6f && (e->lightningNext -= dt) <= 0.f) {
        e->lightning = 1.f;
        e->lightningNext = e->fxRng.range(7.f, 22.f);
        e->thunderIn = e->fxRng.range(0.5f, 3.f);
    }
    if (e->thunderIn > 0.f && (e->thunderIn -= dt) <= 0.f) {
        playSound(e, "trovao", glm::vec3(0.f), 0.6f + 0.4f * e->rain, false);
        e->thunderIn = -1.f;
    }
    if (e->rain < 0.02f) return;
    // gotas em volta da câmera, morrendo no que estiver por cima (telhado, laje, chão)
    glm::vec3 c = e->camera.position;
    if ((e->rainCoverTimer -= dt) <= 0.f || glm::length(glm::vec2(c.x, c.z) - e->rainCoverCenter) > 4.f) {
        updateRainCover(e, c);
        e->rainCoverTimer = 0.4f;
        e->rainCoverCenter = glm::vec2(c.x, c.z);
    }
    const glm::vec3 wind(1.2f, 0.f, 0.6f);
    const float speed = 16.f;
    int n = int(e->rain * (e->quality == 0 ? 600.f : 1500.f) * dt + e->fxRng.f());
    for (int k = 0; k < n; ++k) {
        float a = e->fxRng.range(0.f, 6.2831853f), r = std::sqrt(e->fxRng.f()) * 14.f;
        glm::vec3 p(c.x + std::cos(a) * r, c.y + e->fxRng.range(4.f, 9.f), c.z + std::sin(a) * r);
        float cover = coverAt(e, p.x, p.z);
        if (cover > p.y - 0.3f) continue;  // em cima de um teto alto: nem nasce
        float fall = p.y - std::max(cover, c.y - 12.f);
        ParticleSpawn d;
        d.pos = p; d.vel = glm::vec3(wind.x, -speed, wind.z); d.life = fall / speed;
        d.size0 = d.size1 = 0.016f; d.stretch = 0.04f;
        d.color0 = d.color1 = glm::vec4(0.78f, 0.82f, 0.92f, 0.55f);
        d.shape = PS_Spark; d.additive = false;
        e->particles.emit(d);
        if (cover > -1e8f && e->fxRng.chance(0.35f)) {  // respingo onde a gota cai
            ParticleSpawn s;
            s.pos = glm::vec3(p.x + wind.x * d.life, cover + 0.03f, p.z + wind.z * d.life);
            s.life = 0.22f; s.size0 = 0.02f; s.size1 = 0.13f;
            s.color0 = glm::vec4(0.85f, 0.88f, 0.95f, 0.35f); s.color1 = glm::vec4(0.85f, 0.88f, 0.95f, 0.f);
            s.shape = PS_Ring; s.additive = false;
            e->particles.emit(s);
        }
    }
}

void weatherLook(const Engine* e, Environment& env, DirectionalLight& sun, float& fog) {
    if (e->rain <= 0.001f && e->lightning <= 0.f) return;
    float r = std::round(e->rain * 10.f) / 10.f;  // em degraus: o reflexo do céu é refeito só quando muda
    glm::vec3 grey(0.42f, 0.45f, 0.5f);
    float lum = glm::dot(env.horizon, glm::vec3(0.3f, 0.5f, 0.2f));
    env.zenith = glm::mix(env.zenith, grey * lum * 1.1f, r * 0.85f);
    env.horizon = glm::mix(env.horizon, grey * lum * 1.3f, r * 0.8f);
    env.skyIntensity *= 1.f - 0.35f * r;
    sun.intensity *= 1.f - 0.8f * e->rain;
    fog += e->rain * 0.012f;
    if (e->lightning > 0.f) {  // clarão do raio
        float f = e->lightning * (0.6f + 0.4f * std::sin(e->lightning * 40.f));
        sun.intensity += f * 6.f;
        sun.color = glm::mix(sun.color, glm::vec3(0.75f, 0.82f, 1.f), std::min(1.f, f));
    }
}

void playSound(Engine* e, const char* name, glm::vec3 pos, float gain, bool positional) {
    if (e->sounds.size() < 64) e->sounds.push_back({name, pos, gain, positional});
}

}  // namespace eng
