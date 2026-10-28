#include "procgen/TextureGen.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>
#include <sys/stat.h>
#include <string>
#include <stb_image.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "procgen/Noise.h"

namespace eng {

const char* matName(Mat m) {
    static const char* names[] = {"Reboco", "Fachada casa", "Fachada prédio", "Vidro de escritório", "Tijolo", "Tijolo com janela",
                                  "Telha", "Concreto", "Asfalto", "Rua", "Calçada", "Grama", "Madeira", "Metal", "Metal escuro",
                                  "Plástico", "Quadra", "Água", "Terreno", "Pintura lisa", "Vidro", "Tijolo vermelho",
                                  "Fachada de tijolo", "Ardósia", "Mármore", "Terra", "Pedra", "Cerâmica", "Grafite",
                                  "Alambrado", "Telha ondulada", "Pichações", "Lâmpada", "Cimentado"};
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
        case Mat::ChainLink: return {1.f, 1.f, 0, true};   // tela vista dos dois lados
        case Mat::Mural: return {1.f, 0.f, 0, false};
        case Mat::Corrugated: return {1.f, 0.f, 0, true};
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

// Piso cerâmico: lajotas bege de 40 cm (5 x 5 no tile de 2 m) com rejunte
TexData genCeramic() {
    Img im(512, 512);
    im.each([&](int, int, int i, float u, float v) {
        float x = u * 5.f, y = v * 5.f, fx = x - std::floor(x), fy = y - std::floor(y);
        bool joint = fx < 0.02f || fy < 0.02f;
        float var = hashf(int(x), int(y), 251), n = tileNoise(u, v, 20, 2, 252);
        im.col[i] = joint ? rgb(150, 145, 135) : glm::mix(rgb(214, 196, 168), rgb(228, 212, 186), var) * (0.96f + 0.06f * n);
        im.rough[i] = joint ? 0.9f : 0.35f;
        im.height[i] = joint ? -0.6f : 0.f;
    });
    return finish(im, true, 2.f);
}

// Muro grafitado: reboco sujo com 2 "peças" de letras gordas (preenchimento em degradê, contorno preto e brilho
// branco), escorridos de tinta e pichações finas por cima. Tile 4 x 2 m (1024 x 512).
TexData genGraffiti() {
    Img im(512, 256);
    plasterBase(im, rgb(205, 198, 186), 301, 0.35f);
    struct Letter { glm::vec2 c, h; float rot, round; };
    struct Piece { std::vector<Letter> ls; glm::vec3 a, b; };
    const glm::vec3 pal[] = {rgb(236, 64, 122), rgb(41, 182, 246), rgb(255, 202, 40), rgb(102, 187, 106), rgb(255, 112, 67),
                             rgb(171, 71, 188), rgb(38, 198, 218), rgb(240, 240, 240)};
    std::vector<Piece> pieces;
    uint32_t sd = 311;
    auto rnd = [&]() { return hashf(int(sd++), 7, 313); };
    for (int k = 0; k < 2; ++k) {
        Piece pc;
        pc.a = pal[int(rnd() * 7.99f)];
        pc.b = pal[int(rnd() * 7.99f)];
        float x = 0.06f + k * 0.5f, y = 0.32f + rnd() * 0.25f;
        int n = 4 + int(rnd() * 2.99f);
        float lw = 0.4f / n;
        for (int i = 0; i < n; ++i) {
            Letter l;
            l.c = {x + (i + 0.5f) * lw, y + (rnd() - 0.5f) * 0.08f};
            l.h = {lw * (0.42f + rnd() * 0.12f), 0.15f + rnd() * 0.08f};
            l.rot = (rnd() - 0.5f) * 0.5f;
            l.round = 0.02f + rnd() * 0.03f;
            pc.ls.push_back(l);
        }
        pieces.push_back(pc);
    }
    // distância (em u, com v corrigido para a proporção 2:1) até a peça; dentro < 0
    auto sdf = [&](const Piece& pc, glm::vec2 p) {
        float d = 1e9f;
        for (const Letter& l : pc.ls) {
            glm::vec2 q = p - l.c;
            float c = std::cos(l.rot), s2 = std::sin(l.rot);
            q = {c * q.x + s2 * q.y, -s2 * q.x + c * q.y};
            glm::vec2 dq = glm::abs(q) - l.h + l.round;
            float box = glm::length(glm::max(dq, 0.f)) + std::min(std::max(dq.x, dq.y), 0.f) - l.round;
            // buraco no meio de algumas letras (O, A, D...)
            glm::vec2 hq = glm::abs(q) - l.h * glm::vec2(0.35f, 0.3f);
            float hole = glm::length(glm::max(hq, 0.f)) + std::min(std::max(hq.x, hq.y), 0.f);
            d = std::min(d, std::max(box, -hole));
        }
        return d;
    };
    im.each([&](int, int, int i, float u, float v) {
        glm::vec2 p(u, v * 0.5f);
        for (const Piece& pc : pieces) {
            float d = sdf(pc, p);
            float dsh = sdf(pc, p - glm::vec2(0.008f, 0.008f));  // sombra 3D
            if (dsh < 0.f && d > 0.f) { im.col[i] = glm::mix(im.col[i], rgb(25, 25, 30), 0.85f); im.rough[i] = 0.6f; }
            if (d < 0.006f && d > -0.002f) { im.col[i] = rgb(15, 15, 18); im.rough[i] = 0.55f; }
            else if (d <= -0.002f) {
                float t = std::clamp((p.y - 0.12f) * 3.2f, 0.f, 1.f);
                glm::vec3 c = glm::mix(pc.a, pc.b, t) * (0.93f + 0.1f * tileNoise(u, v, 40.f, 2, 317));
                if (d > -0.007f) c = glm::mix(c, glm::vec3(1.f), 0.6f);  // brilho na borda
                im.col[i] = c; im.rough[i] = 0.5f;
            }
        }
        // escorridos de tinta embaixo das peças
        int col = int(u * 180.f);
        if (hashf(col, 1, 331) > 0.86f) {
            const Piece& pc = pieces[u < 0.5f ? 0 : 1];
            float top = 0.f;
            for (const Letter& l : pc.ls) if (std::abs(u - l.c.x) < l.h.x) top = std::max(top, (l.c.y + l.h.y) * 2.f);
            float len = 0.05f + hashf(col, 2, 331) * 0.2f;
            float fx = u * 180.f - col;
            if (top > 0.f && v > top && v < top + len && std::abs(fx - 0.5f) < 0.18f) im.col[i] = glm::mix(im.col[i], pc.a * 0.8f, 0.85f);
        }
        // pichação: traços finos pretos
        for (int k = 0; k < 6; ++k) {
            float cx = hashf(k, 3, 341), cy = 0.08f + hashf(k, 4, 341) * 0.84f, len = 0.08f + hashf(k, 5, 341) * 0.1f;
            float x = (u - cx) / len;
            if (x < 0.f || x > 1.f) continue;
            float y = cy + 0.035f * std::sin(x * (14.f + k * 3.f)) + 0.02f * std::sin(x * 41.f + k);
            if (std::abs(v - y) < 0.006f) { im.col[i] = rgb(20, 20, 24); im.rough[i] = 0.6f; }
        }
    });
    return finish(im, true, 1.5f);
}

// Alambrado: arame galvanizado em losangos (6 cm), o resto transparente (alfa 0). Tile 1 x 1 m.
TexData genChainLink() {
    Img im(256, 256);
    std::vector<float> a(im.w * im.h, 0.f);
    im.each([&](int, int, int i, float u, float v) {
        const float n = 16.f, t = 0.075f;
        float d1 = (u + v) * n, d2 = (u - v) * n;
        float e1 = std::abs(d1 - std::round(d1)), e2 = std::abs(d2 - std::round(d2));
        float e = std::min(e1, e2);
        im.col[i] = rgb(150, 155, 160) * (0.85f + 0.3f * (1.f - e / t));
        im.metal[i] = 0.8f; im.rough[i] = 0.45f;
        a[i] = e < t ? 1.f : 0.f;
    });
    TexData tx = finish(im, false);
    for (int i = 0; i < im.w * im.h; ++i) tx.albedo[i * 4 + 3] = to8(a[i]);
    return tx;
}

// Telha ondulada de fibrocimento: cinza com ondas (normal), manchas de limo e sujeira. Tile 2 x 2 m.
TexData genCorrugated() {
    Img im(512, 512);
    im.each([&](int, int, int i, float u, float v) {
        float wave = std::sin(u * 6.2831853f * 11.f);
        float n = tileNoise(u, v, 6.f, 4, 351), m = tileNoise(u, v, 24.f, 2, 353);
        glm::vec3 c = rgb(168, 166, 158) * (0.85f + 0.2f * n);
        float lichen = std::clamp((n - 0.55f) * 3.f, 0.f, 1.f);
        c = glm::mix(c, rgb(70, 72, 60), lichen * 0.6f);
        c *= 0.9f + 0.1f * wave * 0.5f + 0.05f * m;
        im.col[i] = c; im.rough[i] = 0.85f;
        im.height[i] = wave * 0.5f;
    });
    return finish(im, true, 4.f);
}

// ---------- pichações e grafites com texto (fontes livres em materials/fontes) ----------
struct Font {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool ok = false;
};
Font loadFont(const char* file) {
    Font f;
    if (gTexDir.empty()) return f;
    FILE* fp = std::fopen((gTexDir + "/fontes/" + file).c_str(), "rb");
    if (!fp) return f;
    std::fseek(fp, 0, SEEK_END);
    long n = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    f.data.resize(size_t(std::max(0L, n)));
    if (n > 0 && std::fread(f.data.data(), 1, size_t(n), fp) == size_t(n))
        f.ok = stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0)) != 0;
    std::fclose(fp);
    return f;
}
std::vector<int> utf8(const char* s) {
    std::vector<int> out;
    for (const unsigned char* p = (const unsigned char*)s; *p;) {
        int c = *p++;
        if (c >= 0xC0 && c < 0xE0 && *p) c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
        else if (c >= 0xE0 && c < 0xF0 && p[0] && p[1]) { c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
        out.push_back(c);
    }
    return out;
}
// Camada do mural (1024 x 512): máscara de cobertura 0..1
struct Layer {
    int w, h;
    std::vector<float> m;
    Layer(int W, int H) : w(W), h(H), m(W * H, 0.f) {}
    float at(int x, int y) const { return x < 0 || y < 0 || x >= w || y >= h ? 0.f : m[y * w + x]; }
};
// Texto centrado em (cx, cy), cabendo em maxW; wobble: letras sobem e descem (jeito de spray). Retorna a largura usada.
float drawText(Layer& L, const Font& f, const char* text, float cx, float cy, float maxW, float px, float wobble, uint32_t seed) {
    if (!f.ok) return 0.f;
    std::vector<int> cps = utf8(text);
    float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    auto width = [&](float sc) {
        float w = 0.f;
        for (size_t i = 0; i < cps.size(); ++i) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&f.info, cps[i], &adv, &lsb);
            w += adv * sc;
            if (i + 1 < cps.size()) w += stbtt_GetCodepointKernAdvance(&f.info, cps[i], cps[i + 1]) * sc;
        }
        return w;
    };
    float tw = width(scale);
    if (tw > maxW) { scale *= maxW / tw; tw = maxW; }
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f.info, &asc, &desc, &gap);
    float base = cy + (asc + desc) * 0.5f * scale;
    float x = cx - tw * 0.5f;
    for (size_t i = 0; i < cps.size(); ++i) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&f.info, cps[i], &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&f.info, cps[i], scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<uint8_t> bmp(size_t(gw * gh));
            stbtt_MakeCodepointBitmap(&f.info, bmp.data(), gw, gh, gw, scale, scale, cps[i]);
            float dy = (hashf(int(i), 3, seed) - 0.5f) * 2.f * wobble;
            int ox = int(x + x0), oy = int(base + y0 + dy);
            for (int yy = 0; yy < gh; ++yy)
                for (int xx = 0; xx < gw; ++xx) {
                    int X = ox + xx, Y = oy + yy;
                    if (X < 0 || Y < 0 || X >= L.w || Y >= L.h) continue;
                    float& d = L.m[Y * L.w + X];
                    d = std::max(d, bmp[yy * gw + xx] / 255.f);
                }
        }
        x += adv * scale;
        if (i + 1 < cps.size()) x += stbtt_GetCodepointKernAdvance(&f.info, cps[i], cps[i + 1]) * scale;
    }
    return tw;
}
// Distância (px) até a máscara (chanfro 3-4, duas passadas)
std::vector<float> distOutside(const Layer& L) {
    const float INF = 1e9f, a = 1.f, b = 1.4142f;
    std::vector<float> d(L.m.size());
    for (size_t i = 0; i < d.size(); ++i) d[i] = L.m[i] > 0.5f ? 0.f : INF;
    auto D = [&](int x, int y) { return x < 0 || y < 0 || x >= L.w || y >= L.h ? INF : d[y * L.w + x]; };
    for (int y = 0; y < L.h; ++y)
        for (int x = 0; x < L.w; ++x) {
            float& v = d[y * L.w + x];
            v = std::min({v, D(x - 1, y) + a, D(x, y - 1) + a, D(x - 1, y - 1) + b, D(x + 1, y - 1) + b});
        }
    for (int y = L.h - 1; y >= 0; --y)
        for (int x = L.w - 1; x >= 0; --x) {
            float& v = d[y * L.w + x];
            v = std::min({v, D(x + 1, y) + a, D(x, y + 1) + a, D(x + 1, y + 1) + b, D(x - 1, y + 1) + b});
        }
    return d;
}
// Formas: coroa, coração e bola (somadas à máscara: ganham contorno junto com as letras)
void crown(Layer& L, float cx, float cy, float w) {
    float h = w * 0.55f;
    for (int y = int(cy - h); y <= int(cy); ++y)
        for (int x = int(cx - w / 2); x <= int(cx + w / 2); ++x) {
            if (x < 0 || y < 0 || x >= L.w || y >= L.h) continue;
            float u = (x - (cx - w / 2)) / w, v = (cy - y) / h;  // v: 0 embaixo, 1 no alto
            float tri = 1.f - std::abs(std::fmod(u * 6.f, 2.f) - 1.f);  // 0 nos vales, 1 nas 3 pontas
            bool in = v < 0.45f || v < 0.45f + 0.55f * tri * (u > 0.02f && u < 0.98f ? 1.f : 0.f);
            if (in) L.m[y * L.w + x] = 1.f;
        }
}
void heart(Layer& L, float cx, float cy, float s) {
    for (int y = int(cy - s); y <= int(cy + s); ++y)
        for (int x = int(cx - s); x <= int(cx + s); ++x) {
            if (x < 0 || y < 0 || x >= L.w || y >= L.h) continue;
            float X = (x - cx) / s * 1.3f, Y = -(y - cy) / s * 1.3f + 0.25f;
            float q = X * X + Y * Y - 1.f;
            if (q * q * q - X * X * Y * Y * Y <= 0.f) L.m[y * L.w + x] = 1.f;
        }
}
void disc(Layer& L, float cx, float cy, float r) {
    for (int y = int(cy - r); y <= int(cy + r); ++y)
        for (int x = int(cx - r); x <= int(cx + r); ++x)
            if (x >= 0 && y >= 0 && x < L.w && y < L.h && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) L.m[y * L.w + x] = 1.f;
}

struct MuralDef { const char* name; const char* text; const char* text2; const char* font; glm::vec3 a, b, line; int deco; int style; };
const MuralDef kMurals[kMuralCount + kSignCount] = {
    {"Favela venceu", "FAVELA VENCEU", "", "RubikWetPaint.ttf", {1.f, 0.85f, 0.1f}, {0.1f, 0.75f, 0.25f}, {0.02f, 0.02f, 0.03f}, 0, 0},
    {"Paz e amor", "PAZ E AMOR", "", "Bangers.ttf", {0.3f, 0.9f, 1.f}, {0.1f, 0.35f, 0.9f}, {0.02f, 0.02f, 0.05f}, 1, 0},
    {"O crime é nós", "O CRIME É NÓS", "", "PermanentMarker.ttf", {0.04f, 0.04f, 0.05f}, {0.04f, 0.04f, 0.05f}, {0.8f, 0.08f, 0.08f}, 0, 1},
    {"Rei Pelé 10", "REI PELÉ", "10", "Bangers.ttf", {1.f, 0.85f, 0.2f}, {0.95f, 0.45f, 0.05f}, {0.03f, 0.03f, 0.03f}, 2, 0},
    {"Neymar Jr 10", "NEYMAR JR", "10", "Bangers.ttf", {1.f, 0.9f, 0.1f}, {0.1f, 0.65f, 0.3f}, {0.05f, 0.15f, 0.6f}, 3, 0},
    {"Fé em Deus", "FÉ EM DEUS", "", "RubikWetPaint.ttf", {1.f, 0.55f, 0.15f}, {0.95f, 0.2f, 0.55f}, {0.02f, 0.02f, 0.03f}, 0, 0},
    {"Pichações", "NÓS POR NÓS", "", "PermanentMarker.ttf", {0.05f, 0.05f, 0.06f}, {0.05f, 0.05f, 0.06f}, {0.f, 0.f, 0.f}, 0, 2},
    {"Comunidade", "COMUNIDADE", "", "Bangers.ttf", {0.75f, 0.35f, 1.f}, {1.f, 0.3f, 0.6f}, {0.02f, 0.02f, 0.03f}, 0, 0},
    // placas das lojas (estilo 3: placa pintada, letra branca)
    {"Loja de armas", "LOJA DE ARMAS", "", "Bangers.ttf", {1.f, 1.f, 1.f}, {0.08f, 0.16f, 0.42f}, {0.02f, 0.04f, 0.12f}, 0, 3},
    {"Fardas", "FARDAS", "", "Bangers.ttf", {1.f, 1.f, 1.f}, {0.1f, 0.12f, 0.16f}, {0.02f, 0.02f, 0.04f}, 0, 3},
    {"Armas", "ARMAS", "", "RubikWetPaint.ttf", {1.f, 0.85f, 0.1f}, {0.95f, 0.3f, 0.05f}, {0.02f, 0.02f, 0.03f}, 0, 0},
    {"Roupas", "ROUPAS", "", "RubikWetPaint.ttf", {0.4f, 1.f, 0.5f}, {0.1f, 0.6f, 0.95f}, {0.02f, 0.02f, 0.03f}, 0, 0},
};
constexpr int kAtlasCells = kMuralCount + kSignCount;

TexData genMurals() {
    const int CW = 1024, CH = 512;
    Img im(CW * 2, CH * (kAtlasCells / 2));
    std::vector<float> alpha(im.w * im.h, 0.f);
    std::string loaded[3];
    Font fonts[3];
    auto font = [&](const char* name) -> const Font& {
        for (int i = 0; i < 3; ++i) if (loaded[i] == name) return fonts[i];
        for (int i = 0; i < 3; ++i) if (loaded[i].empty()) { loaded[i] = name; fonts[i] = loadFont(name); return fonts[i]; }
        return fonts[0];
    };
    for (const MuralDef& M : kMurals) font(M.font);  // carrega antes: as threads só leem
    font("Bangers.ttf");
    auto job = [&](int k) {  // cada mural numa thread (escreve só no seu pedaço do atlas)
        const MuralDef& M = kMurals[k];
        const int ox = (k % 2) * CW, oy = (k / 2) * CH;
        Layer L(CW, CH);
        const Font& F = font(M.font);
        auto put = [&](int x, int y, glm::vec3 c, float a) {
            int i = (oy + y) * im.w + ox + x;
            im.col[i] = glm::mix(im.col[i], c, a);
            alpha[i] = std::max(alpha[i], a);
            im.rough[i] = 0.55f;
        };
        if (M.style == 3) {  // placa de loja: fundo pintado com moldura, letras brancas com sombra
            Layer T(CW, CH);
            drawText(T, F, M.text, 512.f, 256.f, 900.f, 300.f, 0.f, 800 + k);
            std::vector<float> d = distOutside(T);
            for (int y = 0; y < CH; ++y)
                for (int x = 0; x < CW; ++x) {
                    bool frame = x < 24 || y < 24 || x >= CW - 24 || y >= CH - 24;
                    glm::vec3 c = frame ? glm::vec3(0.85f, 0.85f, 0.88f) : M.b * (0.9f + 0.15f * tileNoise(float(x) / CW, float(y) / CH, 6.f, 2, 810));
                    int xs = x - 8, ys = y - 8;
                    if (!frame && xs >= 0 && ys >= 0 && d[ys * CW + xs] < 6.f) c = M.line;
                    if (!frame && d[y * CW + x] < 0.5f) c = M.a;
                    put(x, y, c, 1.f);
                }
            return;
        }
        if (M.style == 2) {  // parede de pichação: várias tags finas
            const char* tags[] = {"NÓS POR NÓS", "MORRO", "VILA", "PAZ", "ZN", "QUEBRADA", "LOKO", "FAVELA"};
            const glm::vec3 cols[] = {{0.05f, 0.05f, 0.06f}, {0.8f, 0.1f, 0.1f}, {0.1f, 0.3f, 0.9f}, {0.05f, 0.05f, 0.06f}};
            for (int t = 0; t < 8; ++t) {
                Layer T(CW, CH);
                float cx = 200.f + (t % 3) * 320.f + (hashf(t, 1, 501) - 0.5f) * 80.f, cy = 90.f + (t / 3) * 150.f + (hashf(t, 2, 501) - 0.5f) * 40.f;
                drawText(T, F, tags[t], cx, cy, 330.f, 90.f + hashf(t, 3, 501) * 50.f, 8.f, 600 + t);
                glm::vec3 c = cols[t % 4];
                for (int y = 0; y < CH; ++y) for (int x = 0; x < CW; ++x) if (float v = T.at(x, y); v > 0.f) put(x, y, c, v);
            }
            return;
        }
        bool big2 = M.text2[0] != 0;
        float textY = M.deco >= 2 ? 300.f : 256.f;
        drawText(L, F, M.text, big2 ? 420.f : 512.f, textY, big2 ? 640.f : 940.f, M.style == 1 ? 230.f : 250.f, M.style == 1 ? 10.f : 14.f, 500 + k);
        if (big2) drawText(L, font("Bangers.ttf"), M.text2, 885.f, textY + 10.f, 170.f, 260.f, 6.f, 700 + k);
        if (M.deco == 1) heart(L, 930.f, 120.f, 70.f);
        if (M.deco >= 2) crown(L, 420.f, 165.f, 230.f);
        Layer ball(CW, CH);
        if (M.deco == 2) disc(ball, 120.f, 400.f, 70.f);
        for (size_t i = 0; i < ball.m.size(); ++i) L.m[i] = std::max(L.m[i], ball.m[i]);
        std::vector<float> d = distOutside(L);
        const float outline = M.style == 1 ? 6.f : 13.f;
        const int sx = M.style == 1 ? 7 : 12, sy = M.style == 1 ? 7 : 12;
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                int i = y * CW + x;
                float v = L.m[i];
                // nuvem de spray atrás (só nas peças)
                if (M.style == 0 && d[i] < 34.f) {
                    float n = tileNoise(float(x) / CW, float(y) / CH, 18.f, 3, 520 + k);
                    if (d[i] < 20.f + n * 22.f) put(x, y, glm::mix(M.b, glm::vec3(1.f), 0.65f), 0.75f);
                }
                // sombra 3D
                int xs = x - sx, ys = y - sy;
                if (xs >= 0 && ys >= 0 && d[ys * CW + xs] < outline) put(x, y, M.style == 1 ? M.line : glm::vec3(0.05f), 1.f);
                if (d[i] < outline) put(x, y, M.line, 1.f);
                if (v > 0.5f) {
                    float t = std::clamp((y - 120.f) / 300.f, 0.f, 1.f);
                    glm::vec3 c = glm::mix(M.a, M.b, t);
                    if (L.at(x - 5, y - 5) < 0.5f && M.style == 0) c = glm::mix(c, glm::vec3(1.f), 0.7f);  // brilho
                    if (ball.at(x, y) > 0.5f) {  // bola: branca com gomos pretos
                        float bx = (x - 120.f) / 70.f, by = (y - 400.f) / 70.f;
                        float r = std::sqrt(bx * bx + by * by);
                        float a = std::atan2(by, bx);
                        bool patch = r < 0.32f && std::cos(5.f * a) * 0.06f + 0.26f > r;  // pentágono do meio
                        for (int g = 0; g < 5 && !patch; ++g) {
                            float ga = g * 1.2566f - 1.5708f;
                            patch = glm::length(glm::vec2(bx - std::cos(ga) * 0.78f, by - std::sin(ga) * 0.78f)) < 0.2f;
                        }
                        c = patch ? glm::vec3(0.05f) : glm::vec3(0.97f);
                    }
                    put(x, y, c, std::min(1.f, v * 1.3f));
                }
            }
        // escorridos de tinta embaixo
        for (int col = 0; col < CW; col += 6) {
            if (hashf(col, k, 531) < 0.86f) continue;
            int top = -1;
            for (int y = CH - 1; y >= 0; --y) if (L.m[y * CW + col] > 0.5f) { top = y; break; }
            if (top < 0) continue;
            int len = 20 + int(hashf(col, 1, 531) * 70.f);
            for (int y = top; y < std::min(CH, top + len); ++y)
                for (int x = col - 2; x <= col + 2; ++x) if (x >= 0 && x < CW) put(x, y, M.a, 1.f);
        }
    };
    std::vector<std::thread> th;
    for (int k = 0; k < kAtlasCells; ++k) th.emplace_back(job, k);
    for (auto& t : th) t.join();
    // metade da resolução (1024 x 1024): leve na memória do celular e ainda nítido na parede
    Img half(im.w / 2, im.h / 2);
    std::vector<float> ha(half.w * half.h);
    for (int y = 0; y < half.h; ++y)
        for (int x = 0; x < half.w; ++x) {
            int i = y * half.w + x, a0 = (2 * y) * im.w + 2 * x, a1 = a0 + 1, b0 = a0 + im.w, b1 = b0 + 1;
            half.col[i] = (im.col[a0] + im.col[a1] + im.col[b0] + im.col[b1]) * 0.25f;
            half.rough[i] = (im.rough[a0] + im.rough[a1] + im.rough[b0] + im.rough[b1]) * 0.25f;
            ha[i] = (alpha[a0] + alpha[a1] + alpha[b0] + alpha[b1]) * 0.25f;
        }
    TexData t = finish(half, false);
    for (int i = 0; i < half.w * half.h; ++i) t.albedo[i * 4 + 3] = to8(ha[i]);
    return t;
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
        case Mat::Stone: { if (TexData t; fromFiles(t, "pedra")) return t; return genSolid(rgb(140, 135, 125), 0.9f, 0.f, 241, 0.4f); }
        case Mat::Ceramic: return genCeramic();
        case Mat::Graffiti: return genGraffiti();
        case Mat::ChainLink: return genChainLink();
        case Mat::Corrugated: return genCorrugated();
        case Mat::Mural: return genMurals();
        case Mat::Cement: return generate(Mat::Concrete);
        case Mat::Lamp: return genSolid(rgb(255, 245, 225), 0.3f, 0.f, 541, 0.f);
        default: return genSolid(rgb(200, 200, 200), 0.8f, 0.f, 1, 0.f);
    }
}

}  // namespace

void setMaterialTextureDir(const char* dir) {
    if (gTexDir == dir) return;
    gTexDir = dir;
    for (TexData& t : gCache) t = TexData{};  // regenera com as imagens novas
}

const char* muralName(int i) { return i >= 0 && i < kAtlasCells ? kMurals[i].name : ""; }
glm::vec4 muralUV(int i) {
    i = std::clamp(i, 0, kAtlasCells - 1);
    const float rows = kAtlasCells / 2;
    float u0 = (i % 2) * 0.5f, v0 = (i / 2) / rows;
    return {u0, v0, u0 + 0.5f, v0 + 1.f / rows};
}

// Cache em disco (shaders/cache): da 2ª abertura em diante as texturas não são geradas de novo.
// A chave muda a cada compilação (código novo = texturas novas).
static std::string cachePath(Mat m) {
    if (gTexDir.empty() || (m != Mat::Mural && m != Mat::Graffiti)) return "";  // só as demoradas (o resto é rápido)
    return gTexDir + "/../cache/tex_" + std::to_string(int(m)) + ".bin";
}
static uint32_t cacheKey() {
    uint32_t h = 2166136261u;
    for (const char* p = __DATE__ __TIME__; *p; ++p) h = (h ^ uint8_t(*p)) * 16777619u;
    return h ^ uint32_t(int(Mat::Count) * 7919);
}
static bool loadCached(Mat m, TexData& t) {
    std::string path = cachePath(m);
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t hdr[5] = {};
    bool ok = std::fread(hdr, 4, 5, f) == 5 && hdr[0] == 0x58455454u && hdr[1] == cacheKey() && hdr[2] > 0 && hdr[3] > 0 && hdr[2] <= 4096 && hdr[3] <= 4096;
    if (ok) {
        size_t n = size_t(hdr[2]) * hdr[3] * 4;
        t.w = int(hdr[2]); t.h = int(hdr[3]);
        t.albedo.resize(n); t.orm.resize(n);
        if (hdr[4]) t.normal.resize(n);
        ok = std::fread(t.albedo.data(), 1, n, f) == n && std::fread(t.orm.data(), 1, n, f) == n &&
             (!hdr[4] || std::fread(t.normal.data(), 1, n, f) == n);
    }
    std::fclose(f);
    if (!ok) t = TexData{};
    return ok;
}
static void saveCached(Mat m, const TexData& t) {
    std::string path = cachePath(m);
    if (path.empty()) return;
    mkdir((gTexDir + "/../cache").c_str(), 0755);
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    uint32_t hdr[5] = {0x58455454u, cacheKey(), uint32_t(t.w), uint32_t(t.h), t.normal.empty() ? 0u : 1u};
    bool ok = std::fwrite(hdr, 4, 5, f) == 5 && std::fwrite(t.albedo.data(), 1, t.albedo.size(), f) == t.albedo.size() &&
              std::fwrite(t.orm.data(), 1, t.orm.size(), f) == t.orm.size() &&
              (t.normal.empty() || std::fwrite(t.normal.data(), 1, t.normal.size(), f) == t.normal.size());
    std::fclose(f);
    if (ok) std::rename(tmp.c_str(), path.c_str()); else std::remove(tmp.c_str());
}

const TexData& proceduralTexture(Mat m) {
    TexData& t = gCache[int(m)];
    if (t.albedo.empty() && !loadCached(m, t)) { t = generate(m); saveCached(m, t); }
    return t;
}

void pregenerateTextures() {
    // as que faltam (sem cache) são geradas em paralelo, uma por núcleo
    std::vector<int> todo;
    for (int k = 0; k < int(Mat::Count); ++k)
        if (gCache[k].albedo.empty() && !loadCached(Mat(k), gCache[k])) todo.push_back(k);
    if (todo.empty()) return;
    // as mais demoradas primeiro
    std::stable_sort(todo.begin(), todo.end(), [](int a, int b) {
        auto heavy = [](int m) { return m == int(Mat::Mural) || m == int(Mat::Graffiti) || m == int(Mat::FacadeHouse) || m == int(Mat::FacadeApt); };
        return heavy(a) && !heavy(b);
    });
    std::atomic<size_t> next{0};
    unsigned n = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned i = 0; i < n; ++i)
        th.emplace_back([&] {
            for (size_t j; (j = next++) < todo.size();) {
                int k = todo[j];
                gCache[k] = generate(Mat(k));
                saveCached(Mat(k), gCache[k]);
            }
        });
    for (auto& t : th) t.join();
}

}  // namespace eng
