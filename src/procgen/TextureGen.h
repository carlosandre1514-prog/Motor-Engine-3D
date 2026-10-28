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
    RedBrick,      // tijolo vermelho aparente (tile 2 x 2 m)
    FacadeBrick,   // fachada de tijolo vermelho com janela e sala iluminada (tile 3.2 x 3 m)
    Slate,         // telhado de ardósia (tile 2 x 2 m)
    Marble,        // piso de mármore (tile 2 x 2 m)
    Dirt,          // terra batida (tile 3 x 3 m)
    Stone,         // piso de pedras (tile 2 x 2 m)
    Ceramic,       // piso cerâmico em lajotas de 40 cm (tile 2 x 2 m)
    Graffiti,      // muro de reboco com grafites coloridos e pichação (tile 4 x 2 m)
    ChainLink,     // alambrado / tela de arame (alfa recortado, tile 1 x 1 m)
    Corrugated,    // telha ondulada de fibrocimento (ondas ao longo de u, tile 2 x 2 m)
    Mural,         // atlas de pichações/grafites com texto (2 x 4 murais de 4 x 2 m, fundo transparente)
    Lamp,          // lâmpada (emissiva: acende à noite)
    Cement,        // cimentado do chão (becos, quintais): concreto que não faz sombra (tile 2 x 2 m)
    Count
};

const char* matName(Mat m);

// Murais do atlas Mat::Mural: textos e coordenadas de textura de cada um
constexpr int kMuralCount = 8;   // pichações (0..7); depois vêm as placas das lojas
constexpr int kSignBase = 8, kSignCount = 4;  // 8 LOJA DE ARMAS, 9 FARDAS (polícia), 10 ARMAS, 11 ROUPAS (favela)
const char* muralName(int i);
glm::vec4 muralUV(int i);  // u0, v0, u1, v1 (v0 = topo)

struct TexData {
    int w = 0, h = 0;
    std::vector<uint8_t> albedo, orm, normal;  // RGBA8; normal vazio = normal plana
};

// Pasta com as texturas de material (shaders/materials): concreto, tijolo, interiores das janelas...
// Sem ela (ou faltando arquivo), o material é gerado só por código.
void setMaterialTextureDir(const char* dir);

// Gera (uma vez, com cache) as texturas de um material
const TexData& proceduralTexture(Mat m);
// Gera de uma vez (em paralelo) as que ainda não estão prontas nem no cache em disco
void pregenerateTextures();

// Parâmetros PBR base de cada material (multiplicam o ORM)
struct MatParams { float roughness, metallic; int mode; bool doubleSided; };
MatParams matParams(Mat m);

}  // namespace eng
