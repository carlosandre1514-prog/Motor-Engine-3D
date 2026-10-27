#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "render/Renderer.h"
#include "scene/Animator.h"
#include "scene/Scene.h"

namespace eng {

struct LoadedModel {
    // Preenchido no carregamento (fora do game loop; alocação dinâmica permitida aqui).
    // Os Animators apontam para skeletons/clips/channels: o LoadedModel não pode ser movido
    // depois de carregado (guarde por unique_ptr).
    std::vector<MeshId> meshes;
    std::vector<MaterialId> materials;
    std::vector<Skeleton> skeletons;
    std::vector<AnimationClip> clips;
    std::vector<Channel> channelStorage;
    std::vector<std::vector<float>> keyData;
    std::vector<Scene::Handle> roots;      // GameObjects raiz instanciados na cena
    std::vector<Scene::Handle> allNodes;   // todos os GameObjects criados (na ordem de criação)
    std::vector<Animator*> animators;      // um por nó com skin
    std::string error;
};

struct LoadContext {
    Renderer& renderer;
    Scene& scene;
    MeshRendererPool& meshRenderers;
    AnimatorPool& animators;
};

// Carrega .gltf/.glb, cria meshes/materiais no Renderer e instancia a hierarquia de nós na Scene.
bool loadGltf(const char* path, LoadContext& ctx, LoadedModel& out);
// Carrega .fbx (Mixamo, Unity, Blender) com ufbx: mesmas estruturas do glTF (src/assets/FbxLoader.cpp)
bool loadFbx(const char* path, LoadContext& ctx, LoadedModel& out);

}  // namespace eng
