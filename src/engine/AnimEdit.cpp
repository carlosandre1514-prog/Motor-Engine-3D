// Animações: retarget humanoide para qualquer modelo importado, animações personalizadas (salvas no projeto)
// e o editor de animação (pose osso a osso + quadros-chave).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "engine/Engine.h"
#include "json.hpp"
#include "scene/Humanoid.h"

namespace eng {

using json = nlohmann::json;

namespace {

const char* kJointLabel[CJ_Count] = {"Quadril", "Coluna", "Peito", "Cabeça", "Braço esq.", "Antebraço esq.", "Mão esq.",
                                     "Braço dir.", "Antebraço dir.", "Mão dir.", "Coxa esq.", "Canela esq.", "Pé esq.",
                                     "Coxa dir.", "Canela dir.", "Pé dir."};

std::string boneName(const Skeleton& S, int j) { return S.name[j][0] ? S.name[j] : "osso_" + std::to_string(j); }

// Nome em português para ossos fora dos 16 principais (pescoço, dedos, ombros...) — padrões Mixamo/Unity/Blender
std::string extraLabel(const std::string& raw) {
    std::string n;
    for (char c : raw) n += char(std::tolower((unsigned char)c));
    std::string side = n.find("left") != std::string::npos || n.find(".l") != std::string::npos || n.find("_l") != std::string::npos ? " esq."
                     : n.find("right") != std::string::npos || n.find(".r") != std::string::npos || n.find("_r") != std::string::npos ? " dir." : "";
    std::string seg;
    for (char c : n) if (std::isdigit((unsigned char)c)) seg = std::string(1, c);
    static const std::pair<const char*, const char*> parts[] = {
        {"thumb", "Polegar"}, {"index", "Indicador"}, {"middle", "Médio"}, {"ring", "Anelar"}, {"pinky", "Mínimo"}, {"little", "Mínimo"},
        {"neck", "Pescoço"}, {"shoulder", "Ombro"}, {"clavicle", "Clavícula"}, {"toe", "Dedos do pé"}, {"eye", "Olho"}, {"jaw", "Queixo"}};
    for (auto& [k, v] : parts)
        if (n.find(k) != std::string::npos) return std::string(v) + (seg.empty() ? "" : " " + seg) + side;
    return "";
}

ClipStore& storeFor(Engine* e, Animator* a) {
    for (auto& s : e->clipStores) if (s->anim == a) return *s;
    auto s = std::make_unique<ClipStore>();
    s->anim = a;
    if (a->retarget) s->own.assign(a->ownClips, a->ownClips + a->ownCount);
    else s->own.assign(a->clips, a->clips + a->clipCount);
    e->clipStores.push_back(std::move(s));
    return *e->clipStores.back();
}

// Monta canais de rotação (um por osso) a partir dos quadros
void buildChannels(const Skeleton& S, const std::vector<std::pair<float, std::vector<glm::quat>>>& keys,
                   std::vector<Channel>& ch, std::vector<float>& data) {
    const int n = S.jointCount, k = int(keys.size());
    data.assign(size_t(k) + size_t(n) * k * 4, 0.f);
    for (int f = 0; f < k; ++f) data[f] = keys[f].first;
    ch.assign(n, Channel{});
    for (int j = 0; j < n; ++j) {
        float* v = data.data() + k + size_t(j) * k * 4;
        glm::quat prev = keys[0].second[j];
        for (int f = 0; f < k; ++f) {
            glm::quat q = keys[f].second[j];
            if (glm::dot(q, prev) < 0.f) q = -q;  // caminho curto
            prev = q;
            v[f * 4] = q.x; v[f * 4 + 1] = q.y; v[f * 4 + 2] = q.z; v[f * 4 + 3] = q.w;
        }
        ch[j].joint = j;
        ch[j].path = ChannelPath::Rotation;
        ch[j].keyCount = k;
        ch[j].times = data.data();
        ch[j].values = v;
    }
}

int addClip(Engine* e, Animator* a, const std::string& name, const std::vector<std::pair<float, std::vector<glm::quat>>>& keys) {
    if (keys.empty() || !a->skeleton) return -1;
    ClipStore& st = storeFor(e, a);
    st.channels.emplace_back();
    st.data.emplace_back();
    buildChannels(*a->skeleton, keys, st.channels.back(), st.data.back());
    AnimationClip c;
    std::snprintf(c.name, sizeof(c.name), "%s", name.c_str());
    c.duration = std::max(keys.back().first, 0.05f);
    c.channelCount = int(st.channels.back().size());
    c.channels = st.channels.back().data();
    st.own.push_back(c);
    if (a->retarget) { a->ownClips = st.own.data(); a->ownCount = int(st.own.size()); a->clipCount = a->refCount + a->ownCount; }
    else { a->clips = st.own.data(); a->clipCount = int(st.own.size()); }
    return a->clipCount - 1;
}

// pose atual (o que está na tela) -> rotação local de cada osso
void captureLocal(Animator* a) {
    const Skeleton& S = *a->skeleton;
    a->update(0.f, false);
    for (int j = 0; j < S.jointCount; ++j) {
        int p = S.parent[j];
        glm::mat4 local = p < 0 ? a->global[j] : glm::inverse(a->global[p]) * a->global[j];
        glm::mat3 m(local);
        for (int c = 0; c < 3; ++c) m[c] = glm::normalize(m[c]);
        a->poseR[j] = glm::normalize(glm::quat_cast(m));
    }
}

std::vector<glm::quat> currentPose(Animator* a) {
    return std::vector<glm::quat>(a->poseR, a->poseR + a->skeleton->jointCount);
}

}  // namespace

Animator* animatorOf(GameObject& root) {
    Animator* anim = nullptr;
    Scene::forEachInSubtree(root, [&](GameObject& g) { if (!anim && g.animator && g.animator->skeleton) anim = g.animator; });
    return anim;
}

bool setupHumanoid(Engine* e, GameObject& root, Animator* anim) {
    if (!anim || !anim->skeleton) return false;
    if (anim->retarget) return true;
    if (!e->humanoidRef) {
        e->humanoidRef = std::make_unique<LoadedModel>();
        MeshBuilder unused;
        buildCharacter(defaultCharacter(), *e->humanoidRef, unused);
    }
    // o próprio esqueleto do motor (personagem exportado e reimportado) já tem as animações dele
    if (anim->skeleton->jointCount == e->humanoidRef->skeletons[0].jointCount && anim->clipCount >= CC_Count) return false;
    e->scene.updateTransforms();
    auto rt = std::make_unique<Retarget>();
    glm::mat4 spaceW = anim->space ? anim->space->world() : glm::mat4(1.f);
    if (!buildRetarget(*anim->skeleton, e->humanoidRef->skeletons[0], spaceW, *rt)) return false;
    // O motor usa personagem olhando para +Z (ombro esquerdo em +X). Modelo virado para -Z: gira 180°.
    auto bindPos = [&](int j) { return glm::vec3((spaceW * anim->skeleton->bindToSkel * glm::inverse(anim->skeleton->inverseBind[j]))[3]); };
    glm::vec3 left = bindPos(rt->targetOf[CJ_LUpper]) - bindPos(rt->targetOf[CJ_RUpper]);
    glm::vec3 lx = glm::inverse(glm::mat3(root.transform.world())) * left;
    if (lx.x < 0.f) {
        glm::quat flip = glm::angleAxis(3.14159265f, glm::vec3(0, 1, 0));
        for (Transform3D* c = root.transform.firstChild; c; c = c->nextSibling) {
            c->setPosition(flip * c->position);
            c->setRotation(flip * c->rotation);
        }
        e->scene.updateTransforms();
        spaceW = anim->space ? anim->space->world() : glm::mat4(1.f);
        if (!buildRetarget(*anim->skeleton, e->humanoidRef->skeletons[0], spaceW, *rt)) return false;
    }
    // lista: animações do motor primeiro, depois as do modelo
    ClipStore* st = nullptr;
    for (auto& s : e->clipStores) if (s->anim == anim) st = s.get();
    anim->ownClips = st ? st->own.data() : anim->clips;
    anim->ownCount = anim->clipCount;
    anim->clips = e->humanoidRef->clips.data();
    anim->refCount = int(e->humanoidRef->clips.size());
    anim->clipCount = anim->refCount + anim->ownCount;
    anim->retarget = rt.get();
    e->retargets.push_back(std::move(rt));
    anim->setClip(anim->ownCount > 0 ? anim->refCount : CC_Idle);  // começa na animação do próprio arquivo, se tiver
    anim->update(0.f, false);
    return true;
}

void applyCustomAnims(Engine* e, GameObject& root, const std::string& arrJson) {
    Animator* a = animatorOf(root);
    json arr = json::parse(arrJson, nullptr, false);
    if (!a || !arr.is_array()) return;
    const Skeleton& S = *a->skeleton;
    for (const json& c : arr) {
        if (!c.is_object()) continue;
        std::vector<std::pair<float, std::vector<glm::quat>>> keys;
        if (auto q = c.find("quadros"); q != c.end() && q->is_array())
            for (const json& f : *q) {
                std::vector<glm::quat> pose(S.bindR, S.bindR + S.jointCount);
                if (auto b = f.find("ossos"); b != f.end() && b->is_object())
                    for (auto& [name, v] : b->items()) {
                        if (!v.is_array() || v.size() < 4) continue;
                        for (int j = 0; j < S.jointCount; ++j)
                            if (boneName(S, j) == name) { pose[j] = glm::normalize(glm::quat(v[3].get<float>(), v[0].get<float>(), v[1].get<float>(), v[2].get<float>())); break; }
                    }
                keys.push_back({f.value("t", 0.f), pose});
            }
        std::sort(keys.begin(), keys.end(), [](auto& x, auto& y) { return x.first < y.first; });
        addClip(e, a, c.value("nome", "Animação"), keys);
    }
}

void forgetClipStores(Engine* e, const std::vector<Animator*>& gone) {
    e->clipStores.erase(std::remove_if(e->clipStores.begin(), e->clipStores.end(), [&](const std::unique_ptr<ClipStore>& s) {
        return std::find(gone.begin(), gone.end(), s->anim) != gone.end();
    }), e->clipStores.end());
    if (e->animEdit.active && std::find(gone.begin(), gone.end(), e->animEdit.anim) != gone.end()) e->animEdit = AnimEditState{};
}

// ---------------- editor ----------------
static AnimEditState* edit(Engine* e) {
    AnimEditState& s = e->animEdit;
    if (!s.active || !s.anim || !e->scene.get(s.root)) { s = AnimEditState{}; return nullptr; }
    return &s;
}

int animEditBegin(Engine* e) {
    animEditEnd(e);
    GameObject* root = rootOf(e, e->scene.get(e->selected));
    if (!root) { e->lastError = "toque primeiro no personagem ou modelo"; return -1; }
    Animator* a = animatorOf(*root);
    if (!a) { e->lastError = "esse modelo não tem esqueleto (ossos): não dá para animar"; return -1; }
    AnimEditState& s = e->animEdit;
    s.active = true;
    s.root = Scene::handleOf(*root);
    s.anim = a;
    captureLocal(a);  // começa da pose que está na tela
    a->posing = true;
    return a->skeleton->jointCount;
}

std::string animEditBones(Engine* e) {
    AnimEditState* s = edit(e);
    if (!s) return "";
    const Skeleton& S = *s->anim->skeleton;
    std::string out;
    for (int j = 0; j < S.jointCount; ++j) {
        std::string label = boneName(S, j);
        std::string extra = extraLabel(label);
        int h = humanoidBone(S.name[j]);
        if (!extra.empty()) label = extra + " (" + label + ")";
        else if (h >= 0 && h < CJ_Count) label = std::string(kJointLabel[h]) + " (" + label + ")";
        out += label + "\n";
    }
    return out;
}

void animEditSelect(Engine* e, int bone) { if (AnimEditState* s = edit(e)) s->bone = bone; }

int animEditSelected(Engine* e) { AnimEditState* s = edit(e); return s ? s->bone : -1; }

// Toque na tela durante a edição: escolhe o osso mais perto do dedo
bool animEditPick(Engine* e, glm::vec2 px) {
    AnimEditState* s = edit(e);
    if (!s) return false;
    Animator* a = s->anim;
    glm::mat4 sp = a->space ? a->space->world() : glm::mat4(1.f);
    glm::mat4 vp = e->camera.viewProj();
    float best = 60.f * 60.f;
    int pick = -1;
    for (int j = 0; j < a->skeleton->jointCount; ++j) {
        glm::vec4 c = vp * (sp * a->global[j][3]);
        if (c.w <= 0.f) continue;
        glm::vec2 sc((c.x / c.w * 0.5f + 0.5f) * e->width, (0.5f - c.y / c.w * 0.5f) * e->height);
        glm::vec2 d = sc - px;
        if (glm::dot(d, d) < best) { best = glm::dot(d, d); pick = j; }
    }
    if (pick < 0) return false;
    s->bone = pick;
    return true;
}

void animEditGet(Engine* e, int bone, float* eul) {
    eul[0] = eul[1] = eul[2] = 0.f;
    AnimEditState* s = edit(e);
    if (!s || bone < 0 || bone >= s->anim->skeleton->jointCount) return;
    glm::vec3 d = glm::degrees(glm::eulerAngles(glm::inverse(s->anim->skeleton->bindR[bone]) * s->anim->poseR[bone]));
    eul[0] = d.x; eul[1] = d.y; eul[2] = d.z;
}

void animEditSet(Engine* e, int bone, const float* eul) {
    AnimEditState* s = edit(e);
    if (!s || bone < 0 || bone >= s->anim->skeleton->jointCount) return;
    s->anim->preview = nullptr;
    s->anim->posing = true;
    s->anim->poseR[bone] = glm::normalize(s->anim->skeleton->bindR[bone] * glm::quat(glm::radians(glm::vec3(eul[0], eul[1], eul[2]))));
}

void animEditResetPose(Engine* e) {
    AnimEditState* s = edit(e);
    if (!s) return;
    const Skeleton& S = *s->anim->skeleton;
    std::copy(S.bindR, S.bindR + S.jointCount, s->anim->poseR);
}

int animEditKey(Engine* e, float t) {
    AnimEditState* s = edit(e);
    if (!s) return 0;
    t = std::max(0.f, t);
    for (auto& k : s->keys) if (std::abs(k.first - t) < 0.01f) { k.second = currentPose(s->anim); return int(s->keys.size()); }
    s->keys.push_back({t, currentPose(s->anim)});
    std::sort(s->keys.begin(), s->keys.end(), [](auto& x, auto& y) { return x.first < y.first; });
    return int(s->keys.size());
}

void animEditDeleteKey(Engine* e, float t) {
    AnimEditState* s = edit(e);
    if (!s) return;
    s->keys.erase(std::remove_if(s->keys.begin(), s->keys.end(), [&](auto& k) { return std::abs(k.first - t) < 0.01f; }), s->keys.end());
}

void animEditGoto(Engine* e, float t) {
    AnimEditState* s = edit(e);
    if (!s) return;
    for (auto& k : s->keys)
        if (std::abs(k.first - t) < 0.01f) {
            std::copy(k.second.begin(), k.second.end(), s->anim->poseR);
            s->anim->preview = nullptr;
            s->anim->posing = true;
        }
}

std::string animEditKeys(Engine* e) {
    AnimEditState* s = edit(e);
    std::string out;
    if (!s) return out;
    char b[16];
    for (auto& k : s->keys) { std::snprintf(b, sizeof(b), "%.2f ", k.first); out += b; }
    return out;
}

bool animEditPreview(Engine* e, bool on) {
    AnimEditState* s = edit(e);
    if (!s) return false;
    Animator* a = s->anim;
    if (!on || s->keys.size() < 2) {
        a->preview = nullptr;
        a->posing = true;
        return false;
    }
    buildChannels(*a->skeleton, s->keys, s->previewChannels, s->previewData);
    s->previewClip = AnimationClip{};
    std::snprintf(s->previewClip.name, sizeof(s->previewClip.name), "prévia");
    s->previewClip.duration = s->keys.back().first;
    s->previewClip.channelCount = int(s->previewChannels.size());
    s->previewClip.channels = s->previewChannels.data();
    a->preview = &s->previewClip;
    a->posing = false;
    a->time = 0.f;
    return true;
}

int animEditSave(Engine* e, const std::string& rawName) {
    AnimEditState* s = edit(e);
    if (!s) return -1;
    if (s->keys.empty()) animEditKey(e, 0.f);  // só uma pose: vira uma "animação" parada
    if (s->keys.size() == 1) s->keys.push_back({std::max(1.f, s->keys[0].first + 1.f), s->keys[0].second});
    std::string name = rawName.empty() ? "Minha animação" : rawName.substr(0, 31);
    Animator* a = s->anim;
    a->preview = nullptr;
    a->posing = false;
    int idx = addClip(e, a, name, s->keys);
    // grava na receita do objeto (volta ao abrir o projeto e vai junto no .glb exportado)
    if (GameObject* root = e->scene.get(s->root); root && root->recipe >= 0 && root->recipe < int(e->recipes.size())) {
        json r = json::parse(e->recipes[root->recipe], nullptr, false);
        if (r.is_object()) {
            const Skeleton& S = *a->skeleton;
            json frames = json::array();
            for (auto& k : s->keys) {
                json bones = json::object();
                for (int j = 0; j < S.jointCount; ++j) {
                    glm::quat q = k.second[j];
                    if (std::abs(glm::dot(q, S.bindR[j])) > 0.99999f) continue;  // igual ao repouso: não precisa salvar
                    bones[boneName(S, j)] = {std::round(q.x * 1e5) / 1e5, std::round(q.y * 1e5) / 1e5, std::round(q.z * 1e5) / 1e5, std::round(q.w * 1e5) / 1e5};
                }
                frames.push_back({{"t", std::round(k.first * 100.f) / 100.f}, {"ossos", bones}});
            }
            if (!r["animacoes"].is_array()) r["animacoes"] = json::array();
            r["animacoes"].push_back({{"nome", name}, {"quadros", frames}});
            e->recipes[root->recipe] = r.dump(-1, ' ', false, json::error_handler_t::replace);
        }
    }
    ++e->revision;
    s->keys.clear();
    a->setClip(idx);
    a->posing = false;
    s->active = false;
    return idx;
}

void animEditEnd(Engine* e) {
    AnimEditState& s = e->animEdit;
    if (s.anim && e->scene.get(s.root)) { s.anim->posing = false; s.anim->preview = nullptr; }
    s = AnimEditState{};
}

void drawAnimEditOverlay(Engine* e, const glm::mat4& vp) {
    AnimEditState* s = edit(e);
    if (!s) return;
    Animator* a = s->anim;
    const Skeleton& S = *a->skeleton;
    glm::mat4 sp = a->space ? a->space->world() : glm::mat4(1.f);
    for (int j = 0; j < S.jointCount; ++j) {
        glm::vec3 pj(sp * a->global[j][3]);
        int p = S.parent[j];
        bool sel = j == s->bone;
        if (p >= 0) {
            glm::vec3 pp(sp * a->global[p][3]);
            e->renderer.line(vp, pp, pj, sel ? glm::vec4(1.f, 0.55f, 0.1f, 1.f) : glm::vec4(0.3f, 0.85f, 1.f, 0.8f), sel ? 5.f : 2.f, false);
        }
        if (sel) {  // cruz no osso selecionado
            float r = 0.06f;
            for (int ax = 0; ax < 3; ++ax) {
                glm::vec3 d(0.f); d[ax] = r;
                e->renderer.line(vp, pj - d, pj + d, glm::vec4(1.f, 0.85f, 0.2f, 1.f), 4.f, false);
            }
        }
    }
}

}  // namespace eng
