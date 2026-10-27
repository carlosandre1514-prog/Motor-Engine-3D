// Teste headless no PC/CI: cria um contexto GLES 3.2 sem janela (EGL surfaceless, Mesa),
// renderiza as cenas do motor num FBO e salva PNGs. Uso:
//   mobile3d_headless <pasta_shaders> <pasta_saida> [modelo.glb ...]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <cstdio>
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

    engine_destroy(e);
    check("destroy");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    std::printf(g_fail ? "FALHOU (%d problemas)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
