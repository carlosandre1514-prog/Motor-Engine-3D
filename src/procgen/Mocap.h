#pragma once
#include <deque>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace eng {

// Animação de captura de movimento (Mixamo e afins) já convertida para o esqueleto do motor (CJ_Total ossos,
// pose de bind sem rotação). As rotações valem para qualquer altura de personagem; o quadril é guardado em
// "alturas do quadril" e escalado na hora de montar o personagem.
struct MocapClip {
    std::string name;             // nome na lista de animações (ex.: "Andando (fuzil)")
    std::string slot;             // papel no motor ("parado", "fuzil_andando"...) ou vazio (só aparece na lista)
    float fps = 30.f;
    int frames = 0;
    bool loop = true;
    std::vector<float> times;     // frames
    std::vector<float> rot;       // [osso][quadro] x,y,z,w — rotação local (canal do Animator aponta direto aqui)
    std::vector<glm::vec3> hips;  // [quadro] deslocamento do quadril / altura do quadril
    float duration() const { return frames > 1 ? (frames - 1) / fps : 0.f; }
};

// Como tratar o deslocamento do quadril ao converter
enum MocapRoot {
    MR_Free,     // como veio (morte, cair): o corpo sai do lugar
    MR_InPlace,  // tira o caminho andado em X/Z, mantém o balanço (andar, correr, recarregar andando)
    MR_PoseOnly  // tira também a subida (pulo: quem sobe é a física, a animação só dobra as pernas)
};
// Converte a animação (a tomada com mais curvas) de um FBX com ossos no padrão Mixamo ("mixamorig:LeftArm"...)
// para o esqueleto do motor.
// armsDownDeg: abaixa os dois braços (girando nos ombros) — ex.: animação de mirar vira "pronto baixo"
bool bakeMocap(const char* fbxPath, int rootMode, MocapClip& out, std::string& err, float armsDownDeg = 0.f);

// Arquivo .anim (binário pequeno) com o resultado da conversão
bool saveMocap(const char* path, const MocapClip& c);
bool loadMocap(const char* path, MocapClip& c);

// Biblioteca usada ao montar personagens: animações com papel trocam as feitas por código, as outras entram
// no fim da lista. deque: os personagens apontam para os dados (não podem mudar de lugar).
std::deque<MocapClip>& mocapLibrary();
// Lê <dir>/biblioteca.txt (linhas "arquivo.anim;papel;Nome;loop") e os .anim. Só na primeira chamada.
void loadMocapLibrary(const std::string& dir);
const MocapClip* mocapForSlot(const char* slot);

}  // namespace eng
