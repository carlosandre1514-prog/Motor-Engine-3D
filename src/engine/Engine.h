#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "assets/GltfLoader.h"
#include "editor/EditorCamera.h"
#include "editor/Gizmo.h"
#include "physics/Physics.h"
#include "procgen/Buildings.h"
#include "procgen/Character.h"
#include "procgen/TextureGen.h"
#include "render/Renderer.h"
#include "scene/Animator.h"
#include "scene/Scene.h"

namespace eng {

// Colisão de cenário gerado (mapas): corpo estático com forma composta, sem GameObject
struct StaticCollision {
    btCollisionShape* shape = nullptr;
    std::vector<btCollisionShape*> children;
    btRigidBody* body = nullptr;
};

// Objeto que segue um osso (arma na mão)
struct Attachment {
    Scene::Handle obj;
    Animator* anim = nullptr;
    int joint = 0;
    glm::mat4 offset{1.f};
};

struct CharacterInfo {
    Scene::Handle root;        // GameObject raiz (origem nos pés)
    Animator* anim = nullptr;
    float height = 1.75f;
    Scene::Handle weapon;      // arma equipada (opcional)
    int weaponKind = -1;
    glm::vec3 muzzle{0.f};     // ponta do cano no espaço da arma
};

// Modelo reaproveitado várias vezes (árvores, casas do mundo infinito): instancing automático
struct Prototype {
    std::vector<std::pair<MeshId, MaterialId>> parts;
    glm::vec3 colliderHalf{0.f};
    float colliderY = 0.f;     // centro do collider acima da base
};

struct TerrainChunk {
    int cx = 0, cz = 0;
    bool used = false;
    MeshId mesh = kInvalidId;
    Scene::Handle ground;
    std::vector<float> heights;  // (res+1)^2, para o heightfield do Bullet (precisa continuar vivo)
    btCollisionShape* hfShape = nullptr;
    btRigidBody* hfBody = nullptr;
    StaticCollision props;
    std::vector<Scene::Handle> objects;
};

enum MapKind { MAP_None = -1, MAP_Infinite = 0, MAP_City, MAP_Favela, MAP_School, MAP_Suburb, MAP_Count };

struct Tracer { glm::vec3 a, b; float life = 0.f; };

struct Engine {
    Renderer renderer;
    Scene scene;
    Physics physics;
    Camera camera;
    EditorCamera editorCam;
    Gizmo gizmo;
    DirectionalLight sun;
    Environment env;
    PointLight points[kMaxPointLights];
    int pointCount = 0;

    // Componentes (pools de capacidade fixa)
    MeshRendererPool meshRenderers;
    AnimatorPool animatorPool;
    RigidBodyPool bodies;

    std::vector<std::unique_ptr<LoadedModel>> models;  // glTF e personagens procedurais
    std::vector<Scene::Handle> objects;                // id público -> handle
    std::vector<Animator*> animators;
    std::vector<CharacterInfo> characters;
    std::vector<Attachment> attachments;

    // Malhas e materiais básicos (recriados a cada cena nova)
    MeshId cube = kInvalidId, sphere = kInvalidId, capsule = kInvalidId, rock = kInvalidId;
    std::vector<glm::vec3> rockHull;
    MaterialId matFloor = kInvalidId;
    MaterialId matPalette[6] = {};
    MaterialId mats[int(Mat::Count)] = {};
    std::map<uint64_t, MaterialId> tintCache;

    // Mapa atual
    int mapKind = MAP_None;
    uint32_t mapSeed = 1;
    bool hasBounds = false;
    glm::vec2 boundsMin{0.f}, boundsMax{0.f};
    std::vector<StaticCollision> statics;
    std::vector<TerrainChunk> chunks;
    std::vector<MeshId> freeChunkMeshes;
    Prototype treeProto[3], rockProto, houseProto[3];
    Scene::Handle water;

    // Jogador
    int player = -1;                 // índice em characters
    glm::vec2 moveInput{0.f};
    bool runInput = false, jumpRequest = false, fireRequest = false;
    float fireCooldown = 0.f;
    Tracer tracers[16];
    struct CamState { glm::vec3 target; float yaw, pitch, distance; } savedCam{};

    bool playing = false;
    bool showGrid = true;
    int width = 1, height = 1;
    Scene::Handle selected;
    std::string lastError;

    // Snapshot para o Stop restaurar o estado do editor
    struct Snap { Scene::Handle h; glm::vec3 p, s; glm::quat r; };
    std::vector<Snap> snapshot;
    struct AnimSnap { float time; int clip; };
    std::vector<AnimSnap> animSnap;

    // Toque
    bool tapCandidate = false, gizmoGrabbed = false;
    glm::vec2 tapStart{0.f};

    // Estatísticas
    float statTime = 0.f, fps = 0.f, frameMs = 0.f, cpuMs = 0.f, cpuAccum = 0.f;
    int statFrames = 0;
};

// ---- cena (Engine.cpp)
GameObject* objectById(Engine* e, int id);
int registerObject(Engine* e, Scene::Handle h);
glm::mat4 renderMatrix(const Engine* e, const GameObject& go, const glm::mat4** palette);
bool subtreeBounds(Engine* e, GameObject& root, AABB& out);
void focusSceneOrSelection(Engine* e);
void releaseComponents(Engine* e, GameObject& g);
void ensureBasics(Engine* e);
void clearScene(Engine* e);
void addFloor(Engine* e, float size = 40.f);
void selectHandle(Engine* e, Scene::Handle h);
Scene::Handle addMeshObject(Engine* e, const char* name, MeshId mesh, MaterialId mat, glm::vec3 pos, glm::vec3 scale,
                            glm::vec3 eulerDeg = glm::vec3(0.f), bool reg = true);
void addBody(Engine* e, Scene::Handle h, ColliderType type, float mass);
// Transforma Geo em GameObjects. local=true: Geo montado na origem; vira objeto editável (raiz registrada,
// collider composto que acompanha o objeto). local=false: cenário de mapa em coordenadas de mundo.
Scene::Handle spawnGeo(Engine* e, Geo& g, const char* name, bool local, glm::vec3 pos = glm::vec3(0.f), float yaw = 0.f);
void addStaticBoxes(Engine* e, const std::vector<BoxCollider>& boxes, StaticCollision* into = nullptr);
void freeStatic(Engine* e, StaticCollision& s);
MaterialId tintedMaterial(Engine* e, MaterialId base, glm::vec3 color);

// ---- personagens e armas (Engine.cpp)
int createCharacter(Engine* e, const CharacterParams& p, glm::vec3 pos, float yaw, bool controllable);
int createWeapon(Engine* e, int kind, int character);   // character < 0: solto no chão
int characterOfSelection(Engine* e);
void setPlayer(Engine* e, int character);
void updatePlayer(Engine* e, float dt);
void updateAttachments(Engine* e);

// ---- mundo (World.cpp)
void loadMap(Engine* e, int kind, uint32_t seed);
void clearWorld(Engine* e);
void updateWorld(Engine* e);           // streaming do mundo infinito
float mapHeight(Engine* e, float x, float z);
Scene::Handle createBuilding(Engine* e, int kind, uint32_t seed, glm::vec3 pos);
void drawMapOverlay(Engine* e, const glm::mat4& vp);
void mapOverview(Engine* e);           // câmera do alto enquadrando o mapa

}  // namespace eng
