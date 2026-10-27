#pragma once
#include <glm/glm.hpp>
#include "assets/GltfLoader.h"
#include "procgen/MeshBuilder.h"

namespace eng {

// Ossos do humanoide procedural (pais sempre antes dos filhos)
enum CharJoint { CJ_Hips, CJ_Spine, CJ_Chest, CJ_Head, CJ_LUpper, CJ_LFore, CJ_LHand, CJ_RUpper, CJ_RFore, CJ_RHand,
                 CJ_LThigh, CJ_LShin, CJ_LFoot, CJ_RThigh, CJ_RShin, CJ_RFoot, CJ_Count };

// Animações geradas para todo personagem
enum CharClip { CC_Idle, CC_Walk, CC_Run, CC_IdleArmed, CC_WalkArmed, CC_RunArmed, CC_Dance, CC_Wave, CC_Count };

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

}  // namespace eng
