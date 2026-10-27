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

namespace {

// Parede de construção: bloco com fachada por fora e reboco claro por dentro (lado `inward`), com collider
void wallIn(Geo& g, MeshBuilder& outer, glm::vec3 mn, glm::vec3 sz, glm::vec3 inward, glm::vec4 innerColor) {
    if (sz.x <= 0.01f || sz.y <= 0.01f || sz.z <= 0.01f) return;
    outer.box(mn, sz);
    g.collider(mn, sz);
    InteriorScope is(g);
    MeshBuilder& in = g[Mat::Plaster];
    glm::vec4 keep = in.color; glm::vec2 kt = in.uvTile;
    in.color = innerColor; in.uvTile = {2.f, 2.f};
    const float v = 0.02f;
    if (inward.z > 0) in.box({mn.x, mn.y, mn.z + sz.z}, {sz.x, sz.y, v});
    else if (inward.z < 0) in.box({mn.x, mn.y, mn.z - v}, {sz.x, sz.y, v});
    else if (inward.x > 0) in.box({mn.x + sz.x, mn.y, mn.z}, {v, sz.y, sz.z});
    else in.box({mn.x - v, mn.y, mn.z}, {v, sz.y, sz.z});
    in.color = keep; in.uvTile = kt;
}

// Móvel simples (caixa colorida) na camada de detalhe; collide = o personagem esbarra
void furn(Geo& g, glm::vec4 col, glm::vec3 mn, glm::vec3 sz, bool collide = false) {
    MeshBuilder& m = g[Mat::Painted];
    glm::vec4 keep = m.color;
    m.color = col;
    m.box(mn, sz);
    m.color = keep;
    if (collide) g.collider(mn, sz);
}

// Sala de recepção de prédio: balcão, sofás, plantas e porta do elevador no fundo
void lobbyFurniture(Geo& g, Rng& r, float w, float d, float wallT) {
    InteriorScope ds(g);
    glm::vec4 wood = srgb(120, 80, 50), top = srgb(230, 225, 215), sofa = r.chance(0.5f) ? srgb(60, 60, 70) : srgb(150, 40, 40);
    float bx = -w / 2 + wallT + 1.5f, bz = -d / 2 + wallT + 2.2f;
    furn(g, wood, {bx, 0, bz}, {2.6f, 1.05f, 0.7f}, true);                // balcão
    furn(g, top, {bx - 0.05f, 1.05f, bz - 0.05f}, {2.7f, 0.06f, 0.8f});
    furn(g, srgb(30, 30, 35), {bx + 1.f, 1.11f, bz + 0.2f}, {0.5f, 0.32f, 0.04f});  // monitor
    for (int k = 0; k < 2; ++k) {                                          // sofás de espera
        float sx = w / 2 - wallT - 1.1f, sz = -1.6f + k * 2.6f;
        furn(g, sofa, {sx, 0, sz}, {0.9f, 0.42f, 2.f}, true);
        furn(g, sofa, {sx + 0.7f, 0.42f, sz}, {0.2f, 0.45f, 2.f});
    }
    for (int k = 0; k < 2; ++k) {                                          // plantas
        MeshBuilder& m = g[Mat::Painted];
        glm::vec4 keep = m.color;
        float px = k ? w / 2 - wallT - 0.6f : -w / 2 + wallT + 0.6f, pz = d / 2 - wallT - 0.8f;
        m.color = srgb(90, 70, 60);
        m.cylinder({px, 0, pz}, {px, 0.5f, pz}, 0.28f, 0.32f, 10);
        m.color = srgb(50, 130, 60);
        m.cylinder({px, 0.5f, pz}, {px, 1.6f, pz}, 0.45f, 0.05f, 8);
        m.color = keep;
    }
    // elevador (portas de metal) no fundo
    g[Mat::Metal].box({-1.1f, 0, -d / 2 + wallT + 0.01f}, {2.2f, 2.5f, 0.05f});
    g[Mat::GunMetal].box({-1.25f, 2.5f, -d / 2 + wallT + 0.01f}, {2.5f, 0.25f, 0.06f});
}

}  // namespace

void buildHouse(Geo& g, Rng& r, float w, float d, int floors, glm::vec4 color, bool frontWall) {
    const float fh = 3.f, t = 0.18f, doorW = 1.f, doorH = 2.15f;
    float H = fh * floors;
    MeshBuilder& wall = g[Mat::FacadeHouse];
    wall.color = color; wall.uvTile = {3.f, 3.f};
    float dx = std::clamp(r.range(-w * 0.25f, w * 0.25f), -w / 2 + 1.3f, w / 2 - 1.3f - doorW);
    glm::vec4 inner = srgb(238, 232, 220);
    // paredes externas ocas, porta na frente (+Z)
    wallIn(g, wall, {-w / 2, 0, -d / 2}, {w, H, t}, {0, 0, 1}, inner);
    wallIn(g, wall, {-w / 2, 0, -d / 2 + t}, {t, H, d - 2 * t}, {1, 0, 0}, inner);
    wallIn(g, wall, {w / 2 - t, 0, -d / 2 + t}, {t, H, d - 2 * t}, {-1, 0, 0}, inner);
    const float zf = d / 2 - t;
    wallIn(g, wall, {-w / 2, 0, zf}, {dx + w / 2, H, t}, {0, 0, -1}, inner);
    wallIn(g, wall, {dx + doorW, 0, zf}, {w / 2 - dx - doorW, H, t}, {0, 0, -1}, inner);
    wallIn(g, wall, {dx, doorH, zf}, {doorW, H - doorH, t}, {0, 0, -1}, inner);
    g.door({dx, 0, zf + t * 0.5f}, doorW, doorH, 0);
    // piso de madeira e lajes (forro de cada andar) e divisórias: só aparecem de perto
    InteriorScope inside(g);
    MeshBuilder& wood = g[Mat::Wood];
    wood.color = glm::vec4(1.f); wood.uvTile = {1.5f, 1.5f};
    wood.box({-w / 2 + t, 0, -d / 2 + t}, {w - 2 * t, 0.04f, d - 2 * t});
    MeshBuilder& conc = g[Mat::Concrete];
    conc.uvTile = {2.f, 2.f};
    for (int k = 1; k <= floors; ++k) {
        conc.box({-w / 2 + t, k * fh - 0.15f, -d / 2 + t}, {w - 2 * t, 0.15f, d - 2 * t});
        g.collider({-w / 2 + t, k * fh - 0.15f, -d / 2 + t}, {w - 2 * t, 0.15f, d - 2 * t});
    }
    // divisória: sala na frente, quarto e cozinha no fundo (vão de passagem no meio)
    const float zp = -d / 2 + t + (d - 2 * t) * 0.45f, ch = fh - 0.15f;
    MeshBuilder& pl = g[Mat::Plaster];
    pl.color = inner; pl.uvTile = {2.f, 2.f};
    auto part = [&](glm::vec3 mn, glm::vec3 sz) { if (sz.x > 0.01f) { pl.box(mn, sz); g.collider(mn, sz); } };
    part({-w / 2 + t, 0, zp}, {w / 2 - t - 0.5f, ch, 0.1f});
    part({0.5f, 0, zp}, {w / 2 - t - 0.5f, ch, 0.1f});
    part({-0.5f, 2.1f, zp}, {1.f, ch - 2.1f, 0.1f});
    // móveis
    {
        glm::vec4 fabric = r.chance(0.5f) ? srgb(70, 90, 130) : srgb(130, 100, 70), woodC = srgb(125, 85, 55);
        float x0 = -w / 2 + t + 0.05f, x1 = w / 2 - t - 0.05f, zc = (zp + zf) * 0.5f;
        float sofaL = std::min(2.f, zf - zp - 0.6f);
        furn(g, fabric, {x0, 0, zc - sofaL / 2}, {0.85f, 0.42f, sofaL}, true);           // sofá
        furn(g, fabric, {x0, 0.42f, zc - sofaL / 2}, {0.22f, 0.45f, sofaL});
        furn(g, woodC, {x1 - 0.45f, 0, zc - 0.8f}, {0.45f, 0.5f, 1.6f}, true);          // rack + TV
        furn(g, srgb(20, 20, 24), {x1 - 0.14f, 0.62f, zc - 0.65f}, {0.06f, 0.75f, 1.3f});
        furn(g, srgb(150, 60, 50), {x0 + 1.3f, 0.01f, zc - 1.f}, {std::max(0.5f, x1 - x0 - 2.3f), 0.02f, 2.f});  // tapete
        furn(g, woodC, {x0 + 1.4f, 0, zc - 0.35f}, {0.9f, 0.42f, 0.7f});              // mesinha
        // quarto: cama e guarda-roupa
        float z0 = -d / 2 + t + 0.05f;
        furn(g, woodC, {x0, 0, z0}, {1.45f, 0.42f, 2.f}, true);
        furn(g, srgb(240, 240, 245), {x0 + 0.03f, 0.42f, z0 + 0.03f}, {1.39f, 0.18f, 1.94f});
        furn(g, r.chance(0.5f) ? srgb(200, 80, 80) : srgb(80, 140, 200), {x0 + 0.03f, 0.6f, z0 + 0.6f}, {1.39f, 0.04f, 1.37f});  // coberta
        furn(g, srgb(250, 250, 250), {x0 + 0.15f, 0.6f, z0 + 0.08f}, {1.1f, 0.12f, 0.4f});  // travesseiro
        if (zp - (z0 + 2.15f) > 1.1f) furn(g, woodC, {x0, 0, z0 + 2.15f}, {0.6f, 2.f, 1.1f}, true);
        // cozinha: bancada com fogão, geladeira, mesa e cadeiras
        float kx = std::max(0.6f, x1 - 2.4f);
        furn(g, srgb(240, 240, 235), {kx, 0, z0}, {x1 - 0.75f - kx, 0.88f, 0.6f}, true);
        furn(g, srgb(60, 60, 65), {kx - 0.02f, 0.88f, z0 - 0.02f}, {x1 - 0.73f - kx, 0.04f, 0.64f});
        furn(g, srgb(25, 25, 25), {kx + 0.1f, 0.92f, z0 + 0.1f}, {0.6f, 0.02f, 0.45f});  // fogão
        furn(g, srgb(235, 238, 240), {x1 - 0.7f, 0, z0}, {0.7f, 1.8f, 0.68f}, true);    // geladeira
        float tz = (z0 + 0.7f + zp) * 0.5f, tx = (kx + x1) * 0.5f - 0.6f;
        if (zp - z0 > 2.6f) {
            furn(g, woodC, {tx, 0.72f, tz - 0.45f}, {1.2f, 0.05f, 0.9f}, true);
            for (int k = 0; k < 4; ++k) furn(g, woodC, {tx + 0.05f + (k & 1) * 1.05f, 0, tz - 0.4f + (k >> 1) * 0.75f}, {0.05f, 0.72f, 0.05f});
            for (int k = 0; k < 2; ++k) furn(g, woodC, {tx + 0.35f, 0, k ? tz + 0.5f : tz - 0.95f}, {0.45f, 0.45f, 0.45f});
        }
    }
    inside.g.interior = inside.prev;
    // telhado de duas águas (cumeeira ao longo do lado maior)
    float roofH = std::min(w, d) * 0.28f;
    g.offset({-w / 2, 0, -d / 2});
    bool slate = (int(w * 10.f) + int(d * 10.f)) % 4 == 0;  // algumas casas com telhado de ardósia
    MeshBuilder& roof = g[slate ? Mat::Slate : Mat::RoofTile];
    roof.gableRoof(w, d, H, roofH, 0.45f, slate ? glm::vec2(2.f, 2.f) : glm::vec2(1.f, 1.f));
    MeshBuilder& gable = g[Mat::Plaster];
    gable.color = color; gable.uvTile = {2.f, 2.f};
    gable.gableEnds(w, d, H, roofH);
    g.offset({w / 2, 0, d / 2});
    g.collider({-w / 2, H, -d / 2}, {w, roofH * 0.6f, d});
    // muro e portão na frente
    if (frontWall) {
        DetailScope ds(g);
        MeshBuilder& m = g[Mat::Plaster];
        m.color = color * 0.9f; m.color.a = 1.f;
        float z = d / 2 + 3.f, mw = w + 2.f, h = 1.8f;
        m.box({-mw / 2, 0, z}, {mw / 2 - 1.5f, h, 0.15f});
        m.box({1.5f, 0, z}, {mw / 2 - 1.5f, h, 0.15f});
        m.box({-mw / 2, 0, -d / 2 - 1.f}, {0.15f, h, z + d / 2 + 1.f});
        m.box({mw / 2 - 0.15f, 0, -d / 2 - 1.f}, {0.15f, h, z + d / 2 + 1.f});
        g[Mat::Metal].box({1.45f, 0.05f, z - 1.5f}, {0.05f, 1.6f, 1.5f});  // portão aberto (folha encostada no muro)
        g.collider({-mw / 2, 0, z}, {mw / 2 - 1.5f, h, 0.15f});
        g.collider({1.5f, 0, z}, {mw / 2 - 1.5f, h, 0.15f});
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
        if (f == 0) {  // térreo oco: porta na frente, cama e mesa dentro
            const float t = 0.14f, dw = 0.85f, dh = 2.05f, x0 = -cw / 2, z0 = -cd / 2;
            glm::vec4 inner = srgb(225, 215, 200);
            wallIn(g, wall, {x0, 0, z0}, {cw, fh, t}, {0, 0, 1}, inner);
            wallIn(g, wall, {x0, 0, z0 + t}, {t, fh, cd - 2 * t}, {1, 0, 0}, inner);
            wallIn(g, wall, {-x0 - t, 0, z0 + t}, {t, fh, cd - 2 * t}, {-1, 0, 0}, inner);
            float zf = cd / 2 - t, dx = -dw / 2;
            wallIn(g, wall, {x0, 0, zf}, {dx - x0, fh, t}, {0, 0, -1}, inner);
            wallIn(g, wall, {dx + dw, 0, zf}, {-x0 - dx - dw, fh, t}, {0, 0, -1}, inner);
            wallIn(g, wall, {dx, dh, zf}, {dw, fh - dh, t}, {0, 0, -1}, inner);
            g.door({dx, 0, zf + t * 0.5f}, dw, dh, 0);
            InteriorScope ds2(g);
            g[Mat::Concrete].box({x0 + t, 0, z0 + t}, {cw - 2 * t, 0.03f, cd - 2 * t});
            glm::vec4 wood = srgb(110, 75, 45);
            furn(g, wood, {x0 + t + 0.05f, 0, z0 + t + 0.05f}, {0.95f, 0.38f, 1.9f}, true);
            furn(g, srgb(230, 225, 210), {x0 + t + 0.07f, 0.38f, z0 + t + 0.07f}, {0.91f, 0.15f, 1.86f});
            furn(g, wood, {-x0 - t - 0.85f, 0, z0 + t + 0.1f}, {0.8f, 0.74f, 0.6f}, true);
            furn(g, srgb(60, 60, 60), {-x0 - t - 0.7f, 0.74f, z0 + t + 0.2f}, {0.35f, 0.25f, 0.3f});  // radinho/tv
        } else {
            wall.box({ox - cw / 2, y, oz - cd / 2}, {cw, fh, cd});
            g.collider({ox - cw / 2, y, oz - cd / 2}, {cw, fh, cd});
        }
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
    DetailScope ds(g);
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
    // recepção (térreo oco): paredes de concreto, frente de vidro com porta, balcão, sofás e elevador
    {
        const float t = 0.25f, zf = d / 2 - t;
        glm::vec4 inner = srgb(235, 232, 225);
        wallIn(g, c, {-w / 2, 0, -d / 2}, {w, lobby, t}, {0, 0, 1}, inner);
        wallIn(g, c, {-w / 2, 0, -d / 2 + t}, {t, lobby, d - 2 * t}, {1, 0, 0}, inner);
        wallIn(g, c, {w / 2 - t, 0, -d / 2 + t}, {t, lobby, d - 2 * t}, {-1, 0, 0}, inner);
        wallIn(g, c, {-w / 2, 0, zf}, {w / 2 - 2.6f, lobby, t}, {0, 0, -1}, inner);
        wallIn(g, c, {2.6f, 0, zf}, {w / 2 - 2.6f, lobby, t}, {0, 0, -1}, inner);
        c.box({-2.6f, 2.9f, zf}, {5.2f, lobby - 2.9f, t});
        g.collider({-2.6f, 2.9f, zf}, {5.2f, lobby - 2.9f, t});
        MeshBuilder& glass = g[Mat::Glass];
        for (float x0 : {-2.6f, 0.8f}) { glass.box({x0, 0, zf + 0.1f}, {1.8f, 2.9f, 0.05f}); g.collider({x0, 0, zf + 0.1f}, {1.8f, 2.9f, 0.05f}); }
        g.door({-0.8f, 0, zf + 0.12f}, 1.6f, 2.9f, 1);
        g[Mat::Marble].uvTile = {2.f, 2.f};
        g[Mat::Marble].box({-w / 2 + t, 0, -d / 2 + t}, {w - 2 * t, 0.03f, d - 2 * t});  // piso de mármore
        {
            DetailScope ds(g);
            g[Mat::Concrete].box({-2.6f, 2.9f, d / 2}, {5.2f, 0.25f, 1.6f});  // marquise
        }
        lobbyFurniture(g, r, w, d, t);
        // elevador: do saguão ao terraço (atrás da casa de máquinas não: no canto livre)
        g.teleport({0.f, 0.05f, -d / 2 + t + 1.3f}, {-w / 2 + 2.f, lobby + fh * floors + 0.05f, d / 2 - 2.f}, "🛗 Subir para o terraço",
                   "🛗 Descer para a recepção");
    }
    // parte dos prédios é de tijolo vermelho aparente (escolha pelo tamanho: não mexe no sorteio da cidade)
    bool brick = (int(w * 10.f) + int(d * 7.f) + floors) % 3 == 0;
    MeshBuilder& f = g[brick ? Mat::FacadeBrick : Mat::FacadeApt];
    f.color = brick ? glm::vec4(1.f) : color; f.uvTile = {3.2f, 3.f};
    f.box({-w / 2, lobby, -d / 2}, {w, fh * floors, d});
    // platibanda e casa de máquinas / caixa d'água (detalhe)
    float tw = std::min(6.f, w * 0.35f), tx = r.range(-w * 0.2f, w * 0.2f);
    {
        DetailScope ds(g);
        MeshBuilder& cd = g[Mat::Concrete];
        cd.uvTile = {2.f, 2.f};
        cd.box({-w / 2 - 0.1f, H, -d / 2 - 0.1f}, {w + 0.2f, 0.9f, 0.2f});
        cd.box({-w / 2 - 0.1f, H, d / 2 - 0.1f}, {w + 0.2f, 0.9f, 0.2f});
        cd.box({-w / 2 - 0.1f, H, -d / 2 + 0.1f}, {0.2f, 0.9f, d - 0.2f});
        cd.box({w / 2 - 0.1f, H, -d / 2 + 0.1f}, {0.2f, 0.9f, d - 0.2f});
    }
    c.box({-w / 2, H, -d / 2}, {w, 0.04f, d});  // laje do terraço
    c.box({-tw / 2 + tx, H, -2.f}, {tw, 3.f, 4.f});
    g.collider({-tw / 2 + tx, H, -2.f}, {tw, 3.f, 4.f});
    g.collider({-w / 2, lobby, -d / 2}, {w, H - lobby, d});                          // andares (maciço acima da recepção)
    for (int k = 0; k < 4; ++k) {                                                      // mureta do terraço
        glm::vec3 mn = k == 0 ? glm::vec3(-w / 2, H, -d / 2) : k == 1 ? glm::vec3(-w / 2, H, d / 2 - 0.2f)
                     : k == 2 ? glm::vec3(-w / 2, H, -d / 2) : glm::vec3(w / 2 - 0.2f, H, -d / 2);
        glm::vec3 sz = k < 2 ? glm::vec3(w, 1.1f, 0.2f) : glm::vec3(0.2f, 1.1f, d);
        g.collider(mn, sz);
    }
}

void buildSkyscraper(Geo& g, Rng& r, float w, float d, int floors) {
    const float fh = 3.6f;
    MeshBuilder& glass = g[Mat::FacadeOffice];
    glass.uvTile = {1.6f, fh};
    MeshBuilder& c = g[Mat::Concrete];
    c.uvTile = {2.f, 2.f};
    {
        DetailScope ds(g);  // calçada em volta
        g[Mat::Concrete].uvTile = {2.f, 2.f};
        g[Mat::Concrete].box({-w / 2 - 1.f, 0, -d / 2 - 1.f}, {w + 2.f, 0.04f, d + 2.f});
    }
    // saguão de vidro (térreo oco, 4,5 m) com porta, recepção e elevadores
    const float lobbyH = 4.5f;
    {
        const float t = 0.3f, zf = d / 2 - t;
        glm::vec4 inner = srgb(225, 225, 228);
        MeshBuilder& gl = g[Mat::Glass];
        wallIn(g, c, {-w / 2, 0, -d / 2}, {w, lobbyH, t}, {0, 0, 1}, inner);
        wallIn(g, gl, {-w / 2, 0, -d / 2 + t}, {t, lobbyH, d - 2 * t}, {1, 0, 0}, inner);
        wallIn(g, gl, {w / 2 - t, 0, -d / 2 + t}, {t, lobbyH, d - 2 * t}, {-1, 0, 0}, inner);
        gl.box({-w / 2, 0, zf}, {w / 2 - 1.2f, lobbyH, t}); g.collider({-w / 2, 0, zf}, {w / 2 - 1.2f, lobbyH, t});
        gl.box({1.2f, 0, zf}, {w / 2 - 1.2f, lobbyH, t}); g.collider({1.2f, 0, zf}, {w / 2 - 1.2f, lobbyH, t});
        c.box({-1.2f, 3.f, zf}, {2.4f, lobbyH - 3.f, t}); g.collider({-1.2f, 3.f, zf}, {2.4f, lobbyH - 3.f, t});
        g.door({-1.2f, 0, zf + t * 0.5f}, 2.4f, 3.f, 1);
        for (float x : {-w / 2, w / 2 - 0.6f}) c.box({x, 0, zf - 0.3f}, {0.6f, lobbyH, 0.6f});  // pilares da frente
        g[Mat::Marble].uvTile = {2.f, 2.f};
        g[Mat::Marble].box({-w / 2 + t, 0, -d / 2 + t}, {w - 2 * t, 0.03f, d - 2 * t});
        lobbyFurniture(g, r, w, d, t);
    }
    int tiers = r.irange(1, 3);
    float y = lobbyH, tw = w, td = d;
    int remaining = std::max(2, floors - 1);
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
    // coroamento, mureta do terraço e elevador até o terraço
    c.box({-tw * 0.3f, y, -td * 0.3f}, {tw * 0.6f, 4.f, td * 0.6f});
    g.collider({-tw * 0.3f, y, -td * 0.3f}, {tw * 0.6f, 4.f, td * 0.6f});
    c.box({-tw / 2, y, -td / 2}, {tw, 1.1f, 0.2f}); c.box({-tw / 2, y, td / 2 - 0.2f}, {tw, 1.1f, 0.2f});
    c.box({-tw / 2, y, -td / 2}, {0.2f, 1.1f, td}); c.box({tw / 2 - 0.2f, y, -td / 2}, {0.2f, 1.1f, td});
    g.collider({-tw / 2, y, -td / 2}, {tw, 1.1f, 0.2f}); g.collider({-tw / 2, y, td / 2 - 0.2f}, {tw, 1.1f, 0.2f});
    g.collider({-tw / 2, y, -td / 2}, {0.2f, 1.1f, td}); g.collider({tw / 2 - 0.2f, y, -td / 2}, {0.2f, 1.1f, td});
    g.teleport({0.f, 0.05f, -d / 2 + 0.3f + 1.3f}, {-tw / 2 + 1.5f, y + 0.05f, -td / 2 + 1.5f}, "🛗 Subir para o terraço",
               "🛗 Descer para o saguão");
    if (r.chance(0.7f)) {
        float ah = r.range(8.f, 20.f);
        DetailScope ds(g);
        g[Mat::Metal].cylinder({0, y + 4.f, 0}, {0, y + 4.f + ah, 0}, 0.25f, 0.08f, 8);
    }
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
    {
        DetailScope ds(g);
        MeshBuilder& cd = g[Mat::Concrete];
        cd.uvTile = {2.f, 2.f};
        for (float x = -len / 2 + 0.2f; x < len / 2; x += 4.f) cd.box({x, 0, depth / 2 + 1.9f}, {0.25f, fh - 0.2f, 0.25f});
    }
    g.collider({-len / 2, 0, -depth / 2}, {len, H + 0.5f, depth});
}

void buildTree(MeshBuilder& p, Rng& r, int kind, bool low) {
    glm::vec4 trunk = srgb(100, 72, 48);
    const int ts = low ? 4 : 7;  // segmentos do tronco
    if (kind == 1) {  // pinheiro
        float h = r.range(6.f, 10.f);
        p.color = trunk;
        p.cylinder({0, 0, 0}, {0, h * 0.35f, 0}, 0.22f, 0.16f, ts, false);
        glm::vec4 leaf = glm::mix(srgb(40, 80, 45), srgb(60, 100, 55), r.f());
        for (int k = 0; k < 3; ++k) {
            float y0 = h * (0.25f + k * 0.22f), rad = h * (0.26f - k * 0.06f);
            p.color = leaf * (0.9f + 0.1f * k); p.color.a = 1.f;
            if (!low || k == 0) p.cylinder({0, y0, 0}, {0, y0 + h * (low ? 0.75f : 0.38f), 0}, rad, 0.f, low ? 5 : 7, false);
        }
    } else if (kind == 2) {  // palmeira
        float h = r.range(6.f, 9.f);
        p.color = srgb(130, 105, 75);
        p.cylinder({0, 0, 0}, {0.4f, h, 0}, 0.2f, 0.14f, ts, false);
        p.color = srgb(70, 120, 50);
        for (int k = 0; k < 7; ++k) {
            float a = k * 0.8975979f;
            glm::vec3 dir(std::cos(a), -0.35f, std::sin(a));
            if (!low || k % 2 == 0) p.cylinder({0.4f, h, 0}, glm::vec3(0.4f, h, 0) + dir * 2.6f, 0.35f, 0.02f, low ? 3 : 4, false);
        }
    } else {  // copa redonda (mangueira, ipê...)
        float h = r.range(3.f, 5.f);
        p.color = trunk;
        p.cylinder({0, 0, 0}, {0, h, 0}, 0.25f, 0.18f, ts, false);
        glm::vec4 leaf = r.chance(0.15f) ? srgb(230, 200, 60) : glm::mix(srgb(50, 95, 40), srgb(85, 125, 50), r.f());
        int blobs = r.irange(2, 4);
        for (int k = 0; k < blobs; ++k) {
            glm::vec3 c(r.range(-1.f, 1.f), h + r.range(0.f, 1.5f), r.range(-1.f, 1.f));
            float rad = r.range(1.3f, 2.f);
            p.color = leaf * r.range(0.85f, 1.1f); p.color.a = 1.f;
            if (low) p.sphere(c, glm::vec3(rad, rad * 0.85f, rad), 5, 3);
            else p.sphere(c, glm::vec3(rad, rad * 0.85f, rad), 7, 5);
        }
    }
    p.color = glm::vec4(1.f);
}

void buildCar(Geo& g, Rng& r) {
    DetailScope ds(g);
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
    DetailScope ds(g);
    MeshBuilder& m = g[Mat::Metal];
    m.cylinder({0, 0, 0}, {0, 7.f, 0}, 0.1f, 0.07f, 8, false);
    m.cylinder({0, 7.f, 0}, {0, 7.2f, 1.4f}, 0.06f, 0.05f, 6, false);
    MeshBuilder& p = g[Mat::Painted];
    p.color = srgb(255, 250, 220);
    p.box({-0.18f, 6.95f, 1.2f}, {0.36f, 0.15f, 0.5f});
    p.color = glm::vec4(1.f);
}

}  // namespace eng
