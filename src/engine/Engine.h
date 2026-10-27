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
#include "render/Effects.h"
#include "render/Particles.h"
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
    int clipMap[CC_Count];     // animação do motor (CharClip) -> índice no Animator
    int handJoint = CJ_RHand;  // osso da mão direita (arma)
    glm::mat4 handFix{1.f};    // corrige o referencial da mão de modelos importados
    Scene::Handle weapon;      // arma equipada (opcional)
    int weaponKind = -1;
    glm::vec3 muzzle{0.f};     // ponta do cano no espaço da arma
};

// Modelo reaproveitado várias vezes (árvores, casas do mundo infinito): instancing automático
struct ProtoPart { MeshId mesh; MaterialId mat; float lodNear = 0.f, lodFar = 1e30f; };
struct Prototype {
    std::vector<ProtoPart> parts;
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

// ---- Lógica de jogo (Game.cpp)
enum BehaviorType { BH_Enemy, BH_Health, BH_Collect, BH_Goal, BH_Spawner, BH_DamageZone, BH_Rotate, BH_Float, BH_Platform,
                    BH_Explosive, BH_Count };
struct Behavior {
    int type = BH_Health;
    Scene::Handle obj;
    // parâmetros (o significado depende do tipo)
    float vida = 100.f, velocidade = 3.f, dano = 10.f, alcance = 25.f, raio = 2.f, intervalo = 5.f, tempo = 3.f;
    int pontos = 10, maximo = 6, total = 0, element = 0;
    float cura = 0.f;
    bool ranged = false;
    std::string armaInimigo;     // spawner: arma dos inimigos criados
    glm::vec3 destino{0.f};
    // estado em tempo de jogo
    float timer = 0.f, hp = 100.f, deadTime = 0.f, accum = 0.f, fxTimer = 0.f;
    int spawnedCount = 0, alive = 0;
    bool dead = false, aggro = false, done = false;
    glm::vec3 start{0.f};
    glm::quat startRot{1, 0, 0, 0};
    int parent = -1;             // inimigo criado por um spawner (índice do spawner)
    bool kinematic = false;
};
struct GameRules {
    std::string objetivo = "livre";  // livre, sobreviver, coletar, chegar, eliminar, pontos
    int quantidade = 0;
    float tempo = 0.f;               // limite de tempo (s); 0 = sem limite
    float vidaJogador = 100.f;
    std::string titulo, descricao;
};
struct GameState {
    int state = 0;                   // 0 jogando, 1 vitória, 2 derrota
    float time = 0.f;
    int score = 0, collected = 0, killed = 0, totalCollect = 0, totalEnemies = 0;
    float playerHp = 100.f, playerMax = 100.f, hurtTimer = 0.f;
    std::vector<Behavior> behaviors;
    std::vector<Scene::Handle> spawned;           // criados durante o Play (apagados no Stop)
    std::vector<Scene::Handle> hidden;            // escondidos durante o Play (voltam no Stop)
    std::vector<RigidBody*> disabledBodies;
    std::string message;                          // aviso curto (ex.: "Vitória!")
};
struct LightObject { Scene::Handle obj; glm::vec3 color{1.f}; float intensity = 8.f, radius = 10.f; };

enum MapKind { MAP_None = -1, MAP_Infinite = 0, MAP_City, MAP_Favela, MAP_School, MAP_Suburb, MAP_Count };

struct Tracer { glm::vec3 a, b; float life = 0.f; };

// Efeitos em andamento
struct FxFlash { FxLight light{}; float life = 0.f, maxLife = 0.f; };
struct Projectile {
    bool active = false;
    glm::vec3 pos{0.f}, vel{0.f};
    FxPalette pal{};
    int element = 0;
    float life = 0.f;
    const btCollisionObject* ignore = nullptr;
};
struct Bolt { glm::vec3 pts[14]; int n = 0; glm::vec3 color{1.f}; float life = 0.f; };
using EmitterPool = ObjectPool<Emitter, 256>;

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
    EmitterPool emitters;

    // Efeitos
    ParticleSystem particles;
    Rng fxRng{7};
    FxFlash flashes[16];
    Projectile projectiles[32];
    Bolt bolts[8];
    float castCooldown = 0.f;

    std::vector<std::unique_ptr<LoadedModel>> models;  // glTF e personagens procedurais
    std::vector<Scene::Handle> objects;                // id público -> handle
    std::vector<Animator*> animators;
    std::vector<CharacterInfo> characters;
    std::unique_ptr<LoadedModel> humanoidRef;            // esqueleto + animações de referência (retarget)
    std::vector<std::unique_ptr<Retarget>> retargets;
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

    // Projeto
    std::vector<std::string> recipes;   // JSON de criação de cada objeto raiz (GameObject::recipe)
    uint64_t revision = 0;              // muda a cada edição (salvamento automático)
    std::string projectName = "Projeto";
    float timeOfDay = 13.f;
    std::vector<LightObject> lightObjects;
    GameRules rules;
    GameState game;
    std::string toolResult;             // resposta da última chamada de ferramenta (IA)

    bool playing = false;
    bool showGrid = true;
    bool previewAnim = true;          // animações rodando também no editor
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
Scene::Handle spawnGeo(Engine* e, Geo& g, const char* name, bool local, glm::vec3 pos = glm::vec3(0.f), float yaw = 0.f,
                       float detailFar = 160.f);
// Distâncias de LOD (metros)
constexpr float kDetailFar = 160.f;   // carros, postes, caixas d'água...
constexpr float kTreeLod = 70.f;      // árvore completa -> simplificada
constexpr float kTreeFar = 420.f;     // árvore some (neblina)
void addStaticBoxes(Engine* e, const std::vector<BoxCollider>& boxes, StaticCollision* into = nullptr);
void freeStatic(Engine* e, StaticCollision& s);
MaterialId tintedMaterial(Engine* e, MaterialId base, glm::vec3 color);

// ---- personagens e armas (Engine.cpp)
int createCharacter(Engine* e, const CharacterParams& p, glm::vec3 pos, float yaw, bool controllable);
int createWeapon(Engine* e, int kind, int character);   // character < 0: solto no chão
int characterOfSelection(Engine* e);
// Modelo importado (glb/fbx) selecionado vira personagem: física, animações por nome ou retarget humanoide
int makeCharacterFromModel(Engine* e, bool controllable);
void setPlayer(Engine* e, int character);
void updatePlayer(Engine* e, float dt);
void updateAttachments(Engine* e);

// ---- efeitos (Effects no Engine.cpp)
int createEffect(Engine* e, int kind, int element);              // objeto com emissor contínuo
void spawnEffect(Engine* e, int kind, int element, glm::vec3 pos); // efeito instantâneo (explosão...)
void castSpell(Engine* e, int element);                          // magia do jogador
void addFlash(Engine* e, const FxLight& l, float life);
void updateEffects(Engine* e, float dt);
int gatherLights(Engine* e, PointLight* out, int max);           // luzes da cena + luzes dos efeitos
void drawEffectsOverlay(Engine* e, const glm::mat4& vp, bool editor);
bool pickEmitter(Engine* e, glm::vec3 o, glm::vec3 d, float& t, GameObject*& hit);

// ---- personagens: movimento compartilhado por jogador e inimigos (Engine.cpp)
bool driveCharacter(Engine* e, CharacterInfo& c, glm::vec3 move, float speed, bool jump, float dt);
void deleteObject(Engine* e, Scene::Handle h);

// ---- jogo (Game.cpp)
void gameStart(Engine* e);
void gameStop(Engine* e);
void gameUpdate(Engine* e, float dt);
void damageAt(Engine* e, GameObject* hit, float dmg, glm::vec3 from);
void damageRadius(Engine* e, glm::vec3 c, float radius, float dmg, bool hurtPlayer = false);
int behaviorTypeFromName(const std::string& n);

// ---- projeto (Project.cpp): formato JSON único para salvar, abrir e comandos da IA
int createFromSpecJson(Engine* e, const std::string& specJson, std::string* err);
std::string exportProject(Engine* e, bool withIds);
bool importProject(Engine* e, const std::string& json, std::string* err);
std::string toolCall(Engine* e, const std::string& name, const std::string& inputJson);
void setTimeOfDay(Engine* e, float hour);
int recordRecipe(Engine* e, GameObject& root, const std::string& specJson);
// Muda um campo da receita do objeto raiz (ex.: cor/material editados no inspetor). valueJson: JSON do valor.
void recipeSet(Engine* e, GameObject& root, const char* key, const std::string& valueJson);
void recipePartSet(Engine* e, GameObject& root, int partIndex, const char* key, const std::string& valueJson);
std::string jsonString(const std::string& s);   // "texto" com escapes JSON
GameObject* rootOf(Engine* e, GameObject* g);   // raiz editável (com id) acima de g
int loadModelFile(Engine* e, const char* path); // .glb/.gltf/.fbx -> id da raiz (-1 = erro em lastError)
std::string gameStatusText(Engine* e);
// Exporta para .glb: rootId >= 0 = só esse objeto (na origem), -1 = cena inteira (Export.cpp)
bool exportGlb(Engine* e, int rootId, const std::string& path, std::string* err);         // texto do objetivo para o HUD

// ---- mundo (World.cpp)
void loadMap(Engine* e, int kind, uint32_t seed, bool withPlayer = true);
void clearWorld(Engine* e);
void updateWorld(Engine* e);           // streaming do mundo infinito
float mapHeight(Engine* e, float x, float z);
Scene::Handle createBuilding(Engine* e, int kind, uint32_t seed, glm::vec3 pos);
void drawMapOverlay(Engine* e, const glm::mat4& vp);
void mapOverview(Engine* e);           // câmera do alto enquadrando o mapa

}  // namespace eng
