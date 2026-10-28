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
// Porta que abre sozinha (dobradiça na origem de xf, folha para +X, abre girando para -Z = para dentro)
struct DoorSpec { glm::mat4 xf; float w, h; int style; };   // style 0 madeira, 1 vidro
// Passagem (elevador): perto de `a` aparece o botão `la` que leva a `b`, e vice-versa
struct TeleSpec { glm::vec3 a, b; const char* la; const char* lb; };

// Geometria agrupada por material + colliders. Construções escrevem aqui; o motor transforma
// cada material não vazio em um GameObject (poucos draw calls) e os colliders em um corpo estático.
// LOD: peças marcadas como "detalhe" (carros, postes, caixas d'água, platibandas...) vão para uma camada
// separada que só é desenhada perto da câmera; a base (volumes, telhados, ruas) é desenhada sempre.
struct Geo {
    MeshBuilder mats[int(Mat::Count)];
    MeshBuilder detailMats[int(Mat::Count)];
    MeshBuilder interiorMats[int(Mat::Count)];  // por dentro (móveis, reboco interno): só desenhado bem perto
    MeshBuilder farMats[int(Mat::Count)];       // versão simples para longe (só desenhada a partir de onde o detalhe some)
    static constexpr int kLayers = 4;
    std::vector<BoxCollider> colliders;
    glm::vec3 origin{0.f};
    float yaw = 0.f;
    bool detail = false;  // true: as próximas peças vão para a camada de detalhe
    bool interior = false;  // true: camada de interior
    bool far = false;       // true: camada "de longe"
    MeshBuilder* layer(int l) { return l == 0 ? mats : l == 1 ? detailMats : l == 2 ? interiorMats : farMats; }
    const MeshBuilder* layer(int l) const { return l == 0 ? mats : l == 1 ? detailMats : l == 2 ? interiorMats : farMats; }

    // Mapa editável: cada place() começa uma "peça" (uma construção, um poste, um carro...) que vira objeto próprio
    std::vector<DoorSpec> doors;
    std::vector<TeleSpec> teles;
    std::vector<glm::vec3> lamps;  // luzes de poste (mundo): acendem à noite
    void lamp(glm::vec3 local) { lamps.push_back(glm::vec3(mats[0].xf * glm::vec4(local, 1.f))); }
    struct Mark {
        uint32_t v[4][int(Mat::Count)], i[4][int(Mat::Count)];
        size_t col, door = 0, tele = 0;
        glm::vec3 origin;
        float yaw;
    };
    bool recordMarks = false;
    std::vector<Mark> marks;
    Mark mark() const {
        Mark m{};
        for (int l = 0; l < kLayers; ++l)
            for (int k = 0; k < int(Mat::Count); ++k) { m.v[l][k] = uint32_t(layer(l)[k].v.size()); m.i[l][k] = uint32_t(layer(l)[k].idx.size()); }
        m.col = colliders.size();
        m.door = doors.size(); m.tele = teles.size();
        m.origin = origin; m.yaw = yaw;
        return m;
    }

    MeshBuilder& operator[](Mat m) { return interior ? interiorMats[int(m)] : far ? farMats[int(m)] : detail ? detailMats[int(m)] : mats[int(m)]; }
    // Posiciona as próximas peças (origem da construção no chão + rotação em Y)
    void place(glm::vec3 pos, float yawRad) {
        origin = pos; yaw = yawRad;
        if (recordMarks) marks.push_back(mark());
        glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1.f), pos), yawRad, glm::vec3(0, 1, 0));
        for (auto* arr : {mats, detailMats, interiorMats, farMats})
            for (int k = 0; k < int(Mat::Count); ++k) { arr[k].xf = m; arr[k].color = glm::vec4(1.f); arr[k].uvTile = glm::vec2(0.f); }
    }
    void offset(glm::vec3 local) {  // desloca o sistema local atual
        for (auto* arr : {mats, detailMats, interiorMats, farMats})
            for (int k = 0; k < int(Mat::Count); ++k) arr[k].xf = arr[k].xf * glm::translate(glm::mat4(1.f), local);
    }
    // Collider em coordenadas locais da construção atual (min + size, como MeshBuilder::box)
    void collider(glm::vec3 mn, glm::vec3 size) {
        glm::vec3 c = mn + size * 0.5f;
        glm::vec3 w = glm::vec3(mats[0].xf * glm::vec4(c, 1.f));
        colliders.push_back({w, size * 0.5f, yaw});
    }
    void door(glm::vec3 hingeLocal, float w, float h, int style = 0) {
        doors.push_back({mats[0].xf * glm::translate(glm::mat4(1.f), hingeLocal), w, h, style});
    }
    void teleport(glm::vec3 aLocal, glm::vec3 bLocal, const char* la, const char* lb) {
        teles.push_back({glm::vec3(mats[0].xf * glm::vec4(aLocal, 1.f)), glm::vec3(mats[0].xf * glm::vec4(bLocal, 1.f)), la, lb});
    }
    // Collider inclinado (rampa): caixa pelo centro local, girada em X (subindo para +Z com pitch < 0)
    void colliderTilted(glm::vec3 centerLocal, glm::vec3 half, float pitch) {
        glm::vec3 w = glm::vec3(mats[0].xf * glm::vec4(centerLocal, 1.f));
        colliders.push_back({w, half, yaw, pitch});
    }
    bool empty() const {
        for (auto& b : mats) if (!b.empty()) return false;
        for (auto& b : detailMats) if (!b.empty()) return false;
        for (auto& b : interiorMats) if (!b.empty()) return false;
        for (auto& b : farMats) if (!b.empty()) return false;
        return true;
    }
};

// Liga a camada de detalhe enquanto existir (RAII)
struct DetailScope {
    Geo& g; bool prev;
    explicit DetailScope(Geo& geo) : g(geo), prev(geo.detail) { g.detail = true; }
    ~DetailScope() { g.detail = prev; }
};
struct FarScope {
    Geo& g; bool prev;
    explicit FarScope(Geo& geo) : g(geo), prev(geo.far) { g.far = true; }
    ~FarScope() { g.far = prev; }
};
struct InteriorScope {
    Geo& g; bool prev;
    explicit InteriorScope(Geo& geo) : g(geo), prev(geo.interior) { g.interior = true; }
    ~InteriorScope() { g.interior = prev; }
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

// Construção montada pelo usuário, andar por andar. Origem: centro da base, frente para +Z.
enum ModuleWall { MW_Plain, MW_Door, MW_Window, MW_Open };
enum ModulePiece { MP_Floor, MP_Wall, MP_Slab, MP_Porch, MP_StairsOut, MP_Roof, MP_Stairs };
// Escadas: reta (interna), externa (lateral, reta), em L, em U e caracol
enum StairKind { SK_None, SK_Straight, SK_Outside, SK_L, SK_U, SK_Spiral };
struct StairHole { float x0, x1, z0, z1; };  // abertura na laje (coordenadas locais)
struct ModuleSpec {
    float w = 4.f, d = 4.f, h = 2.8f;                        // largura (X), profundidade (Z), pé-direito
    int walls[4] = {MW_Door, MW_Plain, MW_Window, MW_Window};  // frente (+Z), fundo (-Z), esquerda (+X), direita (-X)
    Mat wallMat = Mat::Brick;
    glm::vec4 wallColor{1.f};
    Mat floorMat = Mat::Ceramic;
    bool floor = true;      // piso próprio (andar de cima: não, a laje de baixo é o piso)
    bool slab = true;       // laje em cima (teto / piso do próximo andar)
    bool parapet = false;   // mureta em volta da laje (terraço)
    int stairs = SK_None;   // StairKind; as internas ficam no canto esquerdo-fundo e abrem a laje
    bool porch = false;     // varanda coberta na frente (no chão) ou sacada (andar de cima)
    float porchD = 2.f;     // profundidade da varanda/sacada
};
// Altura do topo do módulo (onde o próximo andar começa)
float moduleTop(const ModuleSpec& s);
void buildModule(Geo& g, const ModuleSpec& s);
// Peças avulsas do mesmo jeito (parede de largura w, piso, laje, varanda, escada externa, telhado)
void buildModulePiece(Geo& g, int piece, const ModuleSpec& s, int wallKind);

// Objetos de cenário
void buildTree(MeshBuilder& painted, Rng& r, int kind, bool lowPoly = false);   // 0 copa redonda, 1 pinheiro, 2 palmeira
void buildCar(Geo& g, Rng& r);
void buildStreetLight(Geo& g);
void buildWaterTank(MeshBuilder& painted, glm::vec3 base);

// Arma (ver WeaponKind em Character.h): origem na empunhadura, cano +Z. Retorna a ponta do cano.
glm::vec3 buildWeapon(Geo& g, int kind, bool stock = true);  // stock = false: sem coronha (vai a tática)

}  // namespace eng
