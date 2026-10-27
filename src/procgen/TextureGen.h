#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace eng {

// Biblioteca de materiais procedurais (texturas geradas na CPU, sem arquivos)
enum class Mat : uint8_t {
    Plaster,       // reboco pintado (a cor vem da cor por vértice)
    FacadeHouse,   // parede com janela de casa (tile 3 x 3 m)
    FacadeApt,     // fachada de apartamento (tile 3.2 x 3 m)
    FacadeOffice,  // pele de vidro de arranha-céu (tile 1.6 x 3.6 m)
    Brick,         // tijolo baiano aparente (tile 1 x 1 m)
    BrickWindow,   // tijolo com janelinha de barraco (tile 2.5 x 2.7 m)
    RoofTile,      // telha cerâmica (tile 1 x 1 m)
    Concrete,      // concreto / laje (tile 2 x 2 m)
    Asphalt,       // asfalto liso (tile 4 x 4 m)
    Road,          // pista com faixa central (u: largura da pista, v: 6 m)
    Sidewalk,      // calçada de lajotas (tile 2 x 2 m)
    Grass,         // grama (tile 4 x 4 m)
    Wood,          // madeira (portas, cabos)
    Metal,         // metal escovado claro
    GunMetal,      // metal escuro de arma
    Polymer,       // plástico/polímero preto fosco
    Court,         // quadra poliesportiva (UV 0..1 na quadra inteira)
    Water,         // água
    Terrain,       // terreno (modo 1 no shader: grama/rocha/areia/neve)
    Painted,       // branco liso para cor por vértice (personagens, árvores, caixas d'água)
    Glass,         // vidro escuro (carros)
    Count
};

const char* matName(Mat m);

struct TexData {
    int w = 0, h = 0;
    std::vector<uint8_t> albedo, orm, normal;  // RGBA8; normal vazio = normal plana
};

// Gera (uma vez, com cache) as texturas de um material
const TexData& proceduralTexture(Mat m);

// Parâmetros PBR base de cada material (multiplicam o ORM)
struct MatParams { float roughness, metallic; int mode; bool doubleSided; };
MatParams matParams(Mat m);

}  // namespace eng
