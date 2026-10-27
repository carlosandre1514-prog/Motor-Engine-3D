#include "procgen/TextureGen.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <stb_image.h>
#include "procgen/Noise.h"

namespace eng {

const char* matName(Mat m) {
    static const char* names[] = {"Reboco", "Fachada casa", "Fachada prédio", "Vidro de escritório", "Tijolo", "Tijolo com janela",
                                  "Telha", "Concreto", "Asfalto", "Rua", "Calçada", "Grama", "Madeira", "Metal", "Metal escuro",
                                  "Plástico", "Quadra", "Água", "Terreno", "Pintura lisa", "Vidro", "Tijolo vermelho",
                                  "Fachada de tijolo", "Ardósia", "Mármore", "Terra"};
    return names[int(m)];
}

MatParams matParams(Mat m) {
    switch (m) {
        case Mat::Metal: return {1.f, 1.f, 0, false};
        case Mat::GunMetal: return {1.f, 1.f, 0, false};
        case Mat::Water: return {1.f, 0.f, 0, false};
        case Mat::Terrain: return {1.f, 0.f, 1, false};
        case Mat::RoofTile: return {1.f, 0.f, 0, true};   // telhado visto por baixo também
        case Mat::Slate: return {1.f, 0.f, 0, true};
        default: return {1.f, 1.f, 0, false};
    }
}

namespace {

std::string gTexDir;
TexData gCache[int(Mat::Count)];

// Imagem de arquivo (RGBA8, valores como estão no arquivo: sRGB para cor)
struct Pic {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    explicit operator bool() const { return w > 0; }
    glm::vec4 at(float u, float v) const {  // bilinear, repetindo nas bordas
        float x = u * w - 0.5f, y = v * h - 0.5f;
        int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        float fx = x - x0, fy = y - y0;
        auto P = [&](int xx, int yy) {
            const uint8_t* p = &px[(((yy % h) + h) % h * w + ((xx % w) + w) % w) * 4];
            return glm::vec4(p[0], p[1], p[2], p[3]) / 255.f;
        };
        return glm::mix(glm::mix(P(x0, y0), P(x0 + 1, y0), fx), glm::mix(P(x0, y0 + 1), P(x0 + 1, y0 + 1), fx), fy);
    }
};

Pic loadPic(const char* name) {
    Pic p;
    if (gTexDir.empty()) return p;
    int comp;
    if (unsigned char* d = stbi_load((gTexDir + "/" + name).c_str(), &p.w, &p.h, &comp, 4)) {
        p.px.assign(d, d + p.w * p.h * 4);
        stbi_image_free(d);
    } else p.w = p.h = 0;
    return p;
}

// Material PBR completo de arquivo: <base>_alb.jpg, _nrm.jpg, _orm.jpg (o mesmo tamanho). normalFrom: outro conjunto
bool fromFiles(TexData& t, const char* base, const char* normalFrom = nullptr) {
    std::string b = base, nb = normalFrom ? normalFrom : base;
    Pic a = loadPic((b + "_alb.jpg").c_str()), n = loadPic((nb + "_nrm.jpg").c_str()), o = loadPic((nb + "_orm.jpg").c_str());
    if (!a || !n || !o || n.w != a.w || o.w != a.w || n.h != a.h || o.h != a.h) return false;
    t.w = a.w; t.h = a.h;
    t.albedo = std::move(a.px); t.normal = std::move(n.px); t.orm = std::move(o.px);
    for (size_t i = 0; i < t.orm.size(); i += 4) t.orm[i] = 255;  // R = AO do kit: o motor usa AO separado
    return true;
}

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
// Sala vista pela janela (imagem do interior) e cortina na frente; nulos = vidro liso
struct WindowLook { const Pic* room = nullptr; const Pic* curtain = nullptr; };

bool window(Img& im, int i, float u, float v, float x0, float x1, float y0, float y1, float frame, glm::vec3 frameCol, bool grid,
            WindowLook look = {}) {
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
    if (look.room && *look.room) {
        float s = (u - x0) / (x1 - x0);
        glm::vec3 room = glm::vec3(look.room->at(s, t));
        if (look.curtain && *look.curtain) {
            glm::vec4 c = look.curtain->at(s, t);
            room = glm::mix(room, glm::vec3(c) * 1.6f, c.a);
        }
        im.col[i] = glm::mix(room * 0.85f, rgb(90, 110, 125), 0.12f);  // um pouco do céu refletido no vidro
    }
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

// Sujeira escorrendo da laje/peitoril (imagem do kit), mais forte embaixo das janelas
void grime(Img& im, const Pic& drips, float amount) {
    if (!drips) return;
    im.each([&](int, int, int i, float u, float v) { im.col[i] *= 1.f - amount * (1.f - drips.at(u, v * 0.5f).r) * (0.4f + 0.6f * v); });
}

TexData genFacadeHouse() {
    Img im(512, 512);
    plasterBase(im, rgb(236, 232, 224), 21);
    grime(im, loadPic("escorrido.jpg"), 0.12f);
    Pic room = loadPic("interior_2.jpg"), cur = loadPic("cortina_lados.png");
    im.each([&](int, int, int i, float u, float v) {
        window(im, i, u, v, 0.3f, 0.7f, 0.25f, 0.68f, 0.035f, rgb(245, 245, 245), true, {&room, &cur});
        if (u > 0.27f && u < 0.73f && v > 0.68f && v < 0.71f) { im.col[i] = rgb(200, 198, 192); im.height[i] = 0.5f; }  // peitoril
    });
    return finish(im, true, 1.5f);
}

TexData genFacadeApt() {
    Img im(512, 512);
    plasterBase(im, rgb(236, 232, 224), 31, 0.1f);
    grime(im, loadPic("escorrido.jpg"), 0.15f);
    Pic room = loadPic("interior_1.jpg"), cur = loadPic("cortina_aberta.png");
    im.each([&](int, int, int i, float u, float v) {
        if (v > 0.9f) { im.col[i] = rgb(190, 188, 182) * (0.95f + 0.05f * tileNoise(u, v, 16, 2, 5)); im.height[i] = 0.4f; return; }  // faixa da laje
        if (window(im, i, u, v, 0.18f, 0.82f, 0.22f, 0.7f, 0.025f, rgb(60, 62, 66), true, {&room, &cur})) return;
        if (v > 0.7f && v < 0.9f && u > 0.12f && u < 0.88f) {  // guarda-corpo da varanda
            bool bar = std::fmod(u * 40.f, 1.f) < 0.25f || v < 0.715f;
            if (bar) { im.col[i] = rgb(55, 58, 62); im.metal[i] = 0.8f; im.rough[i] = 0.4f; im.height[i] = 0.4f; }
        }
    });
    return finish(im, true, 1.5f);
}

TexData genFacadeOffice() {
    Img im(256, 512);
    Pic room = loadPic("interior_1.jpg");
    im.each([&](int, int, int i, float u, float v) {
        bool mullion = u < 0.04f || u > 0.96f || v < 0.025f || v > 0.975f;
        bool spandrel = v > 0.8f;  // faixa opaca entre andares
        if (mullion) { im.col[i] = rgb(48, 52, 58); im.metal[i] = 1.f; im.rough[i] = 0.35f; im.height[i] = 0.5f; }
        else if (spandrel) { im.col[i] = rgb(40, 54, 70); im.metal[i] = 0.3f; im.rough[i] = 0.25f; }
        else {
            float g = tileNoise(u, v, 3, 2, 41);
            im.col[i] = glm::mix(rgb(60, 92, 120), rgb(28, 44, 62), v) * (0.9f + 0.2f * g);
            if (room) im.col[i] = glm::mix(im.col[i], glm::vec3(room.at((u - 0.04f) / 0.92f, (v - 0.025f) / 0.775f)) * 1.2f, 0.28f);
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
    if (TexData t; fromFiles(t, "concreto")) return t;
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

// asfalto do kit (cor) com o relevo do concreto
bool asphaltFiles(TexData& t) {
    if (!fromFiles(t, "concreto")) return false;
    Pic a = loadPic("asfalto_alb.jpg");
    if (a.w != t.w || a.h != t.h) return false;
    t.albedo = std::move(a.px);
    return true;
}

TexData genAsphalt() {
    if (TexData t; asphaltFiles(t)) return t;
    Img im(256, 256); asphaltBase(im, 91); return finish(im, true, 1.f);
}

TexData genRoad() {
    TexData real;
    if (asphaltFiles(real)) {  // asfalto do kit + faixas pintadas (u atravessa a pista, v = 6 m)
        for (int y = 0; y < real.h; ++y)
            for (int x = 0; x < real.w; ++x) {
                float u = (x + 0.5f) / real.w, v = (y + 0.5f) / real.h;
                bool center = std::abs(u - 0.5f) < 0.012f && v < 0.55f, edge = std::abs(u - 0.06f) < 0.008f || std::abs(u - 0.94f) < 0.008f;
                if (!center && !edge) continue;
                uint8_t* p = &real.albedo[(y * real.w + x) * 4];
                float wear = 0.75f + 0.25f * hashf(x / 3, y / 3, 7);  // tinta gasta
                glm::vec3 c = center ? glm::vec3(225, 190, 40) : glm::vec3(220, 220, 215);
                for (int k = 0; k < 3; ++k) p[k] = uint8_t(glm::mix(float(p[k]), c[k], wear));
                real.orm[(y * real.w + x) * 4 + 1] = 150;
            }
        return real;
    }
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

// Prédio de tijolo vermelho: tijolo do kit, laje de concreto, janela com sala iluminada e cortina
TexData genFacadeBrick() {
    Pic brick = loadPic("tijolo_vermelho_alb.jpg"), bn = loadPic("tijolo_vermelho_nrm.jpg");
    if (!brick || !bn) return genFacadeApt();
    Pic room = loadPic("interior_1.jpg"), cur = loadPic("cortina_lados.png"), drips = loadPic("escorrido.jpg");
    Img im(512, 512);
    std::vector<glm::vec3> nrm(im.w * im.h, glm::vec3(0.5f, 0.5f, 1.f));
    im.each([&](int, int, int i, float u, float v) {
        float bu = u * 1.6f, bv = v * 1.5f;  // o tijolo do kit cobre 2 x 2 m; o tile da fachada é 3.2 x 3 m
        im.col[i] = glm::vec3(brick.at(bu, bv));
        nrm[i] = glm::vec3(bn.at(bu, bv));
        im.rough[i] = 0.85f;
        if (drips) im.col[i] *= 1.f - 0.15f * (1.f - drips.at(u, v * 0.5f).r) * v;
        if (v > 0.9f) { im.col[i] = rgb(175, 172, 165) * (0.95f + 0.05f * tileNoise(u, v, 16, 2, 5)); nrm[i] = {0.5f, 0.5f, 1.f}; return; }
        if (u > 0.15f && u < 0.85f && v > 0.7f && v < 0.74f) { im.col[i] = rgb(190, 186, 178); nrm[i] = {0.5f, 0.5f, 1.f}; return; }  // peitoril
        if (window(im, i, u, v, 0.18f, 0.82f, 0.18f, 0.7f, 0.03f, rgb(235, 232, 225), true, {&room, &cur})) nrm[i] = {0.5f, 0.5f, 1.f};
    });
    TexData t = finish(im, false);
    t.normal.resize(im.w * im.h * 4);
    for (int i = 0; i < im.w * im.h; ++i) {
        for (int k = 0; k < 3; ++k) t.normal[i * 4 + k] = to8(nrm[i][k]);
        t.normal[i * 4 + 3] = 255;
    }
    return t;
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
        case Mat::RedBrick: { if (TexData t; fromFiles(t, "tijolo_vermelho")) return t; return genBrick(); }
        case Mat::FacadeBrick: return genFacadeBrick();
        case Mat::Slate: { if (TexData t; fromFiles(t, "ardosia")) return t; return genRoofTile(); }
        case Mat::Marble: { if (TexData t; fromFiles(t, "marmore")) return t; return genSolid(rgb(215, 200, 170), 0.3f, 0.f, 221, 0.1f); }
        case Mat::Dirt: { if (TexData t; fromFiles(t, "terra")) return t; return genSolid(rgb(110, 85, 60), 0.95f, 0.f, 231, 0.3f); }
        default: return genSolid(rgb(200, 200, 200), 0.8f, 0.f, 1, 0.f);
    }
}

}  // namespace

void setMaterialTextureDir(const char* dir) {
    if (gTexDir == dir) return;
    gTexDir = dir;
    for (TexData& t : gCache) t = TexData{};  // regenera com as imagens novas
}

const TexData& proceduralTexture(Mat m) {
    TexData& t = gCache[int(m)];
    if (t.albedo.empty()) t = generate(m);
    return t;
}

}  // namespace eng
