// Lógica de jogo: comportamentos dos objetos (inimigos, coletáveis, objetivo, spawner...), dano e regras de vitória/derrota.
// Os comportamentos vêm da receita de cada objeto (Project.cpp): o "tipo" já traz o comportamento principal
// (inimigo, coletavel, objetivo, spawner, zona_dano) e "comportamentos": [...] acrescenta outros.
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include "engine/Engine.h"
#include "json.hpp"

namespace eng {

using json = nlohmann::json;

namespace {

bool sameHandle(Scene::Handle a, Scene::Handle b) { return a.index == b.index && a.gen == b.gen; }

CharacterInfo* charOf(Engine* e, GameObject* g) {
    for (auto& c : e->characters) if (g && e->scene.get(c.root) == g) return &c;
    return nullptr;
}

CharacterInfo* playerChar(Engine* e) {
    if (e->player < 0 || e->player >= int(e->characters.size())) return nullptr;
    CharacterInfo& c = e->characters[e->player];
    return e->scene.get(c.root) ? &c : nullptr;
}

bool isGunKind(int k) { return k == WK_Pistol || k == WK_Rifle || k == WK_Shotgun || k == WK_Sniper; }

void setSubtreeActive(GameObject& g, bool on) { Scene::forEachInSubtree(g, [&](GameObject& o) { o.active = on; }); }

void hideObject(Engine* e, GameObject& g) {
    if (!g.active) return;
    setSubtreeActive(g, false);
    e->game.hidden.push_back(Scene::handleOf(g));
    if (g.rigidBody) { e->physics.setEnabled(*g.rigidBody, false); e->game.disabledBodies.push_back(g.rigidBody); }
}

void hurtPlayer(Engine* e, float dmg) {
    if (e->game.state != 0 || dmg <= 0.f) return;
    e->game.playerHp = std::max(0.f, e->game.playerHp - dmg);
    e->game.hurtTimer = 0.35f;
}

float fnum(const json& j, const char* k, float def) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<float>() : def;
}

int typeFromTipo(const std::string& tipo) {
    if (tipo == "inimigo") return BH_Enemy;
    if (tipo == "coletavel") return BH_Collect;
    if (tipo == "objetivo") return BH_Goal;
    if (tipo == "spawner") return BH_Spawner;
    if (tipo == "zona_dano") return BH_DamageZone;
    return -1;
}

// Valores padrão de cada comportamento
void defaults(Behavior& b, const std::string& item) {
    switch (b.type) {
        case BH_Enemy: b.vida = 100.f; b.velocidade = 3.2f; b.dano = 10.f; b.alcance = 25.f; b.intervalo = 1.1f; b.pontos = 10; break;
        case BH_Health: b.vida = 60.f; b.pontos = 5; break;
        case BH_Explosive: b.vida = 30.f; b.dano = 70.f; b.raio = 5.f; b.pontos = 5; break;
        case BH_Collect:
            b.raio = 1.3f;
            b.pontos = item == "cristal" ? 50 : item == "vida" || item == "municao" ? 0 : 10;
            b.cura = item == "vida" ? 30.f : 0.f;
            break;
        case BH_Goal: b.raio = 2.5f; break;
        case BH_Spawner: b.intervalo = 4.f; b.maximo = 5; b.total = 0; b.alcance = 60.f; b.vida = 60.f; b.velocidade = 3.f; b.dano = 8.f; break;
        case BH_DamageZone: b.raio = 2.f; b.dano = 20.f; break;
        case BH_Rotate: b.velocidade = 90.f; break;
        case BH_Float: b.raio = 0.25f; b.velocidade = 2.f; break;
        case BH_Platform: b.velocidade = 2.f; b.destino = {0.f, 3.f, 0.f}; break;
        default: break;
    }
}

// Parâmetros vindos do JSON (do objeto e, por cima, os do próprio comportamento)
void readParams(Behavior& b, const json& j) {
    b.vida = fnum(j, "vida", b.vida);
    b.velocidade = fnum(j, "velocidade", b.velocidade);
    b.dano = fnum(j, "dano", b.dano);
    b.alcance = fnum(j, "alcance", b.alcance);
    b.raio = fnum(j, b.type == BH_Float ? "altura" : "raio", b.raio);
    b.intervalo = std::max(0.1f, fnum(j, "intervalo", b.intervalo));
    b.pontos = int(fnum(j, "pontos", float(b.pontos)));
    b.maximo = int(fnum(j, "maximo", float(b.maximo)));
    b.total = int(fnum(j, "total", float(b.total)));
    b.cura = fnum(j, "cura", b.cura);
    if (auto it = j.find("arma_inimigo"); it != j.end() && it->is_string()) b.armaInimigo = it->get<std::string>();
    if (auto it = j.find("destino"); it != j.end() && it->is_array() && it->size() >= 3)
        for (int i = 0; i < 3; ++i) if ((*it)[i].is_number()) b.destino[i] = (*it)[i].get<float>();
}

void addBehavior(Engine* e, GameObject& g, int type, const json& objSpec, const json* own, const std::string& item) {
    Behavior b;
    b.type = type;
    b.obj = Scene::handleOf(g);
    defaults(b, item);
    readParams(b, objSpec);
    if (own) readParams(b, *own);
    b.hp = b.vida;
    b.timer = b.type == BH_Spawner ? 1.f : 0.5f;
    b.start = g.transform.position;
    b.startRot = g.transform.rotation;
    if (b.type == BH_Platform && g.rigidBody && g.rigidBody->mass <= 0.f) {
        e->physics.setKinematic(*g.rigidBody, true);
        b.kinematic = true;
    }
    e->game.behaviors.push_back(b);
}

void behaviorsFor(Engine* e, GameObject& g) {
    if (g.recipe < 0 || g.recipe >= int(e->recipes.size())) return;
    json r = json::parse(e->recipes[g.recipe], nullptr, false);
    if (r.is_discarded() || !r.is_object()) return;
    std::string tipo = r.value("tipo", "");
    std::string item = r.value("item", "");
    int main = typeFromTipo(tipo);
    if (main >= 0) addBehavior(e, g, main, r, nullptr, item);
    if (main == BH_Collect) {  // coletável sempre gira e flutua
        addBehavior(e, g, BH_Rotate, json::object(), nullptr, item);
        addBehavior(e, g, BH_Float, json::object(), nullptr, item);
    }
    if (auto it = r.find("comportamentos"); it != r.end() && it->is_array())
        for (const json& c : *it) {
            std::string name = c.is_string() ? c.get<std::string>() : c.is_object() ? c.value("tipo", "") : "";
            int t = behaviorTypeFromName(name);
            if (t < 0 || t == main) continue;
            addBehavior(e, g, t, r, c.is_object() ? &c : nullptr, item);
        }
}

void dieBehavior(Engine* e, Behavior& b, GameObject& g) {
    b.dead = true;
    b.deadTime = 0.f;
    b.startRot = g.transform.rotation;
    e->game.score += b.pontos;
    glm::vec3 pos = g.transform.worldPosition();
    if (b.type == BH_Enemy) {
        ++e->game.killed;
        if (g.rigidBody) { e->physics.setEnabled(*g.rigidBody, false); e->game.disabledBodies.push_back(g.rigidBody); }
        if (CharacterInfo* c = charOf(e, &g)) playDeath(*c);
        e->game.message = "Inimigo derrotado";
    } else if (b.type == BH_Explosive) {
        spawnEffect(e, FX_Explosion, EL_Fire, pos + glm::vec3(0, 0.5f, 0));
        hideObject(e, g);
        damageRadius(e, pos, b.raio, b.dano, true);
    } else {  // objeto com vida: quebra
        spawnEffect(e, FX_Explosion, EL_Light, pos);
        hideObject(e, g);
    }
}

// Ataque de um inimigo no jogador
void enemyAttack(Engine* e, CharacterInfo& c, GameObject& go, Behavior& b, CharacterInfo& pc, GameObject& pg) {
    c.aimTimer = isGunKind(c.weaponKind) ? 1.6f : 0.45f;
    if (!isGunKind(c.weaponKind) && c.anim && c.weaponKind >= 0) {  // golpe recomeça
        int base = armedClipBase(c.weaponKind);
        if (base >= 0 && c.clipMap[base + AP_Aim] >= 0) c.anim->setClip(c.clipMap[base + AP_Aim]);
    }
    glm::vec3 chest = go.transform.position + glm::vec3(0, c.height * 0.72f, 0);
    glm::vec3 target = pg.transform.position + glm::vec3(0, pc.height * 0.6f, 0);
    if (!isGunKind(c.weaponKind)) {  // corpo a corpo
        playSound(e, c.shotSound.empty() ? "golpe_faca" : c.shotSound.c_str(), chest, 0.8f);
        if (glm::distance(chest, target) < 2.f) {
            fxImpact(e->particles, e->fxRng, target, glm::normalize(target - chest));
            hurtPlayer(e, b.dano);
        }
        return;
    }
    glm::vec3 muzzle = chest;
    if (GameObject* w = e->scene.get(c.weapon)) muzzle = glm::vec3(w->transform.world() * glm::vec4(c.muzzle, 1.f));
    float dist = glm::distance(chest, target);
    glm::vec3 miss(e->fxRng.range(-1.f, 1.f), e->fxRng.range(-0.6f, 0.6f), e->fxRng.range(-1.f, 1.f));
    glm::vec3 aim = target + miss * (0.15f + dist * 0.035f);  // erra mais de longe
    glm::vec3 dir = glm::normalize(aim - chest);
    glm::vec3 hit; GameObject* obj = nullptr; float frac;
    btRigidBody* self = go.rigidBody ? go.rigidBody->body : nullptr;
    bool any = e->physics.raycastIgnoring(chest, chest + dir * (dist + 30.f), self, hit, obj, frac);
    glm::vec3 end = any ? hit : chest + dir * (dist + 30.f);
    addFlash(e, fxMuzzleFlash(e->particles, e->fxRng, muzzle, glm::normalize(end - muzzle)), 0.06f);
    playSound(e, c.shotSound.empty() ? "tiro_pistola" : c.shotSound.c_str(), muzzle, 1.f);
    fxBullet(e->particles, muzzle, end);
    if (any) fxImpact(e->particles, e->fxRng, hit, dir);
    if (any && rootOf(e, obj) == &pg) hurtPlayer(e, b.dano);
}

void updateEnemy(Engine* e, size_t i, float dt, CharacterInfo* pc, GameObject* pg) {
    Behavior& b = e->game.behaviors[i];
    GameObject* go = e->scene.get(b.obj);
    if (!go || !go->active) return;
    CharacterInfo* c = charOf(e, go);
    if (!c) return;
    if (b.dead) {  // animação de morte (ou, sem ela, tomba para trás) e some
        b.deadTime += dt;
        bool anim = c->anim && c->clipMap[CC_Death] >= 0;
        float k = std::min(1.f, b.deadTime / 0.5f);
        if (!anim) go->transform.setRotation(b.startRot * glm::angleAxis(-1.5f * k * k, glm::vec3(1, 0, 0)));
        if (b.deadTime > (anim ? 5.f : 4.f)) {
            hideObject(e, *go);
            if (GameObject* w = e->scene.get(c->weapon)) setSubtreeActive(*w, false), e->game.hidden.push_back(c->weapon);
        }
        return;
    }
    glm::vec3 to(0.f);
    float dist = 1e9f;
    bool target = pc && pg && e->game.state == 0;
    if (target) {
        to = pg->transform.position - go->transform.position;
        to.y = 0.f;
        dist = glm::length(to);
        if (dist < b.alcance) b.aggro = true;
        else if (dist > b.alcance * 2.5f) b.aggro = false;
    }
    bool ranged = isGunKind(c->weaponKind);
    float stop = ranged ? std::min(9.f, b.alcance * 0.5f) : 1.3f;
    glm::vec3 move(0.f);
    if (b.aggro && dist > stop && dist > 1e-3f) move = to / dist;
    float speed = b.velocidade * (dist > 6.f ? 1.f : 0.6f);
    glm::vec3 face = b.aggro && target && dist < b.alcance ? to : glm::vec3(0.f);  // inimigo mira no jogador
    driveCharacter(e, *c, move, speed, false, dt, face);
    if (!b.aggro || !target) return;
    if (dist <= stop + 0.3f && dist > 1e-3f && go->rigidBody && go->rigidBody->body) {  // parado: vira para o jogador
        glm::quat q = glm::slerp(go->transform.rotation, glm::angleAxis(std::atan2(to.x, to.z), glm::vec3(0, 1, 0)), 1.f - std::exp(-10.f * dt));
        btTransform t = go->rigidBody->body->getWorldTransform();
        t.setRotation({q.x, q.y, q.z, q.w});
        go->rigidBody->body->setWorldTransform(t);
        go->transform.setRotation(q);
    }
    b.timer -= dt;
    bool inRange = ranged ? dist <= b.alcance : dist <= 1.9f;
    if (inRange && b.timer <= 0.f) {
        b.timer = b.intervalo * e->fxRng.range(0.8f, 1.25f);
        enemyAttack(e, *c, *go, b, *pc, *pg);
    }
}

void spawnFrom(Engine* e, size_t i) {
    Behavior& s = e->game.behaviors[i];
    GameObject* sp = e->scene.get(s.obj);
    if (!sp) return;
    float a = e->fxRng.range(0.f, 6.2831853f);
    glm::vec3 p = sp->transform.worldPosition() + glm::vec3(std::cos(a), 0.f, std::sin(a)) * 1.5f;
    p.y = std::max(p.y, mapHeight(e, p.x, p.z)) + 0.05f;
    json spec = {{"tipo", "inimigo"}, {"pos", {p.x, p.y, p.z}}, {"vida", s.vida}, {"velocidade", s.velocidade}, {"dano", s.dano},
                 {"semente", int(e->fxRng.next() & 0x7fffffff) | 1}};
    if (!s.armaInimigo.empty()) spec["arma"] = s.armaInimigo;
    Scene::Handle selected = e->selected;
    int id = createFromSpecJson(e, spec.dump(), nullptr);
    e->selected = selected;
    GameObject* g = objectById(e, id);
    if (!g) return;
    char name[32];
    std::snprintf(name, sizeof(name), "Inimigo %d", ++e->game.behaviors[i].spawnedCount);
    g->setName(name);
    e->game.spawned.push_back(Scene::handleOf(*g));
    if (CharacterInfo* c = charOf(e, g)) if (GameObject* w = e->scene.get(c->weapon)) e->game.spawned.push_back(c->weapon);
    if (g->rigidBody) e->physics.syncFromTransform(*g->rigidBody);
    size_t before = e->game.behaviors.size();
    behaviorsFor(e, *g);  // pode realocar o vetor: não usar referências antigas
    for (size_t k = before; k < e->game.behaviors.size(); ++k) {
        e->game.behaviors[k].parent = int(i);
        e->game.behaviors[k].aggro = true;
    }
    ++e->game.behaviors[i].alive;
    addFlash(e, {p + glm::vec3(0, 1, 0), {0.6f, 0.2f, 1.f}, 10.f, 6.f}, 0.3f);
}

int enemyTarget(Engine* e) {
    if (e->rules.quantidade > 0) return e->rules.quantidade;
    int n = e->game.totalEnemies;
    for (auto& b : e->game.behaviors) if (b.type == BH_Spawner) { if (b.total <= 0) return -1; n += b.total; }
    return n;
}


// ================= Polícia x Bandidos (partida de times) =================
// Roupas da partida: o personagem base (kit:personagens/base) com a roupa pintada na textura ("visual") e o
// equipamento preso aos ossos (colete, cinto com carregadores e radinho, capacete, boné, corrente, relógio, brinco)
struct SkinDef { const char* name; int price; const char* visual; const char* gear; };
const SkinDef kSkins[2][5] = {
    {{"PM (farda cinza-azulada)", 0, "pm", "colete_pm cinto_tatico"},
     {"PM camuflada", 500, "pm_camuflada", "colete_pm cinto_tatico capacete_preto"},
     {"Polícia Civil", 600, "civil", "colete_civil cinto_tatico relogio_ouro"},
     {"BOPE", 800, "bope", "colete_bope cinto_tatico capacete_preto"},
     {"SWAT", 1500, "swat", "colete_swat cinto_tatico capacete_azul"}},
    {{"Camisa de time e boné", 0, "camisa_time", "bone_tras_branco corrente_ouro cinto_tatico_marrom"},
     {"Regata, ouro e tatuagem", 300, "regata", "bone_tras_vermelho corrente_ouro_grossa relogio_ouro brinco_diamante cinto_tatico_marrom"},
     {"Moletom", 500, "moletom", "bone_tras_preto relogio_ouro brinco_argola cinto_tatico_marrom"},
     {"Vermelho e preto", 800, "vermelho_preto", "bone_tras_preto corrente_ouro relogio_ouro brinco_argola cinto_tatico_marrom"},
     {"Balaclava", 1200, "balaclava_preta", "cinto_tatico relogio_ouro corrente_ouro"}}};
struct GunDef { const char* key; const char* name; int price; int team; float dmg, interval, spread; int burst, pellets; };
const GunDef kGuns[] = {
    {"pistola", "Pistola", 0, -1, 22.f, 0.4f, 0.03f, 2, 1},
    {"m4a1", "M4A1", 1200, 0, 18.f, 0.11f, 0.035f, 4, 1},
    {"m4colt", "M4 Colt", 1500, 0, 20.f, 0.12f, 0.03f, 4, 1},
    {"sniper", "Sniper", 2500, 0, 95.f, 1.4f, 0.004f, 1, 1},
    {"ak47", "AK-47", 1200, 1, 24.f, 0.13f, 0.045f, 3, 1},
    {"ak74", "AK-74", 1400, 1, 21.f, 0.11f, 0.035f, 4, 1},
    {"ak74_tatica", "AK-74 tática", 1800, 1, 22.f, 0.1f, 0.03f, 4, 1},
    {"escopeta", "Escopeta", 1000, -1, 14.f, 0.85f, 0.07f, 1, 6},
};
constexpr int kGunCount = int(sizeof(kGuns) / sizeof(kGuns[0]));
const char* kTeamName[2] = {"Polícia", "Bandidos"};
const char* kTeamIcon[2] = {"🚓", "🔫"};

const GunDef& gunOf(const std::string& key) {
    for (const GunDef& g : kGuns) if (key == g.key) return g;
    return kGuns[0];
}
int charIndexOfRoot(Engine* e, Scene::Handle root) {
    for (size_t i = 0; i < e->characters.size(); ++i) if (sameHandle(e->characters[i].root, root)) return int(i);
    return -1;
}
json recipeOf(Engine* e, const GameObject& g) {
    if (g.recipe < 0 || g.recipe >= int(e->recipes.size())) return json();
    json r = json::parse(e->recipes[g.recipe], nullptr, false);
    return r.is_object() ? r : json();
}
int teamOf(const json& r) {
    if (!r.is_object() || !r.contains("time") || !r["time"].is_string()) return -1;
    std::string t = r["time"].get<std::string>();
    return t.rfind("pol", 0) == 0 ? 0 : t.rfind("band", 0) == 0 ? 1 : -1;
}

void equipGun(Engine* e, Fighter& f) {
    int ci = charIndexOfRoot(e, f.root);
    if (ci < 0) return;
    const std::string& k = f.weapon;
    int kind = k == "pistola" ? WK_Pistol : k == "escopeta" ? WK_Shotgun : k == "sniper" ? WK_Sniper : WK_Rifle;
    WeaponLook look;
    if (kind == WK_Rifle) look.file = "kit:armas/" + k;
    int w = createWeapon(e, kind, ci, look);
    if (GameObject* wg = objectById(e, w)) e->game.spawned.push_back(Scene::handleOf(*wg));
    e->lastError.clear();
}

// Cria (ou recria, ao trocar de roupa) o personagem em jogo do lutador, em pos/yaw
void spawnFighterBody(Engine* e, Fighter& f, glm::vec3 pos, float yaw) {
    const SkinDef& sk = kSkins[f.team][std::clamp(f.skin, 0, 4)];
    json gear = json::array();
    for (const char* p = sk.gear; *p;) {
        const char* q = std::strchr(p, ' ');
        size_t n = q ? size_t(q - p) : std::strlen(p);
        if (n) gear.push_back(std::string(p, n));
        p += n + (q ? 1 : 0);
    }
    json spec = {{"tipo", "modelo"}, {"arquivo", "kit:personagens/base"}, {"personagem", true}, {"visual", sk.visual}, {"equipamento", gear}};
    spec["pos"] = {pos.x, pos.y, pos.z};
    spec["rot_y"] = glm::degrees(yaw);
    spec["nome"] = f.name;
    if (!f.bot) spec["jogador"] = true;
    Scene::Handle selected = e->selected;
    int prevPlayer = e->player;
    // o corpo antigo sai (estava na lista dos criados no Play)
    if (GameObject* old = e->scene.get(f.root)) {
        Scene::Handle oh = f.root;
        int oci = charIndexOfRoot(e, oh);
        if (oci >= 0) if (GameObject* w = e->scene.get(e->characters[oci].weapon)) deleteObject(e, Scene::handleOf(*w));
        (void)old;
        deleteObject(e, oh);
    }
    int id = createFromSpecJson(e, spec.dump(), nullptr);
    e->selected = selected;
    GameObject* g = objectById(e, id);
    if (!g) { e->lastError.clear(); return; }
    f.root = Scene::handleOf(*g);
    e->game.spawned.push_back(f.root);
    int ci = charIndexOfRoot(e, f.root);
    if (f.bot && e->player == ci) e->player = prevPlayer < int(e->characters.size()) ? prevPlayer : -1;
    if (!f.bot && ci >= 0) e->player = ci;
    if (g->rigidBody) e->physics.syncFromTransform(*g->rigidBody);
    equipGun(e, f);
}

glm::vec3 groundAt(Engine* e, glm::vec3 p) {
    glm::vec3 hit; GameObject* obj = nullptr;
    if (e->physics.raycast(p + glm::vec3(0, 30.f, 0), p - glm::vec3(0, 60.f, 0), hit, obj)) return hit + glm::vec3(0, 0.1f, 0);
    return {p.x, mapHeight(e, p.x, p.z) + 0.1f, p.z};
}

int nearestNode(const TeamMatch& M, glm::vec3 p) {
    int best = -1; float bd = 1e18f;
    for (size_t i = 0; i < M.nav.size(); ++i) {
        glm::vec3 d = M.nav[i] - p; d.y *= 2.f;
        float d2 = glm::dot(d, d);
        if (d2 < bd) { bd = d2; best = int(i); }
    }
    return best;
}
std::vector<int> findPath(const TeamMatch& M, int a, int b) {
    std::vector<int> out;
    if (a < 0 || b < 0) return out;
    const int n = int(M.nav.size());
    std::vector<float> dist(n, 1e30f);
    std::vector<int> prev(n, -1);
    std::vector<bool> done(n, false);
    dist[a] = 0.f;
    for (int it = 0; it < n; ++it) {
        int u = -1;
        for (int i = 0; i < n; ++i) if (!done[i] && (u < 0 || dist[i] < dist[u])) u = i;
        if (u < 0 || dist[u] >= 1e29f) break;
        if (u == b) break;
        done[u] = true;
        for (int v : M.navAdj[u]) {
            float w = glm::length(M.nav[v] - M.nav[u]);
            if (dist[u] + w < dist[v]) { dist[v] = dist[u] + w; prev[v] = u; }
        }
    }
    if (dist[b] >= 1e29f) return out;
    for (int v = b; v >= 0; v = prev[v]) out.push_back(v);
    std::reverse(out.begin(), out.end());
    return out;
}

glm::vec3 chestOf(Engine* e, Fighter& f, float k = 0.7f) {
    int ci = charIndexOfRoot(e, f.root);
    GameObject* g = e->scene.get(f.root);
    if (!g || ci < 0) return glm::vec3(1e9f);
    return g->transform.position + glm::vec3(0, e->characters[ci].height * k, 0);
}

void killFighter(Engine* e, int vi, int ai) {
    TeamMatch& M = e->match;
    Fighter& v = M.f[vi];
    if (v.dead) return;
    v.dead = true; v.deadTime = 0.f; ++v.deaths; v.path.clear();
    int ci = charIndexOfRoot(e, v.root);
    if (ci >= 0) playDeath(e->characters[ci]);
    if (GameObject* g = e->scene.get(v.root); g && g->rigidBody) e->physics.setEnabled(*g->rigidBody, false);
    std::string msg;
    if (ai >= 0 && ai != vi && M.f[ai].team != v.team) {
        Fighter& a = M.f[ai];
        ++M.score[a.team]; ++a.kills; a.money += 300;
        msg = std::string(kTeamIcon[a.team]) + " " + a.name + " abateu " + kTeamIcon[v.team] + " " + v.name;
        if (!a.bot) msg += "  (+$300)";
    } else msg = std::string(kTeamIcon[v.team]) + " " + v.name + " caiu";
    if (!v.bot) msg = "Você foi abatido! Renasce na base em 5 s";
    e->game.message = msg;
}

// Distância até o inimigo vivo mais perto
float enemyDist(Engine* e, int team, glm::vec3 p) {
    float best = 1e9f;
    for (Fighter& o : e->match.f) {
        if (o.team == team || o.dead) continue;
        if (GameObject* g = e->scene.get(o.root)) best = std::min(best, glm::length(g->transform.position - p));
    }
    return best;
}

// Onde renascer: um ponto do time sem inimigo por perto (sorteado entre os livres). Se todos estão tomados,
// os bandidos renascem num lugar qualquer dentro da favela longe da polícia; a polícia, no ponto mais longe dos bandidos.
glm::vec3 spawnPoint(Engine* e, int team) {
    TeamMatch& M = e->match;
    const float safe = 25.f;
    std::vector<glm::vec3> free;
    glm::vec3 far = M.base[team];
    float farD = -1.f;
    for (const glm::vec3& s : M.spawns[team]) {
        float d = enemyDist(e, team, s);
        if (d > safe) free.push_back(s);
        if (d > farD) { farD = d; far = s; }
    }
    if (!free.empty()) return free[e->fxRng.irange(0, int(free.size()) - 1)];
    if (team == 1 && !M.nav.empty()) {
        std::vector<glm::vec3> spots;
        for (const glm::vec3& n : M.nav) {
            float ax = n.x / 64.f, az = n.z / 62.f;
            if (ax * ax * ax * ax + az * az * az * az > 1.25f * 1.25f * 1.25f * 1.25f) continue;  // fora da favela
            if (enemyDist(e, team, n) > safe + 10.f) spots.push_back(n);
        }
        if (!spots.empty()) return spots[e->fxRng.irange(0, int(spots.size()) - 1)];
    }
    return far;
}

void respawn(Engine* e, Fighter& f) {
    float a = e->fxRng.range(0.f, 6.2831853f), r = e->fxRng.range(0.5f, 2.5f);
    glm::vec3 p = groundAt(e, spawnPoint(e, f.team) + glm::vec3(std::cos(a) * r, 0.f, std::sin(a) * r));
    GameObject* g = e->scene.get(f.root);
    int ci = charIndexOfRoot(e, f.root);
    if (!g || ci < 0) return;
    g->transform.setPosition(p);
    if (g->rigidBody) { e->physics.setEnabled(*g->rigidBody, true); e->physics.syncFromTransform(*g->rigidBody); g->rigidBody->setVelocity(glm::vec3(0.f)); }
    CharacterInfo& c = e->characters[ci];
    c.dying = false; c.ammo = 30; c.reloadTimer = 0.f;
    if (c.anim && c.clipMap[CC_Idle] >= 0) { c.anim->setClip(c.clipMap[CC_Idle]); c.anim->loop = true; c.anim->speed = 1.f; }
    f.dead = false; f.hp = 100.f; f.path.clear(); f.target = -1;
    if (!f.bot) { e->game.playerHp = e->game.playerMax; e->game.message = "De volta na base"; }
}

void botShoot(Engine* e, int fi, int ti) {
    TeamMatch& M = e->match;
    Fighter& f = M.f[fi];
    int ci = charIndexOfRoot(e, f.root);
    GameObject* go = e->scene.get(f.root);
    if (ci < 0 || !go) return;
    CharacterInfo& c = e->characters[ci];
    const GunDef& gun = gunOf(f.weapon);
    glm::vec3 from = chestOf(e, f, 0.72f), to = chestOf(e, M.f[ti], 0.6f);
    float dist = glm::length(to - from);
    btRigidBody* self = go->rigidBody ? go->rigidBody->body : nullptr;
    glm::vec3 muzzle = from;
    if (GameObject* w = e->scene.get(c.weapon)) muzzle = glm::vec3(w->transform.world() * glm::vec4(c.muzzle, 1.f));
    playSound(e, c.shotSound.empty() ? "tiro_pistola" : c.shotSound.c_str(), muzzle, 1.f);
    c.aimTimer = 1.5f; c.fireAnim = 0.3f;
    for (int p = 0; p < gun.pellets; ++p) {
        float err = gun.spread + 0.012f + dist * 0.0012f;  // robô erra um pouco (mais de longe)
        glm::vec3 j(e->fxRng.range(-1.f, 1.f), e->fxRng.range(-0.7f, 0.7f), e->fxRng.range(-1.f, 1.f));
        glm::vec3 dir = glm::normalize(glm::normalize(to - from) + j * err);
        glm::vec3 hit; GameObject* obj = nullptr; float frac;
        bool any = e->physics.raycastIgnoring(from, from + dir * (dist + 25.f), self, hit, obj, frac);
        glm::vec3 end = any ? hit : from + dir * (dist + 25.f);
        if (p == 0) addFlash(e, fxMuzzleFlash(e->particles, e->fxRng, muzzle, glm::normalize(end - muzzle)), 0.06f);
        fxBullet(e->particles, muzzle, end);
        if (any) {
            fxImpact(e->particles, e->fxRng, hit, dir);
            M.attacker = fi;
            damageAt(e, obj, gun.dmg, from);
            M.attacker = -1;
        }
    }
}

bool visible(Engine* e, Fighter& a, Fighter& b) {
    GameObject* ga = e->scene.get(a.root);
    GameObject* gb = e->scene.get(b.root);
    if (!ga || !gb) return false;
    glm::vec3 from = chestOf(e, a, 0.85f), to = chestOf(e, b, 0.7f);
    glm::vec3 hit; GameObject* obj = nullptr; float frac;
    btRigidBody* self = ga->rigidBody ? ga->rigidBody->body : nullptr;
    if (!e->physics.raycastIgnoring(from, to, self, hit, obj, frac)) return true;
    return rootOf(e, obj) == gb;
}

void botUpdate(Engine* e, int fi, float dt) {
    TeamMatch& M = e->match;
    Fighter& f = M.f[fi];
    GameObject* go = e->scene.get(f.root);
    int ci = charIndexOfRoot(e, f.root);
    if (!go || ci < 0) return;
    CharacterInfo& c = e->characters[ci];
    glm::vec3 pos = go->transform.position;
    // escolhe o alvo: inimigo vivo mais perto que dá para ver
    if ((f.think -= dt) <= 0.f) {
        f.think = e->fxRng.range(0.2f, 0.35f);
        int best = -1; float bd = 55.f;
        for (size_t k = 0; k < M.f.size(); ++k) {
            Fighter& o = M.f[k];
            if (o.team == f.team || o.dead) continue;
            GameObject* og = e->scene.get(o.root);
            if (!og) continue;
            float d = glm::length(og->transform.position - pos);
            if (d < bd && visible(e, f, o)) { bd = d; best = int(k); }
        }
        if (best != f.target) f.burst = 0;
        f.target = best;
    }
    glm::vec3 move(0.f), face(0.f);
    float speed = 5.f;
    bool jump = false;
    if (f.target >= 0 && !M.f[f.target].dead && e->scene.get(M.f[f.target].root)) {
        glm::vec3 to = e->scene.get(M.f[f.target].root)->transform.position - pos;
        to.y = 0.f;
        float d = glm::length(to);
        face = to;
        speed = 2.8f;
        glm::vec3 dir = d > 1e-3f ? to / d : glm::vec3(0, 0, 1), side(-dir.z, 0.f, dir.x);
        if ((f.strafeTimer -= dt) <= 0.f) { f.strafeTimer = e->fxRng.range(0.8f, 2.2f); f.strafe = e->fxRng.chance(0.5f) ? 1.f : -1.f; }
        if (d > 24.f) move = dir;
        else if (d < 7.f) move = -dir * 0.7f + side * f.strafe * 0.5f;
        else move = side * f.strafe * 0.8f;
        // atira em rajadas
        const GunDef& gun = gunOf(f.weapon);
        if ((f.fireTimer -= dt) <= 0.f) {
            botShoot(e, fi, f.target);
            if (++f.burst >= gun.burst) { f.burst = 0; f.fireTimer = gun.interval + e->fxRng.range(0.5f, 1.2f); }
            else f.fireTimer = gun.interval;
        }
    } else {
        // sem inimigo à vista: segue pelas escadarias até um objetivo
        if (!M.nav.empty()) {
            if (f.path.empty() || f.pathPos >= f.path.size()) {
                glm::vec3 goal;
                float pick = e->fxRng.f();
                if (f.team == 0) goal = pick < 0.55f ? M.base[1] : M.nav[e->fxRng.irange(0, int(M.nav.size()) - 1)];
                else goal = pick < 0.2f ? M.base[0] : M.nav[e->fxRng.irange(0, int(M.nav.size()) - 1)];
                f.path = findPath(M, nearestNode(M, pos), nearestNode(M, goal));
                f.pathPos = 0;
            }
            if (f.pathPos < f.path.size()) {
                glm::vec3 wp = M.nav[f.path[f.pathPos]], to = wp - pos;
                to.y = 0.f;
                float d = glm::length(to);
                if (d < 1.6f) ++f.pathPos;
                else move = to / d;
            }
        } else {
            glm::vec3 to = M.base[1 - f.team] - pos;
            to.y = 0.f;
            if (glm::length(to) > 4.f) move = glm::normalize(to);
        }
    }
    // preso (parede, quina): pula e muda o caminho
    if ((f.stuckTimer += dt) > 1.5f) {
        if (glm::length(move) > 0.1f && glm::length(glm::vec2(pos.x - f.lastPos.x, pos.z - f.lastPos.z)) < 0.6f) {
            jump = true;
            if (!f.path.empty()) { if (f.pathPos + 1 < f.path.size()) ++f.pathPos; else f.path.clear(); }
            f.strafe = -f.strafe;
        }
        f.stuckTimer = 0.f;
        f.lastPos = pos;
    }
    driveCharacter(e, c, move, speed, jump, dt, face);
}

void teamStart(Engine* e) {
    TeamMatch& M = e->match;
    int chosen = M.playerTeam;
    M = TeamMatch{};
    M.playerTeam = chosen;
    if (e->rules.objetivo != "times") return;
    M.active = true;
    // bases e lojas do projeto
    glm::vec3 bsum[2]{}; int bn[2] = {0, 0}, tn[2] = {0, 0};
    glm::vec3 tsum[2]{};
    struct Slot { Scene::Handle h; int team; bool player; };
    std::vector<Slot> slots;
    Scene::Handle playerRoot = e->player >= 0 && e->player < int(e->characters.size()) ? e->characters[e->player].root : Scene::Handle{};
    for (GameObject* go : [&] { std::vector<GameObject*> v; e->scene.forEach([&](GameObject& g, Scene::Handle) { if (g.id >= 0 && !g.transform.parent && g.recipe >= 0 && g.active) v.push_back(&g); }); return v; }()) {
        json r = recipeOf(e, *go);
        std::string tipo = r.value("tipo", "");
        int team = teamOf(r);
        if (tipo == "base" && team >= 0) { bsum[team] += go->transform.position; ++bn[team]; M.spawns[team].push_back(go->transform.position); }
        else if (tipo == "loja") M.shops.push_back({Scene::handleOf(*go), r.value("loja", "armas") == "roupas" ? 1 : 0, std::max(team, 0)});
        else if (charOf(e, go)) {
            bool isPlayer = sameHandle(Scene::handleOf(*go), playerRoot);
            if (team < 0 && !isPlayer) continue;  // personagem sem time: fica de fora
            int t = isPlayer ? (M.playerTeam >= 0 ? M.playerTeam : std::max(team, 0)) : team;
            slots.push_back({Scene::handleOf(*go), t, isPlayer});
            tsum[t] += go->transform.position; ++tn[t];
        }
    }
    for (int t = 0; t < 2; ++t) M.base[t] = bn[t] ? bsum[t] / float(bn[t]) : tn[t] ? tsum[t] / float(tn[t]) : glm::vec3(t ? 0.f : 0.f, 0.f, t ? -20.f : 20.f);
    for (int t = 0; t < 2; ++t) if (M.spawns[t].empty()) M.spawns[t].push_back(M.base[t]);
    // com o jogador num time, um robô desse time sai (5 x 5)
    int perTeam[2] = {0, 0};
    for (auto& s : slots) ++perTeam[s.team];
    for (auto& s : slots) if (s.player && perTeam[s.team] > 5) {
        for (auto& b : slots) if (!b.player && b.team == s.team) { b.team = -1; --perTeam[s.team]; break; }
    }
    int pc = 0;
    for (auto& s : slots) {
        GameObject* g = e->scene.get(s.h);
        if (!g) continue;
        // o do projeto fica escondido; em jogo entra uma cópia (apagada no Stop)
        int ci = charIndexOfRoot(e, s.h);
        if (ci >= 0) if (GameObject* w = e->scene.get(e->characters[ci].weapon)) { setSubtreeActive(*w, false); e->game.hidden.push_back(e->characters[ci].weapon); }
        setSubtreeActive(*g, false);
        e->game.hidden.push_back(s.h);
        if (g->rigidBody) { e->physics.setEnabled(*g->rigidBody, false); e->game.disabledBodies.push_back(g->rigidBody); }
        if (s.team < 0) continue;
        Fighter f;
        f.original = s.h;
        f.team = s.team;
        f.bot = !s.player;
        json r = recipeOf(e, *g);
        f.name = f.bot ? std::string(g->name) : "Você";
        f.skin = std::clamp(r.value("roupa", 0), 0, 4);
        std::string arma = r.value("arma", "");
        f.weapon = arma.empty() ? "pistola" : arma;
        if (f.weapon == "fuzil") f.weapon = f.team ? "ak47" : "m4a1";
        bool ok = false;
        for (const GunDef& gd : kGuns) if (f.weapon == gd.key) ok = true;
        if (!ok) f.weapon = "pistola";
        glm::vec3 p = g->transform.position;
        if (s.player && M.playerTeam >= 0) p = groundAt(e, M.spawns[s.team][0] + glm::vec3(1.5f, 0.f, 1.5f));
        glm::vec3 fw = g->transform.rotation * glm::vec3(0, 0, 1);
        M.f.push_back(f);
        spawnFighterBody(e, M.f.back(), p, std::atan2(fw.x, fw.z));
        M.f.back().lastPos = p;
        ++pc;
    }
    if (e->mapKind == MAP_Favela) favelaNavGraph(e, M.nav, M.navAdj);
    e->game.playerHp = e->game.playerMax = 100.f;
    char buf[160];
    int pf = -1;
    for (size_t i = 0; i < M.f.size(); ++i) if (!M.f[i].bot) pf = int(i);
    std::snprintf(buf, sizeof(buf), "%s x %s — %s", kTeamName[0], kTeamName[1],
                  pf >= 0 ? (std::string("você é da ") + kTeamName[M.f[pf].team]).c_str() : "só robôs");
    e->game.message = buf;
    (void)pc;
}

void teamUpdate(Engine* e, float dt) {
    TeamMatch& M = e->match;
    GameState& G = e->game;
    for (size_t i = 0; i < M.f.size(); ++i) {
        Fighter& f = M.f[i];
        if (f.dead) {
            if ((f.deadTime += dt) > 5.f) respawn(e, f);
            continue;
        }
        if (!f.bot) {
            if (G.playerHp <= 0.f) killFighter(e, int(i), M.attacker);
            continue;
        }
        if (G.state == 0) botUpdate(e, int(i), dt);
    }
    if (G.state != 0) return;
    int need = e->rules.quantidade > 0 ? e->rules.quantidade : 25;
    int winner = M.score[0] >= need ? 0 : M.score[1] >= need ? 1 : -2;
    if (winner < -1 && e->rules.tempo > 0.f && G.time >= e->rules.tempo)
        winner = M.score[0] > M.score[1] ? 0 : M.score[1] > M.score[0] ? 1 : -1;
    if (winner >= -1) {
        int pt = -1;
        for (auto& f : M.f) if (!f.bot) pt = f.team;
        G.state = winner >= 0 && (pt < 0 || winner == pt) ? 1 : 2;
        G.message = winner < 0 ? "Empate!" : std::string("Vitória: ") + kTeamName[winner] + "!";
    }
}

// Dano em lutador (sem fogo amigo); devolve false se o atingido não é lutador
bool teamDamage(Engine* e, GameObject* root, float dmg) {
    TeamMatch& M = e->match;
    const int att = M.attacker >= 0 ? M.attacker : playerFighter(e);  // tiro sem robô atirando: foi o jogador
    for (size_t i = 0; i < M.f.size(); ++i) {
        Fighter& f = M.f[i];
        if (e->scene.get(f.root) != root) continue;
        if (f.dead) return true;
        if (att >= 0 && att < int(M.f.size()) && M.f[att].team == f.team) return true;  // sem fogo amigo
        if (!f.bot) {
            hurtPlayer(e, dmg);
            if (e->game.playerHp <= 0.f) killFighter(e, int(i), att);
        } else {
            f.hp -= dmg;
            if (att >= 0) { f.target = att; f.think = 0.3f; }  // vira para quem atirou
            if (f.hp <= 0.f) killFighter(e, int(i), att);
        }
        return true;
    }
    return false;
}

void teamStop(Engine* e) {
    TeamMatch& M = e->match;
    int chosen = M.playerTeam;
    M = TeamMatch{};
    M.playerTeam = chosen;
}
}  // namespace

int behaviorTypeFromName(const std::string& raw) {
    std::string n;
    for (char ch : raw) n += ch == ' ' || ch == '-' ? '_' : char(std::tolower((unsigned char)ch));
    static const std::pair<const char*, int> t[] = {
        {"inimigo", BH_Enemy}, {"vida", BH_Health}, {"destrutivel", BH_Health}, {"quebravel", BH_Health}, {"coletavel", BH_Collect},
        {"objetivo", BH_Goal}, {"spawner", BH_Spawner}, {"zona_dano", BH_DamageZone}, {"dano", BH_DamageZone}, {"girar", BH_Rotate},
        {"flutuar", BH_Float}, {"plataforma", BH_Platform}, {"mover", BH_Platform}, {"explosivo", BH_Explosive}};
    for (auto& [k, v] : t) if (n == k) return v;
    return -1;
}

void gameStart(Engine* e) {
    GameState& G = e->game;
    G = GameState{};
    G.playerMax = G.playerHp = std::max(1.f, e->rules.vidaJogador);
    e->scene.forEach([&](GameObject& g, Scene::Handle) {
        if (g.id >= 0 && !g.transform.parent && g.recipe >= 0) behaviorsFor(e, g);
    });
    for (auto& b : G.behaviors) {
        if (b.type == BH_Enemy) ++G.totalEnemies;
        if (b.type == BH_Collect) ++G.totalCollect;
    }
    G.message = e->rules.titulo;
    teamStart(e);  // Polícia x Bandidos (objetivo "times")
}

void gameStop(Engine* e) {
    GameState& G = e->game;
    Scene::Handle origPlayer;
    for (const Fighter& f : e->match.f) if (!f.bot) origPlayer = f.original;
    const bool hadMatch = e->match.active;
    teamStop(e);
    for (auto& b : G.behaviors)
        if (b.kinematic) if (GameObject* g = e->scene.get(b.obj); g && g->rigidBody) e->physics.setKinematic(*g->rigidBody, false);
    for (RigidBody* rb : G.disabledBodies) e->physics.setEnabled(*rb, true);
    G.disabledBodies.clear();
    for (auto h : G.hidden) if (GameObject* g = e->scene.get(h)) setSubtreeActive(*g, true);
    // criados no Play (inimigos de spawner): apagados de trás para frente
    for (auto it = G.spawned.rbegin(); it != G.spawned.rend(); ++it) deleteObject(e, *it);
    for (auto& c : e->characters) if (c.anim) c.anim->speed = 1.f;
    if (hadMatch) {  // o jogador volta a ser o personagem do projeto
        int ci = -1;
        for (size_t i = 0; i < e->characters.size(); ++i) if (sameHandle(e->characters[i].root, origPlayer)) ci = int(i);
        if (ci >= 0) e->player = ci;
        else if (e->player >= int(e->characters.size())) e->player = -1;
    }
    G = GameState{};
}

void damageAt(Engine* e, GameObject* hit, float dmg, glm::vec3 from) {
    (void)from;
    if (!e->playing || e->game.state != 0 || !hit) return;
    GameObject* root = rootOf(e, hit);
    if (e->match.active && teamDamage(e, root, dmg)) return;
    if (CharacterInfo* pc = playerChar(e); pc && e->scene.get(pc->root) == root) { hurtPlayer(e, dmg); return; }
    for (size_t i = 0; i < e->game.behaviors.size(); ++i) {
        Behavior& b = e->game.behaviors[i];
        if (b.dead || (b.type != BH_Enemy && b.type != BH_Health && b.type != BH_Explosive)) continue;
        if (e->scene.get(b.obj) != root) continue;
        b.hp -= dmg;
        b.aggro = true;
        if (b.hp <= 0.f) dieBehavior(e, e->game.behaviors[i], *root);
        return;
    }
}

void damageRadius(Engine* e, glm::vec3 c, float radius, float dmg, bool hurtPlayer_) {
    if (!e->playing || e->game.state != 0) return;
    if (hurtPlayer_)
        if (CharacterInfo* pc = playerChar(e)) {
            GameObject* pg = e->scene.get(pc->root);
            float d = glm::distance(pg->transform.position + glm::vec3(0, pc->height * 0.5f, 0), c);
            if (d < radius) hurtPlayer(e, dmg * (1.f - d / radius));
        }
    for (size_t i = 0; i < e->game.behaviors.size(); ++i) {
        Behavior b = e->game.behaviors[i];
        if (b.dead || (b.type != BH_Enemy && b.type != BH_Health && b.type != BH_Explosive)) continue;
        GameObject* g = e->scene.get(b.obj);
        if (!g || !g->active) continue;
        float d = glm::distance(g->transform.worldPosition() + glm::vec3(0, 0.8f, 0), c);
        if (d < radius) damageAt(e, g, dmg * (1.f - 0.6f * d / radius), c);
    }
}

void gameUpdate(Engine* e, float dt) {
    GameState& G = e->game;
    G.hurtTimer = std::max(0.f, G.hurtTimer - dt);
    CharacterInfo* pc = playerChar(e);
    GameObject* pg = pc ? e->scene.get(pc->root) : nullptr;
    if (G.state == 0) G.time += dt;
    glm::vec3 pcenter = pg ? pg->transform.position + glm::vec3(0, pc->height * 0.5f, 0) : glm::vec3(1e9f);

    for (size_t i = 0; i < G.behaviors.size(); ++i) {
        int type = G.behaviors[i].type;
        if (type == BH_Enemy) { updateEnemy(e, i, dt, pc, pg); continue; }
        if (type == BH_Spawner) {
            Behavior& s = G.behaviors[i];
            GameObject* sp = e->scene.get(s.obj);
            if (!sp || !sp->active || G.state != 0) continue;
            int alive = 0;
            for (auto& b : G.behaviors) if (b.parent == int(i) && b.type == BH_Enemy && !b.dead) ++alive;
            s.timer -= dt;
            bool near = glm::distance(sp->transform.worldPosition(), pcenter) < s.alcance;
            if (s.timer <= 0.f && near && alive < s.maximo && (s.total <= 0 || s.spawnedCount < s.total)) {
                s.timer = s.intervalo;
                spawnFrom(e, i);  // realoca behaviors
            }
            continue;
        }
        Behavior& b = G.behaviors[i];
        GameObject* g = e->scene.get(b.obj);
        if (!g || !g->active) continue;
        switch (type) {
            case BH_Rotate: {
                g->transform.setRotation(glm::angleAxis(glm::radians(b.velocidade) * dt, glm::vec3(0, 1, 0)) * g->transform.rotation);
                if (g->rigidBody && g->rigidBody->mass <= 0.f) e->physics.syncFromTransform(*g->rigidBody);
                break;
            }
            case BH_Float: {
                glm::vec3 p = g->transform.position;
                p.y = b.start.y + std::sin(G.time * b.velocidade) * b.raio;
                g->transform.setPosition(p);
                break;
            }
            case BH_Platform: {
                float len = std::max(0.1f, glm::length(b.destino));
                float ph = G.time * b.velocidade / len * 3.14159265f;
                g->transform.setPosition(b.start + b.destino * (0.5f - 0.5f * std::cos(ph)));
                break;
            }
            case BH_Collect: {
                if (G.state != 0 || !pg) break;
                if (glm::distance(g->transform.worldPosition(), pcenter) > b.raio + 0.4f) break;
                ++G.collected;
                G.score += b.pontos;
                if (b.cura > 0.f) G.playerHp = std::min(G.playerMax, G.playerHp + b.cura);
                spawnEffect(e, FX_Sparks, b.cura > 0.f ? EL_Poison : EL_Light, g->transform.worldPosition());
                addFlash(e, {g->transform.worldPosition(), {1.f, 0.85f, 0.4f}, 6.f, 4.f}, 0.25f);
                G.message = b.cura > 0.f ? "Vida +" + std::to_string(int(b.cura)) : b.pontos > 0 ? "+" + std::to_string(b.pontos) : "Pegou!";
                hideObject(e, *g);
                break;
            }
            case BH_Goal: {
                if (G.state != 0 || !pg || b.done) break;
                glm::vec3 d = g->transform.worldPosition() - pg->transform.position;
                if (glm::length(glm::vec2(d.x, d.z)) < b.raio && std::abs(d.y) < 4.f) b.done = true;
                break;
            }
            case BH_DamageZone: {
                if (!pg) break;
                glm::vec3 d = g->transform.worldPosition() - pg->transform.position;
                if (glm::length(glm::vec2(d.x, d.z)) < b.raio && std::abs(d.y) < 2.5f) {
                    G.playerHp = std::max(0.f, G.playerHp - b.dano * dt);
                    G.hurtTimer = 0.2f;
                }
                break;
            }
            default: break;
        }
    }

    if (e->match.active) { teamUpdate(e, dt); return; }
    if (G.state != 0) return;
    // ---- regras de vitória / derrota
    const GameRules& R = e->rules;
    auto win = [&](const char* m) { G.state = 1; G.message = m; };
    auto lose = [&](const char* m) { G.state = 2; G.message = m; };
    if (pg && pg->transform.position.y < -60.f) G.playerHp = 0.f;
    if (pg && G.playerHp <= 0.f) { lose("Você foi derrotado"); return; }
    const std::string& o = R.objetivo;
    if (o == "coletar") {
        int need = R.quantidade > 0 ? R.quantidade : G.totalCollect;
        if (need > 0 && G.collected >= need) { win("Você coletou tudo!"); return; }
    } else if (o == "eliminar") {
        int need = enemyTarget(e);
        if (need > 0 && G.killed >= need) { win("Inimigos eliminados!"); return; }
    } else if (o == "pontos") {
        if (R.quantidade > 0 && G.score >= R.quantidade) { win("Pontuação alcançada!"); return; }
    } else if (o == "chegar") {
        for (auto& b : G.behaviors) if (b.type == BH_Goal && b.done) { win("Você chegou!"); return; }
    }
    if (R.tempo > 0.f && G.time >= R.tempo) {
        if (o == "sobreviver") win("Você sobreviveu!");
        else lose("Tempo esgotado");
    }
}

std::string gameStatusText(Engine* e) {
    const GameRules& R = e->rules;
    const GameState& G = e->game;
    char buf[160];
    const std::string& o = R.objetivo;
    if (o == "coletar") std::snprintf(buf, sizeof(buf), "Colete: %d/%d", G.collected, R.quantidade > 0 ? R.quantidade : G.totalCollect);
    else if (o == "eliminar") {
        int need = enemyTarget(e);
        if (need > 0) std::snprintf(buf, sizeof(buf), "Inimigos: %d/%d", G.killed, need);
        else std::snprintf(buf, sizeof(buf), "Inimigos derrotados: %d", G.killed);
    } else if (o == "pontos") std::snprintf(buf, sizeof(buf), "Pontos: %d/%d", G.score, R.quantidade);
    else if (o == "chegar") std::snprintf(buf, sizeof(buf), "Chegue ao objetivo");
    else if (o == "sobreviver") std::snprintf(buf, sizeof(buf), "Sobreviva!");
    else if (o == "times" && e->match.active) {
        const TeamMatch& M = e->match;
        int pf = -1;
        for (size_t i = 0; i < M.f.size(); ++i) if (!M.f[i].bot) pf = int(i);
        int need = R.quantidade > 0 ? R.quantidade : 25;
        if (pf >= 0)
            std::snprintf(buf, sizeof(buf), "🚓 Polícia %d x %d Bandidos 🔫  (até %d)   💰 $%d   abates %d / mortes %d", M.score[0], M.score[1], need,
                          M.f[pf].money, M.f[pf].kills, M.f[pf].deaths);
        else std::snprintf(buf, sizeof(buf), "🚓 Polícia %d x %d Bandidos 🔫  (até %d)", M.score[0], M.score[1], need);
    }
    else buf[0] = 0;
    return buf;
}

}  // namespace eng

namespace eng {
int playerFighter(Engine* e) {
    for (size_t i = 0; i < e->match.f.size(); ++i) if (!e->match.f[i].bot) return int(i);
    return -1;
}
void teamChoose(Engine* e, int team) { e->match.playerTeam = team < 0 ? -1 : std::clamp(team, 0, 1); }

namespace {
int nearShop(Engine* e) {
    int pf = playerFighter(e);
    if (pf < 0 || e->match.f[pf].dead) return -1;
    GameObject* pg = e->scene.get(e->match.f[pf].root);
    if (!pg) return -1;
    for (size_t i = 0; i < e->match.shops.size(); ++i) {
        GameObject* s = e->scene.get(e->match.shops[i].obj);
        if (!s) continue;
        glm::vec3 front = s->transform.position + s->transform.rotation * glm::vec3(0, 0, 1.6f);
        glm::vec3 d = front - pg->transform.position;
        if (glm::length(glm::vec2(d.x, d.z)) < 2.6f && std::abs(d.y) < 2.5f) return int(i);
    }
    return -1;
}
}  // namespace

std::string shopLabel(Engine* e) {
    if (!e->match.active) return "";
    int s = nearShop(e);
    if (s < 0) return "";
    const ShopRT& sh = e->match.shops[s];
    if (sh.team != e->match.f[playerFighter(e)].team) return std::string("🔒 Loja só para ") + (sh.team ? "bandidos" : "policiais");
    return sh.kind ? (sh.team ? "👕 Loja de roupas" : "👮 Loja de fardas") : "🛒 Loja de armas";
}
bool shopInteract(Engine* e) {
    int s = nearShop(e);
    if (s < 0 || e->match.shops[s].team != e->match.f[playerFighter(e)].team) return false;
    e->match.openShop = s;
    return true;
}
void shopClose(Engine* e) { e->match.openShop = -1; }

std::string shopJson(Engine* e) {
    TeamMatch& M = e->match;
    int pf = playerFighter(e);
    if (M.openShop < 0 || M.openShop >= int(M.shops.size()) || pf < 0) return "";
    const ShopRT& sh = M.shops[M.openShop];
    Fighter& f = M.f[pf];
    json j;
    j["loja"] = sh.kind ? (sh.team ? "Loja de roupas da favela" : "Loja de fardas") : (sh.team ? "Armas da favela" : "Loja de armas");
    j["dinheiro"] = f.money;
    json items = json::array();
    auto owned = [&](int id) { return std::find(f.owned.begin(), f.owned.end(), id) != f.owned.end(); };
    if (sh.kind == 0) {
        for (int g = 0; g < kGunCount; ++g) {
            if (kGuns[g].team >= 0 && kGuns[g].team != sh.team) continue;
            items.push_back({{"id", g}, {"nome", kGuns[g].name}, {"preco", kGuns[g].price}, {"tem", kGuns[g].price == 0 || owned(g)},
                             {"usando", f.weapon == kGuns[g].key}});
        }
    } else {
        for (int k = 0; k < 5; ++k)
            items.push_back({{"id", 100 + k}, {"nome", kSkins[sh.team][k].name}, {"preco", kSkins[sh.team][k].price},
                             {"tem", kSkins[sh.team][k].price == 0 || owned(100 + k)}, {"usando", f.skin == k}});
    }
    j["itens"] = items;
    return j.dump();
}

std::string shopBuy(Engine* e, int item) {
    TeamMatch& M = e->match;
    int pf = playerFighter(e);
    if (pf < 0 || M.openShop < 0) return "Loja fechada";
    Fighter& f = M.f[pf];
    const ShopRT& sh = M.shops[M.openShop];
    bool isSkin = item >= 100;
    int price;
    std::string name;
    if (isSkin) {
        int k = item - 100;
        if (sh.kind != 1 || k < 0 || k > 4) return "Item inválido";
        price = kSkins[sh.team][k].price; name = kSkins[sh.team][k].name;
    } else {
        if (sh.kind != 0 || item < 0 || item >= kGunCount || (kGuns[item].team >= 0 && kGuns[item].team != sh.team)) return "Item inválido";
        price = kGuns[item].price; name = kGuns[item].name;
    }
    bool has = price == 0 || std::find(f.owned.begin(), f.owned.end(), item) != f.owned.end();
    if (!has) {
        if (f.money < price) return "Dinheiro insuficiente: faltam $" + std::to_string(price - f.money);
        f.money -= price;
        f.owned.push_back(item);
    }
    if (isSkin) {
        f.skin = item - 100;
        GameObject* g = e->scene.get(f.root);
        glm::vec3 p = g ? g->transform.position : M.base[f.team];
        glm::vec3 fw = g ? g->transform.rotation * glm::vec3(0, 0, 1) : glm::vec3(0, 0, 1);
        spawnFighterBody(e, f, p + glm::vec3(0, 0.05f, 0), std::atan2(fw.x, fw.z));
    } else {
        f.weapon = kGuns[item].key;
        equipGun(e, f);
    }
    return (has ? "Usando: " : "Comprou: ") + name + "  (💰 $" + std::to_string(f.money) + ")";
}
}  // namespace eng
