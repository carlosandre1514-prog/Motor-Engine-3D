#include "procgen/Character.h"
#include "procgen/Mocap.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include "procgen/Buildings.h"
#include "procgen/Noise.h"

namespace eng {

CharacterParams defaultCharacter() {
    CharacterParams p;
    p.skin = srgb(198, 140, 105);
    p.shirt = srgb(40, 110, 190);
    p.pants = srgb(50, 55, 70);
    p.shoes = srgb(240, 240, 240);
    p.hair = srgb(35, 25, 20);
    return p;
}

CharacterParams randomCharacter(uint32_t seed) {
    Rng r(seed);
    static const int skins[][3] = {{250, 214, 185}, {232, 180, 140}, {198, 140, 105}, {150, 98, 66}, {100, 64, 42}, {70, 46, 32}};
    static const int clothes[][3] = {{200, 40, 40}, {40, 110, 190}, {240, 240, 240}, {30, 30, 35}, {250, 200, 40}, {40, 150, 80},
                                     {240, 120, 30}, {120, 70, 160}, {90, 160, 220}, {230, 120, 160}};
    static const int pants[][3] = {{50, 55, 70}, {40, 60, 110}, {30, 30, 32}, {120, 100, 80}, {90, 90, 95}, {70, 90, 60}};
    static const int hairs[][3] = {{20, 16, 14}, {60, 40, 25}, {120, 80, 40}, {200, 170, 110}, {150, 60, 30}, {110, 110, 110}};
    CharacterParams p;
    auto pick = [&](const int (*a)[3], int n) { const int* c = a[r.irange(0, n - 1)]; return srgb(c[0], c[1], c[2]); };
    p.skin = pick(skins, 6);
    p.shirt = pick(clothes, 10);
    p.pants = pick(pants, 6);
    p.shoes = pick(clothes, 10);
    p.hair = pick(hairs, 6);
    p.height = r.range(1.6f, 1.9f);
    p.build = r.irange(0, 2);
    p.hairStyle = r.irange(0, 3);
    p.longSleeves = r.chance(0.3f);
    return p;
}

const char* weaponName(int k) {
    static const char* n[] = {"Pistola", "Fuzil", "Escopeta", "Sniper", "Faca", "Taco"};
    return k >= 0 && k < WK_Count ? n[k] : "Arma";
}

int armedClipBase(int kind) {
    if (kind == WK_Rifle || kind == WK_Shotgun || kind == WK_Sniper) return CC_Rifle;
    if (kind == WK_Pistol) return CC_Pistol;
    if (kind == WK_Knife || kind == WK_Bat) return CC_Melee;
    return -1;
}

glm::mat4 weaponGripOffset(int kind, float H) {
    // origem da arma = empunhadura, um pouco abaixo do pulso (dentro da mão fechada); cano para onde apontam os dedos
    float front = (kind == WK_Knife || kind == WK_Bat) ? 0.f : 0.012f * H;
    return glm::translate(glm::mat4(1.f), glm::vec3(0.f, -0.05f * H, front)) * glm::rotate(glm::mat4(1.f), glm::radians(90.f), glm::vec3(1, 0, 0));
}

namespace {

struct Joints { glm::vec3 pos[CJ_Total]; int parent[CJ_Total]; float H = 1.75f; };

using RotFn = std::function<glm::quat(float)>;    // fase 0..2pi -> rotação local
using PosFn = std::function<glm::vec3(float)>;

glm::quat rx(float a) { return glm::angleAxis(a, glm::vec3(1, 0, 0)); }
glm::quat ry(float a) { return glm::angleAxis(a, glm::vec3(0, 1, 0)); }
glm::quat rz(float a) { return glm::angleAxis(a, glm::vec3(0, 0, 1)); }

// rotação mínima que leva a direção a até b
glm::quat arc(glm::vec3 a, glm::vec3 b) {
    a = glm::normalize(a); b = glm::normalize(b);
    float d = glm::dot(a, b);
    if (d > 0.99999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.99999f) {
        glm::vec3 ax = glm::cross(a, glm::vec3(1, 0, 0));
        if (glm::dot(ax, ax) < 1e-6f) ax = glm::cross(a, glm::vec3(0, 0, 1));
        return glm::angleAxis(3.14159265f, glm::normalize(ax));
    }
    glm::vec3 c = glm::cross(a, b);
    return glm::normalize(glm::quat(1.f + d, c.x, c.y, c.z));
}

struct ArmPose { glm::quat upper, fore, hand; };

// IK de 2 ossos: leva o pulso até `wrist` (espaço do modelo), cotovelo para o lado de `pole`, mão na rotação `handRot`.
// chest = rotação do peito (pai do braço) no espaço do modelo; as rotações devolvidas são locais (bind sem rotação).
ArmPose solveArm(const Joints& J, int up, int fo, int ha, glm::quat chest, glm::vec3 wrist, glm::vec3 pole, glm::quat handRot) {
    glm::vec3 pivot = J.pos[CJ_Chest];
    glm::vec3 S = pivot + chest * (J.pos[up] - pivot);
    float L1 = glm::length(J.pos[fo] - J.pos[up]), L2 = glm::length(J.pos[ha] - J.pos[fo]);
    glm::vec3 D = wrist - S;
    float dist = std::clamp(glm::length(D), 0.05f, (L1 + L2) * 0.999f);
    glm::vec3 u = glm::normalize(D);
    float ca = std::clamp((L1 * L1 + dist * dist - L2 * L2) / (2.f * L1 * dist), -1.f, 1.f);
    float a = std::acos(ca);
    glm::vec3 v = pole - glm::dot(pole, u) * u;
    v = glm::dot(v, v) > 1e-8f ? glm::normalize(v) : glm::normalize(glm::cross(u, glm::vec3(1, 0, 0)));
    glm::vec3 E = S + L1 * (std::cos(a) * u + std::sin(a) * v);
    glm::vec3 W = S + u * dist;
    glm::quat Mu = arc(chest * (J.pos[fo] - J.pos[up]), E - S) * chest;
    glm::quat Mf = arc(Mu * (J.pos[ha] - J.pos[fo]), W - E) * Mu;
    return {glm::normalize(glm::inverse(chest) * Mu), glm::normalize(glm::inverse(Mu) * Mf), glm::normalize(glm::inverse(Mf) * handRot)};
}

struct ClipWriter {
    LoadedModel& m;
    AnimationClip clip;
    float dur;
    size_t first;
    static constexpr int kKeys = 25;
    ClipWriter(LoadedModel& model, const char* name, float duration) : m(model), dur(duration) {
        std::snprintf(clip.name, sizeof(clip.name), "%s", name);
        clip.duration = duration;
        first = m.channelStorage.size();
    }
    void add(int joint, ChannelPath path, int comps, const std::function<void(float, float*)>& f) {
        m.keyData.emplace_back(kKeys + kKeys * comps);
        std::vector<float>& buf = m.keyData.back();
        for (int k = 0; k < kKeys; ++k) {
            float t = dur * k / (kKeys - 1);
            buf[k] = t;
            f(6.2831853f * k / (kKeys - 1), &buf[kKeys + k * comps]);
        }
        Channel c;
        c.joint = joint; c.path = path; c.keyCount = kKeys;
        c.times = buf.data(); c.values = buf.data() + kKeys;
        m.channelStorage.push_back(c);
    }
    void rot(int joint, const RotFn& f) {
        add(joint, ChannelPath::Rotation, 4, [&](float ph, float* o) { glm::quat q = glm::normalize(f(ph)); o[0] = q.x; o[1] = q.y; o[2] = q.z; o[3] = q.w; });
    }
    void pos(int joint, const PosFn& f) {
        add(joint, ChannelPath::Translation, 3, [&](float ph, float* o) { glm::vec3 v = f(ph); o[0] = v.x; o[1] = v.y; o[2] = v.z; });
    }
    void finish() {
        clip.channels = m.channelStorage.data() + first;
        clip.channelCount = int(m.channelStorage.size() - first);
        m.clips.push_back(clip);
    }
};

// Pernas e quadril de andar/correr (amp 0 = parado)
void legs(ClipWriter& w, const Joints& J, float legAmp, float knee, float bob) {
    float hipsY = J.pos[CJ_Hips].y;
    w.pos(CJ_Hips, [=](float ph) { return glm::vec3(0, hipsY - bob + bob * std::abs(std::cos(ph)), 0); });
    w.rot(CJ_Hips, [=](float ph) { return ry(0.08f * legAmp * std::sin(ph)); });
    w.rot(CJ_LThigh, [=](float ph) { return rx(-legAmp * std::sin(ph)); });
    w.rot(CJ_RThigh, [=](float ph) { return rx(legAmp * std::sin(ph)); });
    w.rot(CJ_LShin, [=](float ph) { return rx(0.08f + knee * std::max(0.f, std::cos(ph))); });
    w.rot(CJ_RShin, [=](float ph) { return rx(0.08f + knee * std::max(0.f, -std::cos(ph))); });
    w.rot(CJ_LFoot, [=](float ph) { return rx(-0.3f * knee * std::max(0.f, std::cos(ph))); });
    w.rot(CJ_RFoot, [=](float ph) { return rx(-0.3f * knee * std::max(0.f, -std::cos(ph))); });
}

// Pose de locomoção (andar / correr) sem arma
void locomotion(ClipWriter& w, const Joints& J, float legAmp, float knee, float armAmp, float elbow, float lean, float bob) {
    legs(w, J, legAmp, knee, bob);
    w.rot(CJ_Spine, [=](float) { return rx(lean); });
    w.rot(CJ_Chest, [=](float ph) { return ry(-0.12f * armAmp * std::sin(ph)) * rx(lean * 0.5f); });
    w.rot(CJ_LUpper, [=](float ph) { return rz(0.07f) * rx(armAmp * std::sin(ph)); });
    w.rot(CJ_RUpper, [=](float ph) { return rz(-0.07f) * rx(-armAmp * std::sin(ph)); });
    w.rot(CJ_LFore, [=](float ph) { return rx(-elbow - 0.15f * armAmp * std::max(0.f, std::sin(ph))); });
    w.rot(CJ_RFore, [=](float ph) { return rx(-elbow - 0.15f * armAmp * std::max(0.f, -std::sin(ph))); });
}

// Dedos: curl > 0 fecha a mão (polegar junto); trigger: indicador meio esticado (no gatilho)
void fingers(ClipWriter& w, bool left, float curl, bool trigger) {
    int b = left ? CJ_LThumb1 : CJ_RThumb1;
    float side = left ? 1.f : -1.f;
    w.rot(b, [=](float) { return rz(side * 0.5f * curl) * rx(-0.6f * curl); });
    w.rot(b + 1, [=](float) { return rx(-0.6f * curl); });
    for (int f = 1; f < 5; ++f) {
        float c = (trigger && f == 1) ? 0.35f : curl;
        w.rot(b + f * 2, [=](float) { return rx(-1.35f * c); });
        w.rot(b + f * 2 + 1, [=](float) { return rx(-1.2f * c); });
    }
}

// Poses com arma (IK): fuzil com coronha no ombro, pistola com as duas mãos, faca e taco
void armedClip(ClipWriter& w, const Joints& J, int cls, int mode) {
    const float H = J.H, k = H / 1.75f;
    bool aim = mode == AP_Aim || mode == AP_AimWalk;
    float legAmp = mode == AP_Walk || mode == AP_AimWalk ? 0.5f : mode == AP_Run ? 0.85f : 0.f;
    float knee = mode == AP_Run ? 1.4f : 0.75f, bob = mode == AP_Run ? 0.05f : legAmp > 0.f ? 0.025f : 0.f;
    if (legAmp > 0.f) legs(w, J, legAmp, knee, bob);
    else {
        float hipsY = J.pos[CJ_Hips].y;
        w.pos(CJ_Hips, [=](float ph) { return glm::vec3(0, hipsY - 0.004f + 0.004f * std::sin(ph), 0); });
        // base afastada (postura de tiro)
        w.rot(CJ_LThigh, [=](float) { return rz(0.06f) * rx(-0.12f); });
        w.rot(CJ_RThigh, [=](float) { return rz(-0.06f) * rx(0.1f); });
        w.rot(CJ_LShin, [=](float) { return rx(0.18f); });
        w.rot(CJ_RShin, [=](float) { return rx(0.12f); });
    }
    float lean = mode == AP_Run ? 0.18f : 0.06f;
    float twist = cls == CC_Melee ? 0.f : aim ? 0.28f : 0.18f;  // ombro direito para trás, esquerdo para frente
    w.rot(CJ_Spine, [=](float) { return rx(lean); });
    w.rot(CJ_Chest, [=](float) { return ry(-twist); });
    glm::quat chest = rx(lean) * ry(-twist);
    w.rot(CJ_Neck, [=](float) { return ry(twist * 0.6f); });  // a cabeça continua olhando para frente
    w.rot(CJ_Head, [=](float) { return ry(twist * 0.4f) * rx(aim ? 0.08f : 0.f); });

    const glm::vec3 fwd(0, 0, 1), up(0, 1, 0);
    glm::vec3 pivot = J.pos[CJ_Chest];
    auto shoulder = [&](int j) { return pivot + chest * (J.pos[j] - pivot); };
    // posição do pulso para que a empunhadura (offset da arma) caia em `grip`, com a mão girada em `hand`
    auto wristFor = [&](glm::vec3 grip, glm::quat hand) {
        return grip - hand * glm::vec3(0.f, -0.05f * H, 0.012f * H);
    };
    float bobSway = legAmp > 0.f ? 0.012f * k : 0.004f * k;
    std::function<void(float, ArmPose&, ArmPose&)> pose;
    if (cls == CC_Rifle) {
        pose = [=](float ph, ArmPose& R, ArmPose& L) {
            // cano: mirando = reto para frente; senão para baixo (posição de segurança)
            float pitch = aim ? 0.f : (mode == AP_Run ? -0.75f : -0.6f);
            glm::quat hand = rx(-1.5707963f - pitch);  // mão: dedos (-Y) para o cano, palma (+Z) para cima
            glm::vec3 dir = hand * glm::vec3(0, -1, 0);
            glm::vec3 pocket = shoulder(CJ_RUpper) + glm::vec3(0.07f * k, -0.04f * k, 0.03f * k);  // coronha no ombro
            glm::vec3 grip = pocket + dir * (0.30f * k) + glm::vec3(0, -0.03f * k + bobSway * std::sin(2 * ph), 0);
            R = solveArm(J, CJ_RUpper, CJ_RFore, CJ_RHand, chest, wristFor(grip, hand), glm::vec3(-1, -0.6f, -0.3f), hand);
            glm::quat lhand = hand * rz(-0.5f);
            glm::vec3 guard = grip + dir * (0.24f * k) - (hand * glm::vec3(0, 0, 1)) * (0.035f * k);
            L = solveArm(J, CJ_LUpper, CJ_LFore, CJ_LHand, chest, guard - lhand * glm::vec3(0, -0.035f * H, 0), glm::vec3(1, -1, 0), lhand);
        };
    } else if (cls == CC_Pistol) {
        pose = [=](float ph, ArmPose& R, ArmPose& L) {
            float pitch = aim ? 0.f : -0.8f;
            glm::quat hand = rx(-1.5707963f - pitch);
            glm::vec3 dir = hand * glm::vec3(0, -1, 0);
            glm::vec3 base = aim ? glm::vec3(-0.04f * k, J.pos[CJ_RUpper].y - 0.05f * k, 0.36f * k)
                                 : glm::vec3(-0.05f * k, J.pos[CJ_RUpper].y - 0.24f * k, 0.22f * k);
            glm::vec3 grip = base + glm::vec3(0, bobSway * std::sin(2 * ph), 0);
            R = solveArm(J, CJ_RUpper, CJ_RFore, CJ_RHand, chest, wristFor(grip, hand), glm::vec3(-1, -1, 0), hand);
            glm::quat lhand = hand * rz(-0.9f);  // mão de apoio por baixo/ao lado
            glm::vec3 lw = wristFor(grip, hand) + glm::vec3(0.06f * k, -0.03f * k, -0.01f * k) - dir * (0.01f * k);
            L = solveArm(J, CJ_LUpper, CJ_LFore, CJ_LHand, chest, lw, glm::vec3(1, -1, 0), lhand);
            (void)dir;
        };
    } else {
        pose = [=](float ph, ArmPose& R, ArmPose& L) {
            glm::quat hand;
            glm::vec3 grip;
            if (aim) {  // golpe: de cima/trás para frente/baixo
                float t = std::min(1.f, ph / 3.f);           // ida rápida na 1ª metade, volta depois
                float s = ph < 3.f ? t : 1.f - (ph - 3.f) / 3.2832f;
                float a = glm::mix(2.3f, -1.1f, std::clamp(s, 0.f, 1.f));
                hand = rx(-1.5707963f - a + 1.5707963f);
                grip = shoulder(CJ_RUpper) + glm::vec3(0.06f * k, -0.05f * k, 0.f) + (rx(-a) * glm::vec3(0, 0, 1)) * (0.36f * k) * 0.9f;
            } else {  // pronto: lâmina para frente na altura da cintura
                hand = rx(-1.25f);
                grip = glm::vec3(-0.1f * k, J.pos[CJ_Hips].y + 0.12f * k + bobSway * std::sin(2 * ph), 0.22f * k);
            }
            R = solveArm(J, CJ_RUpper, CJ_RFore, CJ_RHand, chest, wristFor(grip, hand), glm::vec3(-1, -1, -0.2f), hand);
            L = {rz(0.1f) * rx(legAmp * 0.6f * std::sin(ph)), rx(-0.2f - (legAmp > 0.f ? 0.4f : 0.f)), glm::quat(1, 0, 0, 0)};
        };
    }
    auto channel = [&](int joint, int which) {
        w.rot(joint, [=](float ph) {
            ArmPose R, L;
            pose(ph, R, L);
            const ArmPose& A = joint == CJ_RUpper || joint == CJ_RFore || joint == CJ_RHand ? R : L;
            return which == 0 ? A.upper : which == 1 ? A.fore : A.hand;
        });
    };
    channel(CJ_RUpper, 0); channel(CJ_RFore, 1); channel(CJ_RHand, 2);
    channel(CJ_LUpper, 0); channel(CJ_LFore, 1); channel(CJ_LHand, 2);
    fingers(w, false, 1.f, cls != CC_Melee);   // direita: empunhadura, indicador no gatilho
    fingers(w, true, cls == CC_Melee ? 0.3f : 0.85f, false);
}

void idle(ClipWriter& w, const Joints& J) {
    float hipsY = J.pos[CJ_Hips].y;
    w.pos(CJ_Hips, [=](float ph) { return glm::vec3(0, hipsY - 0.004f + 0.004f * std::sin(ph), 0); });
    w.rot(CJ_Chest, [=](float ph) { return rx(0.025f * std::sin(ph)); });
    w.rot(CJ_Neck, [=](float ph) { return ry(0.08f * std::sin(ph)); });
    w.rot(CJ_Head, [=](float ph) { return ry(0.1f * std::sin(ph)) * rx(0.03f * std::sin(2 * ph)); });
    w.rot(CJ_LUpper, [=](float ph) { return rz(0.1f + 0.02f * std::sin(ph)); });
    w.rot(CJ_RUpper, [=](float ph) { return rz(-0.1f - 0.02f * std::sin(ph)); });
    w.rot(CJ_LFore, [=](float) { return rx(-0.12f); });
    w.rot(CJ_RFore, [=](float) { return rx(-0.12f); });
    fingers(w, false, 0.25f, false);
    fingers(w, true, 0.25f, false);
}
void dance(ClipWriter& w, const Joints& J) {
    float hipsY = J.pos[CJ_Hips].y;
    w.pos(CJ_Hips, [=](float ph) { return glm::vec3(0.04f * std::sin(ph), hipsY - 0.05f + 0.05f * std::abs(std::cos(ph)), 0); });
    w.rot(CJ_Hips, [=](float ph) { return rz(0.15f * std::sin(ph)) * ry(0.25f * std::sin(ph * 0.5f)); });
    w.rot(CJ_Chest, [=](float ph) { return rz(-0.2f * std::sin(ph)); });
    w.rot(CJ_Head, [=](float ph) { return rz(0.15f * std::sin(ph + 0.5f)); });
    w.rot(CJ_LUpper, [=](float ph) { return rz(2.3f + 0.35f * std::sin(ph)); });
    w.rot(CJ_RUpper, [=](float ph) { return rz(-2.3f + 0.35f * std::sin(ph)); });
    w.rot(CJ_LFore, [=](float ph) { return rz(0.5f + 0.4f * std::sin(2 * ph)); });
    w.rot(CJ_RFore, [=](float ph) { return rz(-0.5f + 0.4f * std::sin(2 * ph)); });
    w.rot(CJ_LThigh, [=](float ph) { return rx(-0.25f - 0.2f * std::max(0.f, std::sin(ph))); });
    w.rot(CJ_RThigh, [=](float ph) { return rx(-0.25f - 0.2f * std::max(0.f, -std::sin(ph))); });
    w.rot(CJ_LShin, [=](float ph) { return rx(0.45f + 0.35f * std::max(0.f, std::sin(ph))); });
    w.rot(CJ_RShin, [=](float ph) { return rx(0.45f + 0.35f * std::max(0.f, -std::sin(ph))); });
    w.rot(CJ_LFoot, [=](float) { return rx(-0.2f); });
    w.rot(CJ_RFoot, [=](float) { return rx(-0.2f); });
}

void wave(ClipWriter& w, const Joints& J) {
    float hipsY = J.pos[CJ_Hips].y;
    w.pos(CJ_Hips, [=](float ph) { return glm::vec3(0, hipsY - 0.004f + 0.004f * std::sin(ph), 0); });
    w.rot(CJ_RUpper, [=](float) { return rz(-2.65f); });
    w.rot(CJ_RFore, [=](float ph) { return rz(-0.25f + 0.55f * std::sin(2 * ph)); });
    w.rot(CJ_LUpper, [=](float) { return rz(0.1f); });
    w.rot(CJ_Head, [=](float ph) { return rz(-0.08f + 0.04f * std::sin(ph)); });
    w.rot(CJ_Chest, [=](float ph) { return rz(-0.04f * std::sin(ph)); });
}

}  // namespace

void buildCharacter(const CharacterParams& p, LoadedModel& out, MeshBuilder& b) {
    const float H = p.height;
    const float s = p.build == 0 ? 0.85f : p.build == 2 ? 1.2f : 1.f;
    Joints J;
    const float chestHalf = 0.115f * H * s, armHalf = 0.034f * H * (0.9f + 0.1f * s);
    const float sx = chestHalf + armHalf + 0.004f * H, tx = 0.055f * H * s;
    const float shoulderY = 0.825f * H, elbowY = shoulderY - 0.175f * H, wristY = elbowY - 0.155f * H;
    const float thighY = 0.52f * H, kneeY = 0.285f * H, ankleY = 0.045f * H;
    glm::vec3 pos[CJ_Total] = {
        {0, 0.53f * H, 0}, {0, 0.62f * H, 0}, {0, 0.72f * H, 0}, {0, 0.865f * H, 0},
        {sx, shoulderY, 0}, {sx, elbowY, 0}, {sx, wristY, 0}, {-sx, shoulderY, 0}, {-sx, elbowY, 0}, {-sx, wristY, 0},
        {tx, thighY, 0}, {tx, kneeY, 0}, {tx, ankleY, 0}, {-tx, thighY, 0}, {-tx, kneeY, 0}, {-tx, ankleY, 0}};
    int parent[CJ_Total] = {-1, CJ_Hips, CJ_Spine, CJ_Neck, CJ_Chest, CJ_LUpper, CJ_LFore, CJ_Chest, CJ_RUpper, CJ_RFore,
                            CJ_Hips, CJ_LThigh, CJ_LShin, CJ_Hips, CJ_RThigh, CJ_RShin};
    pos[CJ_Neck] = {0, 0.835f * H, 0};
    parent[CJ_Neck] = CJ_Chest;
    // dedos: palma de frente (+Z); polegar e indicador do lado de fora; 2 articulações por dedo
    const float palmBottom = 0.0575f * H;
    for (int side = 0; side < 2; ++side) {
        float sgn = side ? -1.f : 1.f;
        glm::vec3 wr = pos[side ? CJ_RHand : CJ_LHand];
        int hand = side ? CJ_RHand : CJ_LHand, b = side ? CJ_RThumb1 : CJ_LThumb1;
        pos[b] = wr + glm::vec3(sgn * 0.022f * H, -0.015f * H, 0.012f * H);
        pos[b + 1] = wr + glm::vec3(sgn * 0.03f * H, -0.036f * H, 0.02f * H);
        parent[b] = hand; parent[b + 1] = b;
        const float fx[4] = {0.018f, 0.006f, -0.006f, -0.018f};  // indicador, médio, anelar, mínimo
        for (int f = 0; f < 4; ++f) {
            int j = b + 2 + f * 2;
            pos[j] = wr + glm::vec3(sgn * fx[f] * H, -palmBottom, 0.004f * H);
            pos[j + 1] = pos[j] + glm::vec3(0, -(f == 3 ? 0.02f : 0.026f) * H, 0);
            parent[j] = hand; parent[j + 1] = j;
        }
    }
    for (int i = 0; i < CJ_Total; ++i) { J.pos[i] = pos[i]; J.parent[i] = parent[i]; }
    J.H = H;

    // ---- esqueleto (pose de bind sem rotações: inverseBind = translação inversa)
    Skeleton sk{};
    sk.jointCount = CJ_Total;
    {   // ordem de avaliação: pais antes dos filhos (o pescoço vem depois da cabeça na numeração)
        int n = 0;
        bool done[CJ_Total] = {};
        while (n < CJ_Total)
            for (int i = 0; i < CJ_Total; ++i)
                if (!done[i] && (parent[i] < 0 || done[parent[i]])) { done[i] = true; sk.order[n++] = i; }
    }
    for (int i = 0; i < CJ_Total; ++i) {
        sk.parent[i] = parent[i];
        sk.bindT[i] = parent[i] < 0 ? pos[i] : pos[i] - pos[parent[i]];
        sk.bindR[i] = glm::quat(1, 0, 0, 0);
        sk.bindS[i] = glm::vec3(1.f);
        sk.inverseBind[i] = glm::translate(glm::mat4(1.f), -pos[i]);
    }
    // nomes no padrão Mixamo: reconhecidos pelo editor de animação, Unity (Humanoid) e Blender ao exportar .glb
    static const char* names[CJ_Total] = {"Hips", "Spine", "Spine2", "Head", "LeftArm", "LeftForeArm", "LeftHand", "RightArm",
                                          "RightForeArm", "RightHand", "LeftUpLeg", "LeftLeg", "LeftFoot", "RightUpLeg", "RightLeg", "RightFoot",
                                          "Neck",
                                          "LeftHandThumb1", "LeftHandThumb2", "LeftHandIndex1", "LeftHandIndex2", "LeftHandMiddle1",
                                          "LeftHandMiddle2", "LeftHandRing1", "LeftHandRing2", "LeftHandPinky1", "LeftHandPinky2",
                                          "RightHandThumb1", "RightHandThumb2", "RightHandIndex1", "RightHandIndex2", "RightHandMiddle1",
                                          "RightHandMiddle2", "RightHandRing1", "RightHandRing2", "RightHandPinky1", "RightHandPinky2"};
    for (int i = 0; i < CJ_Total; ++i) std::snprintf(sk.name[i], sizeof(sk.name[i]), "%s", names[i]);
    out.skeletons.push_back(sk);

    // ---- malha (caixas presas a um osso cada, estilo low-poly)
    b.xf = glm::mat4(1.f);
    b.uvTile = glm::vec2(0.f);
    auto part = [&](int joint, glm::vec4 col, glm::vec3 center, glm::vec3 size) {
        b.joint = joint; b.color = col; b.boxC(center, size);
    };
    glm::vec4 dark = srgb(25, 20, 18), white = srgb(245, 245, 245), lips = p.skin * glm::vec4(0.75f, 0.55f, 0.55f, 1.f);
    lips.a = 1.f;
    // tronco
    part(CJ_Hips, p.pants, {0, 0.515f * H, 0}, {0.2f * H * s, 0.075f * H, 0.115f * H * s});
    part(CJ_Spine, p.shirt, {0, 0.605f * H, 0}, {0.19f * H * s, 0.12f * H, 0.105f * H * s});
    part(CJ_Chest, p.shirt, {0, 0.75f * H, 0}, {chestHalf * 2.f, 0.165f * H, 0.12f * H * s});
    part(CJ_Chest, p.shirt, {0, 0.83f * H, 0}, {chestHalf * 1.4f, 0.02f * H, 0.1f * H * s});  // ombros
    // cabeça
    const float headY = 0.925f * H, hw = 0.11f * H, hh = 0.13f * H, hd = 0.12f * H, front = hd / 2;
    part(CJ_Neck, p.skin, {0, 0.855f * H, 0}, {0.05f * H, 0.045f * H, 0.05f * H});           // pescoço
    part(CJ_Head, p.skin, {0, headY, 0.004f * H}, {hw, hh, hd});
    for (float side : {-1.f, 1.f}) {
        part(CJ_Head, white, {side * 0.024f * H, headY + 0.008f * H, front + 0.004f * H}, {0.022f * H, 0.014f * H, 0.004f * H});
        part(CJ_Head, dark, {side * 0.024f * H, headY + 0.008f * H, front + 0.0065f * H}, {0.01f * H, 0.012f * H, 0.003f * H});
        part(CJ_Head, p.hair, {side * 0.025f * H, headY + 0.025f * H, front + 0.004f * H}, {0.026f * H, 0.005f * H, 0.004f * H});
        part(CJ_Head, p.skin, {side * hw / 2, headY + 0.002f * H, 0.005f * H}, {0.012f * H, 0.03f * H, 0.022f * H});  // orelhas
    }
    part(CJ_Head, p.skin * 0.95f, {0, headY - 0.008f * H, front + 0.008f * H}, {0.014f * H, 0.024f * H, 0.014f * H});  // nariz
    part(CJ_Head, lips, {0, headY - 0.035f * H, front + 0.004f * H}, {0.034f * H, 0.007f * H, 0.004f * H});            // boca
    // cabelo
    const float top = headY + hh / 2;
    if (p.hairStyle == 1 || p.hairStyle == 2) {
        part(CJ_Head, p.hair, {0, top + 0.008f * H, 0.0f}, {hw + 0.008f * H, 0.022f * H, hd + 0.01f * H});
        part(CJ_Head, p.hair, {0, headY + 0.02f * H, -hd / 2 - 0.002f * H}, {hw + 0.008f * H, hh * 0.75f, 0.012f * H});
        part(CJ_Head, p.hair, {0, top - 0.004f * H, front + 0.004f * H}, {hw + 0.004f * H, 0.018f * H, 0.012f * H});  // franja
        for (float side : {-1.f, 1.f})
            part(CJ_Head, p.hair, {side * (hw / 2 + 0.003f * H), headY + 0.03f * H, -0.01f * H}, {0.008f * H, hh * 0.45f, hd * 0.75f});
        if (p.hairStyle == 2)
            part(CJ_Chest, p.hair, {0, 0.84f * H, -hd / 2 - 0.006f * H}, {hw + 0.01f * H, 0.16f * H, 0.02f * H});
    } else if (p.hairStyle == 3) {  // boné
        part(CJ_Head, p.shirt, {0, top + 0.012f * H, -0.004f * H}, {hw + 0.012f * H, 0.03f * H, hd + 0.01f * H});
        part(CJ_Head, p.shirt * 0.8f, {0, top + 0.0f, front + 0.035f * H}, {hw * 0.9f, 0.008f * H, 0.07f * H});
    }
    // braços (manga curta ou comprida)
    for (int side = 0; side < 2; ++side) {
        float x = side ? -sx : sx;
        int up = side ? CJ_RUpper : CJ_LUpper, fo = side ? CJ_RFore : CJ_LFore, ha = side ? CJ_RHand : CJ_LHand;
        part(up, p.shirt, {x, shoulderY - 0.045f * H, 0}, {armHalf * 2.2f, 0.1f * H, armHalf * 2.2f});
        part(up, p.longSleeves ? p.shirt : p.skin, {x, shoulderY - 0.125f * H, 0}, {armHalf * 1.8f, 0.1f * H, armHalf * 1.8f});
        part(fo, p.longSleeves ? p.shirt : p.skin, {x, elbowY - 0.075f * H, 0}, {armHalf * 1.65f, 0.16f * H, armHalf * 1.65f});
        part(ha, p.skin, {x, wristY - 0.03f * H, 0.004f * H}, {0.05f * H, 0.06f * H, 0.03f * H});  // palma
        int fb = side ? CJ_RThumb1 : CJ_LThumb1;
        for (int f = 0; f < 5; ++f) {
            for (int seg = 0; seg < 2; ++seg) {
                int j = fb + f * 2 + seg;
                glm::vec3 a = pos[j], c = seg == 0 ? pos[j + 1] : pos[j] + (pos[j] - pos[j - 1]) * (f == 0 ? 0.8f : 0.85f);
                glm::vec3 mid = (a + c) * 0.5f;
                float len = glm::length(c - a) + 0.004f * H, w = (f == 0 ? 0.013f : f == 4 ? 0.009f : 0.0105f) * H;
                part(j, p.skin, mid, {w, len, w * 1.15f});
            }
        }
    }
    // pernas
    for (int side = 0; side < 2; ++side) {
        float x = side ? -tx : tx;
        int th = side ? CJ_RThigh : CJ_LThigh, sh = side ? CJ_RShin : CJ_LShin, ft = side ? CJ_RFoot : CJ_LFoot;
        part(th, p.pants, {x, (thighY + kneeY) / 2 + 0.01f * H, 0}, {0.092f * H * s, thighY - kneeY + 0.03f * H, 0.098f * H * s});
        part(sh, p.pants, {x, (kneeY + ankleY) / 2 + 0.01f * H, 0}, {0.076f * H * s, kneeY - ankleY + 0.01f * H, 0.082f * H * s});
        part(ft, p.shoes, {x, 0.026f * H, 0.026f * H}, {0.078f * H, 0.052f * H, 0.155f * H});
    }
    b.joint = -1;
    b.color = glm::vec4(1.f);
    // skinning rígido: todos os vértices já têm joints/weights do osso

    // ---- animações: captura de movimento (biblioteca) onde houver; senão, feitas por código
    const auto& lib = mocapLibrary();
    out.channelStorage.reserve((CC_Count + lib.size()) * 48);  // os ponteiros dos clips não podem mudar
    out.keyData.reserve(out.keyData.size() + (CC_Count + lib.size()) * 48);
    auto mocap = [&](const MocapClip& c, const char* name) {
        AnimationClip clip;
        std::snprintf(clip.name, sizeof(clip.name), "%s", name);
        clip.duration = c.duration();
        size_t first = out.channelStorage.size();
        for (int j = 0; j < CJ_Total; ++j) {  // rotações: direto da biblioteca (iguais para qualquer altura)
            Channel ch;
            ch.joint = j; ch.path = ChannelPath::Rotation; ch.keyCount = c.frames;
            ch.times = c.times.data(); ch.values = c.rot.data() + size_t(j) * c.frames * 4;
            out.channelStorage.push_back(ch);
        }
        out.keyData.emplace_back(size_t(c.frames) * 3);  // quadril na altura deste personagem
        std::vector<float>& hp = out.keyData.back();
        const glm::vec3 hips0 = J.pos[CJ_Hips];
        for (int f = 0; f < c.frames; ++f) {
            glm::vec3 v = hips0 + c.hips[f] * hips0.y;
            hp[f * 3] = v.x; hp[f * 3 + 1] = v.y; hp[f * 3 + 2] = v.z;
        }
        Channel ch;
        ch.joint = CJ_Hips; ch.path = ChannelPath::Translation; ch.keyCount = c.frames;
        ch.times = c.times.data(); ch.values = hp.data();
        out.channelStorage.push_back(ch);
        clip.channels = out.channelStorage.data() + first;
        clip.channelCount = int(out.channelStorage.size() - first);
        out.clips.push_back(clip);
    };
    // cada papel: a animação capturada (se a biblioteca tiver) ou a gerada
    auto slot = [&](int cc, const char* name, float dur, const std::function<void(ClipWriter&)>& gen) {
        if (const MocapClip* m = mocapForSlot(clipSlot(cc))) { mocap(*m, name); return; }
        ClipWriter w(out, name, dur);
        gen(w);
        w.finish();
    };
    slot(CC_Idle, "Parado", 3.f, [&](ClipWriter& w) { idle(w, J); });
    slot(CC_Walk, "Andando", 1.05f, [&](ClipWriter& w) { locomotion(w, J, 0.5f, 0.75f, 0.4f, 0.25f, 0.03f, 0.025f); });
    slot(CC_Run, "Correndo", 0.66f, [&](ClipWriter& w) { locomotion(w, J, 0.95f, 1.5f, 0.85f, 1.25f, 0.2f, 0.06f); });
    slot(CC_Dance, "Dancando", 1.3f, [&](ClipWriter& w) { dance(w, J); });
    slot(CC_Wave, "Acenando", 1.6f, [&](ClipWriter& w) { wave(w, J); });
    const char* cls[3] = {"fuzil", "pistola", "faca"};
    const char* modes[5] = {"Parado", "Andando", "Correndo", "Mirando", "Andando mirando"};
    const float durs[5] = {3.f, 1.05f, 0.7f, 2.f, 1.05f};
    for (int c = 0; c < 3; ++c)
        for (int m = 0; m < 5; ++m) {
            char name[32];
            std::snprintf(name, sizeof(name), "%s (%s)", c == 2 && m >= AP_Aim ? (m == AP_Aim ? "Golpe" : "Golpe andando") : modes[m], cls[c]);
            slot(CC_Rifle + c * 5 + m, name, c == 2 && m >= AP_Aim ? 0.5f : durs[m], [&](ClipWriter& w) { armedClip(w, J, CC_Rifle + c * 5, m); });
        }
    // extras: sem animação capturada, usam uma parecida feita por código
    slot(CC_Death, "Morte", 3.f, [&](ClipWriter& w) { idle(w, J); });
    slot(CC_Reload, "Recarregando (fuzil)", 2.f, [&](ClipWriter& w) { armedClip(w, J, CC_Rifle, AP_Idle); });
    slot(CC_RifleBack, "De costas (fuzil)", 1.05f, [&](ClipWriter& w) { armedClip(w, J, CC_Rifle, AP_AimWalk); });
    slot(CC_RifleLeft, "Lado esquerdo (fuzil)", 1.05f, [&](ClipWriter& w) { armedClip(w, J, CC_Rifle, AP_AimWalk); });
    slot(CC_RifleRight, "Lado direito (fuzil)", 1.05f, [&](ClipWriter& w) { armedClip(w, J, CC_Rifle, AP_AimWalk); });
    slot(CC_RifleFire, "Atirando (fuzil)", 1.f, [&](ClipWriter& w) { armedClip(w, J, CC_Rifle, AP_Aim); });
    slot(CC_Jump, "Pulando", 0.9f, [&](ClipWriter& w) { idle(w, J); });
    slot(CC_CrouchWalk, "Agachado andando (fuzil)", 1.05f, [&](ClipWriter& w) { armedClip(w, J, CC_Rifle, AP_Walk); });
    // o resto da biblioteca (sem papel) entra no fim da lista
    for (const MocapClip& m : lib) if (m.slot.empty() || m.slot == "-") mocap(m, m.name.c_str());
}

const char* clipSlot(int cc) {
    static const char* slots[CC_Count] = {"parado", "andando", "correndo", "dancando", "acenando",
                                          "fuzil_parado", "fuzil_andando", "fuzil_correndo", "fuzil_mirando", "fuzil_andando_mirando",
                                          "pistola_parado", "pistola_andando", "pistola_correndo", "pistola_mirando", "pistola_andando_mirando",
                                          "faca_parado", "faca_andando", "faca_correndo", "faca_golpe", "faca_golpe_andando",
                                          "morte", "recarregar", "fuzil_costas", "fuzil_esquerda", "fuzil_direita", "fuzil_atirando", "pulo",
                                          "agachado_andando"};
    return cc >= 0 && cc < CC_Count ? slots[cc] : "";
}

}  // namespace eng
