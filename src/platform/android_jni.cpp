// Ponte Android: SurfaceView/GLSurfaceView (thread de render) <-> engine_c_api.
// No Flutter, o mais comum é criar a Surface via um Texture/PlatformView e passar o ANativeWindow aqui.
#include <EGL/egl.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include "platform/engine_c_api.h"

static Engine* g_engine = nullptr;

extern "C" {

// Chamado de GLSurfaceView.Renderer.onSurfaceCreated (thread GL já com contexto ES 3.2 ativo)
JNIEXPORT void JNICALL Java_com_mobile3d_NativeBridge_nativeCreate(JNIEnv* env, jclass, jstring shaderDir) {
    const char* dir = env->GetStringUTFChars(shaderDir, nullptr);
    g_engine = engine_create(dir);  // shaders copiados dos assets para o storage interno antes
    env->ReleaseStringUTFChars(shaderDir, dir);
}
JNIEXPORT void JNICALL Java_com_mobile3d_NativeBridge_nativeResize(JNIEnv*, jclass, jint w, jint h) { if (g_engine) engine_resize(g_engine, w, h); }
JNIEXPORT void JNICALL Java_com_mobile3d_NativeBridge_nativeFrame(JNIEnv*, jclass, jfloat dt) { if (g_engine) engine_frame(g_engine, dt); }
JNIEXPORT void JNICALL Java_com_mobile3d_NativeBridge_nativeTouch(JNIEnv* env, jclass, jint action, jintArray ids, jfloatArray xy) {
    if (!g_engine) return;
    jsize n = env->GetArrayLength(ids);
    jint* i = env->GetIntArrayElements(ids, nullptr);
    jfloat* p = env->GetFloatArrayElements(xy, nullptr);
    engine_touch(g_engine, action, n, reinterpret_cast<int*>(i), p);
    env->ReleaseIntArrayElements(ids, i, JNI_ABORT);
    env->ReleaseFloatArrayElements(xy, p, JNI_ABORT);
}

}  // extern "C"

/* iOS: criar CAMetalLayer/CAEAGLLayer (ou GLKView com ES 3.0 enquanto o backend Metal não existe),
 * chamar engine_create no contexto ativo e engine_frame a partir do CADisplayLink.
 * Em Swift: importar engine_c_api.h via bridging header. */
