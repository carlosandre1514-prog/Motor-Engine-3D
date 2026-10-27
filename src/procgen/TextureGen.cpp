#include "procgen/TextureGen.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include "procgen/Noise.h"

namespace eng {

const char* matName(Mat m) {
    static const char* names[] = {"Reboco", "Fachada casa", "Fachada prédio", "Vidro de escritório", "Tijolo", "Tijolo com janela",
                                  "Telha", "Concreto", "Asfalto", "Rua", "Calçada", "Grama", "Madeira", "Metal", "Metal escuro",
                                  "Plástico", "Quadra", "Água", "Terreno", "Pintura lisa", "Vidro"};
    return names[int(m)];
}

MatParams matParams(Mat m) {
    switch (m) {
        case Mat::Metal: return {1.f, 1.f, 0, false};
        case Mat::GunMetal: return {1.f, 1.f, 0, false};
        case Mat::Water: return {1.f, 0.f, 0, false};
        case Mat::Terrain: return {1.f, 0.f, 1, false};
        case Mat::RoofTile: return {1.f, 0.f, 0, true};   // telhado visto por baixo também
        default: return {1.f, 1.f, 0, false};
    }
}

namespace {

struct Img {
    int w, h;
    std::vector<glm::vec3> col;   // albedo linear-ish (0..1, será gravado como sRGB)
    std::vector<float> rough, metal, height;
    Img(int W, int H) : w(W), h(H), col(W * H, glm::vec3(1)), rough(W * H, 0.9f), metal(W * H, 0.f), height(W * H, 0.f) {}
    template <class F> void each(F&& f) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) f(x, y, y * w + x, (x + 0.5f) / w, (y + 0.5f) / h);
    }
};

// Ruído que repete na borda da textura (sem emenda visível ao repetir)
float tileNoise(float u, float v, float freq, int oct, uint32_t seed) {
    float a = fbm(u * freq, v * freq, oct, seed), b = fbm((u - 1) * freq, v * freq, oct, seed);
    float c = fbm(u * freq, (v - 1) * freq, oct, seed), d = fbm((u - 1) * freq, (v - 1) * freq, oct, seed);
    float su = u, sv = v;
    return glm::mix(glm::mix(a, b, su), glm::mix(c, d, su), sv);
}

uint8_t toSrgb8(float x) { return uint8_t(std::clamp(std::pow(std::clamp(x, 0.f, 1.f), 1.f / 2.2f) * 255.f + 0.5f, 0.f, 255.f)); }
uint8_t to8(float x) { return uint8_t(std::clamp(x * 255.f + 0.5f, 0.f, 255.f)); }

TexData finish(const Img& im, bool withNormal, float bump = 2.f) {
    TexData t;
    t.w = im.w; t.h = im.h;
    t.albedo.resize(im.w * im.h * 4);
    t.orm.resize(im.w * im.h * 4);
    for (int i = 0; i < im.w * im.h; ++i) {
        // a cor é pensada em sRGB (como num editor de imagem): grava direto
        t.albedo[i * 4 + 0] = to8(im.col[i].r);
        t.albedo[i * 4 + 1] = to8(im.col[i].g);
        t.albedo[i * 4 + 2] = to8(im.col[i].b);
        t.albedo[i * 4 + 3] = 255;
        t.orm[i * 4 + 0] = 255;
        t.orm[i * 4 + 1] = to8(im.rough[i]);
        t.orm[i * 4 + 2] = to8(im.metal[i]);
        t.orm[i * 4 + 3] = 255;
    }
    if (withNormal) {
        t.normal.resize(im.w * im.h * 4);
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x) {
                auto H = [&](int xx, int yy) { return im.height[((yy + im.h) % im.h) * im.w + (xx + im.w) % im.w]; };
                float dx = (H(x + 1, y) - H(x - 1, y)) * bump, dy = (H(x, y + 1) - H(x, y - 1)) * bump;
                glm::vec3 n = glm::normalize(glm::vec3(-dx, dy, 1.f));  // v da textura cresce para baixo
                int i = (y * im.w + x) * 4;
                t.normal[i] = to8(n.x * 0.5f + 0.5f); t.normal[i + 1] = to8(n.y * 0.5f + 0.5f);
                t.normal[i + 2] = to8(n.z * 0.5f + 0.5f); t.normal[i + 3] = 255;
            }
    }
    (void)toSrgb8;
    return t;
}

glm::vec3 rgb(int r, int g, int b) { return glm::vec3(r, g, b) / 255.f; }

// Janela desenhada no retângulo [x0,x1]x[y0,y1] (em fração do tile). Retorna true se o pixel é da janela.
bool window(Img& im, int i, float u, float v, float x0, float x1, float y0, float y1, float frame, glm::vec3 frameCol, bool grid) {
    if (u < x0 || u > x1 || v < y0 || v > y1) return false;
    bool isFrame = u < x0 + frame || u > x1 - frame || v < y0 + frame || v > y1 - frame;
    if (grid) {
        float cx = (x0 + x1) * 0.5f;
        isFrame = isFrame || std::abs(u - cx) < frame * 0.5f || std::abs(v - (y0 + (y1 - y0) * 0.4f)) < frame * 0.5f;
    }
    if (isFrame) { im.col[i] = frameCol; im.rough[i] = 0.5f; im.height[i] = 0.3f; return true; }
    // vidro: gradiente de "céu refletido" + escurecido embaixo
    float t = (v - y0) / (y1 - y0);
    im.col[i] = glm::mix(rgb(70, 92, 110), rgb(22, 30, 38), t);
    im.rough[i] = 0.06f;
    im.metal[i] = 0.f;
    im.height[i] = -0.4f;
    return true;
}

void plasterBase(Img& im, glm::vec3 base, uint32_t seed, float dirt = 0.25f) {
    im.each([&](int, int, int i, float u, float v) {
        float n = tileNoise(u, v, 8.f, 4, seed), m = tileNoise(u, v, 32.f, 2, seed + 7);
        float stain = std::clamp((v - 0.75f) * 4.f, 0.f, 1.f) * dirt * tileNoise(u, v, 4.f, 3, seed + 3);
        im.col[i] = base * (0.88f + 0.16f * n + 0.06f * m) * (1.f - stain);
        im.rough[i] = 0.85f + 0.1f * m;
        im.height[i] = n * 0.3f + m * 0.2f;
    });
}

TexData genPlaster() { Img im(256, 256); plasterBase(im, rgb(236, 232, 224), 11, 0.15f); return finish(im, true, 1.5f); }

TexData genFacadeHouse() {
    Img im(256, 256);
    plasterBase(im, rgb(236, 232, 224), 21);
    im.each([&](int, int, int i, float u, float v) {
        window(im, i, u, v, 0.3f, 0.7f, 0.25f, 0.68f, 0.035f, rgb(245, 245, 245), true);
        if (u > 0.27f && u < 0.73f && v > 0.68f && v < 0.71f) { im.col[i] = rgb(200, 198, 192); im.height[i] = 0.5f; }  // peitoril
    });
    return finish(im, true, 1.5f);
}

TexData genFacadeApt() {
    Img im(256, 256);
    plasterBase(im, rgb(236, 232, 224), 31, 0.1f);
    im.each([&](int, int, int i, float u, float v) {
        if (v > 0.9f) { im.col[i] = rgb(190, 188, 182) * (0.95f + 0.05f * tileNoise(u, v, 16, 2, 5)); im.height[i] = 0.4f; return; }  // faixa da laje
        if (window(im, i, u, v, 0.18f, 0.82f, 0.22f, 0.7f, 0.025f, rgb(60, 62, 66), true)) return;
        if (v > 0.7f && v < 0.9f && u > 0.12f && u < 0.88f) {  // guarda-corpo da varanda
            bool bar = std::fmod(u * 40.f, 1.f) < 0.25f || v < 0.715f;
            if (bar) { im.col[i] = rgb(55, 58, 62); im.metal[i] = 0.8f; im.rough[i] = 0.4f; im.height[i] = 0.4f; }
        }
    });
    return finish(im, true, 1.5f);
}

TexData genFacadeOffice() {
    Img im(128, 256);
    im.each([&](int, int, int i, float u, float v) {
        bool mullion = u < 0.04f || u > 0.96f || v < 0.025f || v > 0.975f;
        bool spandrel = v > 0.8f;  // faixa opaca entre andares
        if (mullion) { im.col[i] = rgb(48, 52, 58); im.metal[i] = 1.f; im.rough[i] = 0.35f; im.height[i] = 0.5f; }
        else if (spandrel) { im.col[i] = rgb(40, 54, 70); im.metal[i] = 0.3f; im.rough[i] = 0.25f; }
        else {
            float g = tileNoise(u, v, 3, 2, 41);
            im.col[i] = glm::mix(rgb(60, 92, 120), rgb(28, 44, 62), v) * (0.9f + 0.2f * g);
            im.metal[i] = 0.6f; im.rough[i] = 0.05f;
        }
    });
    return finish(im, true, 1.f);
}

// Tijolo baiano: blocos 25 x 12.5 cm (4 por linha em 1 m), juntas de argamassa
void brickPattern(Img& im, float tileW, float tileH, float u0, float v0, float uScale, float vScale, int i, float u, float v, uint32_t seed) {
    float x = (u * uScale + u0) / tileW, y = (v * vScale + v0) / tileH;
    int row = int(std::floor(y));
    float xs = x + (row & 1) * 0.5f;
    int colI = int(std::floor(xs));
    float fx = xs - colI, fy = y - row;
    bool mortar = fx < 0.06f || fy < 0.12f;
    float var = hashf(colI, row, seed);
    float n = fbm(u * 60.f, v * 60.f, 2, seed + 9);
    if (mortar) { im.col[i] = rgb(170, 160, 145) * (0.85f + 0.2f * n); im.rough[i] = 0.95f; im.height[i] = -0.5f; }
    else {
        im.col[i] = glm::mix(rgb(176, 88, 52), rgb(205, 118, 70), var) * (0.85f + 0.25f * n);
        // furos do bloco cerâmico aparecendo em alguns tijolos quebrados
        im.rough[i] = 0.85f; im.height[i] = 0.3f + 0.2f * n;
    }
}

TexData genBrick() {
    Img im(256, 256);
    im.each([&](int, int, int i, float u, float v) { brickPattern(im, 0.25f, 0.125f, 0, 0, 1.f, 1.f, i, u, v, 51); });
    return finish(im, true, 3.f);
}

TexData genBrickWindow() {
    Img im(512, 512);
    const float W = 2.5f, H = 2.7f;
    im.each([&](int, int, int i, float u, float v) {
        brickPattern(im, 0.25f, 0.125f, 0, 0, W, H, i, u, v, 61);
        // janela basculante de ferro
        if (window(im, i, u, v, 0.34f, 0.66f, 0.3f, 0.56f, 0.02f, rgb(45, 48, 50), true)) im.metal[i] = 0.6f;
        // reboco grosseiro em volta da janela
        else if (u > 0.31f && u < 0.69f && v > 0.27f && v < 0.59f) { im.col[i] = rgb(160, 156, 148); im.height[i] = 0.1f; }
    });
    return finish(im, true, 3.f);
}

TexData genRoofTile() {
    Img im(256, 256);
    im.each([&](int, int, int i, float u, float v) {
        float x = u * 5.f, y = v * 6.f;
        int row = int(y);
        float fx = x + (row & 1) * 0.5f;
        float wave = 0.5f + 0.5f * std::cos((fx - std::floor(fx)) * 6.2831853f);
        float fy = y - row;
        float var = hashf(int(std::floor(fx)), row, 71);
        float n = tileNoise(u, v, 16, 3, 72);
        im.height[i] = wave * 0.6f + fy * 0.6f;
        im.col[i] = glm::mix(rgb(150, 62, 38), rgb(185, 88, 52), var) * (0.65f + 0.25f * wave + 0.15f * fy) * (0.9f + 0.2f * n);
        im.rough[i] = 0.7f;
    });
    return finish(im, true, 4.f);
}

TexData genConcrete() {
    Img im(256, 256);
    im.each([&](int, int, int i, float u, float v) {
        float n = tileNoise(u, v, 6, 5, 81), p = tileNoise(u, v, 48, 2, 82);
        im.col[i] = rgb(150, 150, 146) * (0.82f + 0.25f * n + 0.08f * p);
        im.rough[i] = 0.9f; im.height[i] = p * 0.5f;
    });
    return finish(im, true, 1.f);
}

void asphaltBase(Img& im, uint32_t seed) {
    im.each([&](int, int, int i, float u, float v) {
        float n = tileNoise(u, v, 5, 4, seed), g = hashf(int(u * im.w), int(v * im.h), seed + 1);
        im.col[i] = rgb(52, 54, 57) * (0.8f + 0.25f * n + 0.2f * g);
        im.rough[i] = 0.8f + 0.15f * g; im.height[i] = g * 0.4f;
    });
}

TexData genAsphalt() { Img im(256, 256); asphaltBase(im, 91); return finish(im, true, 1.f); }

TexData genRoad() {
    Img im(256, 256);
    asphaltBase(im, 101);
    im.each([&](int, int, int i, float u, float v) {
        bool center = std::abs(u - 0.5f) < 0.012f && v < 0.55f;                 // faixa tracejada
        bool edge = std::abs(u - 0.06f) < 0.008f || std::abs(u - 0.94f) < 0.008f;  // faixas laterais contínuas
        if (center) { im.col[i] = rgb(225, 190, 40); im.rough[i] = 0.6f; }
        else if (edge) { im.col[i] = rgb(220, 220, 215); im.rough[i] = 0.6f; }
    });
    return finish(im, true, 1.f);
}

TexData genSidewalk() {
    Img im(256, 256);
    im.each([&](int, int, int i, float u, float v) {
        float x = u * 4.f, y = v * 4.f;
        float fx = x - std::floor(x), fy = y - std::floor(y);
        bool joint = fx < 0.03f || fy < 0.03f;
        float var = hashf(int(x), int(y), 111), n = tileNoise(u, v, 24, 2, 112);
        im.col[i] = joint ? rgb(110, 108, 104) : rgb(178, 174, 166) * (0.92f + 0.1f * var + 0.06f * n);
        im.rough[i] = 0.85f; im.height[i] = joint ? -0.5f : 0.2f * n;
    });
    return finish(im, true, 2.f);
}

TexData genGrass(uint32_t seed) {
    Img im(256, 256);
    im.each([&](int, int, int i, float u, float v) {
        float n = tileNoise(u, v, 4, 4, seed), b = hashf(int(u * 256), int(v * 256), seed + 1), c = tileNoise(u, v, 24, 2, seed + 2);
        im.col[i] = glm::mix(rgb(62, 98, 38), rgb(104, 132, 52), n) * (0.8f + 0.3f * b) * (0.9f + 0.15f * c);
        im.rough[i] = 0.92f; im.height[i] = b * 0.5f;
    });
    return finish(im, true, 1.5f);
}

TexData genWood() {
    Img im(256, 256);
    im.each([&](int, int, int i, float u, float v) {
        float plank = std::floor(u * 4.f);
        float grain = std::sin((v * 30.f + fbm(u * 4.f, v * 2.f, 3, 121 + int(plank)) * 6.f) * 3.14159f) * 0.5f + 0.5f;
        bool gap = std::fmod(u * 4.f, 1.f) < 0.03f;
        float var = hashf(int(plank), 0, 122);
        im.col[i] = gap ? rgb(50, 32, 20) : glm::mix(rgb(110, 70, 40), rgb(150, 100, 60), grain * 0.6f + var * 0.4f);
        im.rough[i] = 0.7f; im.height[i] = gap ? -0.5f : grain * 0.2f;
    });
    return finish(im, true, 1.5f);
}

TexData genMetal(glm::vec3 c, float rough, uint32_t seed) {
    Img im(128, 128);
    im.each([&](int, int, int i, float u, float v) {
        float brushed = fbm(u * 2.f, v * 64.f, 3, seed);
        im.col[i] = c * (0.9f + 0.15f * brushed);
        im.metal[i] = 1.f; im.rough[i] = rough + 0.1f * brushed;
    });
    return finish(im, false);
}

TexData genSolid(glm::vec3 c, float rough, float metal, uint32_t seed, float variation) {
    Img im(64, 64);
    im.each([&](int, int, int i, float u, float v) {
        float n = tileNoise(u, v, 4, 3, seed);
        im.col[i] = c * (1.f - variation * 0.5f + variation * n);
        im.rough[i] = rough; im.metal[i] = metal;
    });
    return finish(im, false);
}

TexData genCourt() {
    Img im(512, 256);
    // quadra poliesportiva 28 x 15 m: piso verde, área externa vermelha, linhas brancas
    im.each([&](int, int, int i, float u, float v) {
        float x = u * 28.f, y = v * 15.f;
        float n = tileNoise(u, v, 8, 2, 131);
        bool inside = x > 1.f && x < 27.f && y > 1.f && y < 14.f;
        im.col[i] = (inside ? rgb(46, 118, 82) : rgb(160, 60, 48)) * (0.95f + 0.08f * n);
        im.rough[i] = 0.6f;
        const float lw = 0.06f;
        auto line = [&](bool c) { if (c) { im.col[i] = rgb(235, 235, 230); im.rough[i] = 0.5f; } };
        line(inside && (std::abs(x - 1.f) < lw || std::abs(x - 27.f) < lw || std::abs(y - 1.f) < lw || std::abs(y - 14.f) < lw));
        line(std::abs(x - 14.f) < lw && y > 1.f && y < 14.f);
        float r = glm::length(glm::vec2(x - 14.f, y - 7.5f));
        line(std::abs(r - 2.f) < lw);
        // áreas do goleiro (semicírculos de 6 m)
        float rl = glm::length(glm::vec2(x - 1.f, y - 7.5f)), rr = glm::length(glm::vec2(x - 27.f, y - 7.5f));
        line((std::abs(rl - 6.f) < lw && x > 1.f) || (std::abs(rr - 6.f) < lw && x < 27.f));
    });
    return finish(im, false);
}

TexData generate(Mat m) {
    switch (m) {
        case Mat::Plaster: return genPlaster();
        case Mat::FacadeHouse: return genFacadeHouse();
        case Mat::FacadeApt: return genFacadeApt();
        case Mat::FacadeOffice: return genFacadeOffice();
        case Mat::Brick: return genBrick();
        case Mat::BrickWindow: return genBrickWindow();
        case Mat::RoofTile: return genRoofTile();
        case Mat::Concrete: return genConcrete();
        case Mat::Asphalt: return genAsphalt();
        case Mat::Road: return genRoad();
        case Mat::Sidewalk: return genSidewalk();
        case Mat::Grass: return genGrass(141);
        case Mat::Terrain: return genGrass(151);
        case Mat::Wood: return genWood();
        case Mat::Metal: return genMetal(rgb(200, 202, 206), 0.3f, 161);
        case Mat::GunMetal: return genMetal(rgb(60, 62, 66), 0.4f, 171);
        case Mat::Polymer: return genSolid(rgb(28, 28, 30), 0.6f, 0.f, 181, 0.15f);
        case Mat::Court: return genCourt();
        case Mat::Water: return genSolid(rgb(18, 52, 70), 0.04f, 0.f, 191, 0.1f);
        case Mat::Painted: return genSolid(rgb(255, 255, 255), 0.75f, 0.f, 201, 0.04f);
        case Mat::Glass: return genSolid(rgb(30, 38, 46), 0.05f, 0.3f, 211, 0.f);
        default: return genSolid(rgb(200, 200, 200), 0.8f, 0.f, 1, 0.f);
    }
}

}  // namespace

const TexData& proceduralTexture(Mat m) {
    static TexData cache[int(Mat::Count)];
    TexData& t = cache[int(m)];
    if (t.albedo.empty()) t = generate(m);
    return t;
}

}  // namespace eng
