#include "render/Particles.h"
#include <algorithm>
#include <cstddef>
#include "render/Renderer.h"

namespace eng {

bool ParticleSystem::init(Renderer& r) {
    prog_ = r.loadProgram("particle.vert", "particle.frag");
    if (!prog_) return false;
    uRight_ = glGetUniformLocation(prog_, "uRight");
    uUp_ = glGetUniformLocation(prog_, "uUp");
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GpuInstance) * kMax, nullptr, GL_STREAM_DRAW);
    for (int k = 0; k < 4; ++k) {
        glEnableVertexAttribArray(k);
        glVertexAttribPointer(k, 4, GL_FLOAT, GL_FALSE, sizeof(GpuInstance), (void*)(sizeof(glm::vec4) * k));
        glVertexAttribDivisor(k, 1);
    }
    glBindVertexArray(0);
    count_ = 0;
    return true;
}

void ParticleSystem::shutdown() {
    glDeleteProgram(prog_);
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
    prog_ = vbo_ = vao_ = 0;
}

void ParticleSystem::emit(const ParticleSpawn& s) {
    if (count_ >= kMax) return;  // orçamento cheio: descarta (sem alocação)
    P& p = parts_[count_++];
    p.pos = s.pos; p.vel = s.vel; p.age = 0.f; p.life = std::max(s.life, 0.01f);
    p.c0 = s.color0; p.c1 = s.color1;
    p.s0 = s.size0; p.s1 = s.size1;
    p.rot = s.rot; p.rotVel = s.rotVel; p.drag = s.drag; p.gravity = s.gravity; p.stretch = s.stretch;
    seed_ = seed_ * 1664525u + 1013904223u;
    p.seed = (seed_ >> 8) / float(1 << 24);
    p.shape = s.shape; p.additive = s.additive; p.occlusion = s.occlusion;
}

void ParticleSystem::update(float dt) {
    for (int i = 0; i < count_;) {
        P& p = parts_[i];
        p.age += dt;
        if (p.age >= p.life) { parts_[i] = parts_[--count_]; continue; }  // remove trocando com o último
        p.vel *= 1.f / (1.f + p.drag * dt);
        p.vel.y -= p.gravity * dt;
        p.pos += p.vel * dt;
        p.rot += p.rotVel * dt;
        ++i;
    }
}

int ParticleSystem::draw(const Camera& cam) {
    if (!count_ || !prog_) return 0;
    // Alfa (fumaça) primeiro, aditivo (fogo, magia) por cima: menos artefatos sem ordenar por distância
    int n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < count_; ++i) {
            const P& p = parts_[i];
            if (p.additive != (pass == 1)) continue;
            float t = p.age / p.life;
            glm::vec4 c = glm::mix(p.c0, p.c1, t);
            float fadeIn = std::min(1.f, p.age / (p.life * 0.08f + 1e-4f));
            float a = std::clamp(c.a * fadeIn, 0.f, 1.f);
            GpuInstance& g = gpu_[n++];
            g.posSize = glm::vec4(p.pos, glm::mix(p.s0, p.s1, t));
            g.color = p.additive ? glm::vec4(glm::vec3(c) * a, a * p.occlusion) : glm::vec4(glm::vec3(c) * a, a);
            g.velStretch = glm::vec4(p.vel, p.stretch);
            g.misc = glm::vec4(p.rot, float(p.shape), p.seed, 0.f);
        }
    glm::mat3 r = glm::mat3_cast(cam.rotation);
    glm::vec3 right = r[0], up = r[1];
    glUseProgram(prog_);
    glUniform3fv(uRight_, 1, &right[0]);
    glUniform3fv(uUp_, 1, &up[0]);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GpuInstance) * kMax, nullptr, GL_STREAM_DRAW);  // orphaning
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GpuInstance) * n, gpu_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    return 1;
}

}  // namespace eng
