// Projeto: um formato JSON único (chaves em português) usado para
//  - salvar/abrir projetos (salvamento automático no celular),
//  - criar objetos pelo editor (cada botão gera uma "receita"),
//  - os comandos do assistente de IA (ferramentas).
// Cada objeto raiz guarda a receita que o recria; ao salvar, a receita recebe o transform atual.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stb_image.h>
#include "assets/TextureManager.h"
#include "engine/Engine.h"
#include "json.hpp"
#include <glm/gtx/matrix_decompose.hpp>

namespace eng {
std::string kitPath(Engine* e, const std::string& name);  // (mais abaixo)

using json = nlohmann::json;

namespace {

// minúsculas, sem acentos, espaço/hífen viram '_'
std::string norm(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[++i] | 0x20;  // maiúscula acentuada -> minúscula
            const char* m = nullptr;
            if (d >= 0xA0 && d <= 0xA5) m = "a";
            else if (d == 0xA7) m = "c";
            else if (d >= 0xA8 && d <= 0xAB) m = "e";
            else if (d >= 0xAC && d <= 0xAF) m = "i";
            else if (d >= 0xB2 && d <= 0xB6) m = "o";
            else if (d >= 0xB9 && d <= 0xBC) m = "u";
            if (m) o += m;
            continue;
        }
        if (c == ' ' || c == '-') o += '_';
        else o += char(std::tolower(c));
    }
    return o;
}

int lookup(const std::string& raw, std::initializer_list<std::pair<const char*, int>> table, int def = -1) {
    std::string n = norm(raw);
    for (auto& [k, v] : table) if (n == k) return v;
    for (auto& [k, v] : table) if (n.find(k) != std::string::npos) return v;  // "arvore grande" -> arvore
    return def;
}

int mapKindOf(const std::string& s) {
    return lookup(s, {{"vazio", MAP_None}, {"nenhum", MAP_None}, {"infinito", MAP_Infinite}, {"cidade", MAP_City},
                      {"favela", MAP_Favela}, {"escola", MAP_School}, {"colegio", MAP_School}, {"bairro", MAP_Suburb},
                      {"planeta", MAP_Planet}, {"mundo", MAP_Planet}}, -2);
}
const char* mapKindName(int k) {
    static const char* n[] = {"infinito", "cidade", "favela", "escola", "bairro", "planeta"};
    return k >= 0 && k < MAP_Count ? n[k] : "vazio";
}
int buildingOf(const std::string& s) {
    return lookup(s, {{"casa", 0}, {"sobrado", 1}, {"barraco", 2}, {"predio", 3}, {"apartamento", 3}, {"arranha_ceu", 4},
                      {"arranha", 4}, {"escola", 5}, {"colegio", 5}, {"arvore", 6}, {"pinheiro", 7}, {"palmeira", 8},
                      {"carro", 9}, {"poste", 10}, {"muro", 11}, {"rua", 12}, {"pista", 12}, {"estrada", 12}, {"cruzamento", 13},
                      {"calcada", 14}, {"escada", 15}, {"rampa", 16}, {"piso", 17}, {"laje", 17}, {"plataforma", 17},
                      {"parede", 18}, {"cerca", 19}, {"banco", 20}, {"caixa_dagua", 21}, {"caixa_d_agua", 21}});
}
const char* buildingName(int k) {
    static const char* n[] = {"casa", "sobrado", "barraco", "predio", "arranha_ceu", "escola", "arvore", "pinheiro", "palmeira", "carro",
                              "poste", "muro", "rua", "cruzamento", "calcada", "escada", "rampa", "piso", "parede", "cerca", "banco",
                              "caixa_dagua"};
    return k >= 0 && k < 22 ? n[k] : "casa";
}
int weaponOf(const std::string& s) {
    return lookup(s, {{"pistola", WK_Pistol}, {"revolver", WK_Pistol}, {"fuzil", WK_Rifle}, {"rifle", WK_Rifle},
                      {"metralhadora", WK_Rifle}, {"escopeta", WK_Shotgun}, {"sniper", WK_Sniper}, {"faca", WK_Knife},
                      {"taco", WK_Bat}, {"bastao", WK_Bat}});
}
// Armas realistas do app (fuzis com textura): nome -> arquivo do kit. Vazio = não é uma delas.
std::string realWeaponFile(const std::string& s) {
    std::string n = norm(s);
    n.erase(std::remove(n.begin(), n.end(), '_'), n.end());
    if (n.find("ak74tat") != std::string::npos) return "kit:armas/ak74_tatica";
    if (n.find("ak74") != std::string::npos) return "kit:armas/ak74";
    if (n.find("ak47") != std::string::npos || n == "ak") return "kit:armas/ak47";
    if (n.find("m4colt") != std::string::npos || n.find("colt") != std::string::npos) return "kit:armas/m4colt";
    if (n.find("m4") != std::string::npos) return "kit:armas/m4a1";
    if (n.find("tt33") != std::string::npos || n.find("tokarev") != std::string::npos || n.find("pistol") != std::string::npos ||
        n.find("revolver") != std::string::npos)
        return "kit:armas/tt33";
    return {};
}

const char* weaponKey(int k) {
    static const char* n[] = {"pistola", "fuzil", "escopeta", "sniper", "faca", "taco"};
    return k >= 0 && k < WK_Count ? n[k] : "";
}
int fxOf(const std::string& s) {
    return lookup(s, {{"fogueira", FX_Fire}, {"fogo", FX_Fire}, {"tocha", FX_Torch}, {"fumaca", FX_Smoke}, {"faiscas", FX_Sparks},
                      {"aura", FX_Aura}, {"portal", FX_Portal}, {"explosao", FX_Explosion}, {"orbe", FX_Orb},
                      {"coluna", FX_Pillar}}, FX_Fire);
}
int elementOf(const std::string& s) {
    return lookup(s, {{"fogo", EL_Fire}, {"gelo", EL_Ice}, {"trevas", EL_Dark}, {"luz", EL_Light}, {"veneno", EL_Poison},
                      {"raio", EL_Lightning}, {"arcano", EL_Arcane}}, EL_Fire);
}
int matOf(const std::string& s) {
    for (int i = 0; i < int(Mat::Count); ++i) if (norm(matName(Mat(i))) == norm(s)) return i;
    int k = lookup(s, {{"reboco", int(Mat::Plaster)}, {"fachada_casa", int(Mat::FacadeHouse)}, {"fachada_predio", int(Mat::FacadeApt)},
                       {"vidro_escritorio", int(Mat::FacadeOffice)}, {"tijolo_janela", int(Mat::BrickWindow)}, {"tijolo", int(Mat::Brick)},
                       {"telha", int(Mat::RoofTile)}, {"concreto", int(Mat::Concrete)}, {"asfalto", int(Mat::Asphalt)},
                       {"rua", int(Mat::Road)}, {"calcada", int(Mat::Sidewalk)}, {"grama", int(Mat::Grass)}, {"madeira", int(Mat::Wood)},
                       {"metal_escuro", int(Mat::GunMetal)}, {"metal", int(Mat::Metal)}, {"plastico", int(Mat::Polymer)},
                       {"quadra", int(Mat::Court)}, {"agua", int(Mat::Water)}, {"terreno", int(Mat::Terrain)},
                       {"pintura", int(Mat::Painted)}, {"liso", int(Mat::Painted)}, {"vidro", int(Mat::Glass)},
                       {"tijolo_vermelho", int(Mat::RedBrick)}, {"fachada_tijolo", int(Mat::FacadeBrick)}, {"ardosia", int(Mat::Slate)},
                       {"marmore", int(Mat::Marble)}, {"terra", int(Mat::Dirt)}, {"pedra", int(Mat::Stone)},
                       {"ceramica", int(Mat::Ceramic)}, {"lajota", int(Mat::Ceramic)}, {"grafite", int(Mat::Graffiti)},
                       {"alambrado", int(Mat::ChainLink)}, {"tela", int(Mat::ChainLink)}, {"telha_ondulada", int(Mat::Corrugated)},
                       {"fibrocimento", int(Mat::Corrugated)}, {"brasilit", int(Mat::Corrugated)}});
    return k;
}

float num(const json& j, const char* k, float def) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<float>() : def;
}
int inum(const json& j, const char* k, int def) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? int(std::lround(it->get<double>())) : def;
}
bool flag(const json& j, const char* k, bool def) {
    auto it = j.find(k);
    if (it == j.end()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    return def;
}
std::string str(const json& j, const char* k, const char* def = "") {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(def);
}
bool vec3Of(const json& j, const char* k, glm::vec3& out) {
    auto it = j.find(k);
    if (it == j.end()) return false;
    if (it->is_number()) { out = glm::vec3(it->get<float>()); return true; }
    if (!it->is_array() || it->size() < 3) return false;
    for (int i = 0; i < 3; ++i) { if (!(*it)[i].is_number()) return false; out[i] = (*it)[i].get<float>(); }
    return true;
}

// Cor em sRGB 0..1: [r,g,b] (0..1 ou 0..255), "#rrggbb" ou nome
bool colorOf(const json& v, glm::vec3& out) {
    if (v.is_array() && v.size() >= 3) {
        glm::vec3 c;
        for (int i = 0; i < 3; ++i) { if (!v[i].is_number()) return false; c[i] = v[i].get<float>(); }
        if (c.x > 1.f || c.y > 1.f || c.z > 1.f) c /= 255.f;
        out = glm::clamp(c, 0.f, 1.f);
        return true;
    }
    if (!v.is_string()) return false;
    std::string s = v.get<std::string>();
    if (!s.empty() && s[0] == '#' && s.size() >= 7) {
        unsigned r, g, b;
        if (std::sscanf(s.c_str() + 1, "%2x%2x%2x", &r, &g, &b) == 3) { out = glm::vec3(r, g, b) / 255.f; return true; }
        return false;
    }
    static const std::pair<const char*, glm::vec3> names[] = {
        {"vermelho", {0.85f, 0.12f, 0.1f}}, {"verde", {0.2f, 0.7f, 0.25f}}, {"azul", {0.15f, 0.35f, 0.9f}},
        {"amarelo", {0.98f, 0.85f, 0.15f}}, {"laranja", {1.f, 0.5f, 0.1f}}, {"roxo", {0.5f, 0.2f, 0.75f}},
        {"rosa", {0.95f, 0.45f, 0.65f}}, {"branco", {0.95f, 0.95f, 0.95f}}, {"preto", {0.05f, 0.05f, 0.06f}},
        {"cinza", {0.5f, 0.5f, 0.52f}}, {"marrom", {0.45f, 0.28f, 0.15f}}, {"dourado", {1.f, 0.78f, 0.3f}},
        {"ouro", {1.f, 0.78f, 0.3f}}, {"prata", {0.8f, 0.82f, 0.85f}}, {"ciano", {0.2f, 0.85f, 0.9f}},
        {"bege", {0.86f, 0.78f, 0.62f}}, {"vinho", {0.45f, 0.06f, 0.12f}},
        {"clara", {0.95f, 0.8f, 0.67f}}, {"morena", {0.78f, 0.55f, 0.37f}}, {"parda", {0.67f, 0.45f, 0.29f}},
        {"negra", {0.37f, 0.24f, 0.16f}}, {"escura", {0.37f, 0.24f, 0.16f}}};
    std::string n = norm(s);
    for (auto& [k, c] : names) if (n.find(k) != std::string::npos) { out = c; return true; }
    return false;
}
glm::vec4 linear(glm::vec3 c) { return glm::vec4(glm::pow(c, glm::vec3(2.2f)), 1.f); }
double r3(float x) { return std::round(double(x) * 1000.0) / 1000.0; }
json arr3(glm::vec3 v) { return json::array({r3(v.x), r3(v.y), r3(v.z)}); }

glm::vec3 defaultPos(Engine* e) {
    glm::vec3 p = e->editorCam.target;
    p.y = e->mapKind >= 0 ? mapHeight(e, p.x, p.z) : 0.f;
    return p;
}

int charIndexOf(Engine* e, GameObject* root) {
    for (size_t i = 0; i < e->characters.size(); ++i)
        if (root && e->scene.get(e->characters[i].root) == root) return int(i);
    return -1;
}

void applyMaterialTo(Engine* e, GameObject& g, bool subtree, const json& spec) {
    auto each = [&](auto&& f) {
        if (subtree) Scene::forEachInSubtree(g, [&](GameObject& o) { if (o.meshRenderer) f(*o.meshRenderer); });
        else if (g.meshRenderer) f(*g.meshRenderer);
    };
    if (auto it = spec.find("material"); it != spec.end() && it->is_string()) {
        int m = matOf(it->get<std::string>());
        if (m >= 0) each([&](MeshRenderer& mr) { mr.material = e->mats[m]; });
    }
    if (auto it = spec.find("textura"); it != spec.end() && it->is_string()) {
        int w, h, comp;
        std::string file = it->get<std::string>();
        if (file.rfind("kit:", 0) == 0) file = kitPath(e, file.substr(4));  // textura do kit (ex.: skin de arma)
        unsigned char* px = stbi_load(file.c_str(), &w, &h, &comp, 4);
        if (px) {
            GLuint tex = TextureManager::instance().fromPixels(px, w, h, 4, true);
            stbi_image_free(px);
            std::vector<std::pair<MaterialId, MaterialId>> remap;
            each([&](MeshRenderer& mr) {
                for (auto& [from, to] : remap) if (from == mr.material) { mr.material = to; return; }
                const Material* m = e->renderer.material(mr.material);
                if (!m) return;
                Material copy = *m;
                copy.albedo = tex;
                copy.albedoFactor = glm::vec4(1.f);
                copy.mode = 0;
                MaterialId id = e->renderer.createMaterial(copy);
                remap.push_back({mr.material, id});
                mr.material = id;
            });
        } else {
            e->lastError = "não consegui ler a imagem " + it->get<std::string>();
        }
    }
    glm::vec3 c;
    if (auto it = spec.find("cor"); it != spec.end() && colorOf(*it, c)) {
        each([&](MeshRenderer& mr) { mr.material = tintedMaterial(e, mr.material, c); });
        Scene::forEachInSubtree(g, [&](GameObject& o) {
            if (o.emitter) { o.emitter->customColor = true; o.emitter->color = c; }
        });
    }
}

// Nome, transform, material/cor/textura, peças e "ativo"
void applyCommon(Engine* e, GameObject& g, const json& spec, bool isChar) {
    if (auto it = spec.find("nome"); it != spec.end() && it->is_string()) g.setName(it->get<std::string>().c_str());
    glm::vec3 v;
    if (vec3Of(spec, "pos", v)) g.transform.setPosition(v);
    if (vec3Of(spec, "rot", v)) g.transform.setEulerDegrees(v);
    else if (auto it = spec.find("rot_y"); it != spec.end() && it->is_number())
        g.transform.setRotation(glm::angleAxis(glm::radians(it->get<float>()), glm::vec3(0, 1, 0)));
    if (!isChar && vec3Of(spec, "escala", v)) g.transform.setScale(glm::max(v, glm::vec3(0.01f)));
    applyMaterialTo(e, g, true, spec);
    if (auto it = spec.find("pecas"); it != spec.end() && it->is_object()) {
        int idx = 0;
        Scene::forEachInSubtree(g, [&](GameObject& o) {
            auto p = it->find(std::to_string(idx++));
            if (p != it->end() && p->is_object()) applyMaterialTo(e, o, false, *p);
        });
    }
    if (!flag(spec, "ativo", true)) Scene::forEachInSubtree(g, [&](GameObject& o) { o.active = false; });
    if (g.rigidBody) e->physics.syncFromTransform(*g.rigidBody);
}

CharacterParams characterParams(const json& spec, uint32_t seed) {
    CharacterParams p = randomCharacter(seed);
    auto it = spec.find("aparencia");
    if (it == spec.end() || !it->is_object()) return p;
    const json& a = *it;
    p.height = std::clamp(num(a, "altura", p.height), 1.f, 2.4f);
    if (auto b = a.find("corpo"); b != a.end() && b->is_string())
        p.build = lookup(b->get<std::string>(), {{"magro", 0}, {"normal", 1}, {"medio", 1}, {"forte", 2}, {"gordo", 2}}, p.build);
    glm::vec3 c;
    if (auto k = a.find("pele"); k != a.end() && colorOf(*k, c)) p.skin = linear(c);
    if (auto k = a.find("camisa"); k != a.end() && colorOf(*k, c)) p.shirt = linear(c);
    if (auto k = a.find("calca"); k != a.end() && colorOf(*k, c)) p.pants = linear(c);
    if (auto k = a.find("tenis"); k != a.end() && colorOf(*k, c)) p.shoes = linear(c);
    if (auto k = a.find("cabelo"); k != a.end() && colorOf(*k, c)) p.hair = linear(c);
    if (auto k = a.find("estilo_cabelo"); k != a.end() && k->is_string())
        p.hairStyle = lookup(k->get<std::string>(), {{"careca", 0}, {"curto", 1}, {"comprido", 2}, {"longo", 2}, {"bone", 3}}, p.hairStyle);
    p.longSleeves = flag(a, "manga_comprida", p.longSleeves);
    return p;
}

int finish(Engine* e, Scene::Handle h, json& spec, bool isChar) {
    GameObject* g = e->scene.get(h);
    if (!g) return -1;
    applyCommon(e, *g, spec, isChar);
    // a receita guarda o que cria o objeto; transform e nome vêm do objeto na hora de salvar
    if (auto it = spec.find("animacoes"); it != spec.end() && it->is_array()) applyCustomAnims(e, *g, it->dump());
    for (const char* k : {"pos", "rot", "rot_y", "escala", "nome", "id"}) spec.erase(k);
    recordRecipe(e, *g, spec.dump());
    ++e->revision;
    return g->id;
}

std::string errJson(const std::string& m) { return json{{"erro", m}}.dump(-1, ' ', false, json::error_handler_t::replace); }

}  // namespace

GameObject* rootOf(Engine* e, GameObject* g) {
    (void)e;
    while (g && g->transform.parent && g->transform.parent->owner && g->transform.parent->owner->id >= 0) g = g->transform.parent->owner;
    return g;
}

int recordRecipe(Engine* e, GameObject& root, const std::string& specJson) {
    if (root.recipe >= 0 && root.recipe < int(e->recipes.size())) e->recipes[root.recipe] = specJson;
    else { root.recipe = int(e->recipes.size()); e->recipes.push_back(specJson); }
    return root.recipe;
}

void recipeSet(Engine* e, GameObject& root, const char* key, const std::string& valueJson) {
    if (root.recipe < 0 || root.recipe >= int(e->recipes.size())) return;
    json r = json::parse(e->recipes[root.recipe], nullptr, false);
    json v = json::parse(valueJson, nullptr, false);
    if (r.is_discarded() || !r.is_object()) return;
    if (v.is_discarded() || v.is_null()) r.erase(key);
    else r[key] = v;
    e->recipes[root.recipe] = r.dump();
    ++e->revision;
}

void recipePartSet(Engine* e, GameObject& root, int partIndex, const char* key, const std::string& valueJson) {
    if (root.recipe < 0 || root.recipe >= int(e->recipes.size())) return;
    json r = json::parse(e->recipes[root.recipe], nullptr, false);
    json v = json::parse(valueJson, nullptr, false);
    if (r.is_discarded() || !r.is_object() || v.is_discarded()) return;
    json& part = r["pecas"][std::to_string(partIndex)];
    if (!std::strcmp(key, "material")) { part.erase("cor"); part.erase("textura"); }
    if (!std::strcmp(key, "textura")) part.erase("cor");
    part[key] = v;
    e->recipes[root.recipe] = r.dump();
    ++e->revision;
}

std::string jsonString(const std::string& s) { return json(s).dump(-1, ' ', false, json::error_handler_t::replace); }

std::string kitPath(Engine* e, const std::string& name) {
    std::string base = e->renderer.shaderDir() + "/kit/" + name;
    if (name.find('.') != std::string::npos) return base;  // já tem extensão
    FILE* f = std::fopen((base + ".glb").c_str(), "rb");     // armas/... vêm em .glb, peças da cidade em .gltf
    if (f) { std::fclose(f); return base + ".glb"; }
    return base + ".gltf";
}

int loadModelFile(Engine* e, const char* pathIn) {
    // "kit:Nome" = peça do kit de cidade que vem com o app (shaders/kit/Nome.gltf): o projeto guarda só o nome
    std::string resolved = std::strncmp(pathIn, "kit:", 4) == 0 ? kitPath(e, pathIn + 4) : std::string(pathIn);
    const char* path = resolved.c_str();
    auto model = std::make_unique<LoadedModel>();
    LoadContext ctx{e->renderer, e->scene, e->meshRenderers, e->animatorPool};
    std::string ext = path;
    ext = ext.substr(ext.find_last_of('.') == std::string::npos ? ext.size() : ext.find_last_of('.'));
    for (auto& ch : ext) ch = char(std::tolower((unsigned char)ch));
    bool fbx = ext == ".fbx" || ext == ".obj";  // ufbx lê FBX (binário e texto, 6.x a 7.x) e OBJ
    if (!fbx && ext != ".glb" && ext != ".gltf") {
        e->lastError = "formato " + ext + " não suportado: use .glb, .gltf, .fbx ou .obj (para .dae/.blend/.3ds, exporte do Blender como .glb)";
        return -1;
    }
    if (!(fbx ? loadFbx(path, ctx, *model) : loadGltf(path, ctx, *model))) {
        e->lastError = model->error.empty() ? std::string("não consegui abrir ") + path : model->error;
        for (auto h : model->allNodes) if (GameObject* g = e->scene.get(h)) releaseComponents(e, *g);
        for (auto h : model->roots) e->scene.destroy(h);
        return -1;
    }
    int rootId = -1;
    for (auto h : model->allNodes) {
        int id = registerObject(e, h);
        if (rootId < 0) rootId = id;
    }
    for (Animator* a : model->animators) e->animators.push_back(a);
    e->models.push_back(std::move(model));
    e->lastError.clear();
    // modelo pousado no chão e centrado no foco
    if (GameObject* root = objectById(e, rootId)) {
        // humanoide (Mixamo, Unity...): já ganha as animações do motor, mesmo sem virar personagem
        if (Animator* a = animatorOf(*root)) setupHumanoid(e, *root, a);
        e->scene.updateTransforms();
        AABB b;
        if (subtreeBounds(e, *root, b)) {
            glm::vec3 c = (b.min + b.max) * 0.5f;
            glm::vec3 at = e->mapKind >= 0 ? e->editorCam.target : glm::vec3(0.f);
            float ground = mapHeight(e, at.x, at.z);
            root->transform.setPosition(root->transform.position + glm::vec3(at.x - c.x, ground - b.min.y, at.z - c.z));
        }
    }
    return rootId;
}

bool attachGear(Engine* e, int ci, const std::string& name) {
    if (ci < 0 || ci >= int(e->characters.size()) || !e->characters[ci].anim || !e->characters[ci].anim->skeleton) return false;
    // osso de cada peça (gerado junto com as peças pelo tools/gerar_equipamento.py)
    static std::map<std::string, std::string> bones;
    if (bones.empty())
        if (FILE* f = std::fopen(kitPath(e, "personagens/equip/equipamento.txt").c_str(), "r")) {
            char a[96], b[96];
            while (std::fscanf(f, "%95s %95s", a, b) == 2) bones[a] = b;
            std::fclose(f);
        }
    auto it = bones.find(name);
    if (it == bones.end()) { e->lastError = "equipamento desconhecido: " + name; return false; }
    CharacterInfo& c = e->characters[ci];
    const Skeleton& S = *c.anim->skeleton;
    int joint = -1;
    for (int j = 0; j < S.jointCount; ++j) if (it->second == S.name[j]) joint = j;
    if (joint < 0) { e->lastError = "o personagem não tem o osso " + it->second; return false; }
    Scene::Handle selected = e->selected;
    int id = loadModelFile(e, ("kit:personagens/equip/" + name).c_str());
    e->selected = selected;
    GameObject* g = objectById(e, id);
    if (!g) return false;
    Scene::Handle h = Scene::handleOf(*g);
    g->transform.setScale(glm::vec3(1.f));
    // a peça foi modelada na pose de repouso do personagem: o inverso do bind do osso leva ela para o osso
    e->attachments.push_back({h, c.anim, joint, S.inverseBind[joint]});
    c.gear.push_back(h);
    updateAttachments(e);
    return true;
}

int matIndexByName(const std::string& name) { return matOf(name); }

// Modelo de arquivo vira protótipo (árvore/pedra/casa do mapa): mesma altura do original, base no chão
static bool protoFromModel(Engine* e, const std::string& path, Prototype& proto) {
    float target = 0.f;
    for (const ProtoPart& pp : proto.parts)
        if (const Mesh* m = e->renderer.mesh(pp.mesh)) target = std::max(target, m->bounds.max.y);
    int id = loadModelFile(e, path.c_str());
    GameObject* root = objectById(e, id);
    if (!root) return false;
    e->scene.updateTransforms();
    AABB b;
    if (!subtreeBounds(e, *root, b)) { deleteObject(e, Scene::handleOf(*root)); return false; }
    float h = std::max(0.01f, b.max.y - b.min.y);
    float sc = target > 0.f ? target / h : 1.f;
    glm::vec3 base((b.min.x + b.max.x) * 0.5f, b.min.y, (b.min.z + b.max.z) * 0.5f);
    glm::mat4 fix = glm::scale(glm::mat4(1.f), glm::vec3(sc)) * glm::translate(glm::mat4(1.f), -base);
    std::vector<ProtoPart> parts;
    Scene::forEachInSubtree(*root, [&](GameObject& g) {
        if (!g.meshRenderer) return;
        const Mesh* m = e->renderer.mesh(g.meshRenderer->mesh);
        if (!m || m->skinned) return;  // malha com esqueleto não vira cenário fixo
        const glm::mat4* pal;
        parts.push_back({g.meshRenderer->mesh, g.meshRenderer->material, 0.f, 600.f, fix * renderMatrix(e, g, &pal)});
    });
    deleteObject(e, Scene::handleOf(*root));
    if (parts.empty()) return false;
    proto.parts = parts;
    return true;
}

// Arma de arquivo e acessórios no projeto. Arma solta: "arquivo", "virar", "tamanho", "acessorios";
// no personagem as mesmas chaves com "arma_" na frente (e "acessorios").
WeaponLook weaponLookOf(const json& spec, bool onCharacter) {
    WeaponLook l;
    std::string p = onCharacter ? "arma_" : "";
    l.file = str(spec, (p + "arquivo").c_str());
    l.flip = flag(spec, (p + "virar").c_str(), false);
    l.size = num(spec, (p + "tamanho").c_str(), 1.f);
    if (auto it = spec.find("acessorios"); it != spec.end() && it->is_array())
        for (const auto& a : *it) {
            if (!a.is_string()) continue;
            std::string n = norm(a.get<std::string>());
            if (n.find("mira") != std::string::npos || n.find("red") != std::string::npos) l.accessories |= ACC_RedDot;
            else if (n.find("coronha") != std::string::npos) l.accessories |= ACC_Stock;
            else if (n.find("empunhadura") != std::string::npos || n.find("grip") != std::string::npos) l.accessories |= ACC_Foregrip;
        }
    return l;
}

json accessoriesJson(uint32_t acc) {
    json a = json::array();
    if (acc & ACC_RedDot) a.push_back("mira");
    if (acc & ACC_Stock) a.push_back("coronha");
    if (acc & ACC_Foregrip) a.push_back("empunhadura");
    return a;
}

std::string accessoriesList(uint32_t acc) { return accessoriesJson(acc).dump(); }

bool modelParts(Engine* e, const std::string& path, const char* only, std::vector<ProtoPart>& parts, AABB& box, std::vector<std::string>* names) {
    int id = loadModelFile(e, path.c_str());
    GameObject* root = objectById(e, id);
    if (!root) return false;
    e->scene.updateTransforms();
    glm::mat4 un = glm::translate(glm::mat4(1.f), -root->transform.position);  // desfaz o "pousar no foco" do loadModelFile
    parts.clear();
    if (names) names->clear();
    box = AABB{glm::vec3(1e30f), glm::vec3(-1e30f)};
    std::string want = only ? norm(only) : std::string();
    Scene::forEachInSubtree(*root, [&](GameObject& g) {
        if (!g.meshRenderer) return;
        const Mesh* m = e->renderer.mesh(g.meshRenderer->mesh);
        if (!m || m->skinned) return;
        std::string chain;
        for (const Transform3D* t = &g.transform; t; t = t->parent) if (t->owner) chain += norm(t->owner->name) + "/";
        if (!want.empty() && chain.find(want) == std::string::npos) return;
        if (names) names->push_back(chain);
        const glm::mat4* pal;
        glm::mat4 local = un * renderMatrix(e, g, &pal);
        parts.push_back({g.meshRenderer->mesh, g.meshRenderer->material, 0.f, 1e30f, local});
        AABB b = transformAABB(m->bounds, local);
        box.min = glm::min(box.min, b.min); box.max = glm::max(box.max, b.max);
    });
    deleteObject(e, Scene::handleOf(*root));
    return !parts.empty();
}

bool kitPrototype(Engine* e, const char* name, Prototype& out, float lodFar) {
    int id = loadModelFile(e, (std::string("kit:") + name).c_str());
    GameObject* root = objectById(e, id);
    if (!root) return false;
    root->transform.setPosition(glm::vec3(0.f));
    e->scene.updateTransforms();
    AABB b;
    if (!subtreeBounds(e, *root, b)) { deleteObject(e, Scene::handleOf(*root)); return false; }
    glm::vec3 base((b.min.x + b.max.x) * 0.5f, b.min.y, (b.min.z + b.max.z) * 0.5f);
    glm::mat4 fix = glm::translate(glm::mat4(1.f), -base);
    out = Prototype{};
    out.name = name;
    Scene::forEachInSubtree(*root, [&](GameObject& g) {
        if (!g.meshRenderer) return;
        const Mesh* m = e->renderer.mesh(g.meshRenderer->mesh);
        if (!m || m->skinned) return;
        const glm::mat4* pal;
        out.parts.push_back({g.meshRenderer->mesh, g.meshRenderer->material, 0.f, lodFar, fix * renderMatrix(e, g, &pal)});
    });
    deleteObject(e, Scene::handleOf(*root));
    out.colliderHalf = (b.max - b.min) * 0.5f;
    out.colliderY = out.colliderHalf.y;
    return !out.parts.empty();
}

const Prototype* vegPrototype(Engine* e, const std::string& name) {
    if (auto it = e->veg.find(name); it != e->veg.end()) return it->second.parts.empty() ? nullptr : &it->second;
    Prototype& out = e->veg[name];
    const char* suffix[3] = {"", "_longe", "_muito_longe"};
    const float dist[4] = {0.f, 22.f, 65.f, 400.f};
    int levels = 0;
    for (int k = 0; k < 3; ++k) {
        Prototype lp;
        std::string file = "vegetacao/" + name + suffix[k];
        if (!kitPrototype(e, file.c_str(), lp, dist[k + 1])) {
            if (k == 0) { e->lastError.clear(); return nullptr; }
            break;
        }
        if (k == 0) { out.colliderHalf = lp.colliderHalf; out.colliderY = lp.colliderY; }
        for (ProtoPart& pp : lp.parts) { pp.lodNear = dist[k]; out.parts.push_back(pp); }
        ++levels;
    }
    e->lastError.clear();
    if (levels == 1) for (ProtoPart& pp : out.parts) pp.lodFar = dist[3];  // floreira: um nível só
    out.name = name;
    const float h = out.colliderHalf.y * 2.f;
    if (name.rfind("arvore", 0) == 0) { out.colliderHalf = {0.12f, h * 0.4f, 0.12f}; out.colliderY = h * 0.4f; }  // só o tronco
    else if (name.rfind("capim", 0) == 0 || name.rfind("forracao", 0) == 0) out.colliderHalf = glm::vec3(0.f);  // passa por dentro
    return &out;
}

void applyMapSwaps(Engine* e, bool textures, bool models) {
    if (models) for (auto& [key, path] : e->protoSwaps) {
        std::string k = norm(key);
        std::vector<Prototype*> targets;
        if (k == "arvore") targets = {&e->treeProto[0]};
        else if (k == "pinheiro") targets = {&e->treeProto[1]};
        else if (k == "palmeira") targets = {&e->treeProto[2]};
        else if (k == "pedra") targets = {&e->rockProto};
        else if (k == "casa") targets = {&e->houseProto[0], &e->houseProto[1], &e->houseProto[2]};
        for (Prototype* p : targets)
            if (!p->parts.empty() && !protoFromModel(e, path, *p)) e->lastError = "não consegui usar o modelo " + path;
    }
    if (textures) for (auto& [key, path] : e->texSwaps) {
        int m = matOf(key);
        if (m < 0) continue;
        int w, h, comp;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
        if (!px) { e->lastError = "não consegui ler a imagem " + path; continue; }
        GLuint tex = TextureManager::instance().fromPixels(px, w, h, 4, true);
        stbi_image_free(px);
        const Material* old = e->renderer.material(e->mats[m]);
        if (!old) continue;
        Material copy = *old;
        copy.albedo = tex;
        copy.albedoFactor = glm::vec4(1.f);
        copy.mode = 0;  // terreno com textura própria: usa a imagem em vez da mistura automática
        e->mats[m] = e->renderer.createMaterial(copy);
    }
}

// Salva e reabre o projeto com uma mudança no mapa (objetos do usuário continuam)
static bool rebuildMap(Engine* e, bool editable) {
    if (e->playing) return false;
    json j = json::parse(exportProject(e, false), nullptr, false);
    if (j.is_discarded()) return false;
    if (editable) j["mapa"]["editavel"] = true;
    else { j["mapa"].erase("editavel"); j["mapa"].erase("pecas"); }
    std::string err;
    return importProject(e, j.dump(), &err);
}
bool setMapEditable(Engine* e, bool on) {
    if ((e->mapKind == MAP_Infinite || e->mapKind == MAP_Planet) && on) { e->lastError = "o mundo infinito não tem fim: nele dá para trocar modelos e texturas, mas não editar peça por peça"; return false; }
    if (e->mapKind < 0) { e->lastError = "esta cena já é toda editável (não tem mapa pronto)"; return false; }
    return rebuildMap(e, on);
}
bool setMapSwap(Engine* e, bool texture, const std::string& key, const std::string& path) {
    auto& m = texture ? e->texSwaps : e->protoSwaps;
    if (path.empty()) m.erase(key); else m[key] = path;
    return rebuildMap(e, e->mapEditable);
}

int createFromSpecJson(Engine* e, const std::string& specJson, std::string* err) {
    json spec = json::parse(specJson, nullptr, false);
    auto fail = [&](const std::string& m) { if (err) *err = m; e->lastError = m; return -1; };
    if (spec.is_discarded() || !spec.is_object()) return fail("JSON inválido");
    ensureBasics(e);
    std::string tipo = norm(str(spec, "tipo"));
    glm::vec3 pos = defaultPos(e);
    bool hasPos = vec3Of(spec, "pos", pos);
    float yaw = glm::radians(num(spec, "rot_y", 0.f));

    if (tipo == "forma" || tipo == "cubo" || tipo == "esfera" || tipo == "capsula" || tipo == "pedra") {
        std::string f = norm(str(spec, "forma", tipo == "forma" ? "cubo" : tipo.c_str()));
        int k = lookup(f, {{"cubo", 0}, {"caixa", 0}, {"esfera", 1}, {"bola", 1}, {"capsula", 2}, {"pedra", 3}, {"rocha", 3}}, 0);
        static const MeshId Engine::*meshes[] = {&Engine::cube, &Engine::sphere, &Engine::capsule, &Engine::rock};
        static const ColliderType cts[] = {ColliderType::Box, ColliderType::Sphere, ColliderType::Capsule, ColliderType::ConvexMesh};
        static const char* names[] = {"Cubo", "Esfera", "Capsula", "Pedra"};
        static const int defPal[] = {0, 3, 2, 5};
        int pal = lookup(str(spec, "paleta"), {{"laranja", 0}, {"aco", 1}, {"metal", 1}, {"ouro", 2}, {"azul", 3}, {"madeira", 4}, {"pedra", 5}}, defPal[k]);
        glm::vec3 sc = k == 2 ? glm::vec3(0.6f) : glm::vec3(1.f);
        vec3Of(spec, "escala", sc);
        if (!hasPos) pos.y += 2.f;  // nasce acima do foco (cai no Play)
        char name[32];
        std::snprintf(name, sizeof(name), "%s %d", names[k], int(e->objects.size()));
        Scene::Handle h = addMeshObject(e, name, e->*meshes[k], e->matPalette[pal], pos, sc);
        if (flag(spec, "fisica", true)) addBody(e, h, cts[k], std::max(0.f, num(spec, "massa", 1.f)));
        if (GameObject* g = e->scene.get(h); g && g->rigidBody) e->physics.syncFromTransform(*g->rigidBody);
        return finish(e, h, spec, false);
    }
    if (tipo == "chao") {
        float size = std::clamp(num(spec, "tamanho", 40.f), 1.f, 4000.f);
        addFloor(e, size);
        Scene::Handle h = e->objects.empty() ? Scene::Handle{} : e->objects.back();
        return finish(e, h, spec, false);
    }
    if (tipo == "planta" || tipo == "vegetacao") {
        // Árvore/arbusto/capim/forração/floreira do kit de vegetação, com os 3 níveis de detalhe
        std::string name = norm(str(spec, "modelo", "arvore_1"));
        const Prototype* p = vegPrototype(e, name);
        if (!p) return fail("planta desconhecida: " + name);
        float sc = std::clamp(num(spec, "escala", name.rfind("arvore", 0) == 0 ? 2.f : 1.5f), 0.05f, 20.f);
        spec["escala"] = sc;
        if (!hasPos) pos.y = mapHeight(e, pos.x, pos.z);
        Scene::Handle rootH = e->scene.create(name.c_str());
        GameObject* root = e->scene.get(rootH);
        if (!root) return fail("não consegui criar a planta");
        root->transform.setPosition(pos);
        root->transform.setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
        root->transform.setScale(glm::vec3(sc));
        registerObject(e, rootH);
        for (const ProtoPart& pp : p->parts) {
            glm::vec3 s3, tr, skew; glm::vec4 persp; glm::quat rot;
            glm::decompose(pp.local, s3, rot, tr, skew, persp);
            Scene::Handle ch = e->scene.create(name.c_str(), root);
            GameObject* c = e->scene.get(ch);
            if (!c) break;
            c->transform.setPosition(tr); c->transform.setRotation(rot); c->transform.setScale(s3);
            auto rh = e->meshRenderers.acquire();
            if (MeshRenderer* mr = e->meshRenderers.get(rh)) {
                mr->mesh = pp.mesh; mr->material = pp.mat; mr->lodNear = pp.lodNear; mr->lodFar = pp.lodFar;
                c->meshRenderer = mr;
            }
            registerObject(e, ch);
        }
        if (p->colliderHalf.x > 0.f) addBoxCollider(e, rootH, {0.f, p->colliderY, 0.f}, p->colliderHalf);
        spec["tipo"] = "planta";
        spec["modelo"] = name;
        return finish(e, rootH, spec, false);
    }
    if (tipo == "imagem" || tipo == "pichacao" || tipo == "grafite") {
        // Pichação pronta (atlas de murais) ou imagem do celular colada numa parede
        int mural = inum(spec, "mural", -1);
        std::string file = str(spec, "arquivo");
        MaterialId mat = kInvalidId;
        glm::vec4 uv(0.f, 0.f, 1.f, 1.f);
        float aspect = 2.f;
        if (mural >= 0 || file.empty()) {
            mural = std::clamp(mural, 0, kMuralCount - 1);
            spec["mural"] = mural;
            mat = e->mats[int(Mat::Mural)];
            uv = muralUV(mural);
        } else {
            int w = 0, h = 0, comp;
            GLuint tex = TextureManager::instance().named("img:" + file);
            if (!tex) {
                unsigned char* px = stbi_load(file.c_str(), &w, &h, &comp, 4);
                if (!px) return fail("não consegui abrir a imagem: " + file);
                tex = TextureManager::instance().fromPixels(px, w, h, 4, true);
                stbi_image_free(px);
                TextureManager::instance().setNamed("img:" + file, tex);
                TextureManager::instance().setNamed("img:" + file + ":aspecto", GLuint(std::lround(float(w) / std::max(h, 1) * 1000.f)));
            }
            aspect = std::max(0.05f, TextureManager::instance().named("img:" + file + ":aspecto") / 1000.f);
            Material m;
            m.albedo = tex; m.normal = TextureManager::instance().flatNormal(); m.orm = TextureManager::instance().white();
            m.emissive = TextureManager::instance().white();
            m.roughness = 0.8f; m.metallic = 0.f; m.alphaCutoff = 0.1f;  // PNG com fundo transparente
            mat = e->renderer.createMaterial(m);
        }
        float W = std::clamp(num(spec, "largura", mural >= 0 ? 3.f : 1.5f), 0.2f, 20.f), Hh = W / aspect;
        if (!hasPos) {
            glm::vec3 o, d, hit, n;
            e->scene.updateTransforms();
            e->camera.screenRay(e->width * 0.5f, e->height * 0.5f, float(e->width), float(e->height), o, d);
            if (!e->physics.raycastNormal(o, o + d * 400.f, hit, n) || std::abs(n.y) > 0.7f)
                return fail("aponte o centro da tela para uma parede ou muro");
            n.y = 0.f;
            n = glm::normalize(n);
            pos = hit + n * 0.015f;
            yaw = std::atan2(n.x, n.z);
            spec["pos"] = {pos.x, pos.y, pos.z};
            spec["rot_y"] = glm::degrees(yaw);
        }
        MeshBuilder mb;
        glm::vec2 q[4] = {{uv.x, uv.w}, {uv.z, uv.w}, {uv.z, uv.y}, {uv.x, uv.y}};
        mb.quad({-W / 2, -Hh / 2, 0.f}, {W / 2, -Hh / 2, 0.f}, {W / 2, Hh / 2, 0.f}, {-W / 2, Hh / 2, 0.f}, q);
        MeshId mesh = mb.build(e->renderer);
        Scene::Handle h = addMeshObject(e, mural >= 0 ? muralName(mural) : "Imagem", mesh, mat, pos, glm::vec3(1.f), glm::vec3(0.f, glm::degrees(yaw), 0.f));
        if (GameObject* go = e->scene.get(h)) {
            go->noCut = true;
            if (go->meshRenderer) go->meshRenderer->castShadows = false;
        }
        spec["tipo"] = "imagem";
        return finish(e, h, spec, false);
    }
    if (tipo == "abertura" || tipo == "corte") {
        // Porta, janela ou vão aberto em qualquer parede/muro/construção: a caixa corta a parede (imagem e colisão)
        int style = lookup(str(spec, "estilo", tipo == "corte" ? "corte" : "porta"),
                           {{"porta", 0}, {"vao", 1}, {"passagem", 1}, {"janela", 2}, {"corte", 3}, {"buraco", 3}, {"portao", 3}}, 0);
        static const float defW[] = {0.9f, 0.9f, 1.2f, 1.5f}, defH[] = {2.1f, 2.1f, 1.1f, 2.2f};
        float w = std::clamp(num(spec, "largura", defW[style]), 0.3f, 12.f);
        float h = std::clamp(num(spec, "altura", defH[style]), 0.3f, 12.f);
        float sill = style == 2 ? std::clamp(num(spec, "peitoril", 1.f), 0.f, 4.f) : 0.f;
        float t = std::clamp(num(spec, "espessura", 0.25f), 0.05f, 2.f);
        float side = num(spec, "altura_lados", h);  // até onde vão os acabamentos dos lados (corte: até o topo do muro)
        if (!hasPos) {
            // mira: o centro da tela aponta para a parede
            glm::vec3 o, d, hit, n;
            e->scene.updateTransforms();
            e->camera.screenRay(e->width * 0.5f, e->height * 0.5f, float(e->width), float(e->height), o, d);
            if (!e->physics.raycastNormal(o, o + d * 400.f, hit, n) || std::abs(n.y) > 0.7f)
                return fail("aponte o centro da tela para uma parede, muro ou construção");
            n.y = 0.f;
            n = glm::normalize(n);
            // espessura: raio voltando pelo outro lado da parede
            glm::vec3 back, bn;
            t = 0.25f;
            if (e->physics.raycastNormal(hit - n * 1.6f, hit + n * 0.01f, back, bn)) {
                float tt = glm::dot(hit - back, n);
                if (tt > 0.03f && tt < 1.5f) t = tt;
            }
            glm::vec3 c = hit - n * (t * 0.5f);
            // piso: o mais alto dos dois lados, abaixo do ponto mirado (porta do 2º andar pisa na laje)
            float base = -1e9f;
            for (float sgn : {1.f, -1.f}) {
                glm::vec3 p0 = c + n * (sgn * (t * 0.5f + 0.35f)), fh, fn;
                p0.y = hit.y;
                if (e->physics.raycastNormal(p0, p0 - glm::vec3(0, 60.f, 0), fh, fn) && fh.y < hit.y - 0.05f) base = std::max(base, fh.y);
            }
            if (base < -1e8f) base = mapHeight(e, c.x, c.z);
            if (style == 3) {  // corte no muro: do chão até acima do topo
                glm::vec3 top, tn;
                float wallTop = base + h;
                if (e->physics.raycastNormal(glm::vec3(c.x, base + 40.f, c.z), glm::vec3(c.x, base + 0.05f, c.z), top, tn)) wallTop = top.y;
                side = std::max(0.3f, wallTop - base);
                if (!spec.contains("altura")) h = side + 0.15f;
            }
            pos = glm::vec3(c.x, base, c.z);
            yaw = std::atan2(n.x, n.z);
            spec["pos"] = {pos.x, pos.y, pos.z};
            spec["rot_y"] = glm::degrees(yaw);
            spec["espessura"] = t;
            spec["altura"] = h;
            spec["altura_lados"] = side;
        }
        side = std::min(side, h);
        // acabamento em volta (fica dentro da caixa do corte; a própria abertura não é cortada)
        Geo g;
        g.place(glm::vec3(0.f), 0.f);
        Mat fm = style == 3 ? Mat::Plaster : Mat::Wood;
        if (int m = matOf(str(spec, "acabamento", "")); m >= 0) fm = Mat(m);
        MeshBuilder& f = g[fm];
        f.color = fm == Mat::Plaster ? glm::vec4(0.85f, 0.83f, 0.8f, 1.f) : glm::vec4(1.f);
        f.uvTile = {1.f, 1.f};
        const float b = style == 3 ? 0.03f : 0.06f, dz = t + 0.04f;
        f.box({-w / 2, sill, -dz / 2}, {b, style == 3 ? side : h, dz});          // lado esquerdo
        f.box({w / 2 - b, sill, -dz / 2}, {b, style == 3 ? side : h, dz});       // lado direito
        if (style != 3) f.box({-w / 2, sill + h - b, -dz / 2}, {w, b, dz});      // em cima (verga)
        if (style == 2) f.box({-w / 2 - 0.04f, sill - 0.04f, -dz / 2 - 0.04f}, {w + 0.08f, 0.05f, dz + 0.08f});  // peitoril
        if (style == 0) g.door({-w / 2 + b, 0.f, 0.f}, w - 2 * b, h - b, 0);    // folha que abre sozinha
        static const char* names[] = {"Porta (aberta na parede)", "Vão de porta", "Janela (aberta na parede)", "Abertura no muro"};
        Scene::Handle hnd = spawnGeo(e, g, names[style], true, pos, yaw);
        GameObject* go = e->scene.get(hnd);
        if (!go) return fail("não consegui criar a abertura");
        Scene::forEachInSubtree(*go, [](GameObject& o) { o.noCut = true; });
        float y0 = style == 2 ? sill : 0.03f;  // porta/vão: deixa o piso de baixo inteiro
        e->openings.push_back({hnd, glm::vec3(0.f, (y0 + sill + h) * 0.5f, 0.f), glm::vec3(w, sill + h - y0, t + 0.08f)});
        spec["tipo"] = "abertura";
        return finish(e, hnd, spec, false);
    }
    if (tipo == "modulo" || tipo == "andar") {
        // Construção montada pelo usuário: andar/cômodo configurável ou peça avulsa (parede, piso, laje, varanda...)
        ModuleSpec ms;
        ms.w = std::clamp(num(spec, "largura", 4.f), 1.f, 40.f);
        ms.d = std::clamp(num(spec, "profundidade", 4.f), 1.f, 40.f);
        ms.h = std::clamp(num(spec, "altura", 2.8f), 1.8f, 8.f);
        auto wallKindOf = [&](const std::string& s) {
            return lookup(s, {{"lisa", MW_Plain}, {"porta", MW_Door}, {"janela", MW_Window}, {"aberta", MW_Open}, {"sem", MW_Open}}, MW_Plain);
        };
        if (auto it = spec.find("paredes"); it != spec.end() && it->is_array())
            for (int k = 0; k < 4 && k < int(it->size()); ++k) if ((*it)[k].is_string()) ms.walls[k] = wallKindOf((*it)[k].get<std::string>());
        // "material" sozinho pintaria o objeto inteiro (piso e laje junto): nos módulos vale só para as paredes
        if (auto it = spec.find("material"); it != spec.end()) { spec["material_parede"] = *it; spec.erase(it); }
        int wm = matOf(str(spec, "material_parede", "tijolo"));
        ms.wallMat = wm >= 0 ? Mat(wm) : Mat::Brick;
        glm::vec3 wc;
        if (auto it = spec.find("cor_parede"); it != spec.end() && colorOf(*it, wc)) ms.wallColor = linear(wc);
        int fm = matOf(str(spec, "piso", "ceramica"));
        ms.floorMat = fm >= 0 ? Mat(fm) : Mat::Ceramic;
        ms.slab = flag(spec, "laje", true);
        ms.parapet = flag(spec, "mureta", false);
        ms.porch = flag(spec, "varanda", false);
        ms.stairs = lookup(str(spec, "escada", "nenhuma"), {{"nenhuma", SK_None}, {"sem", SK_None}, {"interna", SK_Straight}, {"reta", SK_Straight},
            {"externa", SK_Outside}, {"lateral", SK_Outside}, {"l", SK_L}, {"em_l", SK_L}, {"u", SK_U}, {"em_u", SK_U},
            {"caracol", SK_Spiral}, {"espiral", SK_Spiral}, {"helicoidal", SK_Spiral}}, SK_None);
        // empilhar: em cima do objeto escolhido (mesma posição e giro); andar de cima não tem piso próprio
        if (auto it = spec.find("sobre"); it != spec.end() && it->is_number()) {
            GameObject* base = rootOf(e, objectById(e, it->get<int>()));
            AABB b;
            e->scene.updateTransforms();
            if (base && subtreeBounds(e, *base, b)) {
                float top = b.max.y;
                // andar montado: topo da laje (a grade da varanda, a mureta e os corrimãos ficam acima e não contam)
                if (base->recipe >= 0 && base->recipe < int(e->recipes.size())) {
                    json r = json::parse(e->recipes[base->recipe], nullptr, false);
                    if (r.is_object() && str(r, "tipo") == "modulo" && norm(str(r, "peca", "andar")) == "andar") {
                        ModuleSpec bs;
                        bs.h = std::clamp(num(r, "altura", 2.8f), 1.8f, 8.f);
                        bs.floor = flag(r, "tem_piso", true);
                        bs.slab = flag(r, "laje", true);
                        top = base->transform.position.y + moduleTop(bs);
                        // mesmo tamanho do andar de baixo
                        ms.w = std::clamp(num(r, "largura", 4.f), 1.f, 40.f);
                        ms.d = std::clamp(num(r, "profundidade", 4.f), 1.f, 40.f);
                        spec["largura"] = ms.w;
                        spec["profundidade"] = ms.d;
                    }
                }
                pos = glm::vec3(base->transform.position.x, top, base->transform.position.z);
                hasPos = true;
                glm::vec3 f = base->transform.rotation * glm::vec3(0, 0, 1);
                yaw = std::atan2(f.x, f.z);
                spec["rot_y"] = glm::degrees(yaw);
                if (!spec.contains("tem_piso")) spec["tem_piso"] = false;
            }
            spec.erase("sobre");
        }
        ms.floor = flag(spec, "tem_piso", true);
        std::string peca = norm(str(spec, "peca", "andar"));
        int piece = lookup(peca, {{"parede", MP_Wall}, {"piso", MP_Floor}, {"laje", MP_Slab}, {"varanda", MP_Porch},
                                  {"escada", MP_StairsOut}, {"telhado", MP_Roof}}, -1);
        Geo g;
        g.place(glm::vec3(0.f), 0.f);
        const char* name = "Andar";
        if (piece < 0) buildModule(g, ms);
        else {
            // escada avulsa: reta (lateral) ou em L / U / caracol
            if (piece == MP_StairsOut && (ms.stairs == SK_L || ms.stairs == SK_U || ms.stairs == SK_Spiral)) piece = MP_Stairs;
            buildModulePiece(g, piece, ms, wallKindOf(str(spec, "parede", "lisa")));
            static const char* names[] = {"Piso", "Parede", "Laje", "Varanda", "Escada externa", "Telhado", "Escada"};
            name = names[piece];
        }
        if (!hasPos) pos.y = mapHeight(e, pos.x, pos.z);
        Scene::Handle h = spawnGeo(e, g, name, true, pos, 0.f);
        GameObject* go = e->scene.get(h);
        if (!go) return fail("não consegui criar a construção");
        if (yaw != 0.f) go->transform.setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
        spec["tipo"] = "modulo";
        return finish(e, h, spec, false);
    }
    if (tipo == "construcao" || tipo == "predio" || tipo == "casa" || tipo == "arvore" || tipo == "natureza") {
        std::string m = str(spec, "modelo", tipo == "construcao" || tipo == "natureza" ? "casa" : tipo.c_str());
        int kind = buildingOf(m);
        if (kind < 0) return fail("modelo de construção desconhecido: " + m);
        uint32_t seed = uint32_t(inum(spec, "semente", 0));
        if (!seed) { seed = uint32_t(e->fxRng.next() & 0x7fffffff) | 1u; spec["semente"] = seed; }
        spec["modelo"] = buildingName(kind);
        spec["tipo"] = "construcao";
        Scene::Handle h = createBuilding(e, kind, seed, pos, inum(spec, "andares", 0));
        if (GameObject* g = e->scene.get(h); g && hasPos) g->transform.setPosition(pos);
        if (GameObject* g = e->scene.get(h); g && yaw != 0.f) g->transform.setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)));
        return finish(e, h, spec, false);
    }
    if (tipo == "personagem" || tipo == "inimigo") {
        bool enemy = tipo == "inimigo";
        uint32_t seed = uint32_t(inum(spec, "semente", 0));
        if (!seed) { seed = uint32_t(e->fxRng.next() & 0x7fffffff) | 1u; spec["semente"] = seed; }
        CharacterParams p = characterParams(spec, seed);
        if (enemy && spec.find("aparencia") == spec.end()) {  // inimigo padrão: roupa escura/vermelha
            p.shirt = srgb(120, 20, 25);
            p.pants = srgb(30, 30, 35);
        }
        if (!hasPos && e->mapKind < 0) pos.y = 0.f;
        int prevPlayer = e->player;
        bool controllable = !enemy && flag(spec, "jogador", false);
        int id = createCharacter(e, p, pos, yaw, controllable);
        if (id < 0) return fail(e->lastError.empty() ? "não consegui criar o personagem" : e->lastError);
        int ci = int(e->characters.size()) - 1;
        if (!controllable && e->player == ci) e->player = prevPlayer;  // só vira jogador se pedido (ou se não há outro)
        if (!controllable && prevPlayer < 0 && !enemy) e->player = ci;
        if (auto it = spec.find("arma"); it != spec.end() && it->is_string() && !it->get<std::string>().empty()) {
            WeaponLook look = weaponLookOf(spec, true);
            std::string real = realWeaponFile(it->get<std::string>());
            int wk = real.empty() ? weaponOf(it->get<std::string>()) : real.find("tt33") != std::string::npos ? int(WK_Pistol) : int(WK_Rifle);
            if (!real.empty() && look.file.empty()) look.file = real;
            if (wk >= 0 && createWeapon(e, wk, ci, look) < 0) e->lastError.clear();  // arquivo sumiu: fica sem arma
        }
        spec.erase("jogador");
        return finish(e, e->objects[id], spec, true);
    }
    if (tipo == "arma") {
        std::string armaName = str(spec, "arma", str(spec, "modelo", "pistola").c_str());
        std::string real = realWeaponFile(armaName);
        int wk = real.empty() ? weaponOf(armaName) : real.find("tt33") != std::string::npos ? int(WK_Pistol) : int(WK_Rifle);
        if (wk < 0) return fail("arma desconhecida");
        if (!real.empty() && !spec.contains("arquivo")) spec["arquivo"] = real;
        if (auto it = spec.find("portador"); it != spec.end() && it->is_number()) {  // equipa num personagem
            GameObject* owner = objectById(e, it->get<int>());
            int ci = charIndexOf(e, rootOf(e, owner));
            if (ci < 0) return fail("portador não é personagem");
            WeaponLook look = weaponLookOf(spec, false);
            if (createWeapon(e, wk, ci, look) < 0) return fail(e->lastError);
            GameObject& root = *e->scene.get(e->characters[ci].root);
            recipeSet(e, root, "arma", json(weaponKey(wk)).dump());
            recipeSet(e, root, "arma_arquivo", look.file.empty() ? "null" : json(look.file).dump());
            recipeSet(e, root, "arma_virar", look.flip ? "true" : "null");
            recipeSet(e, root, "arma_tamanho", look.size != 1.f ? json(look.size).dump() : "null");
            recipeSet(e, root, "acessorios", look.accessories ? accessoriesJson(look.accessories).dump() : "null");
            return owner->id;
        }
        int id = createWeapon(e, wk, -1, weaponLookOf(spec, false));
        if (id < 0) return fail(e->lastError.empty() ? "não consegui criar a arma" : e->lastError);
        GameObject* g = objectById(e, id);
        if (!hasPos) spec["pos"] = arr3(pos + glm::vec3(0, 0.6f, 0));
        spec["arma"] = weaponKey(wk);
        return finish(e, Scene::handleOf(*g), spec, false);
    }
    if (tipo == "efeito" || tipo == "objetivo" || tipo == "spawner" || tipo == "zona_dano") {
        int kind, el;
        if (tipo == "objetivo") { kind = FX_Pillar; el = elementOf(str(spec, "elemento", "luz")); }
        else if (tipo == "spawner") { kind = FX_Portal; el = elementOf(str(spec, "elemento", "trevas")); }
        else if (tipo == "zona_dano") { el = elementOf(str(spec, "elemento", "fogo")); kind = el == EL_Fire ? FX_Fire : FX_Aura; }
        else { kind = fxOf(str(spec, "efeito", "fogueira")); el = elementOf(str(spec, "elemento", "fogo")); }
        int id = createEffect(e, kind, el);
        if (id < 0) return fail(e->lastError);
        GameObject* g = objectById(e, id);
        if (tipo == "objetivo") g->setName("Objetivo");
        else if (tipo == "spawner") g->setName("Spawner de inimigos");
        else if (tipo == "zona_dano") {
            g->setName("Zona de dano");
            float r = num(spec, "raio", 2.f);
            g->transform.setScale(glm::vec3(std::max(0.3f, r * 0.6f)));
        }
        g->transform.setPosition(pos);
        return finish(e, Scene::handleOf(*g), spec, false);
    }
    if (tipo == "coletavel" || tipo == "item") {
        std::string item = norm(str(spec, "item", "moeda"));
        int k = lookup(item, {{"moeda", 0}, {"vida", 1}, {"cura", 1}, {"municao", 2}, {"cristal", 3}, {"joia", 3}}, 0);
        static const char* names[] = {"Moeda", "Vida", "Municao", "Cristal"};
        MeshId mesh = k == 0 ? e->sphere : k == 3 ? e->rock : e->cube;
        glm::vec3 sc = k == 0 ? glm::vec3(0.55f, 0.55f, 0.12f) : k == 1 ? glm::vec3(0.45f) : k == 2 ? glm::vec3(0.5f, 0.3f, 0.3f) : glm::vec3(0.35f, 0.7f, 0.35f);
        MaterialId mat = k == 0 ? e->matPalette[2] : k == 1 ? tintedMaterial(e, e->mats[int(Mat::Painted)], {0.9f, 0.12f, 0.15f})
                       : k == 2 ? tintedMaterial(e, e->mats[int(Mat::Painted)], {0.35f, 0.4f, 0.18f}) : e->matPalette[3];
        if (!hasPos) pos.y += 0.9f;
        Scene::Handle h = addMeshObject(e, names[k], mesh, mat, pos, sc);
        spec["tipo"] = "coletavel";
        spec["item"] = norm(names[k]);
        return finish(e, h, spec, false);
    }
    if (tipo == "luz") {
        glm::vec3 c(1.f, 0.85f, 0.6f);
        if (auto it = spec.find("cor"); it != spec.end()) colorOf(*it, c);
        if (!hasPos) pos.y += 2.5f;
        Scene::Handle h = addMeshObject(e, "Luz", e->sphere, tintedMaterial(e, e->mats[int(Mat::Painted)], c), pos, glm::vec3(0.22f));
        if (GameObject* g = e->scene.get(h); g && g->meshRenderer) g->meshRenderer->castShadows = false;
        e->lightObjects.push_back({h, c, num(spec, "intensidade", 8.f), num(spec, "raio", 10.f)});
        json copy = spec;
        copy.erase("cor");  // a cor da lâmpada já foi aplicada
        int id = finish(e, h, copy, false);
        if (GameObject* g = e->scene.get(h)) recipeSet(e, *g, "cor", spec.contains("cor") ? spec["cor"].dump() : "null");
        return id;
    }
    if (tipo == "loja" || tipo == "base") {
        // Partida Polícia x Bandidos: barraca de loja (armas/roupas do time) ou base onde o time nasce
        std::string ts = norm(str(spec, "time", "policia"));
        int team = ts.rfind("band", 0) == 0 ? 1 : 0;
        spec["time"] = team ? "bandidos" : "policia";
        const glm::vec4 teamCol = team ? glm::vec4(0.72f, 0.16f, 0.14f, 1.f) : glm::vec4(0.16f, 0.3f, 0.66f, 1.f);
        if (!hasPos) pos.y = mapHeight(e, pos.x, pos.z);
        Geo g;
        g.place(glm::vec3(0.f), 0.f);
        const char* name;
        if (tipo == "loja") {
            int kind = norm(str(spec, "loja", "armas")) == "roupas" ? 1 : 0;
            spec["loja"] = kind ? "roupas" : "armas";
            MeshBuilder& w = g[Mat::Plaster];
            w.color = glm::mix(teamCol, glm::vec4(0.9f, 0.88f, 0.84f, 1.f), 0.55f);
            w.uvTile = {1.f, 1.f};
            w.box({-1.6f, 0.f, -1.1f}, {3.2f, 2.7f, 0.15f});   // fundo
            w.box({-1.6f, 0.f, -1.1f}, {0.15f, 2.7f, 1.9f});   // lados
            w.box({1.45f, 0.f, -1.1f}, {0.15f, 2.7f, 1.9f});
            g.collider({-1.6f, 0.f, -1.1f}, {3.2f, 2.7f, 0.15f});
            g.collider({-1.6f, 0.f, -1.1f}, {0.15f, 2.7f, 1.9f});
            g.collider({1.45f, 0.f, -1.1f}, {0.15f, 2.7f, 1.9f});
            MeshBuilder& wood = g[Mat::Wood];
            wood.uvTile = {1.f, 1.f};
            wood.box({-1.45f, 0.f, 0.45f}, {2.9f, 1.05f, 0.35f});  // balcão
            g.collider({-1.45f, 0.f, 0.45f}, {2.9f, 1.05f, 0.35f});
            wood.box({-1.45f, 1.5f, -0.95f}, {2.9f, 0.06f, 0.3f});  // prateleira
            MeshBuilder& roof = g[Mat::Corrugated];
            roof.uvTile = {1.f, 1.f};
            roof.color = teamCol;
            roof.box({-1.75f, 2.7f, -1.25f}, {3.5f, 0.06f, 2.95f});  // toldo
            // placa em cima do toldo, virada para a frente
            glm::vec4 uv = muralUV(kSignBase + team * 2 + kind);
            glm::vec2 q[4] = {{uv.x, uv.w}, {uv.z, uv.w}, {uv.z, uv.y}, {uv.x, uv.y}};
            g[Mat::Mural].quad({-1.3f, 2.78f, 1.72f}, {1.3f, 2.78f, 1.72f}, {1.3f, 4.08f, 1.72f}, {-1.3f, 4.08f, 1.72f}, q);
            g[Mat::Wood].box({-1.4f, 2.76f, 1.62f}, {2.8f, 1.36f, 0.08f});  // tábua da placa
            // mercadoria na parede: armas (cinza escuro) ou roupas (coloridas)
            for (int k = 0; k < 4; ++k) {
                float x = -1.1f + k * 0.72f;
                if (kind == 0) g[Mat::GunMetal].box({x, 1.75f, -0.95f}, {0.55f, 0.12f, 0.06f});
                else {
                    MeshBuilder& c = g[Mat::Plaster];
                    static const glm::vec4 cols[2][4] = {{{0.35f, 0.38f, 0.42f, 1}, {0.34f, 0.38f, 0.24f, 1}, {0.08f, 0.09f, 0.1f, 1}, {0.12f, 0.15f, 0.22f, 1}},
                                                         {{0.95f, 0.8f, 0.1f, 1}, {0.9f, 0.9f, 0.9f, 1}, {0.7f, 0.08f, 0.1f, 1}, {0.22f, 0.24f, 0.28f, 1}}};
                    c.color = cols[team][k];
                    c.box({x, 1.6f, -0.95f}, {0.5f, 0.65f, 0.05f});
                }
            }
            name = kind ? (team ? "Loja de roupas (bandidos)" : "Loja de fardas (polícia)") : (team ? "Loja de armas (bandidos)" : "Loja de armas (polícia)");
        } else {
            MeshBuilder& c = g[Mat::Concrete];
            c.uvTile = {1.f, 1.f};
            c.box({-0.6f, 0.f, -0.6f}, {1.2f, 0.3f, 1.2f});
            g.collider({-0.6f, 0.f, -0.6f}, {1.2f, 0.3f, 1.2f});
            g[Mat::Metal].cylinder({0.f, 0.3f, 0.f}, {0.f, 6.f, 0.f}, 0.05f, 0.04f, 8);
            g.collider({-0.06f, 0.3f, -0.06f}, {0.12f, 5.7f, 0.12f});
            MeshBuilder& fl = g[Mat::Plaster];
            fl.color = teamCol;
            fl.box({0.05f, 4.9f, -0.02f}, {1.5f, 1.f, 0.04f});
            name = team ? "Base dos Bandidos" : "Base da Polícia";
        }
        Scene::Handle h = spawnGeo(e, g, name, true, pos, yaw);
        if (!e->scene.get(h)) return fail("não consegui criar");
        spec["tipo"] = tipo;
        return finish(e, h, spec, false);
    }
    if (tipo == "modelo") {
        std::string file = str(spec, "arquivo");
        if (file.empty()) return fail("modelo sem \"arquivo\"");
        int id = loadModelFile(e, file.c_str());
        if (id < 0) return fail(e->lastError);
        GameObject* g = objectById(e, id);
        if (hasPos) g->transform.setPosition(pos);
        if (flag(spec, "personagem", false) || flag(spec, "jogador", false)) {
            applyCommon(e, *g, spec, true);
            e->scene.updateTransforms();
            selectHandle(e, Scene::handleOf(*g));
            makeCharacterFromModel(e, flag(spec, "jogador", false));
            spec["personagem"] = true;
            spec.erase("jogador");
            // roupa pintada na textura do corpo e equipamento preso aos ossos
            if (auto v = spec.find("visual"); v != spec.end() && v->is_string() && !v->get<std::string>().empty())
                applyMaterialTo(e, *g, true, json{{"textura", kitPath(e, "personagens/visual/" + v->get<std::string>() + ".jpg")}});
            if (auto eq = spec.find("equipamento"); eq != spec.end() && eq->is_array()) {
                int ci = charIndexOf(e, g);
                for (const auto& it : *eq)
                    if (it.is_string() && !attachGear(e, ci, it.get<std::string>())) e->lastError.clear();
            }
            return finish(e, Scene::handleOf(*g), spec, true);
        }
        return finish(e, Scene::handleOf(*g), spec, false);
    }
    return fail("tipo desconhecido: \"" + str(spec, "tipo") + "\" (use forma, chao, construcao, personagem, inimigo, arma, efeito, "
                "coletavel, objetivo, spawner, zona_dano, luz, loja, base ou modelo)");
}

// ---------------- hora do dia ----------------
void setTimeOfDay(Engine* e, float hour) {
    hour = std::fmod(std::fmod(hour, 24.f) + 24.f, 24.f);
    e->timeOfDay = hour;
    const float maxEl = glm::radians(65.f);
    float phase = (hour - 6.f) / 12.f * 3.14159265f;
    float el = std::asin(std::sin(phase) * std::sin(maxEl));  // elevação do sol (rad)
    float az = glm::radians((hour - 13.f) * 15.f);
    glm::vec2 h0 = glm::normalize(glm::vec2(0.55f, 0.25f));
    glm::vec2 hz(h0.x * std::cos(az) - h0.y * std::sin(az), h0.x * std::sin(az) + h0.y * std::cos(az));
    float day = glm::smoothstep(-0.08f, 0.2f, el);
    e->night = 1.f - glm::smoothstep(-0.06f, 0.1f, el);  // postes acendem no fim da tarde
    float warm = 1.f - glm::smoothstep(0.05f, 0.45f, el);
    // vendo o globo, o céu e o sol "de verdade" ficam guardados (o globo usa céu de espaço e sol atrás da câmera)
    glm::vec3& sunDir = e->globeMode ? e->globeSavedSun : e->sun.direction;
    if (el > -0.02f) {
        float ce = std::cos(std::max(el, 0.03f)), se = std::sin(std::max(el, 0.03f));
        sunDir = -glm::vec3(hz.x * ce, se, hz.y * ce);
        e->sun.color = glm::mix(glm::vec3(1.f, 0.95f, 0.88f), glm::vec3(1.f, 0.55f, 0.3f), warm);
        e->sun.intensity = 0.3f + 2.9f * day;
    } else {  // noite: luar azulado
        sunDir = glm::normalize(glm::vec3(-0.3f, -1.f, 0.2f));
        e->sun.color = {0.55f, 0.65f, 1.f};
        e->sun.intensity = 0.45f;
    }
    Environment& target = e->globeMode ? e->globeSavedEnv : e->env;
    Environment env = target;
    env.ground = Environment{}.ground;
    glm::vec3 nightZ(0.015f, 0.02f, 0.06f), nightH(0.06f, 0.07f, 0.13f);
    env.zenith = glm::mix(nightZ, glm::mix(glm::vec3(0.18f, 0.36f, 0.75f), glm::vec3(0.25f, 0.3f, 0.6f), warm), day);
    env.horizon = glm::mix(nightH, glm::mix(glm::vec3(0.70f, 0.78f, 0.88f), glm::vec3(0.95f, 0.6f, 0.4f), warm), day);
    env.skyIntensity = 0.12f + 0.43f * day;
    target = env;
    ++e->revision;
}

// ---------------- salvar / abrir ----------------
std::string exportProject(Engine* e, bool withIds) {
    json j;
    j["formato"] = "motor3d-projeto";
    j["versao"] = 1;
    j["nome"] = e->projectName;
    j["mapa"] = {{"tipo", mapKindName(e->mapKind)}, {"semente", e->mapSeed}};
    if (e->mapEditable) j["mapa"]["editavel"] = true;
    if (!e->protoSwaps.empty()) j["mapa"]["trocas"] = e->protoSwaps;
    if (!e->texSwaps.empty()) j["mapa"]["texturas"] = e->texSwaps;
    json pieces = json::object();  // mapa editável: só o que mudou em relação ao mapa base
    j["ambiente"] = {{"hora", r3(e->timeOfDay)}, {"exposicao", r3(e->env.exposure)}, {"sol", r3(e->sun.intensity)},
                     {"grade", e->showGrid}, {"neblina", e->renderer.fogDensity}, {"ciclo_min", r3(e->dayCycleMin)},
                     {"chuva", e->rainMode == 0 ? "nunca" : e->rainMode == 1 ? "automatica" : "sempre"}};
    const Engine::CamState cam = e->playing ? e->savedCam
        : Engine::CamState{e->editorCam.target, e->editorCam.yaw, e->editorCam.pitch, e->editorCam.distance};
    j["camera"] = {{"alvo", arr3(cam.target)}, {"yaw", r3(cam.yaw)}, {"pitch", r3(cam.pitch)}, {"distancia", r3(cam.distance)}};
    const GameRules& R = e->rules;
    j["regras"] = {{"objetivo", R.objetivo}, {"quantidade", R.quantidade}, {"tempo", r3(R.tempo)}, {"vida_jogador", r3(R.vidaJogador)},
                   {"titulo", R.titulo}, {"descricao", R.descricao}};
    json objs = json::array();
    auto isSpawned = [&](Scene::Handle h) {
        for (auto& s : e->game.spawned) if (s.index == h.index && s.gen == h.gen) return true;
        return false;
    };
    for (size_t id = 0; id < e->objects.size(); ++id) {
        GameObject* g = e->scene.get(e->objects[id]);
        if (!g || g->transform.parent || g->recipe < 0 || g->recipe >= int(e->recipes.size())) continue;
        if (isSpawned(e->objects[id])) continue;
        bool attached = false;
        for (const Attachment& a : e->attachments) if (e->scene.get(a.obj) == g) attached = true;
        if (attached) continue;  // arma na mão: salva no personagem ("arma")
        json o = json::parse(e->recipes[g->recipe], nullptr, false);
        if (o.is_discarded() || !o.is_object()) continue;
        glm::vec3 p = g->transform.position, s = g->transform.scale;
        glm::quat q = g->transform.rotation;
        bool active = g->active;
        if (e->playing)  // no Play salva o estado do editor (antes de começar a simulação)
            for (auto& sn : e->snapshot)
                if (sn.h.index == e->objects[id].index && sn.h.gen == e->objects[id].gen) { p = sn.p; s = sn.s; q = sn.r; }
        for (auto& hh : e->game.hidden) if (e->scene.get(hh) == g) active = true;
        if (withIds) o["id"] = int(id);
        o["nome"] = g->name;
        o["pos"] = arr3(p);
        if (std::abs(q.x) < 1e-4f && std::abs(q.z) < 1e-4f) {  // só giro em Y (o mais comum): "rot_y"
            float yaw = glm::degrees(2.f * std::atan2(q.y, q.w));
            yaw = std::fmod(yaw + 540.f, 360.f) - 180.f;
            if (yaw <= -179.9995f) yaw += 360.f;
            if (std::abs(yaw) > 0.01f) o["rot_y"] = r3(yaw);
        } else {
            glm::vec3 eu = glm::degrees(glm::eulerAngles(q));
            for (int k = 0; k < 3; ++k) if (eu[k] <= -179.9995f) eu[k] += 360.f;
            o["rot"] = arr3(eu);
        }
        if (glm::length(s - glm::vec3(1.f)) > 1e-3f) o["escala"] = arr3(s);
        if (!active) o["ativo"] = false;
        if (o.value("tipo", "") == "peca_mapa") {
            int idx = o.value("peca", -1);
            if (idx < 0 || idx >= int(e->mapPieces.size())) continue;
            const Engine::MapPiece& mp = e->mapPieces[idx];
            json d = json::object();
            for (auto& [k, v] : o.items())
                if (k != "tipo" && k != "peca" && k != "id" && k != "nome" && k != "pos" && k != "rot" && k != "rot_y" && k != "escala") d[k] = v;
            bool moved = glm::length(p - mp.p) > 1e-3f || glm::length(s - mp.s) > 1e-3f || std::abs(glm::dot(q, mp.r)) < 0.99999f;
            if (moved) {
                d["pos"] = o["pos"];
                if (o.contains("rot")) d["rot"] = o["rot"];
                d["rot_y"] = o.value("rot_y", 0.0);
                d["escala"] = o.contains("escala") ? o["escala"] : json::array({1, 1, 1});
            }
            if (mp.name != g->name) d["nome"] = g->name;
            if (withIds) d["id"] = int(id);
            if (!d.empty()) pieces[std::to_string(idx)] = d;
            continue;
        }
        int ci = charIndexOf(e, g);
        if (ci >= 0) {
            const CharacterInfo& c = e->characters[ci];
            if (e->scene.get(c.weapon) && c.weaponKind >= 0) o["arma"] = weaponKey(c.weaponKind);
            else o.erase("arma");
            if (ci == e->player) o["jogador"] = true;
        }
        objs.push_back(o);
    }
    j["objetos"] = objs;
    for (size_t k = 0; k < e->mapPieces.size(); ++k)
        if (!e->scene.get(e->mapPieces[k].h)) pieces[std::to_string(k)] = {{"apagada", true}};
    if (!pieces.empty()) j["mapa"]["pecas"] = pieces;
    return j.dump(withIds ? -1 : 1, ' ', false, json::error_handler_t::replace);
}

bool importProject(Engine* e, const std::string& text, std::string* err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { if (err) *err = "arquivo de projeto inválido"; return false; }
    if (str(j, "formato") != "motor3d-projeto") { if (err) *err = "não é um projeto do Motor 3D"; return false; }
    const json mapa = j.value("mapa", json::object());
    int kind = mapKindOf(str(mapa, "tipo", "vazio"));
    if (kind == -2) kind = MAP_None;
    uint32_t seed = uint32_t(std::max(1, inum(mapa, "semente", 1)));
    e->protoSwaps.clear(); e->texSwaps.clear();
    for (const char* key : {"trocas", "texturas"})
        if (auto it = mapa.find(key); it != mapa.end() && it->is_object())
            for (auto& [k, v] : it->items()) if (v.is_string()) (key[0] == 't' && key[1] == 'r' ? e->protoSwaps : e->texSwaps)[k] = v.get<std::string>();
    if (kind >= 0) loadMap(e, kind, seed, false, flag(mapa, "editavel", false));
    else { clearScene(e); applyMapSwaps(e); }
    // mapa editável: aplica as mudanças salvas em cada peça (apagar, mover, cor, material...)
    if (auto it = mapa.find("pecas"); it != mapa.end() && it->is_object() && e->mapEditable) {
        std::vector<Scene::Handle> toDelete;
        for (auto& [k, d] : it->items()) {
            int idx = std::atoi(k.c_str());
            if (idx < 0 || idx >= int(e->mapPieces.size()) || !d.is_object()) continue;
            GameObject* g = e->scene.get(e->mapPieces[idx].h);
            if (!g) continue;
            if (flag(d, "apagada", false)) { toDelete.push_back(e->mapPieces[idx].h); continue; }
            applyCommon(e, *g, d, false);
            json r = json::parse(e->recipes[g->recipe], nullptr, false);
            for (auto& [dk, dv] : d.items())
                if (dk != "pos" && dk != "rot" && dk != "rot_y" && dk != "escala" && dk != "nome" && dk != "id") r[dk] = dv;
            e->recipes[g->recipe] = r.dump();
        }
        for (auto h : toDelete) deleteObject(e, h);
    }
    e->projectName = str(j, "nome", "Projeto");
    const json amb = j.value("ambiente", json::object());
    setTimeOfDay(e, num(amb, "hora", 13.f));
    e->env.exposure = std::clamp(num(amb, "exposicao", 1.f), 0.05f, 8.f);
    if (amb.contains("sol")) e->sun.intensity = std::clamp(num(amb, "sol", 3.2f), 0.f, 20.f);
    e->showGrid = flag(amb, "grade", true);
    if (amb.contains("neblina")) e->renderer.fogDensity = std::clamp(num(amb, "neblina", 0.f), 0.f, 0.05f);
    if (amb.contains("ciclo_min")) e->dayCycleMin = std::clamp(num(amb, "ciclo_min", 0.f), 0.f, 240.f);
    if (amb.contains("chuva")) e->rainMode = lookup(str(amb, "chuva"), {{"nunca", 0}, {"nao", 0}, {"automatica", 1}, {"auto", 1}, {"sempre", 2}}, 0);
    const json reg = j.value("regras", json::object());
    e->rules = GameRules{};
    e->rules.objetivo = norm(str(reg, "objetivo", "livre"));
    e->rules.quantidade = inum(reg, "quantidade", 0);
    e->rules.tempo = num(reg, "tempo", 0.f);
    e->rules.vidaJogador = std::max(1.f, num(reg, "vida_jogador", 100.f));
    e->rules.titulo = str(reg, "titulo");
    e->rules.descricao = str(reg, "descricao");
    std::string errors;
    if (auto it = j.find("objetos"); it != j.end() && it->is_array())
        for (const json& o : *it) {
            std::string m;
            if (createFromSpecJson(e, o.dump(), &m) < 0) errors += m + "; ";
        }
    if (const json cam = j.value("camera", json::object()); cam.is_object() && !cam.empty()) {
        glm::vec3 t(0.f);
        vec3Of(cam, "alvo", t);
        e->editorCam.target = t;
        e->editorCam.yaw = num(cam, "yaw", e->editorCam.yaw);
        e->editorCam.pitch = std::clamp(num(cam, "pitch", e->editorCam.pitch), -1.5f, 1.5f);
        e->editorCam.distance = std::clamp(num(cam, "distancia", e->editorCam.distance), 0.3f, 5000.f);
    }
    selectHandle(e, {});
    e->lastError = errors;
    if (err) *err = errors;
    return true;
}

// ---------------- ferramentas da IA ----------------
std::string toolCall(Engine* e, const std::string& name, const std::string& inputJson) {
    json in = json::parse(inputJson.empty() ? "{}" : inputJson, nullptr, false);
    if (in.is_discarded() || !in.is_object()) return errJson("entrada inválida (JSON)");
    if (e->playing) return errJson("o jogo está rodando (aperte Stop para editar)");
    std::string n = norm(name);

    if (n == "ver_cena") {
        json j = json::parse(exportProject(e, true));
        j["jogador_existe"] = e->player >= 0 && e->scene.get(e->characters[e->player].root);
        if (e->hasBounds) j["mapa"]["limites"] = {{"min", {e->boundsMin.x, e->boundsMin.y}}, {"max", {e->boundsMax.x, e->boundsMax.y}}};
        j["foco_camera"] = arr3(e->editorCam.target);
        return j.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    if (n == "carregar_mapa") {
        int kind = mapKindOf(str(in, "tipo", "vazio"));
        if (kind == -2) return errJson("mapa desconhecido (use vazio, cidade, favela, escola, bairro ou infinito)");
        uint32_t seed = uint32_t(inum(in, "semente", 0));
        if (!seed) seed = uint32_t(e->fxRng.next() & 0x7fffffff) | 1u;
        bool player = flag(in, "com_jogador", true);
        if (kind >= 0) loadMap(e, kind, seed, player);
        else {
            clearScene(e);
            createFromSpecJson(e, R"({"tipo":"chao","tamanho":60})", nullptr);
            if (player) createFromSpecJson(e, R"({"tipo":"personagem","jogador":true,"nome":"Jogador","pos":[0,0,4],"rot_y":180})", nullptr);
            e->editorCam.target = glm::vec3(0.f, 0.5f, 0.f);
            e->editorCam.distance = 25.f;
        }
        json r = {{"ok", true}, {"mapa", mapKindName(kind)}, {"semente", seed}};
        if (e->hasBounds) r["limites"] = {{"min", {e->boundsMin.x, e->boundsMin.y}}, {"max", {e->boundsMax.x, e->boundsMax.y}}};
        if (e->player >= 0) if (GameObject* p = e->scene.get(e->characters[e->player].root)) r["jogador_pos"] = arr3(p->transform.position);
        return r.dump();
    }
    if (n == "criar_objetos") {
        auto it = in.find("objetos");
        if (it == in.end() || !it->is_array()) return errJson("informe \"objetos\": [...]");
        json ids = json::array(), errs = json::array();
        for (const json& o : *it) {
            std::string m;
            int id = createFromSpecJson(e, o.dump(), &m);
            ids.push_back(id);
            if (id < 0) errs.push_back(m);
        }
        json r = {{"ids", ids}};
        if (!errs.empty()) r["erros"] = errs;
        return r.dump();
    }
    if (n == "editar_objeto") {
        GameObject* g = rootOf(e, objectById(e, inum(in, "id", -1)));
        if (!g || g->recipe < 0) return errJson("objeto não encontrado (veja os ids com ver_cena)");
        json r = json::parse(e->recipes[g->recipe], nullptr, false);
        bool rebuild = false;
        for (auto& [k, v] : in.items()) {
            if (k == "id") continue;
            static const char* inPlace[] = {"nome", "pos", "rot", "rot_y", "escala", "cor", "material", "textura", "comportamentos", "ativo",
                                            "jogador", "vida", "dano", "velocidade", "alcance", "pontos", "raio", "intervalo", "maximo",
                                            "total", "cura", "arma_inimigo", "destino", "intensidade"};
            bool ok = false;
            for (const char* p : inPlace) ok |= k == p;
            if (!ok) rebuild = true;
        }
        if (rebuild) {  // muda o que o objeto é: recria com a receita atualizada
            json spec = json::parse(exportProject(e, true))["objetos"];
            json cur;
            for (auto& o : spec) if (o.value("id", -1) == g->id) cur = o;
            for (auto& [k, v] : in.items()) cur[k] = v;
            cur.erase("id");
            deleteObject(e, Scene::handleOf(*g));
            std::string m;
            int id = createFromSpecJson(e, cur.dump(), &m);
            return id < 0 ? errJson(m) : json{{"ok", true}, {"id", id}, {"recriado", true}}.dump();
        }
        int ci = charIndexOf(e, g);
        applyCommon(e, *g, in, ci >= 0);
        if (in.contains("jogador") && ci >= 0 && flag(in, "jogador", false)) setPlayer(e, ci);
        for (auto& [k, v] : in.items()) {
            if (k == "id" || k == "nome" || k == "pos" || k == "rot" || k == "rot_y" || k == "escala" || k == "jogador") continue;
            if (k == "material") { r.erase("cor"); r.erase("textura"); }
            if (k == "textura") r.erase("cor");
            r[k] = v;
        }
        if (in.contains("intensidade") || in.contains("raio") || in.contains("cor"))
            for (auto& l : e->lightObjects)
                if (e->scene.get(l.obj) == g) {
                    l.intensity = num(in, "intensidade", l.intensity);
                    l.radius = num(in, "raio", l.radius);
                    glm::vec3 c;
                    if (in.contains("cor") && colorOf(in["cor"], c)) l.color = c;
                }
        e->recipes[g->recipe] = r.dump();
        ++e->revision;
        return json{{"ok", true}, {"id", g->id}}.dump();
    }
    if (n == "remover_objetos") {
        auto it = in.find("ids");
        if (it == in.end() || !it->is_array()) return errJson("informe \"ids\": [...]");
        int count = 0;
        for (const json& v : *it) {
            if (!v.is_number()) continue;
            if (GameObject* g = rootOf(e, objectById(e, v.get<int>()))) { deleteObject(e, Scene::handleOf(*g)); ++count; }
        }
        return json{{"removidos", count}}.dump();
    }
    if (n == "definir_regras") {
        GameRules& R = e->rules;
        if (in.contains("objetivo")) R.objetivo = norm(str(in, "objetivo", "livre"));
        R.quantidade = inum(in, "quantidade", R.quantidade);
        R.tempo = std::max(0.f, num(in, "tempo", R.tempo));
        R.vidaJogador = std::max(1.f, num(in, "vida_jogador", R.vidaJogador));
        if (in.contains("titulo")) R.titulo = str(in, "titulo");
        if (in.contains("descricao")) R.descricao = str(in, "descricao");
        static const char* valid[] = {"livre", "sobreviver", "coletar", "chegar", "eliminar", "pontos", "times"};
        bool ok = false;
        for (const char* v : valid) ok |= R.objetivo == v;
        if (!ok) { R.objetivo = "livre"; return errJson("objetivo inválido (livre, sobreviver, coletar, chegar, eliminar, pontos, times)"); }
        ++e->revision;
        return json{{"ok", true}}.dump();
    }
    if (n == "definir_ambiente") {
        if (in.contains("hora")) setTimeOfDay(e, num(in, "hora", 13.f));
        if (in.contains("exposicao")) e->env.exposure = std::clamp(num(in, "exposicao", 1.f), 0.05f, 8.f);
        if (in.contains("sol")) e->sun.intensity = std::clamp(num(in, "sol", 3.2f), 0.f, 20.f);
        if (in.contains("neblina")) e->renderer.fogDensity = std::clamp(num(in, "neblina", 0.f), 0.f, 0.05f);
        if (in.contains("ciclo_min")) e->dayCycleMin = std::clamp(num(in, "ciclo_min", 0.f), 0.f, 240.f);
        if (in.contains("chuva")) e->rainMode = lookup(str(in, "chuva"), {{"nunca", 0}, {"nao", 0}, {"automatica", 1}, {"auto", 1}, {"sempre", 2}}, 0);
        ++e->revision;
        return json{{"ok", true}}.dump();
    }
    if (n == "altura_terreno") {
        auto it = in.find("pontos");
        if (it == in.end() || !it->is_array()) return errJson("informe \"pontos\": [[x,z], ...]");
        json out = json::array();
        for (const json& p : *it)
            if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number())
                out.push_back(r3(mapHeight(e, p[0].get<float>(), p[1].get<float>())));
            else out.push_back(nullptr);
        return json{{"alturas", out}}.dump();
    }
    if (n == "adicionar_comportamento") {  // usado pelo menu Jogo do editor
        GameObject* g = rootOf(e, objectById(e, inum(in, "id", -1)));
        if (!g || g->recipe < 0) return errJson("selecione um objeto");
        json r = json::parse(e->recipes[g->recipe], nullptr, false);
        json c = in.value("comportamento", json());
        std::string tn = c.is_string() ? c.get<std::string>() : c.is_object() ? c.value("tipo", "") : "";
        if (behaviorTypeFromName(tn) < 0) return errJson("comportamento desconhecido: " + tn);
        json& list = r["comportamentos"];
        if (!list.is_array()) list = json::array();
        for (auto it = list.begin(); it != list.end(); ++it) {  // mesmo tipo: substitui
            std::string t = it->is_string() ? it->get<std::string>() : it->is_object() ? it->value("tipo", "") : "";
            if (behaviorTypeFromName(t) == behaviorTypeFromName(tn)) { list.erase(it); break; }
        }
        if (in.value("remover", false)) {} else list.push_back(c);
        if (list.empty()) r.erase("comportamentos");
        e->recipes[g->recipe] = r.dump();
        ++e->revision;
        return json{{"ok", true}, {"comportamentos", r.value("comportamentos", json::array())}}.dump();
    }
    if (n == "focar_camera") {
        glm::vec3 t;
        if (vec3Of(in, "alvo", t)) e->editorCam.target = t;
        e->editorCam.distance = std::clamp(num(in, "distancia", e->editorCam.distance), 0.3f, 5000.f);
        return json{{"ok", true}}.dump();
    }
    return errJson("ferramenta desconhecida: " + name);
}

// Skin de arma: troca só a textura de cor (o resto do material e o UV continuam os do modelo)
void applyKitTexture(Engine* e, GameObject& g, const std::string& kitName) {
    applyMaterialTo(e, g, true, json{{"textura", kitPath(e, kitName)}});
}

}  // namespace eng
