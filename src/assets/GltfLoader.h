#pragma once
#include <cstdint>
#include <vector>
#include "render/Renderer.h"
#include "scene/Animator.h"
#include "scene/Scene.h"

namespace eng {

struct LoadedModel {
    // Preenchido no carregamento (fora do game loop; alocação dinâmica permitida aqui)
    std::vector<MeshId> meshes;
    std::vector<MaterialId> materials;
    std::vector<Skeleton> skeletons;
    std::vector<AnimationClip> clips;
    std::vector<Channel> channelStorage;
    std::vector<std::vector<float>> keyData;
    std::vector<Scene::Handle> roots;  // GameObjects instanciados na cena
};

// Carrega .gltf/.glb, cria meshes/materiais no Renderer e instancia a hierarquia de nós na Scene.
bool loadGltf(const char* path, Renderer& renderer, Scene& scene, LoadedModel& out);

}  // namespace eng
