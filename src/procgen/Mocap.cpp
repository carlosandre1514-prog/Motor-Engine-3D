#include "procgen/Mocap.h"
#include <ufbx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <glm/gtx/quaternion.hpp>
#include "procgen/Character.h"

namespace eng {

namespace {

// Nome do osso no padrão do motor (os do Mixamo sem o prefixo "mixamorig:")
const char* kRefNames[CJ_Total] = {"Hips", "Spine", "Spine2", "Head", "LeftArm", "LeftForeArm", "LeftHand", "RightArm",
                                   "RightForeArm", "RightHand", "LeftUpLeg", "LeftLeg", "LeftFoot", "RightUpLeg", "RightLeg",
                                   "RightFoot", "Neck",
                                   "LeftHandThumb1", "LeftHandThumb2", "LeftHandIndex1", "LeftHandIndex2", "LeftHandMiddle1",
                                   "LeftHandMiddle2", "LeftHandRing1", "LeftHandRing2", "LeftHandPinky1", "LeftHandPinky2",
                                   "RightHandThumb1", "RightHandThumb2", "RightHandIndex1", "RightHandIndex2", "RightHandMiddle1",
                                   "RightHandMiddle2", "RightHandRing1", "RightHandRing2", "RightHandPinky1", "RightHandPinky2"};

int refJointOf(const char* nodeName) {
    const char* n = std::strrchr(nodeName, ':');
    n = n ? n + 1 : nodeName;
    for (int j = 0; j < CJ_Total; ++j) if (std::strcmp(n, kRefNames[j]) == 0) return j;
    return -1;
}

glm::mat4 toGlm(const ufbx_matrix& m) {
    glm::mat4 r(1.f);
    for (int c = 0; c < 4; ++c) r[c] = glm::vec4(float(m.cols[c].x), float(m.cols[c].y), float(m.cols[c].z), c == 3 ? 1.f : 0.f);
    return r;
}

glm::quat rotOf(const glm::mat4& m) {
    glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
    return glm::normalize(glm::quat_cast(r));
}

// Rotação que leva (a1, a2) do esqueleto do motor para (b1, b2) do arquivo: a1 -> b1 exato, a2 o mais perto possível
glm::quat alignTwo(glm::vec3 a1, glm::vec3 a2, glm::vec3 b1, glm::vec3 b2) {
    auto basis = [](glm::vec3 x, glm::vec3 y) {
        x = glm::normalize(x);
        glm::vec3 z = glm::normalize(glm::cross(x, y));
        y = glm::cross(z, x);
        return glm::mat3(x, y, z);
    };
    return glm::normalize(glm::quat_cast(basis(b1, b2) * glm::transpose(basis(a1, a2))));
}

glm::quat alignOne(glm::vec3 a, glm::vec3 b) {
    if (glm::length(a) < 1e-6f || glm::length(b) < 1e-6f) return glm::quat(1, 0, 0, 0);
    return glm::rotation(glm::normalize(a), glm::normalize(b));
}

}  // namespace

bool bakeMocap(const char* fbxPath, int rootMode, MocapClip& out, std::string& err, float armsDownDeg) {
    const bool inPlace = rootMode != MR_Free;
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    ufbx_error uerr;
    ufbx_scene* scene = ufbx_load_file(fbxPath, &opts, &uerr);
    if (!scene) { err = std::string("não consegui ler o FBX: ") + uerr.description.data; return false; }
    if (scene->anim_stacks.count == 0) { ufbx_free_scene(scene); err = "o arquivo não tem animação"; return false; }

    // ossos do arquivo que correspondem aos do motor
    const ufbx_node* src[CJ_Total] = {};
    for (size_t i = 0; i < scene->nodes.count; ++i) {
        const ufbx_node* n = scene->nodes.data[i];
        int j = refJointOf(n->name.data);
        if (j >= 0 && !src[j]) src[j] = n;
    }
    for (int need : {CJ_Hips, CJ_Spine, CJ_LUpper, CJ_RUpper, CJ_LFore, CJ_RFore, CJ_LThigh, CJ_RThigh, CJ_LShin, CJ_RShin})
        if (!src[need]) {
            ufbx_free_scene(scene);
            err = std::string("faltou o osso ") + kRefNames[need] + " (use esqueleto do Mixamo)";
            return false;
        }

    // esqueleto do motor (personagem padrão): posições de repouso, sem rotações
    LoadedModel refModel;
    MeshBuilder unused;
    buildCharacter(defaultCharacter(), refModel, unused);
    const Skeleton& ref = refModel.skeletons[0];
    glm::vec3 refPos[CJ_Total];
    for (int j = 0; j < CJ_Total; ++j) refPos[j] = -glm::vec3(ref.inverseBind[j][3]);

    // pose de repouso do arquivo (T-pose do Mixamo)
    glm::vec3 srcPos[CJ_Total];
    glm::quat srcRest[CJ_Total];
    for (int j = 0; j < CJ_Total; ++j) {
        if (!src[j]) continue;
        glm::mat4 w = toGlm(src[j]->node_to_world);
        srcPos[j] = glm::vec3(w[3]);
        srcRest[j] = rotOf(w);
    }
    // alinhamento: leva a pose de repouso do motor (braços para baixo) para a do arquivo (T-pose), osso a osso
    auto dirPair = [&](int a, int b, glm::vec3& r, glm::vec3& s) {
        if (!src[a] || !src[b]) return false;
        r = refPos[b] - refPos[a]; s = srcPos[b] - srcPos[a];
        return true;
    };
    glm::quat A[CJ_Total];
    for (int j = 0; j < CJ_Total; ++j) A[j] = glm::quat(1, 0, 0, 0);
    {
        glm::vec3 r1, s1, r2, s2;
        if (dirPair(CJ_Hips, CJ_Spine, r1, s1) && dirPair(CJ_RThigh, CJ_LThigh, r2, s2)) A[CJ_Hips] = alignTwo(r1, r2, s1, s2);
        if (dirPair(CJ_Chest, CJ_Head, r1, s1) && dirPair(CJ_RUpper, CJ_LUpper, r2, s2)) A[CJ_Chest] = alignTwo(r1, r2, s1, s2);
        else A[CJ_Chest] = A[CJ_Hips];
    }
    // cada osso parte do alinhamento do pai e só corrige a direção: a torção (giro no próprio eixo) vem da
    // cadeia toda, senão o pulso ficava girado e a arma apontava para o lado errado
    auto one = [&](int j, int child) {
        glm::vec3 r, s;
        glm::quat pa = A[ref.parent[j] >= 0 ? ref.parent[j] : 0];
        if (dirPair(j, child, r, s)) A[j] = glm::normalize(alignOne(pa * r, s) * pa);
        else A[j] = pa;
    };
    one(CJ_Spine, CJ_Chest);
    one(CJ_Neck, CJ_Head);
    A[CJ_Head] = A[CJ_Neck];
    for (int side = 0; side < 2; ++side) {
        int up = side ? CJ_RUpper : CJ_LUpper, fo = side ? CJ_RFore : CJ_LFore, ha = side ? CJ_RHand : CJ_LHand;
        int th = side ? CJ_RThigh : CJ_LThigh, sh = side ? CJ_RShin : CJ_LShin, ft = side ? CJ_RFoot : CJ_LFoot;
        int f0 = side ? CJ_RThumb1 : CJ_LThumb1;
        one(up, fo); one(fo, ha);
        {   // mão: dedos (médio) e polegar definem o giro inteiro (palma para o lado certo)
            glm::vec3 r1, s1, r2, s2;
            if (dirPair(ha, f0 + 4, r1, s1) && dirPair(f0 + 6, f0 + 2, r2, s2)) A[ha] = alignTwo(r1, r2, s1, s2);
            else one(ha, f0 + 4);
        }
        one(th, sh); one(sh, ft);
        A[ft] = A[sh];
        for (int f = 0; f < 5; ++f) {
            int b = f0 + f * 2;
            one(b, b + 1);
            A[b + 1] = A[b];
        }
    }
    for (int j = 0; j < CJ_Total; ++j) if (!src[j]) A[j] = A[ref.parent[j] >= 0 ? ref.parent[j] : 0];

    // braços (ombros e tudo depois deles) que descem junto
    bool inArm[CJ_Total] = {};
    for (int k = 0; k < CJ_Total; ++k) {
        int j = ref.order[k], p = ref.parent[j];
        inArm[j] = j == CJ_LUpper || j == CJ_RUpper || (p >= 0 && inArm[p]);
    }
    const glm::quat down = glm::angleAxis(glm::radians(armsDownDeg), glm::vec3(1.f, 0.f, 0.f));
    // amostragem a 30 quadros por segundo
    // a tomada com mais curvas (arquivos do Mixamo com personagem trazem uma "Take 001" vazia antes)
    const ufbx_anim_stack* stack = scene->anim_stacks.data[0];
    size_t bestProps = 0;
    for (size_t i = 0; i < scene->anim_stacks.count; ++i) {
        const ufbx_anim_stack* st = scene->anim_stacks.data[i];
        size_t props = 0;
        for (size_t l = 0; l < st->layers.count; ++l) props += st->layers.data[l]->anim_props.count;
        if (props > bestProps) { bestProps = props; stack = st; }
    }
    double t0 = stack->time_begin, t1 = stack->time_end;
    out = MocapClip{};
    out.fps = 30.f;
    out.frames = std::max(2, int(std::floor((t1 - t0) * out.fps + 0.5)) + 1);
    out.loop = inPlace;
    out.times.resize(out.frames);
    out.rot.assign(size_t(CJ_Total) * out.frames * 4, 0.f);
    out.hips.resize(out.frames);
    const float srcHipsH = std::max(0.05f, srcPos[CJ_Hips].y - std::min(srcPos[CJ_LFoot].y, srcPos[CJ_RFoot].y));
    for (int f = 0; f < out.frames; ++f) {
        double t = std::min(t1, t0 + f / double(out.fps));
        out.times[f] = float(f / double(out.fps));
        ufbx_scene* ev = ufbx_evaluate_scene(scene, stack->anim, t, nullptr, &uerr);
        if (!ev) { ufbx_free_scene(scene); err = "falha ao calcular a animação"; return false; }
        glm::quat G[CJ_Total];
        glm::vec3 hipsW(0.f);
        for (int k = 0; k < CJ_Total; ++k) {
            int j = ref.order[k], p = ref.parent[j];
            glm::quat gs;
            if (src[j]) {
                const ufbx_node* n = ev->nodes.data[src[j]->typed_id];
                glm::mat4 w = toGlm(n->node_to_world);
                gs = rotOf(w);
                if (j == CJ_Hips) hipsW = glm::vec3(w[3]);
                G[j] = glm::normalize(gs * glm::inverse(srcRest[j]) * A[j]);
                if (armsDownDeg != 0.f && inArm[j]) G[j] = glm::normalize(down * G[j]);
            } else {
                G[j] = p >= 0 ? G[p] : glm::quat(1, 0, 0, 0);  // osso que o arquivo não tem: acompanha o pai
            }
            glm::quat local = p >= 0 ? glm::normalize(glm::inverse(G[p]) * G[j]) : G[j];
            float* o = &out.rot[(size_t(j) * out.frames + f) * 4];
            o[0] = local.x; o[1] = local.y; o[2] = local.z; o[3] = local.w;
        }
        out.hips[f] = (hipsW - srcPos[CJ_Hips]) / srcHipsH;
        ufbx_free_scene(ev);
    }
    ufbx_free_scene(scene);
    // quaternions vizinhos no mesmo hemisfério (interpolação sem dar a volta)
    for (int j = 0; j < CJ_Total; ++j)
        for (int f = 1; f < out.frames; ++f) {
            float* a = &out.rot[(size_t(j) * out.frames + f - 1) * 4];
            float* b = a + 4;
            if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.f) for (int c = 0; c < 4; ++c) b[c] = -b[c];
        }
    if (inPlace) {  // tira o caminho andado (tendência linear em X/Z), mantém o balanço
        glm::vec3 a = out.hips.front(), b = out.hips.back();
        for (int f = 0; f < out.frames; ++f) {
            float u = out.frames > 1 ? float(f) / (out.frames - 1) : 0.f;
            glm::vec3 drift = glm::mix(a, b, u);
            out.hips[f].x -= drift.x;
            out.hips[f].z -= drift.z;
            if (rootMode == MR_PoseOnly) out.hips[f].y = std::min(out.hips[f].y, 0.f);  // agacha, mas não sobe
        }
    }
    return true;
}

// ---- arquivo .anim: "M3DA", versão, quadros, fps, loop, nome (64), rot[CJ_Total*quadros*4], hips[quadros*3]
bool saveMocap(const char* path, const MocapClip& c) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    int32_t hdr[4] = {1, c.frames, CJ_Total, c.loop ? 1 : 0};
    char name[64] = {};
    std::snprintf(name, sizeof(name), "%s", c.name.c_str());
    f.write("M3DA", 4);
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&c.fps), sizeof(float));
    f.write(name, sizeof(name));
    f.write(reinterpret_cast<const char*>(c.rot.data()), std::streamsize(c.rot.size() * sizeof(float)));
    f.write(reinterpret_cast<const char*>(c.hips.data()), std::streamsize(c.hips.size() * sizeof(glm::vec3)));
    return bool(f);
}

bool loadMocap(const char* path, MocapClip& c) {
    std::ifstream f(path, std::ios::binary);
    char magic[4];
    int32_t hdr[4];
    char name[64];
    if (!f.read(magic, 4) || std::memcmp(magic, "M3DA", 4) != 0) return false;
    if (!f.read(reinterpret_cast<char*>(hdr), sizeof(hdr)) || hdr[0] != 1 || hdr[2] != CJ_Total || hdr[1] < 2 || hdr[1] > 100000) return false;
    c.frames = hdr[1];
    c.loop = hdr[3] != 0;
    if (!f.read(reinterpret_cast<char*>(&c.fps), sizeof(float)) || !f.read(name, sizeof(name))) return false;
    name[63] = 0;
    if (c.name.empty()) c.name = name;
    c.rot.resize(size_t(CJ_Total) * c.frames * 4);
    c.hips.resize(c.frames);
    if (!f.read(reinterpret_cast<char*>(c.rot.data()), std::streamsize(c.rot.size() * sizeof(float)))) return false;
    if (!f.read(reinterpret_cast<char*>(c.hips.data()), std::streamsize(c.hips.size() * sizeof(glm::vec3)))) return false;
    c.times.resize(c.frames);
    for (int k = 0; k < c.frames; ++k) c.times[k] = k / c.fps;
    return true;
}

std::deque<MocapClip>& mocapLibrary() {
    static std::deque<MocapClip> lib;
    return lib;
}

void loadMocapLibrary(const std::string& dir) {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    std::ifstream list(dir + "/biblioteca.txt");
    std::string line;
    while (std::getline(list, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, ';')) f.push_back(part);
        if (f.size() < 3) continue;
        MocapClip c;
        c.slot = f[1];
        c.name = f[2];
        if (!loadMocap((dir + "/" + f[0]).c_str(), c)) { std::fprintf(stderr, "animação %s não abriu\n", f[0].c_str()); continue; }
        if (f.size() >= 4) c.loop = f[3] == "1";
        mocapLibrary().push_back(std::move(c));
    }
}

const MocapClip* mocapForSlot(const char* slot) {
    for (const MocapClip& c : mocapLibrary()) if (c.slot == slot) return &c;
    return nullptr;
}

}  // namespace eng
