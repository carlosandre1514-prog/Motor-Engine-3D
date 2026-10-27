#include "scene/Humanoid.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include "procgen/Character.h"

namespace eng {

int humanoidBone(const char* raw) {
    std::string s(raw ? raw : "");
    // tira prefixos tipo "mixamorig:", "Armature|", "Character1_"
    size_t cut = s.find_last_of(":|");
    if (cut != std::string::npos) s = s.substr(cut + 1);
    for (auto& ch : s) ch = char(std::tolower((unsigned char)ch));
    // prefixos colados: "mixamorigHips", "mixamorig1:...", "bip001 ", "def-"
    for (const char* pre : {"mixamorig", "bip001", "bip01", "def-", "def_"}) {
        size_t n = std::strlen(pre);
        if (s.compare(0, n, pre) == 0) {
            s.erase(0, n);
            while (!s.empty() && (std::isdigit((unsigned char)s[0]) || s[0] == '_' || s[0] == ' ')) s.erase(0, 1);
        }
    }
    // lado
    int side = 0;  // -1 esquerdo, 1 direito
    auto eraseAll = [&](const char* w) {
        size_t p, n = std::strlen(w);
        bool found = false;
        while ((p = s.find(w)) != std::string::npos) { s.erase(p, n); found = true; }
        return found;
    };
    if (eraseAll("left")) side = -1;
    else if (eraseAll("right")) side = 1;
    // tokens alfanuméricos
    std::vector<std::string> tok;
    std::string cur;
    for (char ch : s) {
        if (std::isalnum((unsigned char)ch)) cur += ch;
        else if (!cur.empty()) { tok.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) tok.push_back(cur);
    std::string base;
    for (auto& t : tok) {
        if (t == "l") { side = -1; continue; }
        if (t == "r") { side = 1; continue; }
        if (t == "bip001" || t == "bip01" || t == "def" || t == "mixamorig" || t == "character1" || t == "jnt" || t == "bone") continue;
        base += t;
    }
    // "upperarml" / "handr" coladas (ex.: "UpperArmL")
    if (side == 0 && base.size() > 2) {
        char last = base.back();
        std::string body = base.substr(0, base.size() - 1);
        static const char* parts[] = {"upperarm", "arm", "forearm", "lowerarm", "hand", "upleg", "upperleg", "thigh", "leg", "lowerleg", "calf", "shin", "foot"};
        for (const char* pt : parts)
            if (body == pt && (last == 'l' || last == 'r')) { side = last == 'l' ? -1 : 1; base = body; break; }
    }
    auto is = [&](std::initializer_list<const char*> names) {
        for (const char* n : names) if (base == n) return true;
        return false;
    };
    if (side == 0) {
        if (is({"hips", "pelvis", "hip"})) return CJ_Hips;
        if (is({"spine", "spine0", "spine00", "abdomen", "spine01"})) return CJ_Spine;
        if (is({"spine2", "chest", "upperchest", "spine02", "spine1"})) return CJ_Chest;
        if (is({"neck", "neck1", "neck01", "head"})) return CJ_Head;
        return -1;
    }
    bool L = side < 0;
    // CJ_L* = lado esquerdo do personagem (+X no motor); a maioria das ferramentas usa a mesma convenção
    if (is({"arm", "upperarm", "shoulder2"})) return L ? CJ_LUpper : CJ_RUpper;
    if (is({"forearm", "lowerarm", "elbow"})) return L ? CJ_LFore : CJ_RFore;
    if (is({"hand", "wrist"})) return L ? CJ_LHand : CJ_RHand;
    if (is({"upleg", "upperleg", "thigh"})) return L ? CJ_LThigh : CJ_RThigh;
    if (is({"leg", "lowerleg", "calf", "shin", "knee"})) return L ? CJ_LShin : CJ_RShin;
    if (is({"foot", "ankle"})) return L ? CJ_LFoot : CJ_RFoot;
    return -1;
}

bool buildRetarget(const Skeleton& T, const Skeleton& S, const glm::mat4& spaceWorld, Retarget& R, char* missing, int missingSize) {
    const int n = T.jointCount;
    for (int i = 0; i < kMaxJoints; ++i) { R.map[i] = -1; R.targetOf[i] = -1; R.align[i] = glm::quat(1, 0, 0, 0); }
    // 1) casa os ossos pelo nome. Spine/Chest/Head: prefere o mais alto da cadeia (spine2 > spine1, neck > head)
    for (int i = 0; i < n; ++i) {
        int b = humanoidBone(T.name[i]);
        if (b < 0) continue;
        if (R.targetOf[b] >= 0) {
            // já tinha: mantém o que está mais perto do quadril para Spine/Head, mais longe para Chest
            int prev = R.targetOf[b];
            int depthPrev = 0, depthNew = 0;
            for (int q = T.parent[prev]; q >= 0; q = T.parent[q]) ++depthPrev;
            for (int q = T.parent[i]; q >= 0; q = T.parent[q]) ++depthNew;
            bool keepNew = b == CJ_Chest ? depthNew > depthPrev : depthNew < depthPrev;
            if (!keepNew) continue;
            R.map[prev] = -1;
        }
        R.targetOf[b] = i;
        R.map[i] = b;
    }
    static const char* names[CJ_Count] = {"quadril", "coluna", "peito", "cabeca", "braco E", "antebraco E", "mao E", "braco D",
                                          "antebraco D", "mao D", "coxa E", "canela E", "pe E", "coxa D", "canela D", "pe D"};
    bool ok = true;
    if (missing && missingSize > 0) missing[0] = 0;
    for (int need : {CJ_Hips, CJ_LUpper, CJ_RUpper, CJ_LFore, CJ_RFore, CJ_LThigh, CJ_RThigh, CJ_LShin, CJ_RShin})
        if (R.targetOf[need] < 0) {
            ok = false;
            if (missing) { std::strncat(missing, names[need], missingSize - std::strlen(missing) - 2); std::strncat(missing, " ", missingSize - std::strlen(missing) - 1); }
        }
    if (!ok) return false;

    // 2) pose de repouso do modelo (das matrizes de bind) e referencial alinhado ao mundo
    glm::vec3 sc, tr, skew; glm::vec4 persp; glm::quat rot;
    glm::decompose(spaceWorld, sc, rot, tr, skew, persp);
    R.spaceRot = glm::normalize(rot);
    R.spaceScale = std::max(1e-6f, (sc.x + sc.y + sc.z) / 3.f);
    glm::mat4 bg[kMaxJoints];
    glm::vec3 posW[kMaxJoints];
    for (int i = 0; i < n; ++i) {
        bg[i] = T.bindToSkel * glm::inverse(T.inverseBind[i]);
        glm::mat4 local = T.parent[i] < 0 ? bg[i] : glm::inverse(bg[T.parent[i]]) * bg[i];
        glm::vec3 s3, t3, k3; glm::vec4 p4; glm::quat r3;
        glm::decompose(local, s3, r3, t3, k3, p4);
        R.restT[i] = t3; R.restR[i] = glm::normalize(r3); R.restS[i] = s3;
        glm::vec3 gs, gt, gk; glm::vec4 gp; glm::quat gr;
        glm::decompose(bg[i], gs, gr, gt, gk, gp);
        R.bindRot[i] = glm::normalize(R.spaceRot * glm::normalize(gr));
        posW[i] = R.spaceRot * (gt * R.spaceScale);
        R.parentScale[i] = 1.f;
        if (T.parent[i] >= 0) {
            glm::vec3 ps, pt, pk; glm::vec4 pp; glm::quat pr;
            glm::decompose(bg[T.parent[i]], ps, pr, pt, pk, pp);
            R.parentScale[i] = std::max(1e-6f, (ps.x + ps.y + ps.z) / 3.f);
        }
    }
    // 3) posições de referência e alinhamento dos ossos (direção osso -> filho)
    glm::vec3 posS[CJ_Count];
    for (int j = 0; j < CJ_Count; ++j) posS[j] = -glm::vec3(S.inverseBind[j][3]);
    static const int childOf[CJ_Count] = {CJ_Spine, CJ_Chest, CJ_Head, -1, CJ_LFore, CJ_LHand, -1, CJ_RFore, CJ_RHand, -1,
                                          CJ_LShin, CJ_LFoot, -1, CJ_RShin, CJ_RFoot, -1};
    for (int k = 0; k < n; ++k) {
        int i = T.order[k], b = R.map[i];
        if (b < 0) continue;
        int cs = childOf[b];
        int ct = cs >= 0 ? R.targetOf[cs] : -1;
        if (ct >= 0) {
            glm::vec3 dt = posW[ct] - posW[i], ds = posS[cs] - posS[b];
            if (glm::length(dt) > 1e-6f && glm::length(ds) > 1e-6f)
                R.align[i] = glm::rotation(glm::normalize(dt), glm::normalize(ds));
        } else {
            // sem filho conhecido (mão, cabeça, pé): herda o alinhamento do osso anterior mapeado
            for (int q = T.parent[i]; q >= 0; q = T.parent[q])
                if (R.map[q] >= 0) { R.align[i] = R.align[q]; break; }
        }
    }
    // 4) escala da altura do quadril (passos e pulos do tamanho certo)
    int hips = R.targetOf[CJ_Hips];
    int lf = R.targetOf[CJ_LFoot] >= 0 ? R.targetOf[CJ_LFoot] : R.targetOf[CJ_LShin];
    float hT = posW[hips].y - posW[lf].y;
    float hS = posS[CJ_Hips].y - posS[R.targetOf[CJ_LFoot] >= 0 ? CJ_LFoot : CJ_LShin].y;
    R.hipsScale = hS > 1e-4f && hT > 1e-4f ? hT / hS : 1.f;
    R.srcHips = CJ_Hips;
    R.src = &S;
    return true;
}

}  // namespace eng
