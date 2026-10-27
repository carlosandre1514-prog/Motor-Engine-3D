#pragma once
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace eng {

// Gerador pseudo-aleatório determinístico (mesma semente = mesmo mapa)
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) { next(); }
    uint32_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return uint32_t(s >> 11);
    }
    float f() { return (next() & 0xFFFFFF) / float(0x1000000); }          // [0, 1)
    float range(float a, float b) { return a + (b - a) * f(); }
    int irange(int a, int b) { return a + int(next() % uint32_t(b - a + 1)); }  // [a, b]
    bool chance(float p) { return f() < p; }
};

inline uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = seed ^ (uint32_t(x) * 0x27d4eb2du) ^ (uint32_t(y) * 0x165667b1u);
    h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}
inline float hashf(int x, int y, uint32_t seed) { return (hash2(x, y, seed) & 0xFFFFFF) / float(0x1000000); }

// Value noise 2D suave (quíntica) em [0, 1]
inline float valueNoise(float x, float y, uint32_t seed) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - xi, fy = y - yi;
    float u = fx * fx * fx * (fx * (fx * 6.f - 15.f) + 10.f), v = fy * fy * fy * (fy * (fy * 6.f - 15.f) + 10.f);
    float a = hashf(xi, yi, seed), b = hashf(xi + 1, yi, seed), c = hashf(xi, yi + 1, seed), d = hashf(xi + 1, yi + 1, seed);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

// Fractal Brownian motion em [0, 1]
inline float fbm(float x, float y, int octaves, uint32_t seed, float lacunarity = 2.f, float gain = 0.5f) {
    float sum = 0.f, amp = 0.5f, norm = 0.f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * valueNoise(x, y, seed + uint32_t(i) * 101u);
        norm += amp;
        amp *= gain;
        x *= lacunarity; y *= lacunarity;
    }
    return sum / norm;
}

// Ruído "ridged" (cristas de montanha) em [0, 1]
inline float ridged(float x, float y, int octaves, uint32_t seed) {
    float sum = 0.f, amp = 0.5f, norm = 0.f;
    for (int i = 0; i < octaves; ++i) {
        float n = 1.f - std::abs(valueNoise(x, y, seed + uint32_t(i) * 131u) * 2.f - 1.f);
        sum += amp * n * n;
        norm += amp;
        amp *= 0.5f;
        x *= 2.f; y *= 2.f;
    }
    return sum / norm;
}

}  // namespace eng
