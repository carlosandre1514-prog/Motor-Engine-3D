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
int     engine_load_model(Engine*, const char* path);           // .glb/.gltf/.fbx; retorna id do objeto raiz ou -1
// Modelo importado selecionado (com esqueleto) vira personagem: física, andar/correr pelas animações dele
// ou, se for humanoide sem elas, pelas animações do motor (retarget). Retorna o id ou -1 (ver engine_last_error).
int     engine_make_character(Engine*, int controllable);
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
const char* engine_animation_list(Engine*);                     // nomes das animações (uma por linha)
int     engine_animation_index(Engine*);
void    engine_set_animation(Engine*, int index);
void    engine_set_anim_preview(Engine*, int on);               // 1 = animações tocam também no editor

// Editor de animação (objeto selecionado com esqueleto): pose osso a osso, quadros-chave e salvar com nome.
// Ângulos em graus, relativos à pose de repouso do osso. A animação salva fica no projeto e vai no .glb.
int     engine_anim_edit_begin(Engine*);                        // nº de ossos ou -1 (engine_last_error)
const char* engine_anim_edit_bones(Engine*);                    // nomes (um por linha)
void    engine_anim_edit_select(Engine*, int bone);             // destaca o osso na tela
int     engine_anim_edit_selected(Engine*);                     // osso escolhido (também por toque na tela)
void    engine_player_aim(Engine*, int on);                     // jogador mirando (botão segurado)
const char* engine_interact_label(Engine*);                     // texto do botão de ação perto do jogador ("" = nada)
int     engine_interact(Engine*);                               // usa (ex.: elevador para o terraço)
int     engine_team_mode(Engine*);                              // 1: objetivo "times" (Polícia x Bandidos)
void    engine_team_choose(Engine*, int team);                  // antes do Play: 0 polícia, 1 bandidos, -1 o do projeto
const char* engine_shop_json(Engine*);                          // loja aberta (JSON com itens) ou ""
const char* engine_shop_buy(Engine*, int item);                 // compra/equipa; devolve a mensagem
void    engine_shop_close(Engine*);
void    engine_anim_edit_get(Engine*, int bone, float* euler3);
void    engine_anim_edit_set(Engine*, int bone, const float* euler3);
void    engine_anim_edit_reset_pose(Engine*);
int     engine_anim_edit_key(Engine*, float time);              // grava a pose no tempo; retorna nº de quadros
void    engine_anim_edit_delete_key(Engine*, float time);
void    engine_anim_edit_goto(Engine*, float time);             // volta à pose gravada nesse tempo
const char* engine_anim_edit_keys(Engine*);                     // tempos dos quadros ("0.00 0.50 ...")
int     engine_anim_edit_preview(Engine*, int on);              // toca a animação em edição (precisa de 2+ quadros)
int     engine_anim_edit_save(Engine*, const char* name);       // índice da animação nova ou -1
void    engine_anim_edit_end(Engine*);

// Estatísticas: out[0] fps, [1] ms por frame, [2] ms de CPU do motor, [3] draw calls, [4] triângulos, [5] objetos
void    engine_get_stats(Engine*, float* out6);
void    engine_get_profile(Engine*, float* out3);
void    engine_set_quality(Engine*, int q);
// Converte a animação de um FBX (esqueleto Mixamo) para o esqueleto do motor e grava em .anim.
// rootMode: 0 como veio (morte), 1 no lugar (andar/correr), 2 só pose (pulo). Retorna o nº de quadros (0 = erro).
// armsDownDeg: abaixa os braços nos ombros (uma animação de mirar vira "pronto baixo"); 0 = como veio.
int     engine_mocap_bake(const char* fbx, const char* outAnim, int rootMode, const char* name, float armsDownDeg);            // 0 desempenho, 1 equilibrado, 2 alta  // ms do último quadro: mundo, lista de desenho, render

// ---- Criação procedural
// Personagem: params[20] = altura, corpo(0 magro,1 normal,2 forte), pele rgb, camisa rgb, calça rgb, tênis rgb,
// cabelo rgb, estilo de cabelo (0 careca,1 curto,2 comprido,3 boné), manga comprida(0/1), controlável(0/1). Cores sRGB 0..1.
int     engine_create_character(Engine*, const float* params20);
int     engine_create_random_character(Engine*, uint32_t seed, int controllable);
// Arma: 0 pistola, 1 fuzil, 2 escopeta, 3 sniper, 4 faca, 5 taco. Equipa no personagem selecionado (ou no jogador).
int     engine_create_weapon(Engine*, int kind);
// Arma com aparência própria: file = arquivo 3D (vazio = a do motor), flip = cano veio para trás, size = multiplica o
// tamanho do tipo, accessories = bits 1 mira red dot, 2 coronha tática, 4 empunhadura frontal. Vai para a mão do
// personagem selecionado (ou do jogador); sem personagem fica solta.
int     engine_create_weapon_ex(Engine*, int kind, const char* file, int flip, float size, int accessories);
// Construção: 0 casa, 1 sobrado, 2 barraco, 3 prédio, 4 arranha-céu, 5 escola, 6 árvore, 7 pinheiro, 8 palmeira,
// 9 carro, 10 poste, 11 muro, 12 rua, 13 cruzamento, 14 calçada, 15 escada, 16 rampa, 17 piso, 18 parede,
// 19 cerca, 20 banco, 21 caixa d'água. Criada no ponto de foco da câmera.
int     engine_create_building(Engine*, int kind, uint32_t seed);
// Mapas: 0 mundo infinito, 1 cidade, 2 favela, 3 colégio, 4 bairro (os de 1 a 4 têm limite com barreira invisível),
// 5 planeta (6 x 6 km: continentes, oceano, cidades com nomes e estradas)
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
int     engine_get_world_pos(Engine*, int id, float* pos3);     // posição no mundo (osso, peça presa)
void    engine_set_near(Engine*, float near_m);                 // plano próximo fixo (0 = automático)
int     engine_player_id(Engine*);                              // id do objeto do jogador (no Play pode ser a cópia em jogo) ou -1
void    engine_set_player(Engine*);                             // personagem selecionado vira o jogador
void    engine_player_input(Engine*, float x, float y, int run); // joystick -1..1 (y para frente)
const char* engine_poll_sounds(Engine*);                        // sons pedidos: "nome\tvolume\tpan" por linha (e limpa a fila)
void    engine_get_weather(Engine*, float* out3);               // chuva 0..1, noite 0..1, hora
void    engine_player_jump(Engine*);
void    engine_player_crouch(Engine*, int on);                  // agachar (1) / levantar (0)
void    engine_player_fire(Engine*);
int     engine_player_weapon(Engine*);                          // tipo da arma ou -1
// ---- Jogo (app do jogo): primeira pessoa, escolhas antes do Play e HUD
void    engine_set_first_person(Engine*, int on);               // câmera no olho do jogador
void    engine_team_loadout(Engine*, int team, int skin, const char* primary, const char* secondary);  // antes do Play ("" = sem 2ª arma)
void    engine_team_gun_skin(Engine*, const char* gun, const char* skin);  // skin da arma do jogador ("" = padrão)
int     engine_player_swap(Engine*);                            // troca rápida principal <-> pistola
int     engine_player_grenade(Engine*);                         // joga granada (1 se jogou)
void    engine_player_reload(Engine*);
const char* engine_hud_json(Engine*);                           // estado da partida para o HUD
void    engine_look(Engine*, float dx, float dy);               // girar o olhar (pixels arrastados)
// ---- Online (sala do Firebase): o app leva e traz os dados; o motor só sabe quem é quem
void    engine_net_roster(Engine*, int host, const char* local_id, const char* roster_json);  // antes do Play ("" = offline)
const char* engine_net_collect(Engine*);                        // estado dos meus lutadores (JSON)
void    engine_net_apply(Engine*, const char* states_json);     // estado dos lutadores dos outros
const char* engine_net_events(Engine*);                         // eventos a enviar (array JSON)
void    engine_net_event(Engine*, const char* event_json);      // evento recebido
void    engine_set_editor_overlay(Engine*, int on);             // borda do mapa, seleção e ícones do editor (jogo: 0)

// Câmera do editor: alvo, ângulos (radianos) e distância
void    engine_set_camera(Engine*, float tx, float ty, float tz, float yaw, float pitch, float distance);

// ---- Efeitos (partículas)
// kind: 0 fogueira, 1 tocha, 2 fumaça, 3 faíscas, 4 aura mágica, 5 portal, 6 explosão (repete), 7 orbe, 8 coluna de luz
// element: 0 fogo, 1 gelo, 2 trevas, 3 luz, 4 veneno, 5 raio, 6 arcano. Cor livre: engine_set_object_color.
int     engine_create_effect(Engine*, int kind, int element);
void    engine_spawn_effect(Engine*, int kind, int element, float x, float y, float z);  // instantâneo
void    engine_set_effect_element(Engine*, int element);        // muda o elemento do efeito selecionado
void    engine_player_cast(Engine*, int element);               // magia do jogador (Play)

// ---- Projeto (formato JSON; ver src/engine/Project.cpp)
const char* engine_export_project(Engine*);                     // JSON do projeto (válido até a próxima chamada)
int     engine_import_project(Engine*, const char* json);       // 0 se inválido (ver engine_last_error)
long long engine_project_revision(Engine*);                     // muda a cada edição (salvamento automático)
void    engine_set_project_name(Engine*, const char* name);
// Ferramentas do assistente: ver_cena, carregar_mapa, criar_objetos, editar_objeto, remover_objetos,
// definir_regras, definir_ambiente, altura_terreno, focar_camera, adicionar_comportamento. Entrada e saída em JSON.
const char* engine_tool_call(Engine*, const char* name, const char* input_json);
int     engine_add_spec(Engine*, const char* spec_json);        // cria um objeto pela receita JSON e seleciona
void    engine_set_time_of_day(Engine*, float hour);
// Planeta (mapa 5): cidades ("nome\tx\tz\ttipo" por linha; tipo 0 capital, 1 cidade, 2 vila), ir para uma cidade
// (move o jogador/câmera), nome da região onde está e imagem do mapa do mundo (RGBA size x size, norte para cima)
const char* engine_planet_places(Engine*);
int     engine_planet_goto(Engine*, int city);
const char* engine_region_name(Engine*);
int     engine_planet_map(Engine*, int size, uint8_t* rgba);
// Mapa editável: refaz o mapa atual com cada construção/árvore/poste/carro/rua como objeto (mover, apagar, cor...).
// O projeto guarda só o que mudou. Trocas: modelo de arquivo no lugar de "arvore", "pinheiro", "palmeira", "pedra",
// "casa" e textura (imagem) no lugar de um material da biblioteca. path vazio desfaz. Retornam 0 se falhar.
int     engine_map_set_editable(Engine*, int on);
int     engine_map_editable(Engine*);
int     engine_map_swap_model(Engine*, const char* kind, const char* path);
int     engine_map_swap_texture(Engine*, int material, const char* path);
// Exporta para .glb (glTF binário: Blender, Unity, Godot...). object_id -1 = cena inteira. 0 se falhar.
int     engine_export_glb(Engine*, int object_id, const char* path);
float   engine_get_time_of_day(Engine*);

// ---- Jogo (Play): out[0] estado (0 jogando, 1 vitória, 2 derrota), [1] vida, [2] vida máx, [3] pontos,
// [4] tempo, [5] limite de tempo (0 = sem), [6] dano recente (>0 pisca), [7] tem objetivo (0/1)
void    engine_game_status(Engine*, float* out8);
const char* engine_game_text(Engine*, int which);               // 0 objetivo, 1 mensagem, 2 título, 3 descrição

// Touch: action 0=down 1=move 2=up; ids/xy em arrays de n dedos (pixels)
void    engine_touch(Engine*, int action, int n, const int* ids, const float* xy);

#ifdef __cplusplus
}
#endif
