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

// ---------------- construção por módulos ----------------
namespace {
constexpr float kWallT = 0.15f, kSlabT = 0.15f;
constexpr float kDoorW = 1.f, kDoorH = 2.1f, kWinW = 1.2f, kWinH = 1.1f, kSill = 1.f;

// Parede reta de `len` metros ao longo de +X local (origem no canto esquerdo, espessura para -Z), com abertura no meio.
// xf: onde fica a parede dentro da construção (girada para cada lado).
void wallRun(Geo& g, const ModuleSpec& s, const glm::mat4& xf, float len, float y0, float hgt, int kind) {
    if (kind == MW_Open) return;
    MeshBuilder& m = g[s.wallMat];
    MeshBuilder& frame = g.interiorMats[int(Mat::Wood)];  // batentes: só desenhados de perto (camada de interior)
    glm::mat4 keep = m.xf, keepF = frame.xf, keepG = g.mats[0].xf;
    const float keepYaw = g.yaw;
    glm::mat4 base = m.xf * xf;
    m.xf = base; frame.xf = base; g.mats[0].xf = base;
    g.yaw += std::atan2(-xf[0][2], xf[0][0]);  // colliders giram junto com a parede
    m.color = s.wallMat == Mat::Plaster || s.wallMat == Mat::Concrete ? s.wallColor : glm::vec4(1.f);
    m.uvTile = s.wallMat == Mat::Brick ? glm::vec2(1.f, 1.f) : s.wallMat == Mat::Graffiti ? glm::vec2(4.f, 2.f) : glm::vec2(2.f, 2.f);
    frame.color = glm::vec4(1.f); frame.uvTile = {1.f, 1.f};
    auto piece = [&](float x, float y, float w, float h) {
        if (w <= 0.01f || h <= 0.01f) return;
        m.box({x, y0 + y, -kWallT}, {w, h, kWallT});
        g.collider({x, y0 + y, -kWallT}, {w, h, kWallT});
    };
    if (kind == MW_Plain || len < (kind == MW_Door ? kDoorW : kWinW) + 0.3f) {
        piece(0, 0, len, hgt);
    } else {
        float ow = kind == MW_Door ? kDoorW : kWinW, oy = kind == MW_Door ? 0.f : kSill, oh = kind == MW_Door ? kDoorH : kWinH;
        float x0 = (len - ow) * 0.5f;
        piece(0, 0, x0, hgt);                                 // esquerda da abertura
        piece(x0 + ow, 0, len - x0 - ow, hgt);                // direita
        piece(x0, oy + oh, ow, hgt - oy - oh);                // em cima (verga)
        if (oy > 0.f) piece(x0, 0, ow, oy);                   // embaixo (peitoril)
        // batente / moldura de madeira em volta da abertura
        const float f = 0.05f;
        frame.box({x0 - f, y0 + oy, -kWallT - 0.01f}, {f, oh, kWallT + 0.02f});
        frame.box({x0 + ow, y0 + oy, -kWallT - 0.01f}, {f, oh, kWallT + 0.02f});
        frame.box({x0 - f, y0 + oy + oh, -kWallT - 0.01f}, {ow + 2 * f, f, kWallT + 0.02f});
        if (oy > 0.f) frame.box({x0 - f, y0 + oy - f, -kWallT - 0.03f}, {ow + 2 * f, f, kWallT + 0.06f});
        if (kind == MW_Door) g.door({x0, y0, -kWallT * 0.5f}, ow, oh, 0);  // folha que abre sozinha
    }
    m.xf = keep; frame.xf = keepF; g.mats[0].xf = keepG;
    g.yaw = keepYaw;
}

// Placa horizontal (piso/laje) de [x0,x1]x[z0,z1], com um buraco opcional (escada)
void slabWithHole(Geo& g, Mat mat, float x0, float x1, float z0, float z1, float y, float t, bool hole, float hx0, float hx1, float hz0, float hz1) {
    MeshBuilder& m = g[mat];
    m.color = glm::vec4(1.f); m.uvTile = {2.f, 2.f};
    auto box = [&](float a0, float a1, float b0, float b1) {
        if (a1 - a0 < 0.01f || b1 - b0 < 0.01f) return;
        m.box({a0, y, b0}, {a1 - a0, t, b1 - b0});
        g.collider({a0, y, b0}, {a1 - a0, t, b1 - b0});
    };
    if (!hole) { box(x0, x1, z0, z1); return; }
    box(x0, x1, z0, hz0);      // antes do buraco
    box(x0, x1, hz1, z1);      // depois
    box(x0, hx0, hz0, hz1);    // dos lados
    box(hx1, x1, hz0, hz1);
}

// Escada reta subindo de (x, y0, zStart) até a altura `rise`, andando `run` metros para -Z
// baseY: de onde os degraus começam por baixo (lance de cima de uma escada em L/U fica maciço até o chão)
void stairsRun(Geo& g, Mat mat, float x0, float width, float y0, float zStart, float run, float rise, float baseY = 1e9f) {
    MeshBuilder& m = g[mat];
    m.color = glm::vec4(1.f); m.uvTile = {1.f, 1.f};
    if (baseY > y0) baseY = y0;
    int n = std::max(4, int(rise / 0.18f + 0.5f));
    for (int i = 0; i < n; ++i) {
        float z1 = zStart - run * i / n, z0 = zStart - run * (i + 1) / n;
        m.box({x0, baseY, z0}, {width, y0 - baseY + rise * (i + 1) / n, z1 - z0 + 0.001f});
    }
    float th = std::atan2(rise, run), len = std::sqrt(rise * rise + run * run);
    glm::vec3 nrm(0.f, std::cos(th), std::sin(th));
    // rampa invisível por cima dos degraus (o personagem sobe andando)
    glm::vec3 c(x0 + width * 0.5f, y0 + rise * 0.5f, zStart - run * 0.5f);
    g.colliderTilted(c - nrm * 0.12f, {width * 0.5f, 0.12f, len * 0.5f}, th);
}

// Placa com vários buracos retangulares (escada em L, caracol...): divide em células e junta as cheias por linha
void slabWithHoles(Geo& g, Mat mat, float x0, float x1, float z0, float z1, float y, float t, const std::vector<StairHole>& holes) {
    if (holes.empty()) { slabWithHole(g, mat, x0, x1, z0, z1, y, t, false, 0, 0, 0, 0); return; }
    MeshBuilder& m = g[mat];
    m.color = glm::vec4(1.f); m.uvTile = {2.f, 2.f};
    std::vector<float> xs{x0, x1}, zs{z0, z1};
    for (const StairHole& h : holes) {
        xs.push_back(std::clamp(h.x0, x0, x1)); xs.push_back(std::clamp(h.x1, x0, x1));
        zs.push_back(std::clamp(h.z0, z0, z1)); zs.push_back(std::clamp(h.z1, z0, z1));
    }
    for (auto* v : {&xs, &zs}) { std::sort(v->begin(), v->end()); v->erase(std::unique(v->begin(), v->end()), v->end()); }
    auto open = [&](float x, float z) {
        for (const StairHole& h : holes) if (x > h.x0 && x < h.x1 && z > h.z0 && z < h.z1) return true;
        return false;
    };
    for (size_t j = 0; j + 1 < zs.size(); ++j) {
        float za = zs[j], zb = zs[j + 1];
        if (zb - za < 0.01f) continue;
        size_t i = 0;
        while (i + 1 < xs.size()) {
            if (open((xs[i] + xs[i + 1]) * 0.5f, (za + zb) * 0.5f)) { ++i; continue; }
            size_t k = i;
            while (k + 1 < xs.size() && !open((xs[k] + xs[k + 1]) * 0.5f, (za + zb) * 0.5f)) ++k;
            if (xs[k] - xs[i] > 0.01f) {
                m.box({xs[i], y, za}, {xs[k] - xs[i], t, zb - za});
                g.collider({xs[i], y, za}, {xs[k] - xs[i], t, zb - za});
            }
            i = k;
        }
    }
}

// Desenha com o sistema local deslocado/girado (lances em outra direção, degraus do caracol)
template <class F> void inFrame(Geo& g, glm::vec3 at, float yawRad, F&& fn) {
    glm::mat4 keep[Geo::kLayers][int(Mat::Count)];
    for (int l = 0; l < Geo::kLayers; ++l) for (int k = 0; k < int(Mat::Count); ++k) keep[l][k] = g.layer(l)[k].xf;
    const float keepYaw = g.yaw;
    glm::mat4 local = glm::rotate(glm::translate(glm::mat4(1.f), at), yawRad, glm::vec3(0, 1, 0));
    for (int l = 0; l < Geo::kLayers; ++l) for (int k = 0; k < int(Mat::Count); ++k) g.layer(l)[k].xf = keep[l][k] * local;
    g.yaw += yawRad;
    fn();
    for (int l = 0; l < Geo::kLayers; ++l) for (int k = 0; k < int(Mat::Count); ++k) g.layer(l)[k].xf = keep[l][k];
    g.yaw = keepYaw;
}

void railing(Geo& g, float x0, float z0, float x1, float z1, float y, float h) {
    MeshBuilder& m = g[Mat::GunMetal];
    m.color = glm::vec4(1.f); m.uvTile = {0.f, 0.f};
    glm::vec2 a(x0, z0), b(x1, z1);
    float L = glm::length(b - a);
    int n = std::max(2, int(L / 0.9f) + 1);
    for (int i = 0; i < n; ++i) {
        glm::vec2 p = glm::mix(a, b, float(i) / (n - 1));
        m.box({p.x - 0.025f, y, p.y - 0.025f}, {0.05f, h, 0.05f});
    }
    glm::vec2 mn = glm::min(a, b), mx = glm::max(a, b);
    m.box({mn.x - 0.03f, y + h - 0.05f, mn.y - 0.03f}, {mx.x - mn.x + 0.06f, 0.05f, mx.y - mn.y + 0.06f});
    g.collider({mn.x - 0.05f, y, mn.y - 0.05f}, {mx.x - mn.x + 0.1f, h, mx.y - mn.y + 0.1f});
}

glm::mat4 wallXf(int side, float w, float d) {  // canto inicial e direção de cada parede (por fora, olhando para ela)
    auto rot = [](float a) { return glm::rotate(glm::mat4(1.f), a, glm::vec3(0, 1, 0)); };
    switch (side) {
        case 0: return glm::translate(glm::mat4(1.f), {-w / 2, 0, d / 2});                                   // frente (+Z)
        case 1: return glm::translate(glm::mat4(1.f), {w / 2, 0, -d / 2}) * rot(3.14159265f);                // fundo (-Z)
        case 2: return glm::translate(glm::mat4(1.f), {w / 2, 0, d / 2 - kWallT}) * rot(1.5707963f);         // esquerda (+X)
        default: return glm::translate(glm::mat4(1.f), {-w / 2, 0, -d / 2 + kWallT}) * rot(-1.5707963f);     // direita (-X)
    }
}
}  // namespace

// Escada encostada no canto esquerdo-fundo (xL = lado +X, zB = fundo -Z), subindo `rise` a partir de y0.
// holes: onde a laje de cima precisa abrir (passagem da cabeça e saída no topo)
static void cornerStairs(Geo& g, int kind, float xL, float zB, float roomW, float roomD, float y0, float rise, std::vector<StairHole>* holes) {
    const float sw = 0.9f;
    const int n = std::max(8, int(rise / 0.18f + 0.5f));
    auto landing = [&](float x0, float x1, float z0, float z1, float top) {
        MeshBuilder& m = g[Mat::Concrete];
        m.color = glm::vec4(1.f); m.uvTile = {1.f, 1.f};
        m.box({x0, y0, z0}, {x1 - x0, top - y0, z1 - z0});
        g.collider({x0, y0, z0}, {x1 - x0, top - y0, z1 - z0});
    };
    switch (kind) {
        case SK_Straight: default: {
            const float run = std::min(roomD - 0.6f, rise * 1.55f), zStart = zB + run + 0.05f;
            stairsRun(g, Mat::Concrete, xL - sw, sw, y0, zStart, run, rise);
            if (holes) holes->push_back({xL - sw - 0.05f, xL + 0.01f, zB - 0.01f, zStart - 0.7f});
            break;
        }
        case SK_L: {  // sobe pela parede esquerda, patamar no canto, segue pela parede do fundo
            const int n1 = n / 2;
            const float r1 = rise * n1 / n, r2 = rise - r1;
            const float tread = std::clamp(std::min((roomD - sw - 0.4f) / n1, (roomW - sw - 0.4f) / (n - n1)), 0.18f, 0.29f);
            const float run1 = tread * n1, run2 = tread * (n - n1);
            stairsRun(g, Mat::Concrete, xL - sw, sw, y0, zB + sw + run1, run1, r1);
            landing(xL - sw, xL, zB, zB + sw, y0 + r1);
            inFrame(g, glm::vec3(0.f), 1.5707963f, [&] { stairsRun(g, Mat::Concrete, -(zB + sw), sw, y0 + r1, xL - sw, run2, r2, y0); });
            if (holes) {
                holes->push_back({xL - sw - run2 - 0.01f, xL + 0.01f, zB - 0.01f, zB + sw + 0.05f});
                holes->push_back({xL - sw - 0.05f, xL + 0.01f, zB + sw, zB + sw + run1 * 0.6f});
            }
            break;
        }
        case SK_U: {  // dois lances paralelos com patamar no fundo; chega no topo virado para a frente
            const int n1 = n / 2;
            const float r1 = rise * n1 / n, r2 = rise - r1;
            const float tread = std::clamp((roomD - sw - 0.4f) / (n - n1), 0.18f, 0.29f);
            const float run1 = tread * n1, run2 = tread * (n - n1);
            stairsRun(g, Mat::Concrete, xL - sw, sw, y0, zB + sw + run1, run1, r1);
            landing(xL - 2 * sw, xL, zB, zB + sw, y0 + r1);
            inFrame(g, glm::vec3(0.f), 3.14159265f, [&] { stairsRun(g, Mat::Concrete, -(xL - sw), sw, y0 + r1, -(zB + sw), run2, r2, y0); });
            if (holes) holes->push_back({xL - 2 * sw - 0.05f, xL + 0.01f, zB - 0.01f, zB + sw + run2});
            break;
        }
        case SK_Spiral: {  // caracol: degraus em leque em volta de um poste, uma volta inteira
            const float R = std::clamp(std::min(roomW, roomD) * 0.5f - 0.3f, 0.7f, 1.f);
            const float cx = xL - R - 0.03f, cz = zB + R + 0.03f, r0 = 0.07f;
            const int ns = std::max(12, int(rise / 0.19f + 0.5f));
            const float dth = 6.2831853f / ns, rm = (R + r0) * 0.5f;
            const float stepRun = rm * dth, th = std::atan2(rise / ns, stepRun);
            const glm::vec3 nrm(0.f, std::cos(th), std::sin(th));
            {
                MeshBuilder& p = g[Mat::GunMetal];
                p.color = glm::vec4(1.f); p.uvTile = {1.f, 1.f};
                p.cylinder({cx, y0, cz}, {cx, y0 + rise + 1.f, cz}, r0, r0, 10);
                g.collider({cx - r0, y0, cz - r0}, {2 * r0, rise + 1.f, 2 * r0});
            }
            for (int i = 0; i < ns; ++i) {
                const float top = y0 + rise * (i + 1) / ns, hw = R * std::tan(dth * 0.5f) + 0.03f;
                inFrame(g, glm::vec3(cx, 0.f, cz), (i + 0.5f) * dth, [&] {
                    MeshBuilder& m = g[Mat::Wood];
                    m.color = glm::vec4(1.f); m.uvTile = {1.f, 1.f};
                    m.box({r0, top - 0.06f, -hw}, {R - r0, 0.06f, 2 * hw});
                    MeshBuilder& rail = g[Mat::GunMetal];
                    rail.box({R - 0.05f, top, -0.02f}, {0.04f, 0.9f, 0.04f});  // poste do corrimão
                    // rampa invisível pelo meio do degrau (o personagem sobe girando)
                    glm::vec3 c(rm, y0 + rise * (i + 0.5f) / ns, 0.f);
                    g.colliderTilted(c - nrm * 0.06f, {(R - r0) * 0.5f, 0.06f, stepRun * 0.6f}, th);
                });
            }
            // a laje fica só em cima do primeiro quarto de volta (degraus baixos): o último degrau sai nela
            const float e = R + 0.05f;
            if (holes) {
                holes->push_back({cx - e, cx, cz - e, cz + e});
                holes->push_back({cx, cx + e, cz, cz + e});
            }
            break;
        }
    }
}

float moduleTop(const ModuleSpec& s) { return (s.floor ? kSlabT : 0.f) + s.h + (s.slab ? kSlabT : 0.f); }

void buildModule(Geo& g, const ModuleSpec& s) {
    const float w = s.w, d = s.d, fy = s.floor ? kSlabT : 0.f, top = fy + s.h;
    // piso
    if (s.floor) slabWithHole(g, s.floorMat, -w / 2, w / 2, -d / 2, d / 2, 0.f, kSlabT, false, 0, 0, 0, 0);
    // paredes (frente e fundo inteiras; laterais entre elas)
    for (int side = 0; side < 4; ++side) {
        float len = side < 2 ? w : d - 2 * kWallT;
        int kind = s.walls[side];
        bool inside = s.stairs != SK_None && s.stairs != SK_Outside;
        // a escada interna fica encostada na parede esquerda (a em L também no fundo)
        if (inside && kind != MW_Open && (side == 2 || (side == 1 && s.stairs == SK_L))) kind = MW_Plain;
        wallRun(g, s, wallXf(side, w, d), len, fy, s.h, kind);
    }
    // escada interna no canto esquerdo-fundo (reta, em L, em U ou caracol); a laje abre em cima dela
    std::vector<StairHole> holes;
    if (s.stairs != SK_None && s.stairs != SK_Outside) {
        InteriorScope in(g);  // de fora não aparece: só desenhada de perto (a colisão vale sempre)
        cornerStairs(g, s.stairs, w / 2 - kWallT, -d / 2 + kWallT, w - 2 * kWallT, d - 2 * kWallT, fy, s.h + kSlabT, &holes);
    }
    // laje (com buraco da escada)
    if (s.slab) {
        slabWithHoles(g, Mat::Concrete, -w / 2, w / 2, -d / 2, d / 2, top, kSlabT, holes);
        if (s.parapet) {  // mureta do terraço
            MeshBuilder& m = g[s.wallMat];
            m.color = s.wallMat == Mat::Plaster || s.wallMat == Mat::Concrete ? s.wallColor : glm::vec4(1.f);
            m.uvTile = {1.f, 1.f};
            const float y = top + kSlabT, ph = 0.9f, t = 0.12f;
            for (auto [mn, sz] : {std::pair{glm::vec3(-w / 2, y, d / 2 - t), glm::vec3(w, ph, t)}, std::pair{glm::vec3(-w / 2, y, -d / 2), glm::vec3(w, ph, t)},
                                  std::pair{glm::vec3(-w / 2, y, -d / 2 + t), glm::vec3(t, ph, d - 2 * t)},
                                  std::pair{glm::vec3(w / 2 - t, y, -d / 2 + t), glm::vec3(t, ph, d - 2 * t)}}) {
                m.box(mn, sz);
                g.collider(mn, sz);
            }
        }
    }
    if (s.stairs == SK_Outside) buildModulePiece(g, MP_StairsOut, s, 0);
    if (s.porch) buildModulePiece(g, MP_Porch, s, 0);
}

void buildModulePiece(Geo& g, int piece, const ModuleSpec& s, int wallKind) {
    const float w = s.w, d = s.d, fy = s.floor ? kSlabT : 0.f, top = fy + s.h;
    switch (piece) {
        case MP_Floor: slabWithHole(g, s.floorMat, -w / 2, w / 2, -d / 2, d / 2, 0.f, kSlabT, false, 0, 0, 0, 0); break;
        case MP_Wall:  wallRun(g, s, glm::translate(glm::mat4(1.f), {-w / 2, 0, kWallT / 2}), w, 0.f, s.h, wallKind); break;
        case MP_Slab:  slabWithHole(g, Mat::Concrete, -w / 2, w / 2, -d / 2, d / 2, 0.f, kSlabT, false, 0, 0, 0, 0); break;
        case MP_Stairs:  // escada avulsa em L, U ou caracol, dentro de w x d, subindo a altura h
            cornerStairs(g, s.stairs, w / 2, -d / 2, w, d, 0.f, s.h, nullptr);
            break;
        case MP_StairsOut: {  // pela lateral direita (-X), de frente para o fundo, até o topo da laje; corrimão do lado de fora
            const float sw = 1.f, x0 = -w / 2 - sw, rise = top + (s.slab ? kSlabT : 0.f);
            stairsRun(g, Mat::Concrete, x0, sw, 0.f, d / 2, d, rise);
            // corrimão: postes ao longo da escada, altura acompanhando os degraus
            MeshBuilder& m = g[Mat::GunMetal];
            for (int i = 0; i <= 4; ++i) {
                float z = d / 2 - d * i / 4.f, y = rise * i / 4.f;
                m.box({x0 - 0.03f, y, z - 0.03f}, {0.05f, 0.95f, 0.05f});
            }
            break;
        }
        case MP_Porch: {  // varanda coberta (no chão) ou sacada com grade (andar de cima), 2 m à frente
            const float pd = s.porchD;
            if (s.floor) slabWithHole(g, s.floorMat, -w / 2, w / 2, d / 2, d / 2 + pd, 0.f, kSlabT, false, 0, 0, 0, 0);
            else slabWithHole(g, Mat::Concrete, -w / 2, w / 2, d / 2, d / 2 + pd, -kSlabT, kSlabT, false, 0, 0, 0, 0);
            MeshBuilder& p = g[Mat::Concrete];
            p.color = glm::vec4(1.f); p.uvTile = {1.f, 1.f};
            for (float x : {-w / 2, w / 2 - 0.2f}) {
                p.box({x, fy, d / 2 + pd - 0.2f}, {0.2f, s.h, 0.2f});
                g.collider({x, fy, d / 2 + pd - 0.2f}, {0.2f, s.h, 0.2f});
            }
            slabWithHole(g, Mat::Concrete, -w / 2, w / 2, d / 2, d / 2 + pd, top, kSlabT, false, 0, 0, 0, 0);  // cobertura
            if (!s.floor) railing(g, -w / 2 + 0.2f, d / 2 + pd - 0.1f, w / 2 - 0.2f, d / 2 + pd - 0.1f, fy, 1.f);
            break;
        }
        case MP_Roof: {  // telhado de duas águas do tamanho do módulo
            g.offset({-w / 2, 0, -d / 2});
            g[Mat::RoofTile].gableRoof(w, d, 0.f, std::min(w, d) * 0.28f, 0.4f, {1.f, 1.f});
            g.offset({w / 2, 0, d / 2});
            g.collider({-w / 2, 0, -d / 2}, {w, std::min(w, d) * 0.15f, d});
            break;
        }
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
    MeshBuilder& p = g[Mat::GunMetal];
    p.color = glm::vec4(1.f);
    p.box({-0.2f, 7.05f, 1.15f}, {0.4f, 0.12f, 0.55f});   // luminária
    MeshBuilder& l = g[Mat::Lamp];
    l.color = glm::vec4(1.f);
    l.box({-0.15f, 6.97f, 1.2f}, {0.3f, 0.08f, 0.45f});   // vidro que acende
    g.lamp({0.f, 6.8f, 1.42f});
}

}  // namespace eng
