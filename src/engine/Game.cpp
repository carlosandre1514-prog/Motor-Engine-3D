// Lógica de jogo: comportamentos dos objetos (inimigos, coletáveis, objetivo, spawner...), dano e regras de vitória/derrota.
// Os comportamentos vêm da receita de cada objeto (Project.cpp): o "tipo" já traz o comportamento principal
// (inimigo, coletavel, objetivo, spawner, zona_dano) e "comportamentos": [...] acrescenta outros.
#include <algorithm>
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
        if (CharacterInfo* c = charOf(e, &g); c && c->anim && c->clipMap[CC_Idle] >= 0) { c->anim->setClip(c->clipMap[CC_Idle]); c->anim->speed = 0.f; }
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
    if (b.dead) {  // cai para trás e some
        b.deadTime += dt;
        float k = std::min(1.f, b.deadTime / 0.5f);
        go->transform.setRotation(b.startRot * glm::angleAxis(-1.5f * k * k, glm::vec3(1, 0, 0)));
        if (b.deadTime > 4.f) {
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
}

void gameStop(Engine* e) {
    GameState& G = e->game;
    for (auto& b : G.behaviors)
        if (b.kinematic) if (GameObject* g = e->scene.get(b.obj); g && g->rigidBody) e->physics.setKinematic(*g->rigidBody, false);
    for (RigidBody* rb : G.disabledBodies) e->physics.setEnabled(*rb, true);
    G.disabledBodies.clear();
    for (auto h : G.hidden) if (GameObject* g = e->scene.get(h)) setSubtreeActive(*g, true);
    // criados no Play (inimigos de spawner): apagados de trás para frente
    for (auto it = G.spawned.rbegin(); it != G.spawned.rend(); ++it) deleteObject(e, *it);
    for (auto& c : e->characters) if (c.anim) c.anim->speed = 1.f;
    G = GameState{};
}

void damageAt(Engine* e, GameObject* hit, float dmg, glm::vec3 from) {
    (void)from;
    if (!e->playing || e->game.state != 0 || !hit) return;
    GameObject* root = rootOf(e, hit);
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
    else buf[0] = 0;
    return buf;
}

}  // namespace eng
