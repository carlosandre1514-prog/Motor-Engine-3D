#pragma once
#include <glm/glm.hpp>
#include "scene/Animator.h"

namespace eng {

// Reconhece o osso humanoide pelo nome (Mixamo "mixamorig:LeftArm", Unity "UpperArm.L", Blender "upper_arm.L",
// 3ds Max "Bip001 L UpperArm"...). Retorna o CharJoint (procgen/Character.h) ou -1.
int humanoidBone(const char* name);

// Monta o retarget do esqueleto `target` (de um modelo importado) para tocar as animações feitas no
// esqueleto de referência `src`. spaceWorld = matriz de mundo do nó acima da raiz do esqueleto.
// Retorna false se não achou os ossos principais (quadril, braços, pernas).
bool buildRetarget(const Skeleton& target, const Skeleton& src, const glm::mat4& spaceWorld, Retarget& out,
                   char* missing = nullptr, int missingSize = 0);

}  // namespace eng
