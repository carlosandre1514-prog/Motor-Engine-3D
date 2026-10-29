// Ponte Android: GLSurfaceView (thread de render) <-> engine_c_api.
// Todas as chamadas chegam na thread GL (o Kotlin usa queueEvent / onDrawFrame).
#include <EGL/egl.h>
#include <android/log.h>
#include <jni.h>
#include <cstring>
#include <string>
#include "platform/engine_c_api.h"

static Engine* g_engine = nullptr;

#define JNI_FN(ret, name) JNIEXPORT ret JNICALL Java_com_mobile3d_NativeBridge_##name

static std::string str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return r;
}

// Texto JSON vai e volta como bytes UTF-8 (o JNI usa UTF-8 "modificado", que quebra com emoji)
static std::string bytes(JNIEnv* env, jbyteArray a) {
    if (!a) return {};
    jsize n = env->GetArrayLength(a);
    std::string s(size_t(n), '\0');
    if (n) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(s.data()));
    return s;
}
static jbyteArray toBytes(JNIEnv* env, const char* s) {
    jsize n = jsize(s ? std::strlen(s) : 0);
    jbyteArray a = env->NewByteArray(n);
    if (n) env->SetByteArrayRegion(a, 0, n, reinterpret_cast<const jbyte*>(s));
    return a;
}

extern "C" {

// onSurfaceCreated: contexto ES 3.2 ativo. Se já havia motor, o contexto antigo foi perdido: recria.
JNI_FN(jboolean, nativeCreate)(JNIEnv* env, jclass, jstring shaderDir) {
    if (g_engine) { engine_destroy(g_engine); g_engine = nullptr; }
    g_engine = engine_create(str(env, shaderDir).c_str());
    if (!g_engine) __android_log_print(ANDROID_LOG_ERROR, "Mobile3D", "engine_create falhou (shaders?)");
    return g_engine != nullptr;
}
JNI_FN(void, nativeDestroy)(JNIEnv*, jclass) { if (g_engine) { engine_destroy(g_engine); g_engine = nullptr; } }
JNI_FN(void, nativeResize)(JNIEnv*, jclass, jint w, jint h) { if (g_engine) engine_resize(g_engine, w, h); }
JNI_FN(void, nativeFrame)(JNIEnv*, jclass, jfloat dt) { if (g_engine) engine_frame(g_engine, dt); }
JNI_FN(void, nativeSetQuality)(JNIEnv*, jclass, jint q) { if (g_engine) engine_set_quality(g_engine, q); }
// sons pedidos pelo jogo desde a última chamada ("nome\tvolume\tpan" por linha) e tempo (chuva, noite, hora)
JNI_FN(jstring, nativePollSounds)(JNIEnv* env, jclass) { return env->NewStringUTF(g_engine ? engine_poll_sounds(g_engine) : ""); }
JNI_FN(void, nativeGetWeather)(JNIEnv* env, jclass, jfloatArray out3) {
    float v[3] = {0, 0, 13};
    if (g_engine) engine_get_weather(g_engine, v);
    env->SetFloatArrayRegion(out3, 0, 3, v);
}

JNI_FN(void, nativeTouch)(JNIEnv* env, jclass, jint action, jintArray ids, jfloatArray xy) {
    if (!g_engine) return;
    jsize n = env->GetArrayLength(ids);
    jint* i = env->GetIntArrayElements(ids, nullptr);
    jfloat* p = env->GetFloatArrayElements(xy, nullptr);
    engine_touch(g_engine, action, n, reinterpret_cast<int*>(i), p);
    env->ReleaseIntArrayElements(ids, i, JNI_ABORT);
    env->ReleaseFloatArrayElements(xy, p, JNI_ABORT);
}

// Modo
JNI_FN(void, nativeSetPlay)(JNIEnv*, jclass, jboolean p) { if (g_engine) engine_set_play(g_engine, p ? 1 : 0); }
JNI_FN(jboolean, nativeIsPlaying)(JNIEnv*, jclass) { return g_engine && engine_is_playing(g_engine); }

// Cenas
JNI_FN(void, nativeNewScene)(JNIEnv*, jclass) { if (g_engine) engine_new_scene(g_engine); }
JNI_FN(void, nativeLoadDemo)(JNIEnv*, jclass) { if (g_engine) engine_load_demo(g_engine); }
JNI_FN(void, nativeLoadPhysicsDemo)(JNIEnv*, jclass) { if (g_engine) engine_load_physics_demo(g_engine); }
JNI_FN(jint, nativeLoadModel)(JNIEnv* env, jclass, jstring path) {
    return g_engine ? engine_load_model(g_engine, str(env, path).c_str()) : -1;
}
JNI_FN(jstring, nativeLastError)(JNIEnv* env, jclass) { return env->NewStringUTF(g_engine ? engine_last_error(g_engine) : "motor não iniciado"); }
JNI_FN(jint, nativeAddObject)(JNIEnv*, jclass, jint kind, jboolean physics) {
    return g_engine ? engine_add_object(g_engine, kind, physics ? 1 : 0) : -1;
}
JNI_FN(void, nativeDeleteSelected)(JNIEnv*, jclass) { if (g_engine) engine_delete_selected(g_engine); }
JNI_FN(void, nativeFocus)(JNIEnv*, jclass) { if (g_engine) engine_focus_selected(g_engine); }

// Hierarquia: uma linha por objeto válido -> "id\tprofundidade\tnome"
JNI_FN(jstring, nativeObjectList)(JNIEnv* env, jclass) {
    std::string out;
    if (g_engine) {
        int n = engine_object_count(g_engine);
        for (int i = 0; i < n; ++i) {
            if (!engine_object_valid(g_engine, i)) continue;
            out += std::to_string(i) + '\t' + std::to_string(engine_object_depth(g_engine, i)) + '\t' + engine_object_name(g_engine, i) + '\n';
        }
    }
    return env->NewStringUTF(out.c_str());
}
JNI_FN(jstring, nativeObjectName)(JNIEnv* env, jclass, jint id) { return env->NewStringUTF(g_engine ? engine_object_name(g_engine, id) : ""); }

// Seleção / inspetor
JNI_FN(void, nativeSelect)(JNIEnv*, jclass, jint id) { if (g_engine) engine_select(g_engine, id); }
JNI_FN(jint, nativeSelected)(JNIEnv*, jclass) { return g_engine ? engine_selected(g_engine) : -1; }
JNI_FN(jboolean, nativeGetTransform)(JNIEnv* env, jclass, jint id, jfloatArray out9) {
    if (!g_engine || env->GetArrayLength(out9) < 9) return false;
    float v[9];
    if (!engine_get_transform(g_engine, id, v, v + 3, v + 6)) return false;
    env->SetFloatArrayRegion(out9, 0, 9, v);
    return true;
}
JNI_FN(void, nativeSetTransform)(JNIEnv* env, jclass, jint id, jfloatArray in9) {
    if (!g_engine || env->GetArrayLength(in9) < 9) return;
    float v[9];
    env->GetFloatArrayRegion(in9, 0, 9, v);
    engine_set_transform(g_engine, id, v, v + 3, v + 6);
}
JNI_FN(void, nativeSetGizmoMode)(JNIEnv*, jclass, jint m) { if (g_engine) engine_set_gizmo_mode(g_engine, m); }

// Visual
JNI_FN(void, nativeSetExposure)(JNIEnv*, jclass, jfloat x) { if (g_engine) engine_set_exposure(g_engine, x); }
JNI_FN(void, nativeSetSunIntensity)(JNIEnv*, jclass, jfloat x) { if (g_engine) engine_set_sun_intensity(g_engine, x); }
JNI_FN(void, nativeSetGrid)(JNIEnv*, jclass, jboolean v) { if (g_engine) engine_set_grid(g_engine, v ? 1 : 0); }

// Animação: troca para a próxima e devolve o nome ("" se não houver)
JNI_FN(jstring, nativeCycleAnimation)(JNIEnv* env, jclass) {
    if (!g_engine || engine_cycle_animation(g_engine) < 0) return env->NewStringUTF("");
    return env->NewStringUTF(engine_animation_name(g_engine));
}

// Criação procedural
JNI_FN(jint, nativeCreateCharacter)(JNIEnv* env, jclass, jfloatArray p20) {
    if (!g_engine || env->GetArrayLength(p20) < 20) return -1;
    float p[20];
    env->GetFloatArrayRegion(p20, 0, 20, p);
    return engine_create_character(g_engine, p);
}
JNI_FN(jint, nativeCreateRandomCharacter)(JNIEnv*, jclass, jint seed, jboolean controllable) {
    return g_engine ? engine_create_random_character(g_engine, uint32_t(seed), controllable ? 1 : 0) : -1;
}
JNI_FN(jint, nativeCreateWeapon)(JNIEnv*, jclass, jint kind) { return g_engine ? engine_create_weapon(g_engine, kind) : -1; }
JNI_FN(jint, nativeCreateWeaponEx)(JNIEnv* env, jclass, jint kind, jstring file, jboolean flip, jfloat size, jint acc) {
    return g_engine ? engine_create_weapon_ex(g_engine, kind, str(env, file).c_str(), flip ? 1 : 0, size, acc) : -1;
}
JNI_FN(jint, nativeCreateBuilding)(JNIEnv*, jclass, jint kind, jint seed) {
    return g_engine ? engine_create_building(g_engine, kind, uint32_t(seed)) : -1;
}
JNI_FN(void, nativeLoadMap)(JNIEnv*, jclass, jint kind, jint seed) { if (g_engine) engine_load_map(g_engine, kind, uint32_t(seed)); }
JNI_FN(void, nativeMapOverview)(JNIEnv*, jclass) { if (g_engine) engine_map_overview(g_engine); }

// Materiais
JNI_FN(jstring, nativeMaterialNames)(JNIEnv* env, jclass) {
    std::string out;
    if (g_engine) for (int i = 0; i < engine_material_count(g_engine); ++i) { out += engine_material_name(g_engine, i); out += '\n'; }
    return env->NewStringUTF(out.c_str());
}
JNI_FN(void, nativeSetObjectMaterial)(JNIEnv*, jclass, jint i) { if (g_engine) engine_set_object_material(g_engine, i); }
JNI_FN(void, nativeSetObjectColor)(JNIEnv*, jclass, jfloat r, jfloat g, jfloat b) { if (g_engine) engine_set_object_color(g_engine, r, g, b); }
JNI_FN(jboolean, nativeSetObjectTexture)(JNIEnv* env, jclass, jstring path) {
    return g_engine && engine_set_object_texture(g_engine, str(env, path).c_str());
}

// Jogador
JNI_FN(jboolean, nativeHasPlayer)(JNIEnv*, jclass) { return g_engine && engine_has_player(g_engine); }
JNI_FN(void, nativeSetPlayer)(JNIEnv*, jclass) { if (g_engine) engine_set_player(g_engine); }
JNI_FN(void, nativePlayerInput)(JNIEnv*, jclass, jfloat x, jfloat y, jboolean run) { if (g_engine) engine_player_input(g_engine, x, y, run ? 1 : 0); }
JNI_FN(void, nativePlayerJump)(JNIEnv*, jclass) { if (g_engine) engine_player_jump(g_engine); }
JNI_FN(void, nativePlayerCrouch)(JNIEnv*, jclass, jboolean on) { if (g_engine) engine_player_crouch(g_engine, on ? 1 : 0); }
JNI_FN(void, nativePlayerFire)(JNIEnv*, jclass) { if (g_engine) engine_player_fire(g_engine); }
JNI_FN(jint, nativePlayerWeapon)(JNIEnv*, jclass) { return g_engine ? engine_player_weapon(g_engine) : -1; }

// Efeitos e personagens importados
JNI_FN(jint, nativeCreateEffect)(JNIEnv*, jclass, jint kind, jint element) { return g_engine ? engine_create_effect(g_engine, kind, element) : -1; }
JNI_FN(void, nativeSetEffectElement)(JNIEnv*, jclass, jint element) { if (g_engine) engine_set_effect_element(g_engine, element); }
JNI_FN(void, nativePlayerCast)(JNIEnv*, jclass, jint element) { if (g_engine) engine_player_cast(g_engine, element); }
JNI_FN(jint, nativeMakeCharacter)(JNIEnv*, jclass, jboolean controllable) { return g_engine ? engine_make_character(g_engine, controllable ? 1 : 0) : -1; }

JNI_FN(void, nativeGetStats)(JNIEnv* env, jclass, jfloatArray out6) {
    if (!g_engine || env->GetArrayLength(out6) < 6) return;
    float s[6];
    engine_get_stats(g_engine, s);
    env->SetFloatArrayRegion(out6, 0, 6, s);
}

// Projeto
JNI_FN(jbyteArray, nativeExportProject)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_export_project(g_engine) : ""); }
JNI_FN(jboolean, nativeImportProject)(JNIEnv* env, jclass, jbyteArray json) {
    return g_engine && engine_import_project(g_engine, bytes(env, json).c_str());
}
JNI_FN(jlong, nativeProjectRevision)(JNIEnv*, jclass) { return g_engine ? jlong(engine_project_revision(g_engine)) : 0; }
JNI_FN(void, nativeSetProjectName)(JNIEnv* env, jclass, jbyteArray name) { if (g_engine) engine_set_project_name(g_engine, bytes(env, name).c_str()); }
JNI_FN(jbyteArray, nativeToolCall)(JNIEnv* env, jclass, jstring name, jbyteArray input) {
    return toBytes(env, g_engine ? engine_tool_call(g_engine, str(env, name).c_str(), bytes(env, input).c_str()) : "{\"erro\":\"motor não iniciado\"}");
}
JNI_FN(jint, nativeAddSpec)(JNIEnv* env, jclass, jbyteArray json) { return g_engine ? engine_add_spec(g_engine, bytes(env, json).c_str()) : -1; }
JNI_FN(void, nativeSetTimeOfDay)(JNIEnv*, jclass, jfloat h) { if (g_engine) engine_set_time_of_day(g_engine, h); }
JNI_FN(jfloat, nativeGetTimeOfDay)(JNIEnv*, jclass) { return g_engine ? engine_get_time_of_day(g_engine) : 13.f; }

JNI_FN(jboolean, nativeExportGlb)(JNIEnv* env, jclass, jint id, jstring path) {
    return g_engine && engine_export_glb(g_engine, id, str(env, path).c_str());
}

JNI_FN(jboolean, nativeMapSetEditable)(JNIEnv*, jclass, jboolean on) { return g_engine && engine_map_set_editable(g_engine, on ? 1 : 0); }
JNI_FN(jboolean, nativeMapEditable)(JNIEnv*, jclass) { return g_engine && engine_map_editable(g_engine); }
JNI_FN(jboolean, nativeMapSwapModel)(JNIEnv* env, jclass, jstring kind, jstring path) {
    return g_engine && engine_map_swap_model(g_engine, str(env, kind).c_str(), str(env, path).c_str());
}
JNI_FN(jboolean, nativeMapSwapTexture)(JNIEnv* env, jclass, jint mat, jstring path) {
    return g_engine && engine_map_swap_texture(g_engine, mat, str(env, path).c_str());
}

JNI_FN(jbyteArray, nativePlanetPlaces)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_planet_places(g_engine) : ""); }
JNI_FN(jboolean, nativePlanetGoto)(JNIEnv*, jclass, jint c) { return g_engine && engine_planet_goto(g_engine, c); }
JNI_FN(jbyteArray, nativeRegionName)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_region_name(g_engine) : ""); }
JNI_FN(jbyteArray, nativePlanetMap)(JNIEnv* env, jclass, jint size) {
    if (!g_engine || size <= 0 || size > 2048) return env->NewByteArray(0);
    std::string buf(size_t(size) * size * 4, '\0');
    if (!engine_planet_map(g_engine, size, reinterpret_cast<uint8_t*>(buf.data()))) return env->NewByteArray(0);
    jbyteArray a = env->NewByteArray(jsize(buf.size()));
    env->SetByteArrayRegion(a, 0, jsize(buf.size()), reinterpret_cast<const jbyte*>(buf.data()));
    return a;
}

// Jogo
JNI_FN(void, nativeGameStatus)(JNIEnv* env, jclass, jfloatArray out8) {
    if (!g_engine || env->GetArrayLength(out8) < 8) return;
    float s[8];
    engine_game_status(g_engine, s);
    env->SetFloatArrayRegion(out8, 0, 8, s);
}
JNI_FN(jbyteArray, nativeGameText)(JNIEnv* env, jclass, jint which) { return toBytes(env, g_engine ? engine_game_text(g_engine, which) : ""); }

// Animações
JNI_FN(jbyteArray, nativeAnimationList)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_animation_list(g_engine) : ""); }
JNI_FN(jint, nativeAnimationIndex)(JNIEnv*, jclass) { return g_engine ? engine_animation_index(g_engine) : -1; }
JNI_FN(void, nativeSetAnimation)(JNIEnv*, jclass, jint i) { if (g_engine) engine_set_animation(g_engine, i); }
JNI_FN(jint, nativeAnimEditBegin)(JNIEnv*, jclass) { return g_engine ? engine_anim_edit_begin(g_engine) : -1; }
JNI_FN(jbyteArray, nativeAnimEditBones)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_anim_edit_bones(g_engine) : ""); }
JNI_FN(void, nativeAnimEditSelect)(JNIEnv*, jclass, jint b) { if (g_engine) engine_anim_edit_select(g_engine, b); }
JNI_FN(jint, nativeAnimEditSelected)(JNIEnv*, jclass) { return g_engine ? engine_anim_edit_selected(g_engine) : -1; }
JNI_FN(jbyteArray, nativeInteractLabel)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_interact_label(g_engine) : ""); }
JNI_FN(jboolean, nativeInteract)(JNIEnv*, jclass) { return g_engine && engine_interact(g_engine); }
JNI_FN(jboolean, nativeTeamMode)(JNIEnv*, jclass) { return g_engine && engine_team_mode(g_engine); }
JNI_FN(void, nativeTeamChoose)(JNIEnv*, jclass, jint team) { if (g_engine) engine_team_choose(g_engine, team); }
JNI_FN(jbyteArray, nativeShopJson)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_shop_json(g_engine) : ""); }
JNI_FN(jbyteArray, nativeShopBuy)(JNIEnv* env, jclass, jint item) { return toBytes(env, g_engine ? engine_shop_buy(g_engine, item) : ""); }
JNI_FN(void, nativeShopClose)(JNIEnv*, jclass) { if (g_engine) engine_shop_close(g_engine); }
JNI_FN(void, nativePlayerAim)(JNIEnv*, jclass, jboolean on) { if (g_engine) engine_player_aim(g_engine, on ? 1 : 0); }
JNI_FN(void, nativeSetFirstPerson)(JNIEnv*, jclass, jboolean on) { if (g_engine) engine_set_first_person(g_engine, on ? 1 : 0); }
JNI_FN(void, nativeTeamLoadout)(JNIEnv* env, jclass, jint team, jint skin, jstring primary, jstring secondary) {
    if (!g_engine) return;
    const char* p = env->GetStringUTFChars(primary, nullptr);
    const char* q = env->GetStringUTFChars(secondary, nullptr);
    engine_team_loadout(g_engine, team, skin, p, q);
    env->ReleaseStringUTFChars(primary, p);
    env->ReleaseStringUTFChars(secondary, q);
}
JNI_FN(void, nativeTeamGunSkin)(JNIEnv* env, jclass, jstring gun, jstring skin) {
    if (!g_engine) return;
    const char* g = env->GetStringUTFChars(gun, nullptr);
    const char* s = env->GetStringUTFChars(skin, nullptr);
    engine_team_gun_skin(g_engine, g, s);
    env->ReleaseStringUTFChars(gun, g);
    env->ReleaseStringUTFChars(skin, s);
}
JNI_FN(jboolean, nativePlayerSwap)(JNIEnv*, jclass) { return g_engine && engine_player_swap(g_engine); }
JNI_FN(jboolean, nativePlayerGrenade)(JNIEnv*, jclass) { return g_engine && engine_player_grenade(g_engine); }
JNI_FN(void, nativePlayerReload)(JNIEnv*, jclass) { if (g_engine) engine_player_reload(g_engine); }
static std::string jstr(JNIEnv* env, jbyteArray a) {
    if (!a) return {};
    jsize n = env->GetArrayLength(a);
    std::string s(size_t(n), '\0');
    env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(s.data()));
    return s;
}
JNI_FN(void, nativeNetRoster)(JNIEnv* env, jclass, jboolean host, jbyteArray id, jbyteArray roster) {
    if (g_engine) engine_net_roster(g_engine, host ? 1 : 0, jstr(env, id).c_str(), jstr(env, roster).c_str());
}
JNI_FN(jbyteArray, nativeNetCollect)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_net_collect(g_engine) : "{}"); }
JNI_FN(void, nativeNetApply)(JNIEnv* env, jclass, jbyteArray s) { if (g_engine) engine_net_apply(g_engine, jstr(env, s).c_str()); }
JNI_FN(jbyteArray, nativeNetEvents)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_net_events(g_engine) : "[]"); }
JNI_FN(void, nativeNetEvent)(JNIEnv* env, jclass, jbyteArray s) { if (g_engine) engine_net_event(g_engine, jstr(env, s).c_str()); }
JNI_FN(void, nativeSetEditorOverlay)(JNIEnv*, jclass, jboolean on) { if (g_engine) engine_set_editor_overlay(g_engine, on ? 1 : 0); }
JNI_FN(void, nativeLook)(JNIEnv*, jclass, jfloat dx, jfloat dy) { if (g_engine) engine_look(g_engine, dx, dy); }
JNI_FN(void, nativeSetCamera)(JNIEnv*, jclass, jfloat tx, jfloat ty, jfloat tz, jfloat yaw, jfloat pitch, jfloat dist) {
    if (g_engine) engine_set_camera(g_engine, tx, ty, tz, yaw, pitch, dist);
}
JNI_FN(jbyteArray, nativeHudJson)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_hud_json(g_engine) : "{}"); }
JNI_FN(void, nativeAnimEditGet)(JNIEnv* env, jclass, jint b, jfloatArray out3) {
    if (!g_engine || env->GetArrayLength(out3) < 3) return;
    float v[3]; engine_anim_edit_get(g_engine, b, v); env->SetFloatArrayRegion(out3, 0, 3, v);
}
JNI_FN(void, nativeAnimEditSet)(JNIEnv* env, jclass, jint b, jfloatArray in3) {
    if (!g_engine || env->GetArrayLength(in3) < 3) return;
    float v[3]; env->GetFloatArrayRegion(in3, 0, 3, v); engine_anim_edit_set(g_engine, b, v);
}
JNI_FN(void, nativeAnimEditResetPose)(JNIEnv*, jclass) { if (g_engine) engine_anim_edit_reset_pose(g_engine); }
JNI_FN(jint, nativeAnimEditKey)(JNIEnv*, jclass, jfloat t) { return g_engine ? engine_anim_edit_key(g_engine, t) : 0; }
JNI_FN(void, nativeAnimEditDeleteKey)(JNIEnv*, jclass, jfloat t) { if (g_engine) engine_anim_edit_delete_key(g_engine, t); }
JNI_FN(void, nativeAnimEditGoto)(JNIEnv*, jclass, jfloat t) { if (g_engine) engine_anim_edit_goto(g_engine, t); }
JNI_FN(jbyteArray, nativeAnimEditKeys)(JNIEnv* env, jclass) { return toBytes(env, g_engine ? engine_anim_edit_keys(g_engine) : ""); }
JNI_FN(jboolean, nativeAnimEditPreview)(JNIEnv*, jclass, jboolean on) { return g_engine && engine_anim_edit_preview(g_engine, on ? 1 : 0); }
JNI_FN(jint, nativeAnimEditSave)(JNIEnv* env, jclass, jbyteArray name) { return g_engine ? engine_anim_edit_save(g_engine, bytes(env, name).c_str()) : -1; }
JNI_FN(void, nativeAnimEditEnd)(JNIEnv*, jclass) { if (g_engine) engine_anim_edit_end(g_engine); }
JNI_FN(void, nativeSetAnimPreview)(JNIEnv*, jclass, jboolean on) { if (g_engine) engine_set_anim_preview(g_engine, on ? 1 : 0); }

}  // extern "C"

/* iOS: criar CAMetalLayer/CAEAGLLayer (ou GLKView com ES 3.0 enquanto o backend Metal não existe),
 * chamar engine_create no contexto ativo e engine_frame a partir do CADisplayLink.
 * Em Swift: importar engine_c_api.h via bridging header. */
