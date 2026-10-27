#pragma once
#include <cmath>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "procgen/MeshBuilder.h"
#include "procgen/Noise.h"
#include "procgen/TextureGen.h"

namespace eng {

// Cor sRGB (como no editor de imagem) -> linear (cor por vértice)
inline glm::vec4 srgb(float r, float g, float b) {
    return glm::vec4(std::pow(r / 255.f, 2.2f), std::pow(g / 255.f, 2.2f), std::pow(b / 255.f, 2.2f), 1.f);
}

struct BoxCollider { glm::vec3 center, half; float yaw; float pitch = 0.f; };  // pitch: inclinação em X (rampa/escada)

// Geometria agrupada por material + colliders. Construções escrevem aqui; o motor transforma
// cada material não vazio em um GameObject (poucos draw calls) e os colliders em um corpo estático.
// LOD: peças marcadas como "detalhe" (carros, postes, caixas d'água, platibandas...) vão para uma camada
// separada que só é desenhada perto da câmera; a base (volumes, telhados, ruas) é desenhada sempre.
struct Geo {
    MeshBuilder mats[int(Mat::Count)];
    MeshBuilder detailMats[int(Mat::Count)];
    std::vector<BoxCollider> colliders;
    glm::vec3 origin{0.f};
    float yaw = 0.f;
    bool detail = false;  // true: as próximas peças vão para a camada de detalhe

    // Mapa editável: cada place() começa uma "peça" (uma construção, um poste, um carro...) que vira objeto próprio
    struct Mark {
        uint32_t v[2][int(Mat::Count)], i[2][int(Mat::Count)];
        size_t col;
        glm::vec3 origin;
        float yaw;
    };
    bool recordMarks = false;
    std::vector<Mark> marks;
    Mark mark() const {
        Mark m{};
        for (int k = 0; k < int(Mat::Count); ++k) {
            m.v[0][k] = uint32_t(mats[k].v.size()); m.i[0][k] = uint32_t(mats[k].idx.size());
            m.v[1][k] = uint32_t(detailMats[k].v.size()); m.i[1][k] = uint32_t(detailMats[k].idx.size());
        }
        m.col = colliders.size();
        m.origin = origin; m.yaw = yaw;
        return m;
    }

    MeshBuilder& operator[](Mat m) { return detail ? detailMats[int(m)] : mats[int(m)]; }
    // Posiciona as próximas peças (origem da construção no chão + rotação em Y)
    void place(glm::vec3 pos, float yawRad) {
        origin = pos; yaw = yawRad;
        if (recordMarks) marks.push_back(mark());
        glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1.f), pos), yawRad, glm::vec3(0, 1, 0));
        for (auto* arr : {mats, detailMats})
            for (int k = 0; k < int(Mat::Count); ++k) { arr[k].xf = m; arr[k].color = glm::vec4(1.f); arr[k].uvTile = glm::vec2(0.f); }
    }
    void offset(glm::vec3 local) {  // desloca o sistema local atual
        for (auto* arr : {mats, detailMats})
            for (int k = 0; k < int(Mat::Count); ++k) arr[k].xf = arr[k].xf * glm::translate(glm::mat4(1.f), local);
    }
    // Collider em coordenadas locais da construção atual (min + size, como MeshBuilder::box)
    void collider(glm::vec3 mn, glm::vec3 size) {
        glm::vec3 c = mn + size * 0.5f;
        glm::vec3 w = glm::vec3(mats[0].xf * glm::vec4(c, 1.f));
        colliders.push_back({w, size * 0.5f, yaw});
    }
    // Collider inclinado (rampa): caixa pelo centro local, girada em X (subindo para +Z com pitch < 0)
    void colliderTilted(glm::vec3 centerLocal, glm::vec3 half, float pitch) {
        glm::vec3 w = glm::vec3(mats[0].xf * glm::vec4(centerLocal, 1.f));
        colliders.push_back({w, half, yaw, pitch});
    }
    bool empty() const {
        for (auto& b : mats) if (!b.empty()) return false;
        for (auto& b : detailMats) if (!b.empty()) return false;
        return true;
    }
};

// Liga a camada de detalhe enquanto existir (RAII)
struct DetailScope {
    Geo& g; bool prev;
    explicit DetailScope(Geo& geo) : g(geo), prev(geo.detail) { g.detail = true; }
    ~DetailScope() { g.detail = prev; }
};

// Paletas de cores típicas
glm::vec4 housePaint(Rng& r);    // tons de casa (pastéis, brancos, amarelos)
glm::vec4 favelaPaint(Rng& r);   // cores fortes de favela
glm::vec4 buildingPaint(Rng& r); // prédios (bege, cinza, branco, terracota)

// Construções. Origem: centro da base, frente voltada para +Z.
void buildHouse(Geo& g, Rng& r, float w, float d, int floors, glm::vec4 color, bool frontWall);
void buildBarraco(Geo& g, Rng& r, float w, float d, int floors, bool exposedBrick, glm::vec4 color, float foundation);
void buildApartment(Geo& g, Rng& r, float w, float d, int floors, glm::vec4 color);
void buildSkyscraper(Geo& g, Rng& r, float w, float d, int floors);
// Prédio escolar (2 andares) com faixa colorida; comprimento ao longo de X
void buildSchoolWing(Geo& g, float len, float depth, int floors, glm::vec4 wall, glm::vec4 band);

// Objetos de cenário
void buildTree(MeshBuilder& painted, Rng& r, int kind, bool lowPoly = false);   // 0 copa redonda, 1 pinheiro, 2 palmeira
void buildCar(Geo& g, Rng& r);
void buildStreetLight(Geo& g);
void buildWaterTank(MeshBuilder& painted, glm::vec3 base);

// Arma (ver WeaponKind em Character.h): origem na empunhadura, cano +Z. Retorna a ponta do cano.
glm::vec3 buildWeapon(Geo& g, int kind);

}  // namespace eng
