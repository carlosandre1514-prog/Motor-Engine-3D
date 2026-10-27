#include "procgen/Character.h"
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

namespace {

struct Joints { glm::vec3 pos[CJ_Count]; int parent[CJ_Count]; };

using RotFn = std::function<glm::quat(float)>;    // fase 0..2pi -> rotação local
using PosFn = std::function<glm::vec3(float)>;

glm::quat rx(float a) { return glm::angleAxis(a, glm::vec3(1, 0, 0)); }
glm::quat ry(float a) { return glm::angleAxis(a, glm::vec3(0, 1, 0)); }
glm::quat rz(float a) { return glm::angleAxis(a, glm::vec3(0, 0, 1)); }

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

// Pose de locomoção (andar / correr). amp 0 = parado.
void locomotion(ClipWriter& w, const Joints& J, float legAmp, float knee, float armAmp, float elbow, float lean, float bob, bool armed) {
    float hipsY = J.pos[CJ_Hips].y;
    w.pos(CJ_Hips, [=](float ph) { return glm::vec3(0, hipsY - bob + bob * std::abs(std::cos(ph)), 0); });
    w.rot(CJ_Hips, [=](float ph) { return ry(0.08f * legAmp * std::sin(ph)); });
    w.rot(CJ_Spine, [=](float) { return rx(lean); });
    w.rot(CJ_Chest, [=](float ph) { return ry(-0.12f * armAmp * std::sin(ph)) * rx(lean * 0.5f); });
    w.rot(CJ_LThigh, [=](float ph) { return rx(-legAmp * std::sin(ph)); });
    w.rot(CJ_RThigh, [=](float ph) { return rx(legAmp * std::sin(ph)); });
    w.rot(CJ_LShin, [=](float ph) { return rx(0.08f + knee * std::max(0.f, std::cos(ph))); });
    w.rot(CJ_RShin, [=](float ph) { return rx(0.08f + knee * std::max(0.f, -std::cos(ph))); });
    w.rot(CJ_LFoot, [=](float ph) { return rx(-0.3f * knee * std::max(0.f, std::cos(ph))); });
    w.rot(CJ_RFoot, [=](float ph) { return rx(-0.3f * knee * std::max(0.f, -std::cos(ph))); });
    if (armed) {
        // mirando: braço direito estendido para frente, esquerdo apoiando a arma
        w.rot(CJ_RUpper, [=](float ph) { return rx(-1.42f + 0.03f * legAmp * std::sin(2 * ph)); });
        w.rot(CJ_RFore, [=](float) { return rx(-0.12f); });
        w.rot(CJ_LUpper, [=](float ph) { return rz(-0.6f) * rx(-1.2f + 0.03f * legAmp * std::sin(2 * ph)); });
        w.rot(CJ_LFore, [=](float) { return rz(-0.25f) * rx(-0.55f); });
    } else {
        w.rot(CJ_LUpper, [=](float ph) { return rz(0.07f) * rx(armAmp * std::sin(ph)); });
        w.rot(CJ_RUpper, [=](float ph) { return rz(-0.07f) * rx(-armAmp * std::sin(ph)); });
        w.rot(CJ_LFore, [=](float ph) { return rx(-elbow - 0.15f * armAmp * std::max(0.f, std::sin(ph))); });
        w.rot(CJ_RFore, [=](float ph) { return rx(-elbow - 0.15f * armAmp * std::max(0.f, -std::sin(ph))); });
    }
}

void idle(ClipWriter& w, const Joints& J, bool armed) {
    float hipsY = J.pos[CJ_Hips].y;
    w.pos(CJ_Hips, [=](float ph) { return glm::vec3(0, hipsY - 0.004f + 0.004f * std::sin(ph), 0); });
    w.rot(CJ_Chest, [=](float ph) { return rx(0.025f * std::sin(ph)); });
    w.rot(CJ_Head, [=](float ph) { return ry(0.18f * std::sin(ph)) * rx(0.03f * std::sin(2 * ph)); });
    if (armed) {
        w.rot(CJ_RUpper, [=](float ph) { return rx(-1.42f + 0.02f * std::sin(ph)); });
        w.rot(CJ_RFore, [=](float) { return rx(-0.12f); });
        w.rot(CJ_LUpper, [=](float ph) { return rz(-0.6f) * rx(-1.2f + 0.02f * std::sin(ph)); });
        w.rot(CJ_LFore, [=](float) { return rz(-0.25f) * rx(-0.55f); });
    } else {
        w.rot(CJ_LUpper, [=](float ph) { return rz(0.1f + 0.02f * std::sin(ph)); });
        w.rot(CJ_RUpper, [=](float ph) { return rz(-0.1f - 0.02f * std::sin(ph)); });
        w.rot(CJ_LFore, [=](float) { return rx(-0.12f); });
        w.rot(CJ_RFore, [=](float) { return rx(-0.12f); });
    }
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
    const glm::vec3 pos[CJ_Count] = {
        {0, 0.53f * H, 0}, {0, 0.62f * H, 0}, {0, 0.72f * H, 0}, {0, 0.865f * H, 0},
        {sx, shoulderY, 0}, {sx, elbowY, 0}, {sx, wristY, 0}, {-sx, shoulderY, 0}, {-sx, elbowY, 0}, {-sx, wristY, 0},
        {tx, thighY, 0}, {tx, kneeY, 0}, {tx, ankleY, 0}, {-tx, thighY, 0}, {-tx, kneeY, 0}, {-tx, ankleY, 0}};
    const int parent[CJ_Count] = {-1, CJ_Hips, CJ_Spine, CJ_Chest, CJ_Chest, CJ_LUpper, CJ_LFore, CJ_Chest, CJ_RUpper, CJ_RFore,
                                  CJ_Hips, CJ_LThigh, CJ_LShin, CJ_Hips, CJ_RThigh, CJ_RShin};
    for (int i = 0; i < CJ_Count; ++i) { J.pos[i] = pos[i]; J.parent[i] = parent[i]; }

    // ---- esqueleto (pose de bind sem rotações: inverseBind = translação inversa)
    Skeleton sk{};
    sk.jointCount = CJ_Count;
    for (int i = 0; i < CJ_Count; ++i) {
        sk.parent[i] = parent[i];
        sk.order[i] = i;
        sk.bindT[i] = parent[i] < 0 ? pos[i] : pos[i] - pos[parent[i]];
        sk.bindR[i] = glm::quat(1, 0, 0, 0);
        sk.bindS[i] = glm::vec3(1.f);
        sk.inverseBind[i] = glm::translate(glm::mat4(1.f), -pos[i]);
    }
    // nomes no padrão Mixamo: reconhecidos pelo editor de animação, Unity (Humanoid) e Blender ao exportar .glb
    static const char* names[CJ_Count] = {"Hips", "Spine", "Spine2", "Head", "LeftArm", "LeftForeArm", "LeftHand", "RightArm",
                                          "RightForeArm", "RightHand", "LeftUpLeg", "LeftLeg", "LeftFoot", "RightUpLeg", "RightLeg", "RightFoot"};
    for (int i = 0; i < CJ_Count; ++i) std::snprintf(sk.name[i], sizeof(sk.name[i]), "%s", names[i]);
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
    part(CJ_Head, p.skin, {0, 0.855f * H, 0}, {0.05f * H, 0.04f * H, 0.05f * H});           // pescoço
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
        part(ha, p.skin, {x, wristY - 0.04f * H, 0.004f * H}, {0.05f * H, 0.08f * H, 0.032f * H});
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

    // ---- animações
    out.channelStorage.reserve(CC_Count * 20);  // exato o suficiente: ponteiros dos clips não podem mudar
    { ClipWriter w(out, "Parado", 3.f); idle(w, J, false); w.finish(); }
    { ClipWriter w(out, "Andando", 1.05f); locomotion(w, J, 0.5f, 0.75f, 0.4f, 0.25f, 0.03f, 0.025f, false); w.finish(); }
    { ClipWriter w(out, "Correndo", 0.66f); locomotion(w, J, 0.95f, 1.5f, 0.85f, 1.25f, 0.2f, 0.06f, false); w.finish(); }
    { ClipWriter w(out, "Parado (arma)", 3.f); idle(w, J, true); w.finish(); }
    { ClipWriter w(out, "Andando (arma)", 1.05f); locomotion(w, J, 0.5f, 0.75f, 0.f, 0.f, 0.05f, 0.025f, true); w.finish(); }
    { ClipWriter w(out, "Correndo (arma)", 0.7f); locomotion(w, J, 0.85f, 1.4f, 0.f, 0.f, 0.15f, 0.05f, true); w.finish(); }
    { ClipWriter w(out, "Dancando", 1.3f); dance(w, J); w.finish(); }
    { ClipWriter w(out, "Acenando", 1.6f); wave(w, J); w.finish(); }
}

}  // namespace eng
