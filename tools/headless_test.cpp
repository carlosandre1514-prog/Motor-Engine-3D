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
#include <sys/stat.h>
#include "json.hpp"
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

    // ---- [13] projeto: salvar, abrir e ferramentas da IA
    std::printf("[13] projeto: salvar/abrir + ferramentas\n");
    {
        auto tool = [&](const char* n, const char* in) {
            std::string r = engine_tool_call(e, n, in);
            std::printf("  %s -> %.160s\n", n, r.c_str());
            if (r.find("\"erro\"") != std::string::npos) { std::printf("  [ERRO] ferramenta %s\n", n); ++g_fail; }
            return r;
        };
        tool("carregar_mapa", R"({"tipo":"vazio","com_jogador":true})");
        tool("criar_objetos", R"({"objetos":[
            {"tipo":"construcao","modelo":"casa","pos":[10,0,-8],"rot_y":30},
            {"tipo":"forma","forma":"cubo","cor":"vermelho","pos":[3,0.5,0],"nome":"Caixa vermelha","fisica":true},
            {"tipo":"forma","forma":"cubo","material":"madeira","pos":[-3,0.5,2],"comportamentos":["explosivo"],"nome":"Barril"},
            {"tipo":"inimigo","pos":[0,0,-6],"arma":"pistola","vida":50},
            {"tipo":"inimigo","pos":[4,0,-7],"vida":40},
            {"tipo":"coletavel","item":"moeda","pos":[0,1,2]},
            {"tipo":"coletavel","item":"moeda","pos":[0,1,3]},
            {"tipo":"coletavel","item":"cristal","pos":[-6,1,6]},
            {"tipo":"efeito","efeito":"fogueira","elemento":"gelo","pos":[-4,0,-4]},
            {"tipo":"luz","cor":"#ffaa44","pos":[2,3,2]},
            {"tipo":"objetivo","pos":[0,0,14]},
            {"tipo":"spawner","pos":[-10,0,-10],"maximo":2,"total":2,"intervalo":0.5},
            {"tipo":"zona_dano","pos":[8,0,8],"raio":2},
            {"tipo":"forma","forma":"cubo","escala":[3,0.4,3],"fisica":true,"massa":0,"pos":[-8,1,0],"comportamentos":[{"tipo":"plataforma","destino":[0,3,0]}]}
        ]})");
        tool("definir_regras", R"({"objetivo":"coletar","quantidade":2,"vida_jogador":100,"titulo":"Teste"})");
        tool("definir_ambiente", R"({"hora":18.2})");
        tool("ver_cena", "{}");
        frames(e, 3);
        save(out, "13_projeto_criado.png");
        long long r0 = engine_project_revision(e);
        frames(e, 30);
        if (engine_project_revision(e) != r0) { std::printf("  [ERRO] revisão muda sem edição (salvaria sem parar)\n"); ++g_fail; }
        std::string saved = engine_export_project(e);
        int objsBefore = engine_object_count(e);
        long long rev = engine_project_revision(e);
        // abre de novo e exporta: precisa dar o mesmo JSON
        if (!engine_import_project(e, saved.c_str())) { std::printf("  [ERRO] import: %s\n", engine_last_error(e)); ++g_fail; }
        std::string again = engine_export_project(e);
        std::printf("  salvo %zu bytes, %d objetos -> reaberto %d objetos, revisão %lld\n", saved.size(), objsBefore, engine_object_count(e), rev);
        if (again != saved) {
            std::printf("  [ERRO] reabrir mudou o projeto\n");
            size_t k = 0; while (k < saved.size() && k < again.size() && saved[k] == again[k]) ++k;
            std::printf("  diferença em %zu: ...%.80s | ...%.80s\n", k, saved.c_str() + (k > 40 ? k - 40 : 0), again.c_str() + (k > 40 ? k - 40 : 0));
            ++g_fail;
        }
        frames(e, 3);
        save(out, "13_projeto_reaberto.png");
        // cidade + reabrir: o mapa é refeito pela semente, objetos voltam do arquivo
        engine_load_map(e, 1, 7);
        engine_add_object(e, 0, 1);
        std::string city = engine_export_project(e);
        engine_import_project(e, city.c_str());
        if (engine_map_kind(e) != 1 || std::string(engine_export_project(e)) != city) { std::printf("  [ERRO] reabrir cidade\n"); ++g_fail; }
        engine_import_project(e, saved.c_str());
    }

    // ---- [14] jogo: coletar moedas, inimigos, dano e vitória
    std::printf("[14] lógica de jogo\n");
    {
        float st[8];
        engine_set_play(e, 1);
        frames(e, 20);
        engine_game_status(e, st);
        std::printf("  início: estado %.0f vida %.0f/%.0f objetivo '%s'\n", st[0], st[1], st[2], engine_game_text(e, 0));
        // o jogador está em (0,0,4) olhando para -Z (rot_y 180): anda para trás até as moedas em z=2..3
        engine_player_input(e, 0.f, 1.f, 0);
        int collectedAt = -1;
        for (int i = 0; i < 240 && collectedAt < 0; ++i) {
            frames(e, 1);
            engine_game_status(e, st);
            if (st[0] == 1.f) collectedAt = i;
        }
        engine_player_input(e, 0.f, 0.f, 0);
        engine_game_status(e, st);
        std::string objective = engine_game_text(e, 0);
        std::printf("  estado %.0f, pontos %.0f, vida %.0f, '%s' / '%s'\n", st[0], st[3], st[1], objective.c_str(), engine_game_text(e, 1));
        save(out, "14_jogo.png");
        if (st[0] != 1.f) { std::printf("  [ERRO] não venceu coletando as moedas\n"); ++g_fail; }
        engine_set_play(e, 0);

        // eliminar: o jogador (fuzil) atira nos inimigos; inimigos atacam o jogador
        engine_tool_call(e, "definir_regras", R"({"objetivo":"eliminar","quantidade":0})");
        engine_set_play(e, 1);
        frames(e, 60);
        engine_game_status(e, st);
        float hp0 = st[1];
        frames(e, 240);  // inimigos se aproximam e atacam; spawner cria 2
        engine_game_status(e, st);
        std::printf("  após 5 s: vida %.0f -> %.0f, '%s'\n", hp0, st[1], engine_game_text(e, 0));
        save(out, "14_inimigos.png");
        if (!(st[1] < hp0)) { std::printf("  [ERRO] inimigos não causaram dano\n"); ++g_fail; }
        int countPlay = engine_object_count(e);
        engine_set_play(e, 0);
        std::string afterStop = engine_export_project(e);
        std::printf("  objetos no play %d; projeto após Stop igual ao salvo: %s\n", countPlay, afterStop.size() > 0 ? "sim" : "?");
        engine_set_play(e, 1);  // 2º play: spawner de novo, sem vazar
        frames(e, 120);
        engine_set_play(e, 0);
    }

    // ---- [15] exemplos prontos (vão para app/src/main/assets/exemplos): montados pelas mesmas ferramentas da IA
    std::printf("[15] projetos de exemplo\n");
    {
        using nlohmann::json;
        std::string dir = out + "/exemplos";
        mkdir(dir.c_str(), 0755);
        struct Ex { const char* file; const char* nome; const char* mapa; unsigned seed; float hora; const char* arma; };
        const Ex exs[] = {
            {"1_moedas_no_bairro", "Moedas no bairro", "bairro", 11, 10.f, "taco"},
            {"2_sobrevivencia_favela", "Sobrevivência na favela", "favela", 5, 21.f, "fuzil"},
            {"3_fuga_da_escola", "Fuga da escola", "escola", 3, 17.f, "pistola"},
            {"4_arena_do_zero", "Arena do zero", "vazio", 1, 13.f, "escopeta"},
        };
        for (int k = 0; k < 4; ++k) {
            const Ex& x = exs[k];
            json r = json::parse(std::string(engine_tool_call(e, "carregar_mapa", json{{"tipo", x.mapa}, {"semente", x.seed}, {"com_jogador", true}}.dump().c_str())));
            json scene = json::parse(std::string(engine_tool_call(e, "ver_cena", "{}")));
            json player;
            for (auto& o : scene["objetos"]) if (o.value("jogador", false)) player = o;
            float px = player["pos"][0], py = player["pos"][1], pz = player["pos"][2];
            float yaw = player.value("rot_y", 0.f) * 3.14159265f / 180.f;
            float fx = std::sin(yaw), fz = std::cos(yaw), sx = std::cos(yaw), sz = -std::sin(yaw);
            // pontos ao longo da rua, à frente do jogador; altura do terreno em cada um
            auto at = [&](float fwd, float side, float up = 0.f) {
                float X = px + fx * fwd + sx * side, Z = pz + fz * fwd + sz * side;
                json h = json::parse(std::string(engine_tool_call(e, "altura_terreno", json{{"pontos", {{X, Z}}}}.dump().c_str())));
                float Y = h["alturas"][0].is_number() ? h["alturas"][0].get<float>() : py;
                return json::array({X, Y + up, Z});
            };
            json objs = json::array();
            engine_tool_call(e, "editar_objeto", json{{"id", player["id"]}, {"arma", x.arma}}.dump().c_str());
            json rules;
            if (k == 0) {
                for (int i = 0; i < 10; ++i) objs.push_back({{"tipo", "coletavel"}, {"item", "moeda"}, {"pos", at(6.f + i * 4.f, (i % 3 - 1) * 1.6f, 0.9f)}});
                for (int i = 0; i < 3; ++i) objs.push_back({{"tipo", "inimigo"}, {"pos", at(20.f + i * 10.f, 2.f)}, {"vida", 60}, {"velocidade", 2.6}});
                objs.push_back({{"tipo", "coletavel"}, {"item", "vida"}, {"pos", at(30.f, -2.f, 0.9f)}});
                rules = {{"objetivo", "coletar"}, {"quantidade", 10}, {"tempo", 120}, {"titulo", x.nome}, {"descricao", "Pegue as 10 moedas em 2 minutos"}};
            } else if (k == 1) {
                objs.push_back({{"tipo", "spawner"}, {"pos", at(28.f, 0.f)}, {"maximo", 3}, {"intervalo", 5}, {"arma_inimigo", "pistola"}, {"vida", 60}, {"dano", 6}});
                objs.push_back({{"tipo", "spawner"}, {"pos", at(-20.f, 0.f)}, {"maximo", 2}, {"intervalo", 7}, {"vida", 80}});
                for (int i = 0; i < 3; ++i) objs.push_back({{"tipo", "coletavel"}, {"item", "vida"}, {"pos", at(-6.f + i * 9.f, 1.5f, 0.9f)}});
                objs.push_back({{"tipo", "efeito"}, {"efeito", "fogueira"}, {"pos", at(4.f, 2.5f)}});
                objs.push_back({{"tipo", "luz"}, {"cor", "#ffb070"}, {"intensidade", 12}, {"raio", 14}, {"pos", at(10.f, 0.f, 4.f)}});
                rules = {{"objetivo", "sobreviver"}, {"tempo", 90}, {"vida_jogador", 150}, {"titulo", x.nome}, {"descricao", "Aguente 90 segundos"}};
            } else if (k == 2) {
                objs.push_back({{"tipo", "objetivo"}, {"pos", at(45.f, 0.f)}});
                for (int i = 0; i < 3; ++i) objs.push_back({{"tipo", "inimigo"}, {"arma", "faca"}, {"pos", at(14.f + i * 9.f, (i % 2 ? 2.5f : -2.5f))}, {"vida", 50}, {"dano", 12}});
                for (int i = 0; i < 2; ++i) objs.push_back({{"tipo", "zona_dano"}, {"pos", at(20.f + i * 12.f, i ? 1.5f : -1.5f)}, {"raio", 1.5}});
                for (int i = 0; i < 3; ++i) objs.push_back({{"tipo", "forma"}, {"forma", "capsula"}, {"nome", "Barril"}, {"cor", "vermelho"}, {"escala", {0.7, 0.6, 0.7}},
                                                            {"pos", at(10.f + i * 11.f, 3.f, 0.6f)}, {"comportamentos", {"explosivo"}}});
                rules = {{"objetivo", "chegar"}, {"tempo", 60}, {"titulo", x.nome}, {"descricao", "Chegue à luz em 1 minuto"}};
            } else {
                for (int i = 0; i < 4; ++i) {
                    float a = i * 1.5708f;
                    float X = std::cos(a) * 9.f, Z = std::sin(a) * 9.f;
                    objs.push_back({{"tipo", "forma"}, {"forma", "cubo"}, {"nome", "Plataforma"}, {"material", "metal"}, {"fisica", true}, {"massa", 0},
                                    {"escala", {3, 0.4, 3}}, {"pos", {X, 0.6 + i * 0.8, Z}},
                                    {"comportamentos", {{{"tipo", "plataforma"}, {"destino", {0, 2.5 + i * 0.5, 0}}, {"velocidade", 1.5}}}}});
                    objs.push_back({{"tipo", "coletavel"}, {"item", "cristal"}, {"pos", {X, 4.5 + i * 1.3, Z}}});
                }
                for (int i = 0; i < 6; ++i)
                    objs.push_back({{"tipo", "forma"}, {"forma", "cubo"}, {"nome", "Caixa"}, {"material", "madeira"}, {"escala", 0.9},
                                    {"pos", {-4 + i * 1.6, 0.45, -6}}, {"comportamentos", {"destrutivel"}}});
                objs.push_back({{"tipo", "inimigo"}, {"arma", "pistola"}, {"pos", {0, 0, -14}}, {"vida", 120}});
                objs.push_back({{"tipo", "luz"}, {"cor", "#88aaff"}, {"pos", {0, 6, 0}}, {"intensidade", 10}, {"raio", 18}});
                rules = {{"objetivo", "pontos"}, {"quantidade", 150}, {"titulo", x.nome}, {"descricao", "Faça 150 pontos: cristais nas plataformas e caixas"}};
            }
            json res = json::parse(std::string(engine_tool_call(e, "criar_objetos", json{{"objetos", objs}}.dump().c_str())));
            if (res.contains("erros")) { std::printf("  [ERRO] %s: %s\n", x.file, res["erros"].dump().c_str()); ++g_fail; }
            engine_tool_call(e, "definir_regras", rules.dump().c_str());
            engine_tool_call(e, "definir_ambiente", json{{"hora", x.hora}}.dump().c_str());
            engine_set_project_name(e, x.nome);
            std::string proj = engine_export_project(e);
            FILE* f = std::fopen((dir + "/" + x.file + ".json").c_str(), "wb");
            if (f) { std::fwrite(proj.data(), 1, proj.size(), f); std::fclose(f); }
            // joga um pouco para ver se não quebra
            engine_set_play(e, 1);
            frames(e, 90);
            float st[8]; engine_game_status(e, st);
            char name[96];
            std::snprintf(name, sizeof(name), "15_%s.png", x.file);
            save(out, name);
            std::printf("  %s: %zu bytes, vida %.0f, objetivo '%s'\n", x.nome, proj.size(), st[1], engine_game_text(e, 0));
            engine_set_play(e, 0);
        }
    }

    // ---- [16] exportar .glb e abrir de novo (personagem animado + cidade inteira)
    std::printf("[16] exportar .glb\n");
    {
        engine_new_scene(e);
        int ch = engine_create_random_character(e, 77, 1);
        engine_create_weapon(e, 1);
        std::string charPath = out + "/16_personagem.glb", cityPath = out + "/16_cidade.glb";
        if (!engine_export_glb(e, ch, charPath.c_str())) { std::printf("  [ERRO] exportar personagem: %s\n", engine_last_error(e)); ++g_fail; }
        engine_load_map(e, 1, 7);
        if (!engine_export_glb(e, -1, cityPath.c_str())) { std::printf("  [ERRO] exportar cidade: %s\n", engine_last_error(e)); ++g_fail; }
        for (const std::string& pth : {charPath, cityPath}) {
            FILE* f = std::fopen(pth.c_str(), "rb");
            long size = 0;
            if (f) { std::fseek(f, 0, SEEK_END); size = std::ftell(f); std::fclose(f); }
            std::printf("  %s: %.1f KB\n", pth.c_str(), size / 1024.0);
        }
        engine_new_scene(e);
        int id = engine_load_model(e, charPath.c_str());
        std::printf("  personagem reaberto: id %d, animação '%s'\n", id, engine_animation_name(e));
        if (id < 0 || std::string(engine_animation_list(e)).find("Andando") == std::string::npos) {
            std::printf("  [ERRO] personagem exportado sem animações: %s\n", engine_last_error(e)); ++g_fail;
        }
        engine_set_animation(e, 1);
        frames(e, 20);
        save(out, "16_personagem_glb.png");
        engine_new_scene(e);
        id = engine_load_model(e, cityPath.c_str());
        frames(e, 3);
        stats(e, "cidade.glb");
        save(out, "16_cidade_glb.png");
        if (id < 0) { std::printf("  [ERRO] cidade.glb não abriu: %s\n", engine_last_error(e)); ++g_fail; }
    }

    // ---- [17] .obj (sem esqueleto) vira personagem que anda
    std::printf("[17] .obj como personagem\n");
    {
        std::string objPath = out + "/17_estatua.obj";
        FILE* f = std::fopen(objPath.c_str(), "w");
        std::fprintf(f, "o Estatua\n");
        const float V[8][3] = {{-0.3f,0,-0.2f},{0.3f,0,-0.2f},{0.3f,1.8f,-0.2f},{-0.3f,1.8f,-0.2f},{-0.3f,0,0.2f},{0.3f,0,0.2f},{0.3f,1.8f,0.2f},{-0.3f,1.8f,0.2f}};
        for (auto& v : V) std::fprintf(f, "v %f %f %f\n", v[0], v[1], v[2]);
        std::fprintf(f, "f 1 2 3 4\nf 5 8 7 6\nf 1 5 6 2\nf 2 6 7 3\nf 3 7 8 4\nf 5 1 4 8\n");
        std::fclose(f);
        engine_new_scene(e);
        int id = engine_load_model(e, objPath.c_str());
        if (id < 0) { std::printf("  [ERRO] .obj não abriu: %s\n", engine_last_error(e)); ++g_fail; }
        else {
            engine_select(e, id);
            int ch = engine_make_character(e, 1);
            std::printf("  personagem %d (%s)\n", ch, engine_last_error(e));
            float p0[3], rr[3], ss[3], p1[3];
            engine_get_transform(e, ch, p0, rr, ss);
            engine_set_play(e, 1);
            frames(e, 20);
            engine_player_input(e, 0.f, 1.f, 1);
            frames(e, 60);
            engine_player_input(e, 0.f, 0.f, 0);
            engine_get_transform(e, ch, p1, rr, ss);
            engine_set_play(e, 0);
            float moved = std::hypot(p1[0] - p0[0], p1[2] - p0[2]);
            std::printf("  andou %.1f m\n", moved);
            if (ch < 0 || moved < 2.f) { std::printf("  [ERRO] .obj não virou personagem jogável\n"); ++g_fail; }
        }
    }

    engine_destroy(e);
    check("destroy");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    std::printf(g_fail ? "FALHOU (%d problemas)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
