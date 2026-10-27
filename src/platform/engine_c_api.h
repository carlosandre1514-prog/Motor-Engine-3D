#pragma once
#include <stdint.h>
// API C estável: consumida via JNI (Android), dart:ffi (Flutter) e Swift/ObjC (iOS).
// Todas as funções devem ser chamadas na thread que tem o contexto GL (thread de render).
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Engine Engine;

Engine* engine_create(const char* shader_dir);                  // contexto GL já ativo na thread de render
void    engine_destroy(Engine*);
void    engine_resize(Engine*, int w, int h);
void    engine_frame(Engine*, float dt_seconds);                // update + render de um frame
void    engine_set_target_fbo(Engine*, uint32_t fbo);           // 0 = tela

// Modo
void    engine_set_play(Engine*, int playing);                  // 1 = Play (física/animação), 0 = Stop (restaura snapshot)
int     engine_is_playing(Engine*);

// Cenas
void    engine_new_scene(Engine*);                              // só chão + luz
void    engine_load_demo(Engine*);                              // vitrine de materiais PBR
void    engine_load_physics_demo(Engine*);                      // pilha de caixas, esferas, cápsulas e pedras
int     engine_load_model(Engine*, const char* path);           // .glb/.gltf; retorna id do objeto raiz ou -1
const char* engine_last_error(Engine*);
int     engine_add_object(Engine*, int kind, int with_physics); // kind: 0 cubo, 1 esfera, 2 cápsula, 3 pedra
void    engine_delete_selected(Engine*);
void    engine_focus_selected(Engine*);                         // enquadra a seleção (ou a cena toda)

// Hierarquia: ids vão de 0 a engine_object_count()-1; ids de objetos apagados ficam inválidos
int     engine_object_count(Engine*);
int     engine_object_valid(Engine*, int id);
const char* engine_object_name(Engine*, int id);
int     engine_object_parent(Engine*, int id);                  // -1 = raiz
int     engine_object_depth(Engine*, int id);

// Seleção / inspetor
void    engine_select(Engine*, int object_id);                  // -1 limpa a seleção
int     engine_selected(Engine*);
int     engine_get_transform(Engine*, int id, float* pos3, float* euler3, float* scale3);  // 0 se id inválido
void    engine_set_transform(Engine*, int id, const float* pos3, const float* euler3, const float* scale3);
void    engine_set_gizmo_mode(Engine*, int mode);               // 0 mover, 1 girar, 2 escalar

// Visual
void    engine_set_exposure(Engine*, float exposure);
float   engine_get_exposure(Engine*);
void    engine_set_sun_intensity(Engine*, float intensity);
float   engine_get_sun_intensity(Engine*);
void    engine_set_grid(Engine*, int visible);

// Animação (objeto selecionado ou, sem seleção animada, todos)
int     engine_cycle_animation(Engine*);                        // próxima animação; retorna o índice ou -1
const char* engine_animation_name(Engine*);

// Estatísticas: out[0] fps, [1] ms por frame, [2] ms de CPU do motor, [3] draw calls, [4] triângulos, [5] objetos
void    engine_get_stats(Engine*, float* out6);

// ---- Criação procedural
// Personagem: params[20] = altura, corpo(0 magro,1 normal,2 forte), pele rgb, camisa rgb, calça rgb, tênis rgb,
// cabelo rgb, estilo de cabelo (0 careca,1 curto,2 comprido,3 boné), manga comprida(0/1), controlável(0/1). Cores sRGB 0..1.
int     engine_create_character(Engine*, const float* params20);
int     engine_create_random_character(Engine*, uint32_t seed, int controllable);
// Arma: 0 pistola, 1 fuzil, 2 escopeta, 3 sniper, 4 faca, 5 taco. Equipa no personagem selecionado (ou no jogador).
int     engine_create_weapon(Engine*, int kind);
// Construção: 0 casa, 1 sobrado, 2 barraco, 3 prédio, 4 arranha-céu, 5 escola, 6 árvore, 7 pinheiro, 8 palmeira,
// 9 carro, 10 poste, 11 muro. Criada no ponto de foco da câmera.
int     engine_create_building(Engine*, int kind, uint32_t seed);
// Mapas: 0 mundo infinito, 1 cidade, 2 favela, 3 colégio, 4 bairro (os de 1 a 4 têm limite com barreira invisível)
void    engine_load_map(Engine*, int kind, uint32_t seed);
int     engine_map_kind(Engine*);                               // -1 = nenhum
void    engine_map_overview(Engine*);                           // câmera do alto mostrando o mapa todo

// ---- Materiais / texturas do objeto selecionado
int     engine_material_count(Engine*);
const char* engine_material_name(Engine*, int index);
void    engine_set_object_material(Engine*, int index);
void    engine_set_object_color(Engine*, float r, float g, float b);   // sRGB 0..1
int     engine_set_object_texture(Engine*, const char* image_path);   // PNG/JPG; 0 se falhar

// ---- Jogador (Play)
int     engine_has_player(Engine*);
void    engine_set_player(Engine*);                             // personagem selecionado vira o jogador
void    engine_player_input(Engine*, float x, float y, int run); // joystick -1..1 (y para frente)
void    engine_player_jump(Engine*);
void    engine_player_fire(Engine*);
int     engine_player_weapon(Engine*);                          // tipo da arma ou -1

// Touch: action 0=down 1=move 2=up; ids/xy em arrays de n dedos (pixels)
void    engine_touch(Engine*, int action, int n, const int* ids, const float* xy);

#ifdef __cplusplus
}
#endif
