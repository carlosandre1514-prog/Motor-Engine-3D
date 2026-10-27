#include "procgen/Buildings.h"
#include <algorithm>

namespace eng {

glm::vec4 housePaint(Rng& r) {
    static const int c[][3] = {{240, 236, 222}, {245, 222, 170}, {214, 232, 210}, {200, 220, 236}, {238, 200, 180},
                               {250, 240, 200}, {230, 230, 230}, {222, 196, 150}, {190, 210, 180}};
    const int* p = c[r.irange(0, 8)];
    return srgb(p[0], p[1], p[2]);
}
glm::vec4 favelaPaint(Rng& r) {
    static const int c[][3] = {{235, 90, 70}, {250, 200, 60}, {80, 160, 210}, {120, 190, 100}, {240, 140, 180},
                               {250, 150, 60}, {150, 110, 200}, {245, 245, 240}, {90, 200, 190}, {220, 220, 120}};
    const int* p = c[r.irange(0, 9)];
    return srgb(p[0], p[1], p[2]);
}
glm::vec4 buildingPaint(Rng& r) {
    static const int c[][3] = {{232, 224, 208}, {210, 205, 198}, {245, 245, 240}, {200, 150, 120}, {180, 190, 200},
                               {225, 210, 180}, {170, 175, 170}};
    const int* p = c[r.irange(0, 6)];
    return srgb(p[0], p[1], p[2]);
}

void buildWaterTank(MeshBuilder& p, glm::vec3 base) {
    glm::vec4 old = p.color;
    p.color = srgb(40, 110, 200);  // caixa d'água azul
    p.cylinder(base, base + glm::vec3(0, 0.8f, 0), 0.55f, 0.62f, 14, false);
    p.cylinder(base + glm::vec3(0, 0.8f, 0), base + glm::vec3(0, 0.95f, 0), 0.65f, 0.1f, 14, false);
    p.color = old;
}

void buildHouse(Geo& g, Rng& r, float w, float d, int floors, glm::vec4 color, bool frontWall) {
    const float fh = 3.f;
    float H = fh * floors;
    MeshBuilder& wall = g[Mat::FacadeHouse];
    wall.color = color; wall.uvTile = {3.f, 3.f};
    wall.box({-w / 2, 0, -d / 2}, {w, H, d});
    // embasamento (rodapé) mais escuro
    MeshBuilder& conc = g[Mat::Concrete];
    conc.uvTile = {2.f, 2.f};
    conc.box({-w / 2 - 0.05f, 0, -d / 2 - 0.05f}, {w + 0.1f, 0.35f, d + 0.1f});
    // porta
    MeshBuilder& wood = g[Mat::Wood];
    wood.color = glm::vec4(1.f);
    float dx = r.range(-w * 0.25f, w * 0.25f);
    wood.box({dx - 0.5f, 0, d / 2}, {1.f, 2.15f, 0.07f});
    // telhado de duas águas (cumeeira ao longo do lado maior)
    float roofH = std::min(w, d) * 0.28f;
    g.offset({-w / 2, 0, -d / 2});
    MeshBuilder& roof = g[Mat::RoofTile];
    roof.gableRoof(w, d, H, roofH, 0.45f, {1.f, 1.f});
    MeshBuilder& gable = g[Mat::Plaster];
    gable.color = color; gable.uvTile = {2.f, 2.f};
    gable.gableEnds(w, d, H, roofH);
    g.offset({w / 2, 0, d / 2});
    g.collider({-w / 2, 0, -d / 2}, {w, H + roofH * 0.6f, d});
    // muro e portão na frente
    if (frontWall) {
        MeshBuilder& m = g[Mat::Plaster];
        m.color = color * 0.9f; m.color.a = 1.f;
        float z = d / 2 + 3.f, mw = w + 2.f, h = 1.8f;
        m.box({-mw / 2, 0, z}, {mw / 2 - 1.5f, h, 0.15f});
        m.box({1.5f, 0, z}, {mw / 2 - 1.5f, h, 0.15f});
        m.box({-mw / 2, 0, -d / 2 - 1.f}, {0.15f, h, z + d / 2 + 1.f});
        m.box({mw / 2 - 0.15f, 0, -d / 2 - 1.f}, {0.15f, h, z + d / 2 + 1.f});
        g[Mat::Metal].box({-1.5f, 0.05f, z + 0.03f}, {3.f, 1.6f, 0.05f});  // portão
        g.collider({-mw / 2, 0, z}, {mw, h, 0.15f});
    }
}

void buildBarraco(Geo& g, Rng& r, float w, float d, int floors, bool brick, glm::vec4 color, float foundation) {
    const float fh = 2.7f;
    // fundação/pilares até o chão (terreno inclinado)
    if (foundation > 0.05f) {
        MeshBuilder& c = g[Mat::Concrete];
        c.uvTile = {2.f, 2.f};
        c.box({-w / 2, -foundation, -d / 2}, {w, foundation, d});
        g.collider({-w / 2, -foundation, -d / 2}, {w, foundation, d});
    }
    float y = 0.f, ox = 0.f, oz = 0.f, cw = w, cd = d;
    for (int f = 0; f < floors; ++f) {
        MeshBuilder& wall = (brick || (f > 0 && r.chance(0.4f))) ? g[Mat::BrickWindow] : g[Mat::FacadeHouse];
        bool isBrick = &wall == &g[Mat::BrickWindow];
        wall.color = isBrick ? glm::vec4(1.f) : color;
        wall.uvTile = isBrick ? glm::vec2(2.5f, 2.7f) : glm::vec2(2.5f, 2.7f);
        wall.box({ox - cw / 2, y, oz - cd / 2}, {cw, fh, cd});
        g.collider({ox - cw / 2, y, oz - cd / 2}, {cw, fh, cd});
        // laje
        MeshBuilder& slab = g[Mat::Concrete];
        slab.box({ox - cw / 2 - 0.12f, y + fh, oz - cd / 2 - 0.12f}, {cw + 0.24f, 0.16f, cd + 0.24f});
        y += fh + 0.16f;
        // andar de cima um pouco menor / deslocado (puxadinho)
        if (f + 1 < floors) {
            cw = std::max(2.4f, cw - r.range(0.f, 1.f));
            cd = std::max(2.4f, cd - r.range(0.f, 1.f));
            ox += r.range(-0.4f, 0.4f); oz += r.range(-0.4f, 0.4f);
        }
    }
    // laje de cima: caixa d'água, esperas de ferro/pilares, mureta
    MeshBuilder& p = g[Mat::Painted];
    if (r.chance(0.75f)) buildWaterTank(p, {ox + r.range(-cw * 0.25f, cw * 0.25f), y, oz + r.range(-cd * 0.25f, cd * 0.25f)});
    if (r.chance(0.6f)) {
        MeshBuilder& c = g[Mat::Concrete];
        for (int k = 0; k < 4; ++k) {
            float sx = (k & 1) ? cw / 2 - 0.15f : -cw / 2, sz = (k & 2) ? cd / 2 - 0.15f : -cd / 2;
            c.box({ox + sx, y, oz + sz}, {0.15f, r.range(0.4f, 1.f), 0.15f});
        }
    }
    if (r.chance(0.35f)) {
        MeshBuilder& b = g[Mat::Brick];
        b.uvTile = {1.f, 1.f};
        b.box({ox - cw / 2, y, oz + cd / 2 - 0.12f}, {cw, 0.9f, 0.12f});
    }
}

void buildApartment(Geo& g, Rng& r, float w, float d, int floors, glm::vec4 color) {
    const float fh = 3.f, lobby = 3.6f;
    float H = lobby + fh * floors;
    MeshBuilder& c = g[Mat::Concrete];
    c.uvTile = {2.f, 2.f};
    c.box({-w / 2, 0, -d / 2}, {w, lobby, d});
    // entrada com vidro
    g[Mat::Glass].box({-2.f, 0.1f, d / 2}, {4.f, 2.8f, 0.05f});
    c.box({-2.6f, 2.9f, d / 2}, {5.2f, 0.25f, 1.6f});  // marquise
    MeshBuilder& f = g[Mat::FacadeApt];
    f.color = color; f.uvTile = {3.2f, 3.f};
    f.box({-w / 2, lobby, -d / 2}, {w, fh * floors, d});
    // platibanda, laje e casa de máquinas / caixa d'água
    c.box({-w / 2 - 0.1f, H, -d / 2 - 0.1f}, {w + 0.2f, 0.9f, 0.2f});
    c.box({-w / 2 - 0.1f, H, d / 2 - 0.1f}, {w + 0.2f, 0.9f, 0.2f});
    c.box({-w / 2 - 0.1f, H, -d / 2 + 0.1f}, {0.2f, 0.9f, d - 0.2f});
    c.box({w / 2 - 0.1f, H, -d / 2 + 0.1f}, {0.2f, 0.9f, d - 0.2f});
    float tw = std::min(6.f, w * 0.35f);
    c.box({-tw / 2 + r.range(-w * 0.2f, w * 0.2f), H, -2.f}, {tw, 3.f, 4.f});
    g.collider({-w / 2, 0, -d / 2}, {w, H + 0.9f, d});
}

void buildSkyscraper(Geo& g, Rng& r, float w, float d, int floors) {
    const float fh = 3.6f;
    MeshBuilder& glass = g[Mat::FacadeOffice];
    glass.uvTile = {1.6f, fh};
    MeshBuilder& c = g[Mat::Concrete];
    c.uvTile = {2.f, 2.f};
    // embasamento
    c.box({-w / 2 - 1.f, 0, -d / 2 - 1.f}, {w + 2.f, 0.4f, d + 2.f});
    int tiers = r.irange(1, 3);
    float y = 0.f, tw = w, td = d;
    int remaining = floors;
    for (int t = 0; t < tiers; ++t) {
        int fl = t + 1 == tiers ? remaining : std::max(4, int(remaining * r.range(0.45f, 0.7f)));
        remaining -= fl;
        glass.box({-tw / 2, y, -td / 2}, {tw, fl * fh, td});
        g.collider({-tw / 2, y, -td / 2}, {tw, fl * fh, td});
        y += fl * fh;
        c.box({-tw / 2 - 0.15f, y, -td / 2 - 0.15f}, {tw + 0.3f, 0.6f, td + 0.3f});  // cinta de concreto
        y += 0.6f;
        if (remaining <= 0) break;
        tw *= r.range(0.65f, 0.85f); td *= r.range(0.65f, 0.85f);
    }
    // coroamento e antena
    c.box({-tw * 0.3f, y, -td * 0.3f}, {tw * 0.6f, 4.f, td * 0.6f});
    if (r.chance(0.7f)) g[Mat::Metal].cylinder({0, y + 4.f, 0}, {0, y + 4.f + r.range(8.f, 20.f), 0}, 0.25f, 0.08f, 8);
}

void buildSchoolWing(Geo& g, float len, float depth, int floors, glm::vec4 wallColor, glm::vec4 band) {
    const float fh = 3.4f;
    float H = fh * floors;
    MeshBuilder& f = g[Mat::FacadeHouse];
    f.color = wallColor; f.uvTile = {3.4f, fh};
    f.box({-len / 2, 0, -depth / 2}, {len, H, depth});
    MeshBuilder& p = g[Mat::Plaster];
    p.color = band; p.uvTile = {2.f, 2.f};
    for (int k = 1; k <= floors; ++k)  // faixa colorida em cada laje
        p.box({-len / 2 - 0.05f, k * fh - 0.45f, -depth / 2 - 0.05f}, {len + 0.1f, 0.45f, depth + 0.1f});
    MeshBuilder& c = g[Mat::Concrete];
    c.uvTile = {2.f, 2.f};
    c.box({-len / 2 - 0.2f, H, -depth / 2 - 0.2f}, {len + 0.4f, 0.5f, depth + 0.4f});
    // varanda/corredor coberto na frente
    c.box({-len / 2, fh - 0.2f, depth / 2}, {len, 0.2f, 2.2f});
    for (float x = -len / 2 + 0.2f; x < len / 2; x += 4.f) c.box({x, 0, depth / 2 + 1.9f}, {0.25f, fh - 0.2f, 0.25f});
    g.collider({-len / 2, 0, -depth / 2}, {len, H + 0.5f, depth});
}

void buildTree(MeshBuilder& p, Rng& r, int kind) {
    glm::vec4 trunk = srgb(100, 72, 48);
    if (kind == 1) {  // pinheiro
        float h = r.range(6.f, 10.f);
        p.color = trunk;
        p.cylinder({0, 0, 0}, {0, h * 0.35f, 0}, 0.22f, 0.16f, 7, false);
        glm::vec4 leaf = glm::mix(srgb(40, 80, 45), srgb(60, 100, 55), r.f());
        for (int k = 0; k < 3; ++k) {
            float y0 = h * (0.25f + k * 0.22f), rad = h * (0.26f - k * 0.06f);
            p.color = leaf * (0.9f + 0.1f * k); p.color.a = 1.f;
            p.cylinder({0, y0, 0}, {0, y0 + h * 0.38f, 0}, rad, 0.f, 7, false);
        }
    } else if (kind == 2) {  // palmeira
        float h = r.range(6.f, 9.f);
        p.color = srgb(130, 105, 75);
        p.cylinder({0, 0, 0}, {0.4f, h, 0}, 0.2f, 0.14f, 7, false);
        p.color = srgb(70, 120, 50);
        for (int k = 0; k < 7; ++k) {
            float a = k * 0.8975979f;
            glm::vec3 dir(std::cos(a), -0.35f, std::sin(a));
            p.cylinder({0.4f, h, 0}, glm::vec3(0.4f, h, 0) + dir * 2.6f, 0.35f, 0.02f, 4, false);
        }
    } else {  // copa redonda (mangueira, ipê...)
        float h = r.range(3.f, 5.f);
        p.color = trunk;
        p.cylinder({0, 0, 0}, {0, h, 0}, 0.25f, 0.18f, 7, false);
        glm::vec4 leaf = r.chance(0.15f) ? srgb(230, 200, 60) : glm::mix(srgb(50, 95, 40), srgb(85, 125, 50), r.f());
        int blobs = r.irange(2, 4);
        for (int k = 0; k < blobs; ++k) {
            glm::vec3 c(r.range(-1.f, 1.f), h + r.range(0.f, 1.5f), r.range(-1.f, 1.f));
            float rad = r.range(1.3f, 2.f);
            p.color = leaf * r.range(0.85f, 1.1f); p.color.a = 1.f;
            p.sphere(c, glm::vec3(rad, rad * 0.85f, rad), 7, 5);
        }
    }
    p.color = glm::vec4(1.f);
}

void buildCar(Geo& g, Rng& r) {
    static const int cols[][3] = {{200, 30, 30}, {240, 240, 240}, {30, 30, 35}, {150, 150, 155}, {30, 70, 160}, {230, 190, 40}, {60, 110, 70}};
    const int* c = cols[r.irange(0, 6)];
    MeshBuilder& body = g[Mat::Painted];
    body.color = srgb(c[0], c[1], c[2]);
    body.box({-0.9f, 0.3f, -2.1f}, {1.8f, 0.7f, 4.2f});        // carroceria
    body.box({-0.8f, 1.0f, -1.1f}, {1.6f, 0.62f, 2.2f});       // cabine
    MeshBuilder& gl = g[Mat::Glass];
    gl.box({-0.82f, 1.05f, 1.06f}, {1.64f, 0.5f, 0.06f});      // para-brisa
    gl.box({-0.82f, 1.05f, -1.12f}, {1.64f, 0.5f, 0.06f});
    gl.box({-0.83f, 1.08f, -1.f}, {1.66f, 0.45f, 2.f}, 4 | 8 | 16 | 32);  // laterais
    MeshBuilder& tire = g[Mat::Polymer];
    for (int k = 0; k < 4; ++k) {
        float x = (k & 1) ? 0.92f : -0.92f, z = (k & 2) ? 1.35f : -1.35f;
        tire.cylinder({x - 0.12f, 0.33f, z}, {x + 0.12f, 0.33f, z}, 0.33f, 0.33f, 8, true);
    }
    body.color = srgb(250, 245, 200);
    body.box({-0.75f, 0.6f, 2.1f}, {0.3f, 0.15f, 0.03f});      // faróis
    body.box({0.45f, 0.6f, 2.1f}, {0.3f, 0.15f, 0.03f});
    body.color = glm::vec4(1.f);
    g.collider({-0.9f, 0.f, -2.1f}, {1.8f, 1.6f, 4.2f});
}

void buildStreetLight(Geo& g) {
    MeshBuilder& m = g[Mat::Metal];
    m.cylinder({0, 0, 0}, {0, 7.f, 0}, 0.1f, 0.07f, 8, false);
    m.cylinder({0, 7.f, 0}, {0, 7.2f, 1.4f}, 0.06f, 0.05f, 6, false);
    MeshBuilder& p = g[Mat::Painted];
    p.color = srgb(255, 250, 220);
    p.box({-0.18f, 6.95f, 1.2f}, {0.36f, 0.15f, 0.5f});
    p.color = glm::vec4(1.f);
}

}  // namespace eng
