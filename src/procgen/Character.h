#pragma once
#include <glm/glm.hpp>
#include "assets/GltfLoader.h"
#include "procgen/MeshBuilder.h"

namespace eng {

// Ossos do humanoide procedural (pais sempre antes dos filhos)
enum CharJoint { CJ_Hips, CJ_Spine, CJ_Chest, CJ_Head, CJ_LUpper, CJ_LFore, CJ_LHand, CJ_RUpper, CJ_RFore, CJ_RHand,
                 CJ_LThigh, CJ_LShin, CJ_LFoot, CJ_RThigh, CJ_RShin, CJ_RFoot, CJ_Count };

// Ossos extras (depois dos 16 humanoides principais): pescoço e dedos (2 articulações cada)
enum CharJointExtra { CJ_Neck = CJ_Count,
                      CJ_LThumb1, CJ_LThumb2, CJ_LIndex1, CJ_LIndex2, CJ_LMiddle1, CJ_LMiddle2, CJ_LRing1, CJ_LRing2, CJ_LPinky1, CJ_LPinky2,
                      CJ_RThumb1, CJ_RThumb2, CJ_RIndex1, CJ_RIndex2, CJ_RMiddle1, CJ_RMiddle2, CJ_RRing1, CJ_RRing2, CJ_RPinky1, CJ_RPinky2,
                      CJ_Total };

// Animações geradas para todo personagem. Com arma: 5 por tipo (parado, andando, correndo, mirando/atacando,
// andando mirando), na ordem de AP_*; a base de cada tipo é CC_Rifle, CC_Pistol ou CC_Melee.
// Depois delas: morte, recarregar, mirando com fuzil andar de costas e de lado (esquerda/direita), atirar e pular.
enum CharClip { CC_Idle, CC_Walk, CC_Run, CC_Dance, CC_Wave,
                CC_Rifle, CC_Pistol = CC_Rifle + 5, CC_Melee = CC_Pistol + 5,
                CC_Death = CC_Melee + 5, CC_Reload, CC_RifleBack, CC_RifleLeft, CC_RifleRight, CC_RifleFire, CC_Jump, CC_CrouchWalk, CC_Count };
// Papel de cada animação (para a biblioteca de captura de movimento: shaders/anim/biblioteca.txt)
const char* clipSlot(int charClip);
enum ArmedPose { AP_Idle, AP_Walk, AP_Run, AP_Aim, AP_AimWalk };
// tipo de pose da arma (WeaponKind abaixo): fuzil/escopeta/sniper, pistola, faca/taco
int armedClipBase(int weaponKind);

struct CharacterParams {
    float height = 1.75f;     // metros
    int build = 1;            // 0 magro, 1 normal, 2 forte
    glm::vec4 skin, shirt, pants, shoes, hair;
    int hairStyle = 1;        // 0 careca, 1 curto, 2 comprido, 3 boné
    bool longSleeves = false;
};

CharacterParams defaultCharacter();
CharacterParams randomCharacter(uint32_t seed);

// Gera esqueleto + animações em `out` (skeletons[0], clips) e a malha com skinning em `mesh`
void buildCharacter(const CharacterParams& p, LoadedModel& out, MeshBuilder& mesh);

// Armas: origem no ponto de empunhadura, cano para +Z, topo para +Y
enum WeaponKind { WK_Pistol, WK_Rifle, WK_Shotgun, WK_Sniper, WK_Knife, WK_Bat, WK_Count };
const char* weaponName(int kind);

// Encaixe da arma na mão direita (espaço do osso da mão): empunhadura descendo do pulso, cano seguindo os dedos
glm::mat4 weaponGripOffset(int kind, float height);

}  // namespace eng
