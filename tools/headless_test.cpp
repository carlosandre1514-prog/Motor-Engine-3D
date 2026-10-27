// Teste headless no PC/CI: cria um contexto GLES 3.2 sem janela (EGL surfaceless, Mesa),
// renderiza as cenas do motor num FBO e salva PNGs. Uso:
//   mobile3d_headless <pasta_shaders> <pasta_saida> [modelo.glb ...]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include "platform/engine_c_api.h"

static const int W = 960, H = 540;
static GLuint g_fbo = 0;
static int g_fail = 0;

static void check(const char* what) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) { std::printf("  [ERRO] glGetError 0x%x em %s\n", e, what); ++g_fail; }
}

static void frames(Engine* e, int n, float dt = 1.f / 60.f) {
    for (int i = 0; i < n; ++i) engine_frame(e, dt);
    glFinish();
    check("frame");
}

static void save(const std::string& dir, const char* name) {
    std::vector<unsigned char> px(W * H * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    std::string path = dir + "/" + name;
    stbi_write_png(path.c_str(), W, H, 4, px.data(), W * 4);
    std::printf("  salvo %s\n", path.c_str());
}

static void stats(Engine* e, const char* label) {
    float s[6];
    engine_get_stats(e, s);
    std::printf("  %-22s draws=%3.0f tris=%7.0f objetos=%4.0f cpu=%.2fms\n", label, s[3], s[4], s[5], s[2]);
}

static void touch(Engine* e, int action, float x, float y) {
    int id = 0; float xy[2] = {x, y};
    engine_touch(e, action, 1, &id, xy);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("uso: %s <shaders> <saida> [modelo.glb...]\n", argv[0]); return 2; }
    std::string out = argv[2];

    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                                        : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, nullptr, nullptr)) { std::printf("eglInitialize falhou\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg; EGLint ncfg = 0;
    eglChooseConfig(dpy, cfgAttr, &cfg, 1, &ncfg);
    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, ncfg ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { std::printf("contexto GLES 3.2 falhou\n"); return 1; }
    std::printf("GL: %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    // FBO no lugar da tela
    GLuint color, depth;
    glGenRenderbuffers(1, &color); glBindRenderbuffer(GL_RENDERBUFFER, color); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
    glGenFramebuffers(1, &g_fbo); glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);

    Engine* e = engine_create(argv[1]);
    if (!e) { std::printf("engine_create falhou (shaders?)\n"); return 1; }
    engine_set_target_fbo(e, g_fbo);
    engine_resize(e, W, H);
    check("init");

    std::printf("[1] demo PBR + IBL + sombras\n");
    engine_load_demo(e);
    frames(e, 3);
    stats(e, "demo");
    save(out, "1_demo.png");

    std::printf("[2] gizmo: rotação e escala\n");
    engine_set_gizmo_mode(e, 1); frames(e, 1); save(out, "2_gizmo_rotacao.png");
    engine_set_gizmo_mode(e, 2); frames(e, 1); save(out, "2_gizmo_escala.png");
    engine_set_gizmo_mode(e, 0);

    std::printf("[3] toque: seleção por raycast\n");
    engine_select(e, -1);
    touch(e, 0, W * 0.5f, H * 0.62f); touch(e, 2, W * 0.5f, H * 0.62f);
    std::printf("  tocou no centro -> selecionado: %d (%s)\n", engine_selected(e), engine_object_name(e, engine_selected(e)));
    touch(e, 0, 20, 20); touch(e, 2, 20, 20);
    std::printf("  tocou no céu -> selecionado: %d\n", engine_selected(e));
    if (engine_selected(e) != -1) ++g_fail;

    std::printf("[4] arrastar gizmo (eixo X)\n");
    int target = 2;
    engine_select(e, target);
    frames(e, 1);
    float p0[3], r[3], s[3];
    engine_get_transform(e, target, p0, r, s);
    // procura um ponto sobre o eixo X do gizmo varrendo a tela a partir do centro do objeto
    bool grabbed = false;
    for (int y = 0; y < H && !grabbed; y += 6)
        for (int x = 0; x < W && !grabbed; x += 6) {
            touch(e, 0, float(x), float(y));
            touch(e, 1, float(x + 40), float(y));
            float p1[3];
            engine_get_transform(e, target, p1, r, s);
            touch(e, 2, float(x + 40), float(y));
            if (std::abs(p1[0] - p0[0]) > 0.01f && std::abs(p1[1] - p0[1]) < 1e-4f && std::abs(p1[2] - p0[2]) < 1e-4f) {
                std::printf("  arrastou em (%d,%d): x %.2f -> %.2f\n", x, y, p0[0], p1[0]);
                grabbed = true;
            } else {
                engine_set_transform(e, target, p0, r, s);
            }
        }
    if (!grabbed) { std::printf("  [ERRO] gizmo não moveu o objeto\n"); ++g_fail; }

    std::printf("[5] Play/Stop com física\n");
    engine_load_physics_demo(e);
    frames(e, 2);
    save(out, "3_fisica_inicio.png");
    stats(e, "fisica edit");
    float before[3]; engine_get_transform(e, 20, before, r, s);
    engine_set_play(e, 1);
    frames(e, 150);
    float during[3]; engine_get_transform(e, 20, during, r, s);
    save(out, "4_fisica_depois_de_cair.png");
    stats(e, "fisica play");
    engine_set_play(e, 0);
    float after[3]; engine_get_transform(e, 20, after, r, s);
    std::printf("  objeto 20 (%s): y %.2f -> %.2f (play) -> %.2f (stop)\n", engine_object_name(e, 20), before[1], during[1], after[1]);
    if (!(during[1] < before[1] - 1.f) || std::abs(after[1] - before[1]) > 1e-4f) { std::printf("  [ERRO] física/snapshot\n"); ++g_fail; }
    engine_set_play(e, 1); frames(e, 150);  // Play de novo: tem que cair igual
    float again[3]; engine_get_transform(e, 20, again, r, s);
    std::printf("  2º play: y %.2f\n", again[1]);
    engine_set_play(e, 0);

    std::printf("[6] exposição\n");
    engine_load_demo(e);
    engine_set_exposure(e, 0.5f); frames(e, 1); save(out, "5_exposicao_0.5.png");
    engine_set_exposure(e, 1.6f); frames(e, 1); save(out, "5_exposicao_1.6.png");
    engine_set_exposure(e, 1.f);

    for (int i = 3; i < argc; ++i) {
        std::printf("[7] modelo %s\n", argv[i]);
        engine_new_scene(e);
        int id = engine_load_model(e, argv[i]);
        if (id < 0) { std::printf("  [ERRO] %s\n", engine_last_error(e)); ++g_fail; continue; }
        std::printf("  raiz id=%d '%s', %d objetos, animação: '%s'\n", id, engine_object_name(e, id), engine_object_count(e), engine_animation_name(e));
        frames(e, 2);
        char name[64];
        std::snprintf(name, sizeof(name), "6_modelo_%d_edit.png", i);
        save(out, name);
        engine_set_play(e, 1);
        frames(e, 40);
        std::snprintf(name, sizeof(name), "6_modelo_%d_play.png", i);
        save(out, name);
        stats(e, "modelo");
        if (engine_cycle_animation(e) >= 0) {
            std::printf("  próxima animação: '%s'\n", engine_animation_name(e));
            frames(e, 30);
            std::snprintf(name, sizeof(name), "6_modelo_%d_anim2.png", i);
            save(out, name);
        }
        engine_set_play(e, 0);
        engine_delete_selected(e);
        frames(e, 1);
        std::printf("  apagou o modelo: %d objetos válidos restantes\n", [&] { int c = 0; for (int k = 0; k < engine_object_count(e); ++k) c += engine_object_valid(e, k); return c; }());
    }

    std::printf("[8] personagens e armas\n");
    engine_new_scene(e);
    for (int i = 0; i < 5; ++i) {
        engine_create_random_character(e, 100 + i, 0);
        float p[3], rr[3], ss[3];
        int id = engine_selected(e);
        engine_get_transform(e, id, p, rr, ss);
        p[0] = (i - 2) * 1.1f;
        engine_set_transform(e, id, p, rr, ss);
        if (i < 4) engine_create_weapon(e, i);       // pistola, fuzil, escopeta, sniper
        else for (int k = 0; k < 6; ++k) engine_cycle_animation(e);  // último: dançando
    }
    engine_select(e, -1);
    frames(e, 30);
    {
        float p[3] = {0, 0, 0}, rr[3], ss[3];
        (void)p; (void)rr; (void)ss;
    }
    engine_focus_selected(e);
    frames(e, 2);
    save(out, "7_personagens.png");
    stats(e, "personagens");

    std::printf("[9] construções\n");
    engine_new_scene(e);
    const char* bnames[] = {"casa", "sobrado", "barraco", "predio", "arranha-ceu", "escola", "arvore", "pinheiro", "palmeira", "carro", "poste", "muro"};
    float x = -60.f;
    for (int k = 0; k < 12; ++k) {
        int id = engine_create_building(e, k, 7 + k);
        float p[3], rr[3], ss[3];
        if (id < 0 || !engine_get_transform(e, id, p, rr, ss)) { std::printf("  [ERRO] %s\n", bnames[k]); ++g_fail; continue; }
        float wdt = k == 4 ? 34.f : k == 3 || k == 5 ? 24.f : 12.f;
        x += wdt * 0.5f;
        p[0] = x; p[2] = k == 5 ? -10.f : 0.f;
        x += wdt * 0.5f + 2.f;
        engine_set_transform(e, id, p, rr, ss);
    }
    engine_select(e, -1);
    engine_focus_selected(e);
    frames(e, 2);
    save(out, "8_construcoes.png");
    stats(e, "construcoes");

    std::printf("[11] efeitos\n");
    engine_new_scene(e);
    engine_set_sun_intensity(e, 1.2f);
    engine_set_exposure(e, 0.9f);
    for (int k = 0; k < 9; ++k) {           // todos os tipos, elemento variando
        int id = engine_create_effect(e, k, k % 7);
        float p[3], rr[3], ss[3];
        engine_get_transform(e, id, p, rr, ss);
        p[0] = (k - 4) * 3.f; p[2] = 0.f;
        engine_set_transform(e, id, p, rr, ss);
    }
    for (int el = 0; el < 7; ++el) {        // fogueiras dos 7 elementos
        int id = engine_create_effect(e, 0, el);
        float p[3], rr[3], ss[3];
        engine_get_transform(e, id, p, rr, ss);
        p[0] = (el - 3) * 3.f; p[2] = 6.f;
        engine_set_transform(e, id, p, rr, ss);
    }
    {
        int id = engine_create_effect(e, 4, 0);   // aura com cor livre (rosa)
        engine_set_object_color(e, 1.f, 0.4f, 0.7f);
        float p[3], rr[3], ss[3];
        engine_get_transform(e, id, p, rr, ss);
        p[0] = 0.f; p[2] = -6.f;
        engine_set_transform(e, id, p, rr, ss);
    }
    engine_select(e, -1);
    engine_set_camera(e, 0.f, 1.f, 2.f, 3.14159f + 0.25f, 0.32f, 19.f);
    frames(e, 60);
    save(out, "10_efeitos.png");
    stats(e, "efeitos");
    // magia e tiro no Play
    engine_new_scene(e);
    for (int i = 0; i < 6; ++i) {  // alvos à frente do jogador (+Z)
        int id = engine_add_object(e, i % 4, 1);
        float p[3], rr[3], ss[3];
        engine_get_transform(e, id, p, rr, ss);
        p[0] = (i % 3 - 1) * 1.5f; p[1] = 0.6f + (i / 3) * 1.1f; p[2] = 9.f;
        engine_set_transform(e, id, p, rr, ss);
    }
    engine_set_camera(e, 0.f, 1.f, 0.f, 0.f, 0.f, 8.f);
    engine_create_random_character(e, 5, 1);
    engine_create_weapon(e, 1);
    engine_set_play(e, 1);
    frames(e, 20);
    engine_player_cast(e, 0);
    frames(e, 9);
    save(out, "11_magia_fogo.png");
    frames(e, 60);
    engine_player_cast(e, 2);
    frames(e, 26);
    save(out, "11_magia_trevas.png");
    frames(e, 60);
    engine_player_cast(e, 5);
    frames(e, 2);
    save(out, "11_magia_raio.png");
    frames(e, 30);
    engine_player_fire(e);
    frames(e, 1);
    save(out, "11_tiro.png");
    engine_set_play(e, 0);
    engine_set_sun_intensity(e, 3.2f);
    engine_set_exposure(e, 1.f);

    // modelos humanoides importados (argumentos *.glb / *.fbx cujo nome contém "Soldier", "Xbot" ou "Samba")
    for (int i = 3; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strstr(a, "Soldier") && !std::strstr(a, "Samba") && !std::strstr(a, "Xbot")) continue;
        std::printf("[12] personagem importado %s\n", a);
        engine_new_scene(e);
        int id = engine_load_model(e, a);
        if (id < 0) { std::printf("  [ERRO] %s\n", engine_last_error(e)); ++g_fail; continue; }
        int ch = engine_make_character(e, 1);
        std::printf("  modelo id=%d, personagem=%d (%s), animação: '%s'\n", id, ch, engine_last_error(e), engine_animation_name(e));
        if (ch < 0) { ++g_fail; continue; }
        engine_create_weapon(e, 1);
        engine_set_camera(e, 0.f, 1.f, 0.f, 3.14159f, 0.15f, 4.5f);
        frames(e, 3);
        char name[96];
        const char* base = std::strrchr(a, '/') ? std::strrchr(a, '/') + 1 : a;
        std::snprintf(name, sizeof(name), "12_%s_parado.png", base);
        save(out, name);
        engine_set_play(e, 1);
        frames(e, 20);
        float p0[3], rr[3], ss[3];
        engine_get_transform(e, ch, p0, rr, ss);
        engine_player_input(e, 0.f, 1.f, 0);
        frames(e, 50);
        std::snprintf(name, sizeof(name), "12_%s_andando.png", base);
        save(out, name);
        engine_player_input(e, 0.f, 1.f, 1);
        frames(e, 40);
        std::snprintf(name, sizeof(name), "12_%s_correndo.png", base);
        save(out, name);
        float p1[3]; engine_get_transform(e, ch, p1, rr, ss);
        std::printf("  andou %.1f m, animação: '%s'\n", std::sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[2] - p0[2]) * (p1[2] - p0[2])), engine_animation_name(e));
        engine_player_input(e, 0.f, 0.f, 0);
        engine_set_play(e, 0);
    }

    const char* maps[] = {"infinito", "cidade", "favela", "colegio", "bairro"};
    for (int mk = 0; mk < 5; ++mk) {
        std::printf("[10] mapa %s\n", maps[mk]);
        engine_load_map(e, mk, 42);
        int player = engine_selected(e);
        frames(e, 3);
        stats(e, maps[mk]);
        char name[64];
        std::snprintf(name, sizeof(name), "9_mapa_%d_%s.png", mk, maps[mk]);
        save(out, name);
        if (!engine_has_player(e)) { std::printf("  [ERRO] sem jogador\n"); ++g_fail; continue; }
        float p0[3], rr[3], ss[3];
        engine_get_transform(e, player, p0, rr, ss);
        engine_create_weapon(e, 1);
        engine_set_play(e, 1);
        frames(e, 30);  // assenta no chão
        float p1[3]; engine_get_transform(e, player, p1, rr, ss);
        engine_player_input(e, 0.f, 1.f, 1);
        frames(e, 90);
        engine_player_jump(e);
        frames(e, 10);
        engine_player_input(e, 0.f, 0.f, 0);
        engine_player_fire(e);
        frames(e, 2);
        std::snprintf(name, sizeof(name), "9_mapa_%d_%s_play.png", mk, maps[mk]);
        save(out, name);
        float p2[3]; engine_get_transform(e, player, p2, rr, ss);
        stats(e, "play");
        float moved = std::sqrt((p2[0] - p1[0]) * (p2[0] - p1[0]) + (p2[2] - p1[2]) * (p2[2] - p1[2]));
        std::printf("  jogador: inicio y=%.2f, assentou y=%.2f, andou %.1f m, y final %.2f\n", p0[1], p1[1], moved, p2[1]);
        if (moved < 3.f) { std::printf("  [ERRO] jogador não andou\n"); ++g_fail; }
        if (p2[1] < p0[1] - 5.f) { std::printf("  [ERRO] jogador caiu do mapa\n"); ++g_fail; }
        engine_set_play(e, 0);
    }

    engine_destroy(e);
    check("destroy");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    std::printf(g_fail ? "FALHOU (%d problemas)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
