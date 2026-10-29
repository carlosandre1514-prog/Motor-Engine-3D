// Teste headless no PC/CI: cria um contexto GLES 3.2 sem janela (EGL surfaceless, Mesa),
// renderiza as cenas do motor num FBO e salva PNGs. Uso:
//   mobile3d_headless <pasta_shaders> <pasta_saida> [modelo.glb ...]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <glm/glm.hpp>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <sys/stat.h>
#include "json.hpp"
#include "platform/engine_c_api.h"
#include "procgen/TextureGen.h"

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

    auto tCreate = std::chrono::steady_clock::now();
    Engine* e = engine_create(argv[1]);
    std::printf("motor iniciado em %.2f s\n", std::chrono::duration<float>(std::chrono::steady_clock::now() - tCreate).count());
    if (!e) { std::printf("engine_create falhou (shaders?)\n"); return 1; }
    engine_set_target_fbo(e, g_fbo);
    engine_resize(e, W, H);
    check("init");

    // Poses com arma (também rodam no teste completo): personagem de lado e de frente em cada pose
    auto poses = [&](const char* prefix) {
        const int weapons[3] = {1, 0, 4};
        const char* tags[3] = {"fuzil", "pistola", "faca"};
        for (int w = 0; w < 3; ++w) {
            engine_new_scene(e);
            int ch = engine_create_random_character(e, 21, 1);
            engine_select(e, ch);
            engine_create_weapon(e, weapons[w]);
            engine_select(e, ch);
            std::string list = engine_animation_list(e);
            int idx = 0;
            for (size_t pos = 0, nl; (nl = list.find('\n', pos)) != std::string::npos; pos = nl + 1, ++idx) {
                std::string n = list.substr(pos, nl - pos);
                if (n.find(tags[w]) == std::string::npos || (n.find("Parado") != 0 && n.find("Mirando") != 0 && n.find("Golpe (") != 0)) continue;
                engine_set_animation(e, idx);
                frames(e, n.find("Golpe") == 0 ? 8 : 30);
                for (int view = 0; view < 2; ++view) {
                    engine_set_camera(e, 0.f, 1.15f, 0.f, view ? 0.35f : 1.5708f, 0.1f, 2.3f);
                    frames(e, 1);
                    char name[96];
                    std::snprintf(name, sizeof(name), "%s_%s_%s_%s.png", prefix, tags[w], n.substr(0, n.find(' ')).c_str(), view ? "frente" : "lado");
                    save(out, name);
                }
            }
        }
    };
    if (std::getenv("SO_POSES")) { poses("pose"); engine_destroy(e); return g_fail ? 1 : 0; }

    // Interiores: casa com móveis, porta que abre e elevador do prédio até o terraço
    auto interiors = [&]() {
        std::printf("[20] interiores, portas e elevador\n");
        engine_tool_call(e, "carregar_mapa", R"({"tipo":"vazio","com_jogador":true})");
        engine_tool_call(e, "criar_objetos", R"({"objetos":[{"tipo":"construcao","modelo":"casa","semente":3,"pos":[0,0,-8]},
            {"tipo":"construcao","modelo":"predio","semente":5,"pos":[30,0,-10]},{"tipo":"construcao","modelo":"barraco","semente":8,"pos":[-14,0,-6]}]})");
        int player = -1, door = -1, house = -1;
        for (int id = 0; id < engine_object_count(e); ++id) {
            if (!engine_object_valid(e, id)) continue;
            std::string n = engine_object_name(e, id);
            if (n == "Jogador") player = id;
            if (n == "Casa") house = id;
            if (n == "Porta" && door < 0 && engine_object_parent(e, id) == house) door = id;
        }
        // por dentro (editor): câmera no meio da sala
        engine_set_camera(e, 0.f, 1.3f, -7.f, 2.6f, 0.25f, 0.4f);
        frames(e, 2);
        save(out, "20_casa_dentro.png");
        engine_set_camera(e, 0.f, 1.5f, -8.f, 0.4f, 0.35f, 14.f);
        frames(e, 2);
        save(out, "20_casa_fora.png");
        if (player < 0 || door < 0) { std::printf("  [ERRO] sem jogador/porta (%d, %d)\n", player, door); ++g_fail; return; }
        float dp[3], dr[3], ds[3], pp[3], pr[3], ps[3];
        engine_get_transform(e, door, dp, dr, ds);
        engine_get_transform(e, player, pp, pr, ps);
        engine_set_play(e, 1);
        float to[3] = {dp[0] + 0.5f, 0.05f, -8.f + dp[2] + 1.2f};
        engine_set_transform(e, player, to, pr, ps);
        frames(e, 50);
        float r1[3]; engine_get_transform(e, door, dp, r1, ds);
        std::printf("  porta: rotação Y %.0f° com o jogador perto\n", r1[1]);
        if (std::abs(r1[1]) < 45.f) { std::printf("  [ERRO] porta não abriu\n"); ++g_fail; }
        // entra na casa
        engine_player_input(e, 0.f, 0.f, 0);
        float in[3] = {dp[0] + 0.5f, 0.05f, -8.f + dp[2] - 1.5f};
        engine_set_transform(e, player, in, pr, ps);
        frames(e, 20);
        engine_set_camera(e, in[0], 1.4f, in[2], 0.2f, 0.2f, 2.5f);
        frames(e, 2);
        save(out, "20_dentro_play.png");
        // elevador do prédio: procura o ponto do botão andando para o fundo da recepção
        std::string label;
        for (float dz = 0.f; dz < 12.f && label.empty(); dz += 0.4f) {
            float q[3] = {30.f, 0.05f, -10.f - dz};
            engine_set_transform(e, player, q, pr, ps);
            frames(e, 1);
            label = engine_interact_label(e);
        }
        std::printf("  botão: '%s'\n", label.c_str());
        engine_interact(e);
        frames(e, 30);
        float top[3]; engine_get_transform(e, player, top, pr, ps);
        std::printf("  depois do elevador: y = %.1f m, botão agora: '%s'\n", top[1], engine_interact_label(e));
        engine_set_camera(e, top[0], top[1] + 1.5f, top[2], 0.6f, 0.45f, 8.f);
        frames(e, 2);
        save(out, "20_terraco.png");
        if (label.empty() || top[1] < 8.f) { std::printf("  [ERRO] elevador\n"); ++g_fail; }
        engine_set_play(e, 0);
    };
    auto planet = [&]() {
        std::printf("[21] planeta\n");
        engine_load_map(e, 5, 7);
        std::string places = engine_planet_places(e);
        std::printf("%s", places.c_str());
        int nCities = int(std::count(places.begin(), places.end(), '\n'));
        std::vector<unsigned char> img(512 * 512 * 4);
        engine_planet_map(e, 512, img.data());
        stbi_write_png((out + "/21_mapa_do_mundo.png").c_str(), 512, 512, 4, img.data(), 512 * 4);
        frames(e, 3);
        save(out, "21_globo.png");
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.9f, 0.4f, 4200.f);  // gira e afasta o globo
        frames(e, 2);
        save(out, "21_globo2.png");
        // aproxima: chegando perto da superfície, desce para o terreno daquele ponto
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.9f, 0.4f, 2300.f);
        for (int k = 0; k < 40; ++k) frames(e, 1);
        std::printf("  desceu do globo: região '%s'\n", engine_region_name(e));
        save(out, "21_desceu.png");
        // e afastando muito volta para o globo
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.9f, 1.f, 3000.f);
        frames(e, 2);
        save(out, "21_voltou_globo.png");
        frames(e, 3);
        stats(e, "planeta");
        save(out, "21_planeta_visao.png");
        int player = engine_selected(e);
        engine_set_play(e, 1);
        frames(e, 30);
        std::printf("  região: %s\n", engine_region_name(e));
        save(out, "21_capital_play.png");
        float p0[3], rr[3], ss[3], p1[3];
        engine_get_transform(e, player, p0, rr, ss);
        engine_player_input(e, 0.f, 1.f, 1);
        frames(e, 180);
        engine_player_input(e, 0.f, 0.f, 0);
        engine_get_transform(e, player, p1, rr, ss);
        stats(e, "planeta play");
        std::printf("  andou %.1f m na capital (y %.1f -> %.1f)\n", std::hypot(p1[0] - p0[0], p1[2] - p0[2]), p0[1], p1[1]);
        engine_set_play(e, 0);
        if (nCities >= 2 && engine_planet_goto(e, 1)) {
            for (int k = 0; k < 60; ++k) frames(e, 1);  // streaming: 1 chunk por frame
            std::printf("  região: %s\n", engine_region_name(e));
            save(out, "21_cidade2.png");
        }
        // chão do campo: roças em volta da capital, mata e serra (camadas de textura do terreno)
        auto heightAt = [&](float x, float z) {
            char in[96];
            std::snprintf(in, sizeof(in), "{\"pontos\":[[%.1f,%.1f]]}", x, z);
            std::string r = engine_tool_call(e, "altura_terreno", in);
            return float(std::atof(r.c_str() + r.find('[') + 1));
        };
        // procura terra firme: baixada (6-25 m), morro (25-60 m) e serra (> 60 m), em volta da capital
        float spots[4][4] = {{-1822.f + 560.f, 274.f, 0.3f, 0.f}};
        spots[0][3] = heightAt(spots[0][0], spots[0][1]);
        const float lo[3] = {6.f, 25.f, 60.f}, hi[3] = {25.f, 60.f, 400.f};
        for (int t = 0; t < 3; ++t) {
            spots[t + 1][3] = -1e9f;
            for (float rad = 1000.f; rad < 2600.f && spots[t + 1][3] < -1e8f; rad += 120.f)
                for (int k = 0; k < 24; ++k) {
                    float a = k * 0.2618f + t, x = -1822.f + std::cos(a) * rad, z = 274.f + std::sin(a) * rad, h = heightAt(x, z);
                    if (h > lo[t] && h < hi[t]) { spots[t + 1][0] = x; spots[t + 1][1] = z; spots[t + 1][2] = a + 1.f; spots[t + 1][3] = h; break; }
                }
        }
        for (int k = 0; k < 4; ++k) {
            if (spots[k][3] < -1e8f) continue;
            engine_set_camera(e, spots[k][0], std::max(spots[k][3], 0.f) + 1.f, spots[k][1], spots[k][2], 0.22f, 26.f);
            for (int f = 0; f < 50; ++f) frames(e, 1);
            char name[64];
            std::snprintf(name, sizeof(name), "21_chao_%d.png", k);
            std::printf("  chão %d: altura %.0f m\n", k, spots[k][3]);
            save(out, name);
        }
        if (nCities < 3) { std::printf("  [ERRO] poucas cidades\n"); ++g_fail; }
    };
    auto water = [&]() {
        std::printf("[22] água\n");
        for (int mk : {0, 5}) {
            engine_load_map(e, mk, 3);
            // procura mar perto do jogador
            float best[3] = {0, 0, 0};
            bool found = false;
            for (float rad = 40.f; rad < 1200.f && !found; rad += 40.f)
                for (int k = 0; k < 24 && !found; ++k) {
                    float a = k * 0.2618f;
                    float x = std::cos(a) * rad, z = std::sin(a) * rad;
                    if (mk == 5) { x += -1822.f; z += 274.f; }
                    char in[96];
                    std::snprintf(in, sizeof(in), "{\"pontos\":[[%.1f,%.1f]]}", x, z);
                    std::string r = engine_tool_call(e, "altura_terreno", in);
                    float h = std::atof(r.c_str() + r.find('[') + 1);
                    if (h < -3.f) { best[0] = x; best[2] = z; found = true; }
                }
            for (int k = 0; k < 3; ++k) {
                engine_set_camera(e, best[0], 0.f, best[2], 0.4f + k * 0.9f, 0.12f + 0.08f * k, 18.f + k * 10.f);
                frames(e, 40, 1.f / 20.f);
                char name[64];
                std::snprintf(name, sizeof(name), "22_agua_%d_%d.png", mk, k);
                save(out, name);
            }
        }
    };
    if (std::getenv("SO_AGUA")) { water(); engine_destroy(e); return g_fail ? 1 : 0; }
    auto kit = [&]() {
        std::printf("[23] kit cidade\n");
        engine_new_scene(e);
        const char* pieces[] = {"Building_Small_1", "Building_Medium_2_001", "Building_Large_2", "Street_4WayIntersection", "Prop_ACUnit"};
        float x = -30.f;
        for (const char* pc : pieces) {
            int id = engine_load_model(e, (std::string("kit:") + pc).c_str());
            if (id < 0) { std::printf("  [ERRO] kit %s: %s\n", pc, engine_last_error(e)); ++g_fail; continue; }
            float p[3], rr[3], ss[3];
            engine_get_transform(e, id, p, rr, ss);
            p[0] = x; p[2] = 0.f; p[1] += 0.06f;  // acima do chão da cena (sem z-fighting)
            engine_set_transform(e, id, p, rr, ss);
            x += 24.f;
        }
        engine_select(e, -1);
        engine_set_camera(e, 0.f, 8.f, 0.f, 0.35f, 0.15f, 60.f);
        frames(e, 3);
        stats(e, "kit");
        save(out, "23_kit.png");
        engine_set_camera(e, -30.f, 6.f, 0.f, 0.2f, 0.05f, 22.f);
        frames(e, 2);
        save(out, "23_kit_perto.png");
        engine_set_camera(e, 42.f, 0.f, 0.f, 0.6f, 0.7f, 26.f);  // cruzamento visto de cima
        frames(e, 2);
        save(out, "23_kit_rua.png");
    };
    // desempenho no planeta: passeio da câmera (editor) e corrida no Play; tempo de CPU por parte do quadro
    auto perf = [&]() {
        std::printf("[24] desempenho no planeta\n");
        if (const char* q = std::getenv("QUALIDADE")) engine_set_quality(e, std::atoi(q));
        engine_load_map(e, 5, 7);
        frames(e, 3);
        auto run = [&](const char* label, int n, auto&& step) {
            float sum[3] = {0, 0, 0}, mx[3] = {0, 0, 0}, total = 0, worst = 0;
            for (int k = 0; k < n; ++k) {
                step(k);
                auto t0 = std::chrono::steady_clock::now();
                engine_frame(e, 1.f / 30.f);
                float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
                float p[3];
                engine_get_profile(e, p);
                for (int i = 0; i < 3; ++i) { sum[i] += p[i]; mx[i] = std::max(mx[i], p[i]); }
                total += ms; worst = std::max(worst, ms);
            }
            std::printf("  %-18s mundo %.2f (pior %.1f) | lista %.2f (pior %.1f) | render %.2f (pior %.1f) | quadro %.1f (pior %.1f) ms\n", label,
                        sum[0] / n, mx[0], sum[1] / n, mx[1], sum[2] / n, mx[2], total / n, worst);
            stats(e, label);
        };
        run("descendo do globo", 60, [&](int k) { engine_set_camera(e, -1822.f, 0.f, 274.f, 0.9f, 0.5f, 2600.f - k * 40.f); });
        run("voo baixo", 160, [&](int k) { engine_set_camera(e, -1822.f + k * 12.f, 10.f, 274.f + k * 4.f, 0.9f, 0.35f, 120.f); });
        run("voo alto", 160, [&](int k) { engine_set_camera(e, -1822.f + k * 12.f, 10.f, 274.f - k * 6.f, 0.9f, 0.9f, 650.f); });
        engine_planet_goto(e, 0);
        for (int k = 0; k < 40; ++k) frames(e, 1);
        engine_set_play(e, 1);
        frames(e, 10);
        run("correndo (play)", 240, [&](int k) { engine_player_input(e, k % 80 < 40 ? 0.7f : -0.7f, 0.7f, 1); });
        engine_player_input(e, 0.f, 0.f, 0);
        engine_set_play(e, 0);
    };
    // abre um arquivo 3D qualquer e salva vistas (para conferir modelos enviados): SO_MODELO=/caminho/arquivo.fbx
    if (const char* path = std::getenv("SO_MODELO")) {
        engine_new_scene(e);
        int id = engine_load_model(e, path);
        std::printf("modelo %s -> %d %s\n", path, id, engine_last_error(e));
        if (id >= 0) {
            float p[3], r[3], sc[3];
            engine_get_transform(e, id, p, r, sc);
            std::printf("  pos %.2f %.2f %.2f escala %.3f\n", p[0], p[1], p[2], sc[0]);
            engine_select(e, id);
            engine_focus_selected(e);
            frames(e, 2);
            save(out, "modelo_1.png");
            engine_select(e, -1);
            frames(e, 2);
            save(out, "modelo_2.png");
            stats(e, "modelo");
        }
        engine_destroy(e);
        return id >= 0 ? 0 : 1;
    }
    // armas de arquivo e acessórios (mira red dot, coronha, empunhadura) na mão do personagem
    auto weapons = [&]() {
        std::printf("[25] armas com acessórios\n");
        engine_new_scene(e);
        int ch = engine_create_random_character(e, 5, 1);
        engine_select(e, ch);
        int w = engine_create_weapon_ex(e, 1, "", 0, 1.f, 7);
        if (w < 0) { std::printf("  [ERRO] fuzil com acessórios: %s\n", engine_last_error(e)); ++g_fail; }
        engine_set_camera(e, 0.f, 1.3f, 0.f, 1.2f, 0.1f, 1.6f);
        frames(e, 3);
        save(out, "25_fuzil_acessorios.png");
        const char* m4 = std::getenv("ARMA_ARQUIVO");
        std::string kitM4 = "kit:armas/m4a1";
        engine_select(e, ch);
        w = engine_create_weapon_ex(e, 1, m4 ? m4 : kitM4.c_str(), 0, 1.f, 0);
        if (w < 0) { std::printf("  [ERRO] arma de arquivo: %s\n", engine_last_error(e)); ++g_fail; }
        frames(e, 3);
        save(out, "25_m4.png");
        engine_select(e, ch);
        w = engine_create_weapon_ex(e, 1, m4 ? m4 : kitM4.c_str(), 0, 1.f, 1);
        frames(e, 3);
        save(out, "25_m4_mira.png");
        engine_set_camera(e, 0.f, 1.f, 0.f, 0.6f, 0.15f, 4.f);
        frames(e, 3);
        save(out, "25_m4_corpo.png");
        // de perto, soltas: fuzil do motor e M4, as duas com os 3 acessórios
        engine_new_scene(e);
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.f, 0.f, 3.f);
        frames(e, 1);
        int a = engine_create_weapon_ex(e, 1, "", 0, 1.f, 7);
        int b = engine_create_weapon_ex(e, 1, m4 ? m4 : kitM4.c_str(), 0, 1.f, 7);
        float p[3], rr[3], ss[3];
        if (a >= 0 && engine_get_transform(e, a, p, rr, ss)) { p[0] = -0.f; p[1] = 0.9f; p[2] = 0.f; rr[0] = rr[1] = rr[2] = 0.f; engine_set_transform(e, a, p, rr, ss); }
        if (b >= 0 && engine_get_transform(e, b, p, rr, ss)) { p[0] = 0.f; p[1] = 0.5f; p[2] = 0.f; rr[0] = rr[1] = rr[2] = 0.f; engine_set_transform(e, b, p, rr, ss); }
        engine_select(e, -1);
        engine_set_camera(e, 0.f, 0.7f, 0.15f, 1.5707963f, 0.05f, 1.6f);
        frames(e, 3);
        save(out, "25_lado.png");
        // armas prontas do app, lado a lado (cano para a esquerda da imagem)
        engine_new_scene(e);
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.f, 0.f, 3.f);
        frames(e, 1);
        const char* prontas[] = {"kit:armas/m4a1", "kit:armas/m4colt", "kit:armas/ak47", "kit:armas/ak74", "kit:armas/ak74_tatica"};
        for (int k = 0; k < 5; ++k) {
            int w = engine_create_weapon_ex(e, 1, prontas[k], 0, 1.f, 0);
            if (w < 0) { std::printf("  [ERRO] %s: %s\n", prontas[k], engine_last_error(e)); ++g_fail; continue; }
            float p[3], rr[3], ss[3];
            engine_get_transform(e, w, p, rr, ss);
            p[0] = 0.f; p[1] = 1.3f - k * 0.28f; p[2] = 0.f; rr[0] = rr[1] = rr[2] = 0.f;
            engine_set_transform(e, w, p, rr, ss);
        }
        engine_select(e, -1);
        engine_set_camera(e, 0.f, 0.75f, 0.1f, 1.5707963f, 0.05f, 2.1f);
        frames(e, 3);
        save(out, "25_prontas.png");
        engine_set_camera(e, 0.f, 0.75f, 0.1f, 1.2f, 0.12f, 0.75f);
        frames(e, 3);
        save(out, "25_prontas_perto.png");
    };
    // Play direto do globo (como no celular): o jogador tem que estar em terra, com céu de dia
    auto globePlay = [&]() {
        std::printf("[26] play direto do globo\n");
        engine_load_map(e, 5, 7);
        frames(e, 3);
        save(out, "26_globo.png");
        engine_set_play(e, 1);
        for (int k = 0; k < 90; ++k) frames(e, 1);
        save(out, "26_play.png");
        int pl = -1;
        for (int i = 0; i < engine_object_count(e) && pl < 0; ++i)
            if (std::strcmp(engine_object_name(e, i), "Jogador") == 0) pl = i;
        float p[3] = {0, 0, 0}, r[3], s[3];
        if (pl >= 0) engine_get_transform(e, pl, p, r, s);
        std::printf("  jogador %d em %.1f %.1f %.1f, região %s\n", pl, p[0], p[1], p[2], engine_region_name(e));
        engine_player_input(e, 0.f, 1.f, 1);
        for (int k = 0; k < 60; ++k) frames(e, 1);
        engine_player_input(e, 0.f, 0.f, 0);
        save(out, "26_play_andou.png");
        if (pl >= 0) engine_get_transform(e, pl, p, r, s);
        std::printf("  depois de correr: %.1f %.1f %.1f\n", p[0], p[1], p[2]);
        if (p[1] < 0.5f) { std::printf("  [ERRO] jogador caiu na água/embaixo do chão\n"); ++g_fail; }
        engine_set_play(e, 0);
        // projeto salvo em outra cidade, com regras de jogo, reaberto (como o app faz ao abrir)
        engine_planet_goto(e, 2);
        for (int k = 0; k < 40; ++k) frames(e, 1);
        engine_tool_call(e, "definir_regras", "{\"tempo\":120,\"vida\":100}");
        std::string proj = engine_export_project(e);
        std::printf("  projeto: %zu bytes\n", proj.size());
        engine_new_scene(e);
        frames(e, 2);
        if (!engine_import_project(e, proj.c_str())) { std::printf("  [ERRO] reabrir: %s\n", engine_last_error(e)); ++g_fail; }
        for (int k = 0; k < 5; ++k) frames(e, 1);
        save(out, "26_reaberto.png");
        engine_set_play(e, 1);
        for (int k = 0; k < 90; ++k) frames(e, 1);
        save(out, "26_reaberto_play.png");
        pl = -1;
        for (int i = 0; i < engine_object_count(e) && pl < 0; ++i)
            if (std::strcmp(engine_object_name(e, i), "Jogador") == 0) pl = i;
        if (pl >= 0) engine_get_transform(e, pl, p, r, s);
        std::printf("  reaberto: jogador %d em %.1f %.1f %.1f, região %s\n", pl, p[0], p[1], p[2], engine_region_name(e));
        if (pl < 0 || p[1] < 0.5f) { std::printf("  [ERRO] reaberto: jogador sumiu ou caiu\n"); ++g_fail; }
        engine_set_play(e, 0);
        // reabrir o projeto com o globo na tela (céu de espaço) e dar Play: o céu tem que voltar a ser de dia
        engine_load_map(e, 5, 7);
        frames(e, 3);
        engine_import_project(e, proj.c_str());
        frames(e, 3);
        engine_set_play(e, 1);
        for (int k = 0; k < 60; ++k) frames(e, 1);
        save(out, "26_reaberto_do_globo.png");
        std::vector<unsigned char> px(4);
        glReadPixels(W / 2, H - 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::printf("  céu no topo da tela: %d %d %d\n", px[0], px[1], px[2]);
        if (px[2] < 90) { std::printf("  [ERRO] céu escuro depois de reabrir do globo\n"); ++g_fail; }
        engine_set_play(e, 0);
    };
    // MOCAP_LISTA=<manifesto> MOCAP_OUT=<pasta>: converte as animações (Mixamo) para .anim do motor e escreve a
    // biblioteca.txt. Linhas do manifesto: arquivo.fbx;saida.anim;papel (- = só na lista);Nome;modo (solto|lugar|pose)
    if (const char* list = std::getenv("MOCAP_LISTA")) {
        std::string outDir = std::getenv("MOCAP_OUT") ? std::getenv("MOCAP_OUT") : ".";
        std::string base = list;
        base = base.substr(0, base.find_last_of('/') + 1);
        FILE* in = std::fopen(list, "r");
        FILE* lib = std::fopen((outDir + "/biblioteca.txt").c_str(), "w");
        if (!in || !lib) { std::printf("  [ERRO] manifesto ou saída\n"); return 1; }
        std::fprintf(lib, "# gerado por tools/headless_test (MOCAP_LISTA): arquivo;papel;Nome;loop\n");
        char line[512];
        while (std::fgets(line, sizeof(line), in)) {
            std::string l = line;
            l.erase(l.find_last_not_of("\r\n") + 1);
            if (l.empty() || l[0] == '#') continue;
            std::vector<std::string> f;
            size_t a = 0;
            for (size_t b; (b = l.find(';', a)) != std::string::npos; a = b + 1) f.push_back(l.substr(a, b - a));
            f.push_back(l.substr(a));
            if (f.size() < 5) continue;
            int mode = f[4] == "lugar" ? 1 : f[4] == "pose" ? 2 : 0;
            float down = f.size() >= 6 && f[5].rfind("abaixar=", 0) == 0 ? float(std::atof(f[5].c_str() + 8)) : 0.f;
            int n = engine_mocap_bake((base + f[0]).c_str(), (outDir + "/" + f[1]).c_str(), mode, f[3].c_str(), down);
            std::printf("  %-34s %-24s %s %4d quadros\n", f[0].c_str(), f[2].c_str(), n ? "ok" : "ERRO", n);
            if (!n) { ++g_fail; continue; }
            std::fprintf(lib, "%s;%s;%s;%d\n", f[1].c_str(), f[2].c_str(), f[3].c_str(), mode == 1 ? 1 : 0);
        }
        std::fclose(in);
        std::fclose(lib);
        engine_destroy(e);
        return g_fail ? 1 : 0;
    }
    // SO_ANIMS: personagem do motor tocando cada animação da lista (2 momentos de cada), para conferir
    if (std::getenv("SO_ANIMS")) {
        engine_new_scene(e);
        int ch = engine_create_random_character(e, 3, 0);
        engine_select(e, ch);
        engine_create_weapon(e, 1);
        engine_select(e, ch);
        engine_set_anim_preview(e, 1);
        std::string names = engine_animation_list(e);
        int idx = 0;
        size_t pos = 0;
        while (pos < names.size()) {
            size_t nl = names.find('\n', pos);
            std::string nm = names.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? names.size() : nl + 1;
            const char* only = std::getenv("SO_ANIMS_DE");
            if (!only || idx >= std::atoi(only)) {
                engine_set_animation(e, idx);
                for (int k = 0; k < 2; ++k) {
                    engine_set_camera(e, 0.f, 0.9f, 0.f, 0.7f + 0.0f * k, 0.15f, 3.6f);
                    frames(e, k == 0 ? 8 : 18);
                    char fn[96];
                    std::snprintf(fn, sizeof(fn), "anim_%02d_%d.png", idx, k);
                    save(out, fn);
                }
                std::printf("  %02d %s\n", idx, nm.c_str());
            }
            ++idx;
        }
        engine_destroy(e);
        return 0;
    }
    if (std::getenv("SO_GLOBO_PLAY")) { globePlay(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    // mão esquerda na arma: personagem com a M4A1 nas animações de fuzil, de perto (lado e frente)
    auto supportHand = [&]() {
        std::printf("[27] mão esquerda na arma\n");
        engine_new_scene(e);
        int ch = -1;
        if (std::getenv("SOLDADO")) {  // personagem pronto do app (modelo importado, animações do motor por retarget)
            int id = engine_load_model(e, std::getenv("SOLDADO_ARQ") ? std::getenv("SOLDADO_ARQ") : "kit:personagens/soldado");
            engine_select(e, id);
            ch = engine_make_character(e, 1);
            if (ch < 0) { std::printf("  [ERRO] soldado: %s\n", engine_last_error(e)); ++g_fail; return; }
            float sp[3], sr[3], ss[3];
            engine_get_transform(e, ch, sp, sr, ss);
            std::printf("  soldado em %.2f %.2f %.2f\n", sp[0], sp[1], sp[2]);
            sp[0] = sp[2] = 0.f; sp[1] = 0.f; sr[0] = sr[1] = sr[2] = 0.f;
            engine_set_transform(e, ch, sp, sr, ss);
        } else ch = engine_create_random_character(e, 3, 1);
        engine_select(e, ch);
        const char* arma = std::getenv("ARMA_TESTE") ? std::getenv("ARMA_TESTE") : "kit:armas/m4a1";
        if (!std::getenv("SEM_ARMA")) engine_create_weapon_ex(e, std::getenv("PISTOLA") ? 0 : 1, std::getenv("PISTOLA") ? "" : arma, 0, 1.f, 0);
        engine_select(e, ch);
        engine_set_anim_preview(e, 1);
        std::vector<int> clips = {5, 6, 7, 8, 9, 22, 23, 24, 25};
        if (std::getenv("PISTOLA")) clips = {10, 11, 12, 13, 14};
        if (std::getenv("TODAS_ANIMS")) { clips.clear(); for (int k = 0; k < 48; ++k) clips.push_back(k); }
        for (size_t k = 0; k < clips.size(); ++k) {
            engine_set_animation(e, clips[k]);
            frames(e, 12);
            for (int v = 0; v < 2; ++v) {
                engine_set_camera(e, 0.f, 1.15f, 0.f, std::getenv("FRENTE") ? (v ? 3.14159f : 0.f) : (v ? -1.5707963f : 1.5707963f), 0.12f, std::getenv("LONGE") ? 8.f : 2.f);
                frames(e, 1, 0.f);
                char fn[64];
                std::snprintf(fn, sizeof(fn), "27_mao_%02d_%d.png", clips[k], v);
                save(out, fn);
            }
        }
    };
    if (std::getenv("SO_MAO")) { supportHand(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    // construção livre: andares empilhados, paredes com porta/janela, escadas, varanda, pisos
    auto modules = [&]() {
        std::printf("[28] construção por andares\n");
        engine_new_scene(e);
        auto add = [&](const std::string& spec) {
            int id = engine_add_spec(e, spec.c_str());
            if (id < 0) { std::printf("  [ERRO] %s: %s\n", spec.c_str(), engine_last_error(e)); ++g_fail; }
            return id;
        };
        int t = add(R"({"tipo":"modulo","pos":[0,0,0],"largura":5,"profundidade":5,"paredes":["porta","lisa","janela","janela"],"material":"tijolo","piso":"ceramica","escada":"interna","varanda":true})");
        int a2 = add(R"({"tipo":"modulo","sobre":)" + std::to_string(t) + R"(,"largura":5,"profundidade":5,"paredes":["janela","janela","lisa","porta"],"material":"reboco","cor_parede":"#e8b04a","escada":"externa","varanda":true})");
        add(R"({"tipo":"modulo","sobre":)" + std::to_string(a2) + R"(,"largura":5,"profundidade":5,"paredes":["aberta","aberta","aberta","aberta"],"altura":0.1,"laje":false,"mureta":false,"peca":"laje"})");
        int b = add(R"({"tipo":"modulo","pos":[9,0,0],"largura":4,"profundidade":4,"paredes":["porta","janela","lisa","lisa"],"material":"tijolo_vermelho","piso":"madeira","mureta":true})");
        (void)b;
        add(R"({"tipo":"modulo","peca":"piso","pos":[0,0,7],"largura":6,"profundidade":3,"piso":"pedra"})");
        add(R"({"tipo":"modulo","peca":"piso","pos":[7,0,7],"largura":6,"profundidade":3,"piso":"calcada"})");
        add(R"({"tipo":"modulo","peca":"parede","pos":[-6,0,4],"largura":3,"parede":"janela","material":"concreto"})");
        // como o app manda (todas as chaves); empilhar com outro tamanho usa o tamanho de baixo
        int u = add(R"({"tipo":"modulo","pos":[-14,0,6],"largura":4.0,"profundidade":4.0,"altura":2.8,"paredes":["porta","janela","lisa","janela"],"material":"reboco","cor_parede":"#8fb9d8","piso":"marmore","laje":true,"mureta":false,"varanda":true,"escada":"externa"})");
        add(R"({"tipo":"modulo","largura":9.0,"profundidade":9.0,"altura":2.8,"paredes":["janela","janela","lisa","janela"],"material":"tijolo","cor_parede":"#ffffff","piso":"ceramica","laje":true,"mureta":true,"varanda":true,"escada":"nenhuma","sobre":)" + std::to_string(u) + "}");
        for (const char* pc : {"varanda", "escada", "telhado", "laje"})
            add(std::string(R"({"tipo":"modulo","peca":")") + pc + R"(","pos":[)" + std::to_string(-26 + 6 * int(std::string("telv").find(pc[0]))) + R"(,0,14],"largura":4.0,"profundidade":4.0,"altura":2.8,"parede":"lisa","material":"tijolo","cor_parede":"#f2ebdd","piso":"pedra"})");
        add(R"({"tipo":"construcao","modelo":"barraco","andares":1,"pos":[-7,0,-6],"semente":5})");
        add(R"({"tipo":"construcao","modelo":"barraco","andares":4,"pos":[-12,0,-6],"semente":5})");
        engine_select(e, -1);
        for (int v = 0; v < 3; ++v) {
            engine_set_camera(e, 1.f, 3.f, 1.f, 0.6f + v * 1.4f, 0.35f, 17.f);
            frames(e, 3);
            char fn[64];
            std::snprintf(fn, sizeof(fn), "28_construcao_%d.png", v);
            save(out, fn);
        }
        engine_set_camera(e, -16.f, 1.f, 10.f, 0.4f, 0.4f, 20.f);
        frames(e, 3);
        save(out, "28_pecas.png");
        // cômodo sem laje visto de cima: escada interna, piso de cerâmica, porta e janelas
        add(R"({"tipo":"modulo","pos":[0,0,-12],"largura":5,"profundidade":5,"paredes":["porta","janela","janela","lisa"],"piso":"ceramica","escada":"interna","laje":false})");
        engine_set_camera(e, 0.f, 1.f, -12.f, 0.5f, 1.1f, 9.f);
        frames(e, 3);
        save(out, "28_escada_cima.png");
        // dentro do térreo: escada interna e abertura na laje
        engine_set_camera(e, 0.f, 1.6f, 0.f, 2.6f, 0.25f, 2.2f);
        frames(e, 3);
        save(out, "28_dentro.png");
        // o personagem sobe a escada interna até o segundo andar
        int ch = engine_create_random_character(e, 7, 1);
        float p[3], r[3], sc[3];
        engine_get_transform(e, ch, p, r, sc);
        p[0] = 1.9f; p[1] = 0.3f; p[2] = 2.1f; r[0] = r[2] = 0.f; r[1] = 180.f;  // olhando para o fundo
        engine_set_transform(e, ch, p, r, sc);
        engine_set_play(e, 1);
        frames(e, 20);
        engine_player_input(e, 0.f, 1.f, 0);  // anda para a frente (a escada sobe para o fundo, -Z)
        for (int k = 0; k < 150; ++k) {
            frames(e, 1);
            if (k % 30 == 0) { engine_get_transform(e, ch, p, r, sc); std::printf("  %.2f %.2f %.2f\n", p[0], p[1], p[2]); }
        }
        engine_player_input(e, 0.f, 0.f, 0);
        engine_get_transform(e, ch, p, r, sc);
        save(out, "28_escada_play.png");
        std::printf("  depois da escada: y = %.2f (segundo andar a partir de %.2f)\n", p[1], 0.15f + 2.8f + 0.15f);
        if (p[1] < 2.5f) { std::printf("  [ERRO] não subiu a escada interna\n"); ++g_fail; }
        engine_set_play(e, 0);
    };
    // Personagem andando no Play guiado por uma direção no mundo (câmera fixa em yaw 0: frente = -Z)
    auto steer = [&](int ch, int maxFrames, const std::function<glm::vec2(glm::vec3)>& dirOf, int run = 0) {
        float p[3], r[3], sc[3];
        for (int k = 0; k < maxFrames; ++k) {
            engine_get_transform(e, ch, p, r, sc);
            glm::vec2 d = dirOf(glm::vec3(p[0], p[1], p[2]));
            if (std::getenv("DEPURAR") && k % 40 == 0) std::printf("    %3d: %.2f %.2f %.2f -> %.2f %.2f\n", k, p[0], p[1], p[2], d.x, d.y);
            if (glm::length(d) > 1.f) d = glm::normalize(d);
            engine_set_camera(e, p[0], p[1] + 1.f, p[2], 0.f, 0.3f, 4.f);
            engine_player_input(e, d.x, -d.y, run);
            frames(e, 1);
        }
        engine_player_input(e, 0.f, 0.f, 0);
        engine_get_transform(e, ch, p, r, sc);
        return glm::vec3(p[0], p[1], p[2]);
    };
    auto placeChar = [&](int ch, glm::vec3 at) {
        float p[3], r[3], sc[3];
        engine_get_transform(e, ch, p, r, sc);
        p[0] = at.x; p[1] = at.y; p[2] = at.z;
        engine_set_transform(e, ch, p, r, sc);
    };

    // Escadas em L, em U e caracol: vistas de cima (sem laje) e o personagem subindo até a laje
    auto stairs = [&]() {
        std::printf("[29] escadas reta, em L, em U e caracol\n");
        engine_new_scene(e);
        const char* kinds[] = {"reta", "l", "u", "caracol"};
        for (int k = 0; k < 4; ++k) {
            char spec[256];
            std::snprintf(spec, sizeof(spec), R"({"tipo":"modulo","pos":[%d,0,0],"largura":5,"profundidade":5,"paredes":["porta","lisa","lisa","lisa"],"escada":"%s","laje":false})", k * 7, kinds[k]);
            engine_add_spec(e, spec);
            std::snprintf(spec, sizeof(spec), R"({"tipo":"modulo","pos":[%d,0,9],"largura":5,"profundidade":5,"paredes":["porta","lisa","lisa","lisa"],"escada":"%s"})", k * 7, kinds[k]);
            engine_add_spec(e, spec);
            std::snprintf(spec, sizeof(spec), R"({"tipo":"modulo","peca":"escada","pos":[%d,0,-8],"largura":3,"profundidade":3,"altura":2.8,"escada":"%s"})", k * 7, kinds[k]);
            engine_add_spec(e, spec);
        }
        engine_select(e, -1);
        engine_set_camera(e, 10.f, 0.f, -1.f, 0.25f, 1.15f, 22.f);
        frames(e, 3);
        save(out, "29_escadas_cima.png");
        engine_set_camera(e, 10.f, 0.f, -6.f, 0.5f, 0.45f, 16.f);
        frames(e, 3);
        save(out, "29_escadas_avulsas.png");
        // sobe cada uma (casas da fileira z = 9, com laje). Topo da laje: 0.15 + 2.8 + 0.15
        const float xL = 2.35f, zB = -2.35f, top = 3.1f;
        int ch = engine_create_random_character(e, 7, 1);
        for (int k = 0; k < 4; ++k) {
            const float ox = k * 7.f, oz = 9.f;
            engine_set_play(e, 0);
            if (k == 3) placeChar(ch, {ox + 1.3f + 0.15f, 0.3f, oz - 1.32f + 0.95f});
            else placeChar(ch, {ox + xL - 0.45f, 0.3f, oz + 1.9f});
            engine_set_play(e, 1);
            frames(e, 10);
            if (std::getenv("DEPURAR")) { float q[3], r[3], sc[3]; engine_get_transform(e, ch, q, r, sc); std::printf("  início %.2f %.2f %.2f\n", q[0], q[1], q[2]); }
            int phase = 0;
            glm::vec2 last(0.f);
            glm::vec3 end = steer(ch, 420, [&](glm::vec3 p) -> glm::vec2 { return last = [&]() -> glm::vec2 {
                float lx = p.x - ox, lz = p.z - oz;
                if (p.y > top - 0.04f && phase < 9) phase = 9;  // chegou na laje: anda mais um pouco e para
                if (phase >= 9) { ++phase; return phase > 18 ? glm::vec2(0.f) : k == 0 ? glm::vec2(-1.f, 0.f) : last; }  // reta: sai pelo lado
                switch (k) {
                    case 0: return {0.f, -1.f};
                    case 1: if (lz > zB + 0.5f && phase == 0) return {0.f, -1.f}; phase = 1; return {-1.f, 0.f};
                    case 2:
                        if (phase == 0 && lz > zB + 0.45f) return {0.f, -1.f};
                        if (phase <= 1 && lx > xL - 1.35f) { phase = 1; return {-1.f, 0.f}; }
                        phase = 2; return {0.f, 1.f};
                    default: {  // caracol: anda na tangente, puxando para o raio do meio
                        const float cx = 1.32f, cz = -1.32f, rm = 0.53f;
                        float dx = lx - cx, dz = lz - cz, rr = std::sqrt(dx * dx + dz * dz) + 1e-4f;
                        float phi = std::atan2(-dz, dx);
                        glm::vec2 tan(-std::sin(phi), -std::cos(phi)), rad(dx / rr, dz / rr);
                        return tan + rad * ((rm - rr) * 3.f);
                    }
                }
            }(); });
            std::printf("  escada %-8s topo em y = %.2f (laje %.2f)\n", kinds[k], end.y, top);
            if (end.y < top - 0.25f) { std::printf("  [ERRO] não subiu a escada %s\n", kinds[k]); ++g_fail; }
        }
        engine_set_play(e, 0);
    };

    // Abrir porta / janela / vão em parede pronta e cortar um muro
    auto openings = [&]() {
        std::printf("[30] aberturas em paredes e muros\n");
        engine_new_scene(e);
        int house = engine_add_spec(e, R"({"tipo":"modulo","pos":[0,0,0],"largura":5,"profundidade":5,"paredes":["lisa","lisa","lisa","lisa"],"material":"reboco","cor_parede":"#e8d9b0"})");
        int muro = engine_add_spec(e, R"({"tipo":"construcao","modelo":"muro","pos":[0,0,10],"semente":1})");
        if (house < 0 || muro < 0) { std::printf("  [ERRO] cena\n"); ++g_fail; return; }
        auto cut = [&](float tx, float ty, float tz, float yaw, const char* spec) {
            engine_select(e, -1);
            engine_set_camera(e, tx, ty, tz, yaw, 0.f, 6.f);
            frames(e, 1);
            int id = engine_add_spec(e, spec);
            if (id < 0) { std::printf("  [ERRO] %s: %s\n", spec, engine_last_error(e)); ++g_fail; }
            return id;
        };
        cut(0.f, 1.2f, 2.5f, 0.f, R"({"tipo":"abertura","estilo":"porta"})");              // frente da casa
        cut(1.f, 1.5f, -2.5f, 3.14159f, R"({"tipo":"abertura","estilo":"janela","largura":1.4})");  // fundo
        cut(-2.5f, 1.2f, 0.f, -1.5708f, R"({"tipo":"abertura","estilo":"vao","largura":1.2})");    // lado direito
        cut(1.5f, 1.f, 10.f, 0.f, R"({"tipo":"abertura","estilo":"corte","largura":2.0})");      // muro
        engine_select(e, -1);
        engine_set_camera(e, 0.f, 1.f, 4.f, 0.5f, 0.3f, 13.f);
        frames(e, 3);
        save(out, "30_aberturas_frente.png");
        engine_set_camera(e, 0.f, 1.f, 2.f, 3.14159f + 0.4f, 0.3f, 11.f);
        frames(e, 3);
        save(out, "30_aberturas_fundo.png");
        engine_set_camera(e, 0.f, 1.4f, 0.f, 0.3f, 0.1f, 1.5f);
        frames(e, 3);
        save(out, "30_dentro.png");
        // passa pela porta cortada e pelo corte do muro
        int ch = engine_create_random_character(e, 9, 1);
        placeChar(ch, {0.f, 0.2f, 4.f});
        engine_set_play(e, 1);
        frames(e, 10);
        glm::vec3 p = steer(ch, 120, [](glm::vec3) { return glm::vec2(0.f, -1.f); });
        std::printf("  entrou pela porta cortada: z = %.2f (parede em 2.5)\n", p.z);
        if (p.z > 1.5f) { std::printf("  [ERRO] não passou pela porta\n"); ++g_fail; }
        engine_set_play(e, 0);
        placeChar(ch, {1.5f, 0.2f, 12.f});
        engine_set_play(e, 1);
        frames(e, 10);
        p = steer(ch, 120, [](glm::vec3) { return glm::vec2(0.f, -1.f); });
        std::printf("  passou pelo corte do muro: z = %.2f (muro em 10)\n", p.z);
        if (p.z > 9.f) { std::printf("  [ERRO] não passou pelo muro\n"); ++g_fail; }
        engine_set_play(e, 0);
        placeChar(ch, {-3.5f, 0.2f, 12.f});
        engine_set_play(e, 1);
        frames(e, 10);
        p = steer(ch, 120, [](glm::vec3) { return glm::vec2(0.f, -1.f); });
        std::printf("  fora do corte o muro segura: z = %.2f\n", p.z);
        if (p.z < 10.f) { std::printf("  [ERRO] atravessou o muro fora do corte\n"); ++g_fail; }
        engine_set_play(e, 0);
    };
    // Mapa da favela (projeto "Favela Game"): vistas e o jogador subindo da entrada até o campinho
    auto favela = [&]() {
        std::printf("[31] mapa da favela\n");
        auto t0 = std::chrono::steady_clock::now();
        engine_tool_call(e, "carregar_mapa", R"({"tipo":"favela","semente":5,"com_jogador":true})");
        float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  gerado em %.2f s\n", secs);
        auto hAt = [&](float x, float z) {
            nlohmann::json r = nlohmann::json::parse(std::string(engine_tool_call(e, "altura_terreno", nlohmann::json{{"pontos", {{x, z}}}}.dump().c_str())));
            return r["alturas"][0].is_number() ? r["alturas"][0].get<float>() : 0.f;
        };
        engine_select(e, -1);
        struct View { const char* name; float x, y, z, yaw, pitch, dist; bool ground; };
        const View views[] = {
            {"31_visao_geral", 0.f, 12.f, 0.f, 0.55f, 0.72f, 175.f, false},
            {"31_entrada", 0.f, 1.7f, 58.f, 0.f, 0.12f, 6.f, true},
            {"31_campinho", 2.f, 1.f, -41.f, 0.35f, 0.38f, 32.f, true},
            {"31_praca", -33.f, 1.f, 27.f, -0.7f, 0.3f, 20.f, true},
            {"31_beco", 0.f, 1.6f, 30.f, 0.f, 0.06f, 2.5f, true},
            {"31_lajes", 8.f, 9.f, 2.f, 0.8f, 0.28f, 26.f, true},
            {"31_area_alta", -46.f, 1.7f, -45.f, 3.6f, 0.08f, 2.f, true},
            {"31_area_baixa", 45.f, 1.f, 22.f, -2.4f, 0.3f, 22.f, true},
            {"31_lado_norte", 3.f, 6.f, -80.f, 3.14159f, 0.25f, 45.f, true},
            {"31_lado_leste", 80.f, 6.f, 20.f, -1.5708f, 0.25f, 45.f, true},
            {"31_lado_oeste", -80.f, 6.f, 26.f, 1.5708f, 0.25f, 45.f, true},
            {"31_degraus", 4.f, 1.2f, -80.f, 3.14159f + 0.5f, 0.2f, 5.f, true},
            {"31_degraus_2", 85.f, 1.2f, 20.5f, -1.5708f + 0.4f, 0.25f, 6.f, true},
        };
        for (const View& v : views) {
            float y = v.ground ? hAt(v.x, v.z) + v.y : v.y;
            engine_set_camera(e, v.x, y, v.z, v.yaw, v.pitch, v.dist);
            frames(e, 4);
            save(out, (std::string(v.name) + ".png").c_str());
            stats(e, v.name);
        }
        // planta avulsa do kit (3 níveis de detalhe)
        for (const char* pl : {"arvore_2", "capim_florido", "floreira_1"}) {
            std::string sp = std::string(R"({"tipo":"planta","modelo":")") + pl + R"(","pos":[)" + std::to_string(-4 + 4 * int(pl[0] == 'c') + 8 * int(pl[0] == 'f')) + ",10,48]}";
            if (engine_add_spec(e, sp.c_str()) < 0) { std::printf("  [ERRO] planta %s: %s\n", pl, engine_last_error(e)); ++g_fail; }
        }
        // noite: postes acesos e as pichações
        engine_tool_call(e, "definir_ambiente", R"({"hora":21.5})");
        for (const View& v : {views[1], views[4], views[3]}) {
            float y = v.ground ? hAt(v.x, v.z) + v.y : v.y;
            engine_set_camera(e, v.x, y, v.z, v.yaw, v.pitch, v.dist);
            frames(e, 4);
            save(out, (std::string(v.name) + "_noite.png").c_str());
        }
        engine_tool_call(e, "definir_ambiente", R"({"hora":16.9})");
        // desempenho andando no Play
        int pl = -1;
        for (int id = 0; id < engine_object_count(e) && pl < 0; ++id)
            if (engine_object_valid(e, id) && std::string(engine_object_name(e, id)) == "Jogador") pl = id;
        if (pl < 0) { std::printf("  [ERRO] sem jogador\n"); ++g_fail; return; }
        engine_set_play(e, 1);
        frames(e, 15);
        // sobe pela escadaria central: entrada -> (0,22) -> (-14,10) -> (-14,-14) -> (-6,-29) -> campinho
        const glm::vec2 path[] = {{0, 42}, {0, 22}, {-14, 10}, {-14, -14}, {-6, -29}, {-2, -38}};
        size_t wp = 0;
        float worst = 0.f, total = 0.f;
        int n = 0;
        glm::vec3 end = steer(pl, 1200, [&](glm::vec3 p) -> glm::vec2 {
            while (wp < 6 && glm::length(glm::vec2(p.x, p.z) - path[wp]) < 1.2f) ++wp;
            if (wp >= 6) return {0.f, 0.f};
            return glm::normalize(path[wp] - glm::vec2(p.x, p.z));
        }, 1);
        (void)worst; (void)total; (void)n;
        std::printf("  jogador chegou em %.1f %.1f %.1f (alvo: campinho em z = -38), ponto %zu de 6\n", end.x, end.y, end.z, wp);
        if (wp < 6) { std::printf("  [ERRO] não chegou ao campinho pelas escadarias\n"); ++g_fail; }
        save(out, "31_play_campinho.png");
        engine_set_play(e, 0);
        // entrada norte: sobe da rua de baixo até o campinho pela escadaria
        {
            float p[3], r[3], sc[3];
            engine_get_transform(e, pl, p, r, sc);
            p[0] = 3.f; p[2] = -96.f; p[1] = hAt(3.f, -96.f) + 0.3f;
            engine_set_transform(e, pl, p, r, sc);
            engine_set_play(e, 1);
            frames(e, 10);
            const glm::vec2 np[] = {{4, -70}, {2, -53}, {2, -45}};
            size_t w2 = 0;
            glm::vec3 en = steer(pl, 900, [&](glm::vec3 q) -> glm::vec2 {
                while (w2 < 3 && glm::length(glm::vec2(q.x, q.z) - np[w2]) < 1.2f) ++w2;
                if (w2 >= 3) return {0.f, 0.f};
                return glm::normalize(np[w2] - glm::vec2(q.x, q.z));
            }, 1);
            std::printf("  pela entrada norte chegou em %.1f %.1f %.1f, ponto %zu de 3\n", en.x, en.y, en.z, w2);
            if (w2 < 3) { std::printf("  [ERRO] não subiu pela entrada norte\n"); ++g_fail; }
            engine_set_play(e, 0);
        }
        // desempenho jogando (correndo pelas escadarias) em cada qualidade
        for (int q : {1, 0}) {
            engine_set_quality(e, q);
            float p[3], r[3], sc[3];
            engine_get_transform(e, pl, p, r, sc);
            p[0] = 0.f; p[2] = 40.f; p[1] = hAt(0.f, 40.f) + 0.3f;
            engine_set_transform(e, pl, p, r, sc);
            engine_set_play(e, 1);
            frames(e, 10);
            float tot = 0.f, worst = 0.f;
            const glm::vec2 path[] = {{0, 22}, {-14, 10}, {-14, -14}};
            size_t w3 = 0;
            for (int k = 0; k < 150; ++k) {
                engine_get_transform(e, pl, p, r, sc);
                while (w3 < 3 && glm::length(glm::vec2(p[0], p[2]) - path[w3]) < 1.2f) ++w3;
                glm::vec2 d = w3 < 3 ? glm::normalize(path[w3] - glm::vec2(p[0], p[2])) : glm::vec2(0.f);
                engine_set_camera(e, p[0], p[1] + 1.f, p[2], std::atan2(-d.x, -d.y), 0.3f, 5.5f);
                engine_player_input(e, 0.f, glm::length(d) > 0 ? 1.f : 0.f, 1);
                auto t0 = std::chrono::steady_clock::now();
                engine_frame(e, 1.f / 30.f);
                glFinish();
                float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
                tot += ms; worst = std::max(worst, ms);
            }
            engine_player_input(e, 0.f, 0.f, 0);
            std::printf("  qualidade %d jogando: quadro %.1f ms (pior %.1f)\n", q, tot / 150.f, worst);
            stats(e, q ? "jogando_q1" : "jogando_q0");
            save(out, q ? "31_jogando_q1.png" : "31_jogando_q0.png");
            engine_set_play(e, 0);
        }
        engine_set_quality(e, 1);
        // chuva forte no Play: gotas, respingos, céu fechado, trovão
        engine_select(e, pl);
        engine_create_weapon_ex(e, 1, "kit:armas/ak47", 0, 1.f, 0);
        engine_tool_call(e, "definir_ambiente", R"({"chuva":"sempre","ciclo_min":10})");
        engine_set_play(e, 1);
        float wt[3] = {0, 0, 0};
        int thunder = 0, shots = 0;
        for (int k = 0; k < 900; ++k) {
            if (k % 40 == 0) engine_player_fire(e);
            frames(e, 1);
            std::string snd = engine_poll_sounds(e);
            if (snd.find("trovao") != std::string::npos) ++thunder;
            if (snd.find("tiro_ak47") != std::string::npos) ++shots;
        }
        engine_get_weather(e, wt);
        std::printf("  chuva %.2f, hora %.2f (andou com o ciclo), trovões %d, tiros com som %d\n", wt[0], wt[2], thunder, shots);
        if (wt[0] < 0.9f) { std::printf("  [ERRO] não choveu\n"); ++g_fail; }
        if (shots == 0) { std::printf("  [ERRO] tiro sem som\n"); ++g_fail; }
        save(out, "31_chuva.png");
        engine_set_play(e, 0);
        engine_tool_call(e, "definir_ambiente", R"({"chuva":"automatica"})");
    };
    if (std::getenv("SO_VEGETACAO")) {  // modelos de planta convertidos (perto e longe), lado a lado
        engine_new_scene(e);
        engine_tool_call(e, "definir_ambiente", R"({"hora":15.5})");
        const char* names[] = {"arvore_1", "arvore_2", "arvore_3", "arvore_4", "arvore_5", "arvores_dupla", "arvores_trio",
                               "arbusto_redondo_1", "arbusto_redondo_2", "arbusto_1", "arbusto_2", "arbusto_3", "capim_1", "capim_2",
                               "capim_3", "capim_florido", "forracao_1", "forracao_2", "forracao_3", "floreira_1", "floreira_2", "floreira_3"};
        for (int k = 0; k < 22; ++k) {
            for (int far = 0; far < 2; ++far) {
                std::string path = std::string("kit:vegetacao/") + names[k] + (far ? "_longe" : "");
                int id = engine_load_model(e, path.c_str());
                if (id < 0) { if (!(far && k >= 19)) { std::printf("  [ERRO] %s: %s\n", path.c_str(), engine_last_error(e)); ++g_fail; } continue; }
                float p[3], r[3], sc[3];
                engine_get_transform(e, id, p, r, sc);
                p[0] = (k % 8) * 4.f - 14.f + far * 2.f; p[1] = 0.f; p[2] = (k / 8) * 5.f - 5.f + far * 0.f;
                if (far) p[2] -= 0.f;
                engine_set_transform(e, id, p, r, sc);
            }
        }
        engine_select(e, -1);
        engine_set_camera(e, 0.f, 0.8f, 0.f, 0.f, 0.35f, 19.f);
        frames(e, 4);
        save(out, "veg_geral.png");
        engine_set_camera(e, -10.f, 1.3f, -5.f, 0.25f, 0.15f, 6.f);
        frames(e, 4);
        save(out, "veg_arvores.png");
        engine_destroy(e);
        std::printf(g_fail ? "FALHOU\n" : "OK\n");
        return g_fail ? 1 : 0;
    }
    if (std::getenv("SO_MURAIS")) {  // atlas das pichações (confere as fontes)
        eng::setMaterialTextureDir((std::string(argv[1]) + "/materials").c_str());
        const eng::TexData& t = eng::proceduralTexture(eng::Mat::Mural);
        std::vector<uint8_t> px(t.albedo);
        for (size_t i = 0; i < px.size(); i += 4) {  // fundo cinza onde é transparente
            float a = px[i + 3] / 255.f;
            for (int c = 0; c < 3; ++c) px[i + c] = uint8_t(px[i + c] * a + 120 * (1.f - a));
            px[i + 3] = 255;
        }
        stbi_write_png((out + "/murais.png").c_str(), t.w, t.h, 4, px.data(), t.w * 4);
        engine_destroy(e);
        return 0;
    }
    // Polícia x Bandidos no mapa da favela: monta o projeto pelas ferramentas (o mesmo exemplo do app) e joga
    auto teamProject = [&]() {
        using nlohmann::json;
        engine_tool_call(e, "carregar_mapa", R"({"tipo":"favela","semente":5,"com_jogador":true})");
        auto hAt = [&](float x, float z) {
            json r = json::parse(std::string(engine_tool_call(e, "altura_terreno", json{{"pontos", {{x, z}}}}.dump().c_str())));
            return r["alturas"][0].is_number() ? r["alturas"][0].get<float>() : 0.f;
        };
        auto P = [&](float x, float z) { return json::array({x, hAt(x, z), z}); };
        json objs = json::array();
        // fora da favela (rua de baixo, na frente dos prédios): base e as lojas da polícia
        objs.push_back({{"tipo", "base"}, {"time", "policia"}, {"pos", P(0.f, 106.f)}});
        objs.push_back({{"tipo", "base"}, {"time", "policia"}, {"pos", P(-44.f, 106.f)}});
        objs.push_back({{"tipo", "loja"}, {"loja", "armas"}, {"time", "policia"}, {"pos", P(-22.f, 107.5f)}, {"rot_y", 180}});
        objs.push_back({{"tipo", "loja"}, {"loja", "roupas"}, {"time", "policia"}, {"pos", P(22.f, 107.5f)}, {"rot_y", 180}});
        // dentro da favela: base no campinho, armas na área baixa, roupas na praça
        objs.push_back({{"tipo", "base"}, {"time", "bandidos"}, {"pos", P(-8.f, -44.f)}});
        objs.push_back({{"tipo", "base"}, {"time", "bandidos"}, {"pos", P(13.f, -37.f)}});
        objs.push_back({{"tipo", "loja"}, {"loja", "armas"}, {"time", "bandidos"}, {"pos", P(52.f, 28.f)}, {"rot_y", -90}});
        objs.push_back({{"tipo", "loja"}, {"loja", "roupas"}, {"time", "bandidos"}, {"pos", P(-42.f, 32.f)}, {"rot_y", 90}});
        const char* pol[] = {"Sgt. Rocha", "Cb. Lima", "Sd. Nunes", "Ten. Costa", "Sd. Alves"};
        const char* pa[] = {"m4a1", "m4colt", "pistola", "escopeta", "m4a1"};
        const char* ban[] = {"Magrão", "Baixinho", "Cabeça", "Zóio", "Bigode"};
        const char* ba[] = {"ak47", "ak74", "pistola", "escopeta", "ak47"};
        for (int i = 0; i < 5; ++i) {
            // skin de cada um (a mesma da loja: "roupa" 0..4)
            const char* pv[] = {"pm", "civil", "exercito", "choque", "especial"};
            const char* bv[] = {"rubro", "alvinegro", "tricolor", "canarinho", "faixa"};
            objs.push_back({{"tipo", "modelo"}, {"arquivo", std::string("kit:personagens/skins/") + pv[i]}, {"personagem", true},
                            {"time", "policia"}, {"nome", pol[i]}, {"arma", pa[i]}, {"roupa", i}, {"pos", P(-6.f + i * 3.f, 104.f)}, {"rot_y", 180}});
            objs.push_back({{"tipo", "modelo"}, {"arquivo", std::string("kit:personagens/skins/") + bv[i]}, {"personagem", true},
                            {"time", "bandidos"}, {"nome", ban[i]}, {"arma", ba[i]}, {"roupa", i}, {"pos", P(-4.f + i * 3.f, -38.f)}, {"rot_y", 0}});
        }
        json res = json::parse(std::string(engine_tool_call(e, "criar_objetos", json{{"objetos", objs}}.dump().c_str())));
        if (res.contains("erros")) { std::printf("  [ERRO] Polícia x Bandidos: %s\n", res["erros"].dump().c_str()); ++g_fail; }
        engine_tool_call(e, "definir_regras", json{{"objetivo", "times"}, {"quantidade", 25}, {"tempo", 600}, {"titulo", "Polícia x Bandidos"},
                                                  {"descricao", "5 contra 5 na favela. Abates dão $300: compre armas e roupas nas lojas do seu time. Vence quem fizer 25 abates."}}.dump().c_str());
        engine_tool_call(e, "definir_ambiente", R"({"hora":16.5})");
        engine_set_project_name(e, "Polícia x Bandidos");
        return std::string(engine_export_project(e));
    };
    auto times = [&]() {
        using nlohmann::json;
        std::printf("[32] Polícia x Bandidos\n");
        std::string proj = teamProject();
        std::string dir = out + "/exemplos";
        mkdir(dir.c_str(), 0755);
        if (FILE* f = std::fopen((dir + "/5_policia_x_bandidos.json").c_str(), "wb")) { std::fwrite(proj.data(), 1, proj.size(), f); std::fclose(f); }
        if (!engine_team_mode(e)) { std::printf("  [ERRO] objetivo times não ficou ativo\n"); ++g_fail; }
        int before = engine_object_count(e);
        // lojas e bases de cima
        engine_select(e, -1);
        for (auto v : {std::array<float, 6>{0.f, 6.f, 104.f, 3.4f, 0.35f, 22.f}, std::array<float, 6>{-8.f, 0.f, -44.f, 0.5f, 0.4f, 24.f},
                       std::array<float, 6>{52.f, 0.f, 28.f, -1.2f, 0.25f, 9.f}, std::array<float, 6>{-42.f, 0.f, 32.f, 1.9f, 0.25f, 9.f}}) {
            static int n = 0;
            json h = json::parse(std::string(engine_tool_call(e, "altura_terreno", json{{"pontos", {{v[0], v[2]}}}}.dump().c_str())));
            engine_set_camera(e, v[0], (h["alturas"][0].is_number() ? h["alturas"][0].get<float>() : 10.f) + 1.5f, v[2], v[3], v[4], v[5]);
            frames(e, 3);
            char nm[48]; std::snprintf(nm, sizeof(nm), "32_lojas_%d.png", n++);
            save(out, nm);
        }
        engine_set_quality(e, 0);
        engine_team_choose(e, 0);  // jogador na polícia
        engine_set_play(e, 1);
        frames(e, 3);
        std::printf("  %s\n", engine_game_text(e, 1));
        save(out, "32_inicio.png");
        // jogador: vai até a loja de armas da polícia
        json scene = json::parse(std::string(engine_tool_call(e, "ver_cena", "{}")));
        int pid = engine_player_id(e);
        float p[3], r[3], sc[3];
        if (pid < 0 || !engine_get_transform(e, pid, p, r, sc)) {
            std::printf("  [ERRO] sem jogador na partida (has_player %d)\n", engine_has_player(e)); ++g_fail;
            return;
        }
        std::printf("  jogador (id %d) nasceu em %.1f %.1f %.1f\n", pid, p[0], p[1], p[2]);
        p[0] = -22.f; p[2] = 107.5f - 2.f; p[1] += 0.3f;
        engine_set_transform(e, pid, p, r, sc);
        frames(e, 10);
        std::string label = engine_interact_label(e);
        std::printf("  na loja: botão '%s'\n", label.c_str());
        if (label.find("armas") == std::string::npos || !engine_interact(e)) { std::printf("  [ERRO] loja de armas não abriu\n"); ++g_fail; }
        else {
            json shop = json::parse(std::string(engine_shop_json(e)));
            std::printf("  %s: $%d, %zu itens\n", shop["loja"].get<std::string>().c_str(), shop["dinheiro"].get<int>(), shop["itens"].size());
            int m4 = -1;
            for (auto& it : shop["itens"]) if (it["nome"] == "M4A1") m4 = it["id"];
            for (auto& it : shop["itens"]) if (it["nome"] == "AK-47") { std::printf("  [ERRO] AK-47 na loja da polícia\n"); ++g_fail; }
            std::printf("  comprar M4A1: %s\n", engine_shop_buy(e, m4));
            engine_shop_close(e);
        }
        // loja de fardas: compra a PM camuflada ($500) e troca a roupa
        p[0] = 22.f; engine_set_transform(e, pid, p, r, sc);
        frames(e, 10);
        if (engine_interact(e)) {
            json shop = json::parse(std::string(engine_shop_json(e)));
            std::string msg = engine_shop_buy(e, 101);
            std::printf("  comprar farda: %s\n", msg.c_str());
            if (msg.find("Comprou") == std::string::npos) { std::printf("  [ERRO] não comprou a farda\n"); ++g_fail; }
            engine_shop_close(e);
            frames(e, 5);
            save(out, "32_farda_nova.png");
        } else { std::printf("  [ERRO] loja de fardas não abriu\n"); ++g_fail; }
        // loja dos bandidos não abre para policial
        p[0] = 52.f - 2.f; p[2] = 28.f; p[1] = 0.f;
        json h = json::parse(std::string(engine_tool_call(e, "altura_terreno", json{{"pontos", {{p[0], p[2]}}}}.dump().c_str())));
        p[1] = (h["alturas"][0].is_number() ? h["alturas"][0].get<float>() : 12.f) + 0.3f;
        pid = engine_player_id(e);
        engine_set_transform(e, pid, p, r, sc);
        frames(e, 10);
        std::printf("  na loja dos bandidos: '%s'\n", engine_interact_label(e));
        if (engine_interact(e)) { std::printf("  [ERRO] policial abriu a loja dos bandidos\n"); ++g_fail; engine_shop_close(e); }
        // a partida corre sozinha (robôs se caçam pelas escadarias)
        auto t0 = std::chrono::steady_clock::now();
        int s0 = 0, s1 = 0;
        for (int sec = 0; sec < 120; ++sec) {
            frames(e, 20, 0.05f);
            std::sscanf(engine_game_text(e, 0), "%*s Polícia %d x %d", &s0, &s1);
            if (sec % 15 == 14) {
                std::string st = engine_game_text(e, 0), msg = engine_game_text(e, 1);
                std::printf("  %3d s: %s | %s\n", sec + 1, st.c_str(), msg.c_str());
                char nm[48]; std::snprintf(nm, sizeof(nm), "32_partida_%03d.png", sec + 1);
                save(out, nm);
            }
        }
        float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  placar após 2 min: polícia %d x %d bandidos (simulado em %.0f s)\n", s0, s1, secs);
        if (s0 + s1 == 0) { std::printf("  [ERRO] ninguém abateu ninguém em 2 minutos\n"); ++g_fail; }
        engine_set_play(e, 0);
        frames(e, 2);
        int after = engine_object_count(e);
        std::printf("  objetos antes %d, depois do Stop %d\n", before, after);
        scene = json::parse(std::string(engine_tool_call(e, "ver_cena", "{}")));
        int chars = 0;
        for (auto& o : scene["objetos"]) if (o.contains("time")) ++chars;
        std::printf("  personagens com time no projeto: %d\n", chars);
    };
    // Prévia das skins de arma (loja): cada arma do kit com cada textura de skin, renderizada pelo motor
    if (const char* dir = std::getenv("SO_SKINS")) {
        using nlohmann::json;
        engine_new_scene(e);
        engine_tool_call(e, "definir_ambiente", R"({"hora":13,"grade":false,"neblina":0})");
        engine_set_grid(e, 0);
        engine_select(e, -1);
        engine_set_camera(e, 0.f, 1.02f, 0.f, 1.5708f + 0.18f, 0.12f, 1.05f);
        frames(e, 3);
        save(out, "skin_fundo.png");  // fundo sem arma: a diferença vira a transparência da prévia
        const char* guns[] = {"m4a1", "m4colt", "ak47", "ak74", "ak74_tatica"};
        const char* skins[] = {"", "dourado", "selva", "deserto", "urbana", "neon", "favela", "dragao"};
        for (const char* g : guns)
            for (const char* sk : skins) {
                json spec = {{"tipo", "arma"}, {"arma", g}, {"pos", {0, 1.0, 0}}};
                if (*sk) spec["textura"] = std::string(dir) + "/" + sk + ".png";
                int id = engine_add_spec(e, spec.dump().c_str());
                if (id < 0) { std::printf("  [ERRO] %s %s: %s\n", g, sk, engine_last_error(e)); ++g_fail; continue; }
                engine_select(e, -1);
                engine_set_camera(e, 0.f, 1.02f, 0.f, 1.5708f + 0.18f, 0.12f, 1.05f);
                frames(e, 3);
                std::string name = std::string("skin_") + g + "_" + (*sk ? sk : "padrao") + ".png";
                save(out, name.c_str());
                engine_tool_call(e, "remover_objetos", json{{"ids", {id}}}.dump().c_str());
            }
        // pistola TT-33 e as skins dela (texturas do kit: armas/skins/tt33_<skin>.jpg)
        for (const char* sk : {"", "crimson_forge", "nebula_shadow", "gold_relic"}) {
            json spec = {{"tipo", "arma"}, {"arma", "tt33"}, {"pos", {0, 1.0, 0}}};
            if (*sk) spec["textura"] = std::string("kit:armas/skins/tt33_") + sk + ".jpg";
            int id = engine_add_spec(e, spec.dump().c_str());
            if (id < 0) { std::printf("  [ERRO] tt33 %s: %s\n", sk, engine_last_error(e)); ++g_fail; continue; }
            engine_select(e, -1);
            engine_set_camera(e, 0.f, 1.02f, 0.f, 1.5708f + 0.18f, 0.12f, 0.45f);
            frames(e, 3);
            save(out, (std::string("skin_tt33_") + (*sk ? sk : "padrao") + ".png").c_str());
            engine_tool_call(e, "remover_objetos", json{{"ids", {id}}}.dump().c_str());
        }
        // primeira pessoa: a arma vista de trás (como nas mãos) e a favela na altura dos olhos
        for (int k = 0; k < 2; ++k) {
            engine_set_camera(e, 0.f, 1.02f, 0.f, 3.14159f - (k ? 0.75f : 0.5f), 0.17f, 0.62f);
            frames(e, 3);
            save(out, ("fps_fundo_" + std::to_string(k) + ".png").c_str());
        }
        for (const char* g : {"m4a1", "ak47"}) {
            int id = engine_add_spec(e, json{{"tipo", "arma"}, {"arma", g}, {"pos", {0, 1.0, 0}}}.dump().c_str());
            engine_select(e, -1);
            for (int k = 0; k < 2; ++k) {
                float yaw = 3.14159f - (k ? 0.75f : 0.5f);
                engine_set_camera(e, 0.f, 1.02f, 0.f, yaw, 0.17f, 0.62f);
                frames(e, 3);
                std::string name = std::string("fps_") + g + "_" + std::to_string(k) + ".png";
                save(out, name.c_str());
            }
            engine_tool_call(e, "remover_objetos", json{{"ids", {id}}}.dump().c_str());
        }
        engine_tool_call(e, "carregar_mapa", R"({"tipo":"favela","semente":5,"com_jogador":false})");
        engine_tool_call(e, "definir_ambiente", R"({"hora":15.5})");
        struct V { float x, z, yaw, pitch; } views[] = {{0.f, 30.f, 0.f, 0.03f}, {-14.f, 6.f, 0.4f, 0.05f}, {2.f, -34.f, 3.14159f, 0.02f}, {-30.f, 26.f, -0.9f, 0.02f}};
        int n = 0;
        for (auto& v : views) {
            json h = json::parse(std::string(engine_tool_call(e, "altura_terreno", json{{"pontos", {{v.x, v.z}}}}.dump().c_str())));
            float y = h["alturas"][0].is_number() ? h["alturas"][0].get<float>() : 10.f;
            engine_set_camera(e, v.x, y + 1.65f, v.z, v.yaw, v.pitch, 0.05f);
            frames(e, 4);
            std::string name = "fps_favela_" + std::to_string(n++) + ".png";
            save(out, name.c_str());
        }
        engine_destroy(e);
        std::printf(g_fail ? "FALHOU\n" : "OK\n");
        return g_fail ? 1 : 0;
    }
    // Primeira pessoa com braços: personagem segurando a arma, câmera nos olhos (na mão e mirando)
    if (std::getenv("SO_FPS")) {
        using nlohmann::json;
        auto envf = [](const char* k, float d) { const char* v = std::getenv(k); return v ? float(std::atof(v)) : d; };
        float mx = envf("MAPA_X", 0.f), mz = envf("MAPA_Z", 30.f), rotY = envf("ROT_Y", 180.f);
        float y0 = 0.f;
        if (std::getenv("FAVELA")) {
            engine_tool_call(e, "carregar_mapa", R"({"tipo":"favela","semente":5,"com_jogador":false})");
            engine_tool_call(e, "definir_ambiente", R"({"hora":15.5})");
            json h = json::parse(std::string(engine_tool_call(e, "altura_terreno", json{{"pontos", {{mx, mz}}}}.dump().c_str())));
            y0 = h["alturas"][0].get<float>();
        } else { engine_new_scene(e); mx = mz = 0.f; }
        int ch = std::getenv("SOLDADO")
            ? engine_add_spec(e, json{{"tipo", "modelo"}, {"arquivo", "kit:personagens/soldado"}, {"personagem", true}, {"pos", {mx, y0, mz}}, {"rot_y", rotY}}.dump().c_str())
            : engine_add_spec(e, json{{"tipo", "personagem"}, {"semente", 3}, {"pos", {mx, y0, mz}}, {"rot_y", rotY},
                                        {"arma", std::getenv("ARMA") ? std::getenv("ARMA") : "m4a1"},
                                        {"aparencia", {{"camisa", "#5d6872"}, {"manga_comprida", true}}}}.dump().c_str());
        if (ch < 0) { std::printf("[ERRO] %s\n", engine_last_error(e)); return 1; }
        if (std::getenv("SOLDADO")) engine_add_spec(e, json{{"tipo", "arma"}, {"arma", "m4a1"}, {"portador", ch}}.dump().c_str());
        engine_select(e, ch);
        engine_set_anim_preview(e, 1);
        float r = glm::radians(rotY), fx = std::sin(r), fz = std::cos(r);
        for (int clip : {5, 8}) {
            engine_set_animation(e, clip);
            frames(e, 20);
            engine_select(e, -1);
            const bool aim = clip == 8;
            float ey = envf(aim ? "OLHO_Y_MIRA" : "OLHO_Y", 1.62f), ez = envf(aim ? "OLHO_Z_MIRA" : "OLHO_Z", 0.16f), ex = envf(aim ? "OLHO_X_MIRA" : "OLHO_X", 0.f);
            float px = mx + fx * ez + fz * ex, pz = mz + fz * ez - fx * ex, py = y0 + ey, cyaw = r + 3.14159f + envf("YAW_ADD", 0.f);
            // soldado: olho na cabeça (osso), olhando para onde a arma está
            int head = -1, gun = -1;
            for (int i = 0; i < engine_object_count(e); ++i) {
                std::string n = engine_object_name(e, i);
                if (n == "mixamorig:Head") head = i;
                if (n == "Fuzil") gun = i;
            }
            if (head >= 0 && gun >= 0) {
                float hp[3], gp[3];
                engine_get_world_pos(e, head, hp);
                engine_get_world_pos(e, gun, gp);
                glm::vec3 f(gp[0] - hp[0], 0.f, gp[2] - hp[2]);
                f = glm::normalize(f);
                glm::vec3 side(-f.z, 0.f, f.x);
                glm::vec3 eye = glm::vec3(hp[0], hp[1], hp[2]) + glm::vec3(0, envf("OLHO_ALTO", 0.08f), 0) + f * envf("OLHO_FRENTE", 0.1f) + side * envf("OLHO_LADO", 0.f);
                px = eye.x; py = eye.y; pz = eye.z;
                cyaw = std::atan2(-f.x, -f.z) + envf("YAW_ADD", 0.f);
                std::printf("  cabeça %.2f %.2f %.2f arma %.2f %.2f %.2f\n", hp[0], hp[1], hp[2], gp[0], gp[1], gp[2]);
            }
            engine_set_camera(e, px, py, pz, cyaw, envf(aim ? "PITCH_MIRA" : "PITCH", 0.f), 0.02f);
            engine_set_near(e, envf("PERTO", 0.22f));
            frames(e, 2, 0.f);
            save(out, aim ? "fp_mirando.png" : "fp_na_mao.png");
            engine_set_near(e, 0.f);
            engine_set_camera(e, mx, y0 + 1.3f, mz, r + 3.14159f + 2.2f, 0.15f, 2.4f);  // de fora, para conferir a pose
            frames(e, 1, 0.f);
            save(out, aim ? "fp_mirando_fora.png" : "fp_na_mao_fora.png");
            engine_select(e, ch);
        }
        engine_destroy(e);
        std::printf("OK\n");
        return 0;
    }
    // Arma solta vista como na primeira pessoa (posição baixa e mirando): GIRO="x,y,z" CAM="yaw,pitch,dist,alvo_y"
    if (const char* list = std::getenv("SO_VISTA_ARMA")) {
        using nlohmann::json;
        engine_new_scene(e);
        engine_set_grid(e, 0);
        engine_tool_call(e, "definir_ambiente", R"({"hora":13,"neblina":0})");
        // cada item: nome:arma:rx,ry,rz:yaw,pitch,dist,ty[:textura]
        std::string all = list;
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(';', pos);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(pos, end - pos);
            pos = end + 1;
            char name[64], gun[32], tex[256] = "";
            float rx, ry, rz, yaw, pitch, dist, ty;
            if (std::sscanf(item.c_str(), "%63[^:]:%31[^:]:%f,%f,%f:%f,%f,%f,%f:%255s", name, gun, &rx, &ry, &rz, &yaw, &pitch, &dist, &ty, tex) < 9) continue;
            json spec = {{"tipo", "arma"}, {"arma", gun}, {"pos", {0, 1.0, 0}}, {"rot", {rx, ry, rz}}};
            if (tex[0]) spec["textura"] = tex;
            int id = engine_add_spec(e, spec.dump().c_str());
            engine_select(e, -1);
            engine_set_camera(e, 0.f, 1.f + ty, 0.f, yaw, pitch, dist);
            frames(e, 3);
            save(out, (std::string(name) + ".png").c_str());
            engine_tool_call(e, "remover_objetos", json{{"ids", {id}}}.dump().c_str());
            frames(e, 2);
            save(out, (std::string(name) + "_fundo.png").c_str());
        }
        engine_destroy(e);
        std::printf("OK\n");
        return 0;
    }
    // Elenco: o personagem base com cada visual (textura) lado a lado, de frente e de costas
    if (const char* base = std::getenv("SO_ELENCO")) {
        using nlohmann::json;
        engine_new_scene(e);
        engine_set_grid(e, 0);
        engine_tool_call(e, "definir_ambiente", R"({"hora":14,"neblina":0})");
        std::string list = std::getenv("VISUAIS") ? std::getenv("VISUAIS") : "";
        std::vector<std::string> files;
        for (size_t p = 0; p <= list.size();) {
            size_t q = list.find(',', p);
            if (q == std::string::npos) q = list.size();
            if (q > p) files.push_back(list.substr(p, q - p));
            p = q + 1;
        }
        if (files.empty()) files.push_back("");
        const float gap = 0.95f;
        for (size_t i = 0; i < files.size(); ++i) {
            json spec = {{"tipo", "modelo"}, {"arquivo", base}, {"personagem", true}, {"pos", {(float(i) - (files.size() - 1) * 0.5f) * gap, 0, 0}}};
            if (!files[i].empty()) spec["textura"] = files[i];
            if (const char* eq = std::getenv("EQUIP")) spec["equipamento"] = json::parse(eq);
            if (const char* eqs = std::getenv("EQUIPS")) {  // um por personagem: [[...],[...]]
                json all = json::parse(eqs);
                if (i < all.size()) spec["equipamento"] = all[i];
            }
            if (files[i].rfind("kit:", 0) == 0) { spec.erase("textura"); spec["arquivo"] = "kit:personagens/skins/" + files[i].substr(4); }
            int id = engine_add_spec(e, spec.dump().c_str());
            if (id < 0) std::printf("  [ERRO] %s\n", engine_last_error(e));
        }
        engine_select(e, -1);
        engine_set_anim_preview(e, 1);
        frames(e, 30);
        float dist = std::max(3.2f, files.size() * gap * 1.05f);
        engine_set_camera(e, 0.f, 1.0f, 0.f, 0.f, 0.08f, dist);
        frames(e, 2, 0.f);
        save(out, "elenco_frente.png");
        engine_set_camera(e, 0.f, 1.0f, 0.f, 3.14159f, 0.08f, dist);
        frames(e, 2, 0.f);
        save(out, "elenco_costas.png");
        engine_set_camera(e, 0.f, 1.45f, 0.f, 0.f, 0.05f, std::max(1.6f, dist * 0.45f));
        frames(e, 2, 0.f);
        save(out, "elenco_rosto.png");
        engine_destroy(e);
        std::printf("OK\n");
        return 0;
    }
    // Postura com o fuzil em Play: pronto (parado) e mirando, vistas de lado/frente/de perto
    if (std::getenv("SO_MIRA")) {
        using nlohmann::json;
        engine_new_scene(e);
        engine_set_grid(e, 0);
        std::string arq = std::getenv("BASE") ? std::getenv("BASE") : std::string("kit:personagens/skins/") + (std::getenv("VISUAL") ? std::getenv("VISUAL") : "pm");
        int id = engine_add_spec(e, json{{"tipo", "modelo"}, {"arquivo", arq}, {"personagem", true}, {"jogador", true}, {"pos", {0, 0, 0}}}.dump().c_str());
        if (id < 0) { std::printf("[ERRO] %s\n", engine_last_error(e)); return 1; }
        engine_select(e, id);
        engine_set_player(e);
        engine_create_weapon_ex(e, 1, std::getenv("ARMA_TESTE") ? std::getenv("ARMA_TESTE") : "kit:armas/m4a1", 0, 1.f, 0);
        engine_select(e, -1);
        engine_set_play(e, 1);
        for (int st = 0; st < 4; ++st) {
            const int aim = st & 1, crouch = st >> 1;
            engine_player_aim(e, aim);
            engine_player_crouch(e, crouch);
            frames(e, 40);
            const char* names[] = {"pronto", "mirando", "agachado", "agachado_mirando"};
            const char* nm = names[st];
            float pp[3], pr[3], ps[3];
            engine_get_transform(e, engine_player_id(e), pp, pr, ps);
            float ry = glm::radians(pr[1]);
            if (std::abs(pr[0]) > 90.f) ry = 3.14159f - ry;  // euler com X e Z = 180
            struct V { const char* s; float yaw, pitch, dist, ty; } vs[] = {{"lado", ry - 1.5708f, 0.05f, 2.0f, 1.3f}, {"frente", ry + 0.5f, 0.05f, 2.2f, 1.25f},
                                                                           {"perto", ry - 1.1f, 0.06f, 0.85f, 1.48f}, {"costas", ry + 3.14159f - 0.5f, 0.1f, 1.1f, 1.5f},
                                                                           {"frente_esq", ry + 0.9f, 0.15f, 1.3f, 1.3f}, {"frente_dir", ry - 0.6f, 0.15f, 1.3f, 1.3f}};
            for (auto& v : vs) {
                engine_set_camera(e, pp[0], pp[1] + v.ty, pp[2], v.yaw, v.pitch, v.dist);
                frames(e, 1, 0.f);
                save(out, (std::string("mira_") + nm + "_" + v.s + ".png").c_str());
            }
        }
        engine_destroy(e);
        std::printf("OK\n");
        return 0;
    }
    // Mira em primeira pessoa de cada arma (alça de mira/luneta na frente do olho): SO_ADS
    if (std::getenv("SO_ADS")) {
        using nlohmann::json;
        engine_new_scene(e);
        engine_set_grid(e, 0);
        int id = engine_add_spec(e, json{{"tipo", "modelo"}, {"arquivo", "kit:personagens/skins/pm"}, {"personagem", true}, {"jogador", true}, {"pos", {0, 0, 0}}}.dump().c_str());
        if (id < 0) { std::printf("[ERRO] %s\n", engine_last_error(e)); return 1; }
        // alvo: caixa a 15 m na frente (o personagem olha para -Z com yaw 0)
        engine_add_spec(e, json{{"tipo", "forma"}, {"forma", "cubo"}, {"fisica", false}, {"pos", {0, 1.6f, -15.f}}, {"escala", {0.4f, 0.4f, 0.4f}}, {"paleta", "laranja"}}.dump().c_str());
        engine_add_spec(e, json{{"tipo", "forma"}, {"forma", "cubo"}, {"fisica", false}, {"pos", {0, 0.6f, -15.f}}, {"escala", {0.2f, 1.2f, 0.2f}}, {"paleta", "azul"}}.dump().c_str());
        struct G { const char* nm; int kind; const char* file; } guns[] = {{"m4a1", 1, "kit:armas/m4a1"}, {"m4colt", 1, "kit:armas/m4colt"}, {"ak47", 1, "kit:armas/ak47"},
            {"ak74", 1, "kit:armas/ak74"}, {"ak74_tatica", 1, "kit:armas/ak74_tatica"}, {"tt33", 0, "kit:armas/tt33"}};
        const char* only = std::getenv("ARMA");
        engine_set_first_person(e, 1);
        for (auto& g : guns) {
            if (only && std::string(only) != g.nm) continue;
            engine_select(e, id);
            engine_set_player(e);
            engine_create_weapon_ex(e, g.kind, g.file, 0, 1.f, 0);
            engine_select(e, -1);
            engine_set_play(e, 1);
            engine_set_camera(e, 0.f, 0.f, 0.f, 0.f, 0.0f, 3.f);
            engine_player_aim(e, 0);
            frames(e, 40);
            save(out, (std::string("ads_") + g.nm + "_quadril.png").c_str());
            engine_player_aim(e, 1);
            frames(e, 40);
            save(out, (std::string("ads_") + g.nm + "_mira.png").c_str());
            engine_player_aim(e, 0);
            engine_set_play(e, 0);
        }
        engine_destroy(e);
        std::printf("OK\n");
        return 0;
    }
    // Jogo em primeira pessoa (app do jogo): escolhas antes do Play, câmera no olho, mira, troca de arma, granada e HUD
    auto jogo = [&]() {
        using nlohmann::json;
        std::printf("[33] jogo em primeira pessoa\n");
        teamProject();
        engine_set_quality(e, 1);
        engine_team_gun_skin(e, "tt33", "gold_relic");
        engine_team_loadout(e, 0, 2, "m4colt", "tt33");
        engine_set_first_person(e, 1);
        engine_set_play(e, 1);
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.f, 0.05f, 3.f);
        frames(e, 30);
        auto hud = [&]() { return json::parse(std::string(engine_hud_json(e))); };
        json h = hud();
        std::printf("  HUD: %s\n", h.dump().substr(0, 400).c_str());
        if (!h.value("ativo", false) || h["armas"][0]["arma"] != "m4colt") { std::printf("  [ERRO] arma escolhida não entrou\n"); ++g_fail; }
        save(out, "33_quadril.png");
        engine_player_aim(e, 1);
        frames(e, 30);
        save(out, "33_mira.png");
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.f, -0.55f, 3.f);
        frames(e, 10);
        save(out, "33_mira_cima.png");
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.f, 0.6f, 3.f);
        frames(e, 10);
        save(out, "33_mira_baixo.png");
        engine_player_aim(e, 0);
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.f, 0.05f, 3.f);
        for (int k = 0; k < 40; ++k) { engine_player_fire(e); frames(e, 1); if (k == 6) save(out, "33_tiro_quadril.png"); }
        h = hud();
        int balas = h["armas"][0]["balas"];
        std::printf("  depois de atirar: %d balas, reserva %d\n", balas, int(h["armas"][0]["reserva"]));
        if (balas >= 30) { std::printf("  [ERRO] as balas não gastaram\n"); ++g_fail; }
        engine_player_reload(e);
        frames(e, 2);
        if (!hud().value("recarregando", false)) { std::printf("  [ERRO] não recarregou\n"); ++g_fail; }
        frames(e, 150);
        engine_player_swap(e);
        frames(e, 20);
        h = hud();
        if (h.value("slot", 0) != 1) { std::printf("  [ERRO] troca rápida não trocou\n"); ++g_fail; }
        save(out, "33_pistola.png");
        engine_player_aim(e, 1);
        frames(e, 25);
        save(out, "33_pistola_mira.png");
        engine_player_aim(e, 0);
        engine_player_swap(e);
        frames(e, 10);
        engine_player_input(e, 0.f, 1.f, 1);
        frames(e, 40);
        save(out, "33_correndo.png");
        engine_player_input(e, 0.f, 0.f, 0);
        engine_player_crouch(e, 1);
        frames(e, 30);
        save(out, "33_agachado.png");
        engine_player_crouch(e, 0);
        engine_set_camera(e, 0.f, 0.f, 0.f, 0.f, -0.15f, 3.f);
        frames(e, 5);
        if (!engine_player_grenade(e)) { std::printf("  [ERRO] granada não saiu\n"); ++g_fail; }
        frames(e, 25);
        save(out, "33_granada.png");
        frames(e, 170);
        save(out, "33_granada_explodiu.png");
        h = hud();
        std::printf("  granadas: %d, placar %s\n", int(h["granadas"]), h["placar"].dump().c_str());
        if (h["granadas"] != 1) { std::printf("  [ERRO] contador de granadas\n"); ++g_fail; }
        for (int k = 0; k < 30 * 40; ++k) frames(e, 1, 1.f / 30.f);
        h = hud();
        std::printf("  após 40 s: placar %s, você %d abates %d mortes\n", h["placar"].dump().c_str(), int(h["abates"]), int(h["mortes"]));
        save(out, "33_depois.png");
        engine_set_play(e, 0);
        engine_set_first_person(e, 0);
    };
    if (std::getenv("SO_JOGO")) { jogo(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_TIMES")) { times(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_FAVELA")) { favela(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_ESCADAS")) { stairs(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_ABERTURAS")) { openings(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_CONSTRUCAO")) { modules(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_ARMAS")) { weapons(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_DESEMPENHO")) { perf(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_KIT")) { kit(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_PLANETA")) { planet(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }
    if (std::getenv("SO_INTERIORES")) { interiors(); engine_destroy(e); std::printf(g_fail ? "FALHOU\n" : "OK\n"); return g_fail ? 1 : 0; }

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
    float x = -60.f, bx[12] = {};
    for (int k = 0; k < 12; ++k) {
        int id = engine_create_building(e, k, 7 + k);
        float p[3], rr[3], ss[3];
        if (id < 0 || !engine_get_transform(e, id, p, rr, ss)) { std::printf("  [ERRO] %s\n", bnames[k]); ++g_fail; continue; }
        float wdt = k == 4 ? 34.f : k == 3 || k == 5 ? 24.f : 12.f;
        x += wdt * 0.5f;
        p[0] = x; p[2] = k == 5 ? -10.f : 0.f;
        bx[k] = x;
        x += wdt * 0.5f + 2.f;
        engine_set_transform(e, id, p, rr, ss);
    }
    engine_select(e, -1);
    engine_focus_selected(e);
    frames(e, 2);
    save(out, "8_construcoes.png");
    // fachadas de perto: salas nas janelas, tijolo, concreto
    for (int k : {0, 3, 4}) {
        engine_set_camera(e, bx[k], k == 0 ? 2.5f : 8.f, 0.f, 0.25f, 0.08f, k == 0 ? 11.f : 16.f);
        frames(e, 2);
        save(out, (std::string("8_fachada_") + bnames[k] + ".png").c_str());
    }
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

    {  // 5º exemplo: Polícia x Bandidos na favela (a partida é testada em SO_TIMES)
        std::string proj = teamProject();
        if (FILE* f = std::fopen((out + "/exemplos/5_policia_x_bandidos.json").c_str(), "wb")) { std::fwrite(proj.data(), 1, proj.size(), f); std::fclose(f); }
        engine_team_choose(e, 1);
        engine_set_play(e, 1);
        frames(e, 60, 0.05f);
        std::printf("  Polícia x Bandidos: %zu bytes, %s\n", proj.size(), engine_game_text(e, 0));
        save(out, "15_5_policia_x_bandidos.png");
        engine_set_play(e, 0);
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

    // ---- [18] animações: modelo importado ganha as do motor; animação personalizada salva e reabre
    std::printf("[18] animações personalizadas\n");
    {
        for (int i = 3; i < argc; ++i) {
            std::string m = argv[i];
            if (m.find("Samba") == std::string::npos) continue;
            engine_new_scene(e);
            int id = engine_load_model(e, m.c_str());
            engine_select(e, id);
            std::string list = engine_animation_list(e);
            std::printf("  %s: %zu animações, motor: %s\n", m.c_str(), std::count(list.begin(), list.end(), '\n'),
                        list.find("(motor)") != std::string::npos ? "sim" : "não");
            if (list.find("(motor)") == std::string::npos) { std::printf("  [ERRO] humanoide importado sem as animações do motor\n"); ++g_fail; }
        }
        engine_tool_call(e, "carregar_mapa", R"({"tipo":"vazio","com_jogador":false})");
        int ch = engine_create_random_character(e, 5, 0);
        engine_select(e, ch);
        int bones = engine_anim_edit_begin(e);
        std::string names = engine_anim_edit_bones(e);
        int arm = -1, k = 0;
        for (size_t pos = 0, nl; (nl = names.find('\n', pos)) != std::string::npos; pos = nl + 1, ++k)
            if (names.compare(pos, std::strlen("Braço dir"), "Braço dir") == 0) arm = k;
        engine_anim_edit_select(e, arm);
        float down[3] = {0, 0, 0}, up[3] = {0, 0, 150};
        engine_anim_edit_set(e, arm, down); engine_anim_edit_key(e, 0.f);
        engine_anim_edit_set(e, arm, up);   engine_anim_edit_key(e, 0.5f);
        engine_anim_edit_set(e, arm, down); engine_anim_edit_key(e, 1.f);
        engine_anim_edit_goto(e, 0.5f);
        engine_set_camera(e, 0.f, 1.f, 0.f, 0.3f, 0.15f, 4.f);
        frames(e, 2);
        save(out, "18_pose.png");
        int clip = engine_anim_edit_save(e, "Tchau");
        std::printf("  ossos %d, braço %d, animação salva no índice %d\n", bones, arm, clip);
        std::string proj = engine_export_project(e);
        engine_import_project(e, proj.c_str());
        int found = -1;
        for (int id = 0; id < engine_object_count(e); ++id)
            if (engine_object_valid(e, id) && engine_object_parent(e, id) < 0) {
                engine_select(e, id);
                if (std::string(engine_animation_list(e)).find("Tchau") != std::string::npos) found = id;
            }
        std::printf("  depois de reabrir: %s\n", found >= 0 ? "animação 'Tchau' presente" : "sumiu");
        if (arm < 0 || clip < 0 || found < 0) { std::printf("  [ERRO] animação personalizada\n"); ++g_fail; }
        else {
            engine_select(e, found);
            std::string l = engine_animation_list(e);
            int idx = 0; for (size_t pos = 0; pos < l.find("Tchau"); ++pos) if (l[pos] == '\n') ++idx;
            engine_set_animation(e, idx);
            engine_set_camera(e, 0.f, 1.f, 0.f, 0.3f, 0.15f, 4.f);
            frames(e, 30);
            save(out, "18_tchau.png");
        }
    }

    // ---- [19] mapa editável: cada peça vira objeto; mudanças salvas; trocas de modelo e textura
    std::printf("[19] editar mapa\n");
    {
        engine_load_map(e, 4, 9);  // bairro
        frames(e, 2);
        float st0[6]; engine_get_stats(e, st0);
        engine_set_camera(e, -30.f, 2.f, -60.f, 0.6f, 0.35f, 45.f);
        frames(e, 3);
        save(out, "19_mapa_base.png");
        if (!engine_map_set_editable(e, 1)) { std::printf("  [ERRO] editável: %s\n", engine_last_error(e)); ++g_fail; }
        frames(e, 2);
        float st1[6]; engine_get_stats(e, st1);
        std::printf("  bairro: %.0f objetos -> editável %.0f objetos, draws %.0f -> %.0f\n", st0[5], st1[5], st0[3], st1[3]);
        // acha casas e árvores pelas raízes registradas
        std::vector<int> casas, arvores;
        for (int id = 0; id < engine_object_count(e); ++id) {
            if (!engine_object_valid(e, id) || engine_object_parent(e, id) >= 0) continue;
            std::string n = engine_object_name(e, id);
            if (n == "Casa") casas.push_back(id);
            if (n == "Arvore") arvores.push_back(id);
        }
        std::map<std::string, int> kinds;
        for (int id = 0; id < engine_object_count(e); ++id)
            if (engine_object_valid(e, id) && engine_object_parent(e, id) < 0) ++kinds[engine_object_name(e, id)];
        for (auto& [k, v] : kinds) std::printf("    %s: %d\n", k.c_str(), v);
        engine_set_camera(e, -30.f, 2.f, -60.f, 0.6f, 0.35f, 45.f);
        frames(e, 3);
        save(out, "19_mapa_editavel.png");
        std::printf("  casas %zu, árvores %zu\n", casas.size(), arvores.size());
        if (casas.size() < 10 || arvores.size() < 5) { std::printf("  [ERRO] peças não viraram objetos\n"); ++g_fail; }
        else {
            engine_select(e, casas[0]); engine_delete_selected(e);
            float p[3], r[3], sc[3];
            engine_get_transform(e, casas[1], p, r, sc);
            p[1] += 3.f; r[1] += 45.f;
            engine_set_transform(e, casas[1], p, r, sc);
            engine_select(e, casas[2]); engine_set_object_color(e, 0.9f, 0.1f, 0.1f);
        }
        std::string a1 = engine_export_project(e);
        engine_import_project(e, a1.c_str());
        std::string a2 = engine_export_project(e);
        size_t pecas = a1.find("\"pecas\"");
        std::printf("  projeto %zu bytes, mudanças salvas: %s, reabrir igual: %s\n", a1.size(), pecas != std::string::npos ? "sim" : "não", a1 == a2 ? "sim" : "não");
        if (pecas == std::string::npos || a1 != a2) { std::printf("  [ERRO] mudanças do mapa não voltaram iguais\n"); ++g_fail; }
        // trocas: árvores viram a estátua .obj; grama ganha uma imagem
        std::string img = out + "/19_grama.png";
        std::vector<unsigned char> px(64 * 64 * 4);
        for (int i = 0; i < 64 * 64; ++i) { px[i * 4] = 200; px[i * 4 + 1] = (i / 64 % 8 < 4) ? 40 : 220; px[i * 4 + 2] = 60; px[i * 4 + 3] = 255; }
        stbi_write_png(img.c_str(), 64, 64, 4, px.data(), 64 * 4);
        int grass = -1;
        for (int i = 0; i < engine_material_count(e); ++i) if (std::string(engine_material_name(e, i)) == "Grama") grass = i;
        bool ok1 = engine_map_swap_model(e, "arvore", (out + "/17_estatua.obj").c_str());
        bool ok2 = engine_map_swap_texture(e, grass, img.c_str());
        std::string a3 = engine_export_project(e);
        std::printf("  troca árvore: %s, troca grama: %s, trocas no projeto: %s\n", ok1 ? "ok" : "falhou", ok2 ? "ok" : "falhou",
                    a3.find("\"trocas\"") != std::string::npos && a3.find("\"texturas\"") != std::string::npos ? "sim" : "não");
        if (!ok1 || !ok2) { std::printf("  [ERRO] trocas: %s\n", engine_last_error(e)); ++g_fail; }
        engine_set_camera(e, -30.f, 2.f, -60.f, 0.6f, 0.35f, 45.f);
        frames(e, 3);
        save(out, "19_mapa_editado.png");
    }

    interiors();
    planet();
    globePlay();
    kit();
    weapons();
    modules();
    stairs();
    openings();
    favela();
    supportHand();

    engine_destroy(e);
    check("destroy");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    std::printf(g_fail ? "FALHOU (%d problemas)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
