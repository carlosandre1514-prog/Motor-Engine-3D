#pragma once
#include <stdint.h>
// API C estável: consumida via dart:ffi (Flutter), JNI (Android) e Swift/ObjC (iOS).
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Engine Engine;

Engine* engine_create(const char* shader_dir);                  // contexto GL já ativo na thread de render
void    engine_destroy(Engine*);
void    engine_resize(Engine*, int w, int h);
void    engine_frame(Engine*, float dt_seconds);                // update + render de um frame

// Modo
void    engine_set_play(Engine*, int playing);                  // 1 = Play (física/animação), 0 = Stop (restaura snapshot)
int     engine_is_playing(Engine*);

// Assets / cena
int     engine_load_model(Engine*, const char* path);           // .glb/.gltf; retorna id do 1º objeto raiz ou -1
void    engine_load_demo(Engine*);                              // cubos PBR + chão (teste do renderer)
int     engine_object_count(Engine*);
const char* engine_object_name(Engine*, int index);

// Seleção / inspetor
void    engine_select(Engine*, int object_id);
void    engine_get_transform(Engine*, int id, float* pos3, float* euler3, float* scale3);
void    engine_set_transform(Engine*, int id, const float* pos3, const float* euler3, const float* scale3);
void    engine_set_gizmo_mode(Engine*, int mode);               // 0 translate, 1 rotate

// Touch: action 0=down 1=move 2=up; ids/xy em arrays de n dedos (pixels)
void    engine_touch(Engine*, int action, int n, const int* ids, const float* xy);

#ifdef __cplusplus
}
#endif
