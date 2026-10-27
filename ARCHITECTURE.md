# Mobile3D Engine — Arquitetura

## Decisões principais
| Tema | Escolha | Motivo |
|---|---|---|
| Core | C++20, sem exceções/RTTI | Previsível, portável (NDK / iOS) |
| API gráfica | OpenGL ES 3.2 (backend isolado) | Cobre quase todos os Android e iOS; Vulkan/Metal entram como novos backends |
| Matemática | GLM | Testada, SIMD-friendly |
| Assets | tinygltf + KTX (ASTC/ETC2) | glTF é o padrão para mobile |
| Física | Bullet (passo fixo 60 Hz) | Leve, roda bem em ARM |
| UI/Editor | Flutter (dart:ffi → C API) | Painéis em Flutter; viewport nativo em Surface/CAMetalLayer |

## Camadas
```
Flutter UI (Inspetor, Hierarquia, Play/Stop)
        │ dart:ffi (engine_c_api.h)
        ▼
Plataforma (JNI Android / Swift iOS): Surface + EGL, input touch
        ▼
Engine ─ Scene ─ Renderer ─ Physics ─ Assets ─ Editor
          │         │          │
      Transform3D  RenderQueue  Bullet
      GameObject   CSM + PBR    RigidBody/Colliders
      Animator     Instancing
```

## Alocação zero no game loop
- `ObjectPool<T,N>` com handles de geração para GameObjects, componentes e luzes.
- `FrameArena` linear, resetada todo frame, para dados temporários.
- Buffers e filas com capacidade fixa reservada no `init`: nada de `new`, `push_back` com realocação ou `std::string` no loop.
- Hierarquia por lista intrusiva (`firstChild/nextSibling`).

## Pipeline de um frame (alvo 16.6 ms)
1. Input → EditorCamera/Gizmo/seleção por raycast (Edit) ou empurrão em corpos (Play)
2. `Physics::step` (1/60, até 3 substeps) → MotionState escreve no Transform3D
3. `Animator::update` → paleta de joints (no Edit só reavalia a pose)
4. `Scene::updateTransforms` (só nós sujos)
5. `Renderer::submit`: culling da câmera e de cada cascata → máscara de passes por item
6. Shadow pass: 3 cascatas em `GL_TEXTURE_2D_ARRAY`, só os itens daquela cascata
7. Opaque pass ordenado por (material, mesh) → instancing
8. Skinned: draw individual com UBO de joints
9. Céu procedural por último (só pixels vazios)
10. Linhas do editor (grade com depth test, gizmo e caixa de seleção por cima)
11. `glInvalidateFramebuffer` no depth

## Iluminação
- PBR metallic-roughness (GGX + Smith + Schlick), sol com CSM e até 8 point lights.
- Céu procedural (`Environment`): gradiente horizonte/zênite/chão + disco do sol.
- IBL aproximado: irradiância difusa em SH9 calculada na CPU a partir do céu (só quando o céu muda);
  especular com cubemap HDR 128² capturado do céu, mips por rugosidade e `envBRDFApprox` (Karis, mobile).
- Sombras: PCF 3×3 de amostras com comparação por hardware, normal offset proporcional ao texel da cascata,
  polygon offset no shadow pass e fade no fim da distância de sombra.
- Exposição ajustável antes do tone mapping ACES.
- Todos os shaders recebem `shaders/common.glsl` (bloco `SceneBlock` único, igual ao `SceneUBO` do C++).

## Orçamento (celular intermediário)
- ≤ 150 draw calls, ≤ 300k triângulos/frame
- Sombras: 3 cascatas × 1024², PCF por hardware (`sampler2DArrayShadow`)
- ≤ 8 point lights (UBO std140)
- Texturas ASTC 6x6 (albedo/normal) e 8x8 (ORM), sempre com mipmaps
- `glInvalidateFramebuffer` no depth após os passes (economiza banda em GPUs tiled)

## Geração procedural (src/procgen)
- `TextureGen`: 21 materiais gerados na CPU (fachadas com janelas, tijolo baiano, telha, asfalto com faixa, quadra...),
  com albedo + ORM + normal map; cache em memória, reenviados à GPU a cada cena.
- `MeshBuilder`: caixas com UV em metros (janelas alinhadas por andar), telhado de duas águas, cilindros, esferas;
  cor por vértice (atributo 10) para juntar objetos de cores diferentes no mesmo draw call.
- `Buildings`: casa, sobrado, barraco (puxadinho, laje, caixa d'água), prédio, arranha-céu em degraus, ala de escola,
  árvores (3 tipos), carro, poste. Cada construção gera também colliders em caixa.
- `Character`: humanoide de 16 ossos com skinning rígido, rosto e cabelo; 8 animações procedurais
  (parado, andar, correr, com arma, dançar, acenar). `Weapons`: 6 armas, presas ao osso da mão.
- Mapas (`src/engine/World.cpp`): construções agrupadas por célula e material (poucos draw calls), colisão estática
  em `btCompoundShape`. Mundo infinito: chunks de 96 m (heightfield do Bullet + vegetação/vilas determinísticas por
  semente) carregados em volta da câmera, 1 por frame. Mapas com limite: 4 paredes estáticas invisíveis.
- Jogador: cápsula dinâmica com rotação travada, velocidade definida pelo joystick relativo à câmera, pulo com
  raycast de chão, animação pela velocidade, tiro por raycast que empurra corpos.

## LOD (desempenho dos mapas)
- `Geo` separa base (volumes, telhados, ruas: sempre visível, juntada em grupos grandes) e detalhe (carros, postes,
  caixas d'água, platibandas, portas: some além de 160 m). Árvores: completa até 70 m, simplificada até 420 m.
- `MeshRenderer.lodNear/lodFar` (distância ao centro do mesh) testado no submit; objetos com LOD só fazem
  sombra nas 2 cascatas próximas.

## Efeitos (src/render/Particles.*, Effects.*)
- 6000 partículas em pool fixo, 1 draw call instanciado (quad por instância), alfa pré-multiplicado: aditivo
  (fogo, magia) e opaco (fumaça das trevas) no mesmo passe; `occlusion` mantém a cor saturada de dia.
- Formatos no shader: brilho, fumaça (ruído), faísca esticada pela velocidade, anel, estrela, cristal, chama.
- Emissor como componente (`Emitter`) num GameObject: escala pelo gizmo, elemento/cor editáveis.
- Luzes dinâmicas: até 8 por frame, as mais relevantes entre fogo, magia, explosões e clarões de tiro.

## Importação e retarget humanoide
- `FbxLoader` (ufbx): malhas trianguladas, materiais com textura embutida ou na pasta, skin, take -> clip.
- glTF e FBX unem todas as skins num esqueleto único (corpo + roupa + acessórios animam juntos).
- `Humanoid`: reconhece ossos pelo nome (Mixamo, Unity, Blender, Biped) e monta um `Retarget`: rotação global
  desejada = anim de referência * alinhamento (T-pose -> pose do motor) * repouso do modelo, em referencial alinhado
  ao mundo (Armature em Z-up e escala 0,01 tratados). Quadril escalado pela altura das pernas.
- Personagem importado: cápsula de física, animações próprias casadas por nome (idle/walk/run) ou as 8 do motor,
  arma na mão (referencial corrigido) e virada automática para +Z.

## Projeto, jogo e IA (src/engine/Project.cpp, Game.cpp)
- Formato único em JSON (chaves em português): cada objeto raiz guarda a **receita** que o cria
  (`{"tipo":"construcao","modelo":"casa","semente":7}`, `{"tipo":"inimigo","arma":"pistola"}`...). Salvar = mapa
  (tipo + semente, refeito igual ao abrir) + ambiente + regras + receitas com o transform atual. Os botões do editor,
  o arquivo do projeto e o assistente de IA usam o mesmo caminho (`createFromSpecJson`).
- `revision` muda a cada edição: o app salva quando ela muda (30 s, ao pausar e ao sair). Gravação atômica (tmp + rename).
- Lógica de jogo: comportamentos montados no Play a partir das receitas (inimigo com IA de perseguir/atirar/bater,
  coletável, objetivo, spawner, zona de dano, girar, flutuar, plataforma cinemática, destrutível, explosivo), dano por
  tiro/magia/explosão, regras de vitória/derrota. O Stop apaga o que o Play criou e restaura o resto.
- Ferramentas (`engine_tool_call`): ver_cena, carregar_mapa, criar_objetos, editar_objeto, remover_objetos,
  definir_regras, definir_ambiente, altura_terreno, focar_camera. O app (Assistant.kt) chama a API da Anthropic com
  essas ferramentas e executa cada uma na thread GL.

## Exportação .glb (src/engine/Export.cpp)
Malhas e texturas lidas de volta da GPU (glMapBufferRange / FBO + glReadPixels), materiais PBR com cor por vértice
(COLOR_0), esqueleto como nós + skin (inverseBind do motor) e animações assadas a 30 fps a partir da pose calculada
pelo Animator (inclui retarget). Objeto selecionado vai para a origem; LOD simplificado não é exportado.

## Estado
Feito: Pool/Scene, PBR, glTF (meshes, materiais, AO, alpha mask, double-sided, skins, animações), CSM com culling
por cascata, instancing, Animator, Bullet (Box/Sphere/Capsule/ConvexMesh), editor por toque (gizmo mover/girar/escalar,
seleção, hierarquia, inspetor), céu + IBL, estatísticas.

Próximos: compressão de texturas ASTC/ETC2 no CI (toktx), UI em Flutter, transparência real (BLEND),
morph targets, animação de nós sem skin, scripts (Lua), backends Vulkan/Metal.

## Estrutura
```
CMakeLists.txt
shaders/   common.glsl pbr.* shadow.* sky.* line.*
src/core/  Pool.h
src/scene/ Scene.h Animator.h
src/render/ Camera.h Lights.h Renderer.h Renderer.cpp
src/assets/ GltfLoader.* TextureManager.h MeshUtils.h (primitivas, normais, tangentes)
src/physics/ Physics.h
src/editor/ EditorCamera.h Gizmo.h
src/procgen/ Noise.h MeshBuilder.h TextureGen.* Buildings.* Character.* Weapons.cpp
src/engine/ Engine.h Engine.cpp (cena, personagens, jogador) World.cpp (mapas, mundo infinito)
           Project.cpp (formato do projeto, ferramentas da IA) Game.cpp (comportamentos, regras)
src/platform/ engine_c_api.h engine_c_api.cpp android_jni.cpp
app/       Android (Kotlin): GLSurfaceView + UI do editor, ProjectStore (projetos/autosave), Assistant (IA)
app/src/main/assets/exemplos/  projetos prontos (gerados pelo teste headless)
tools/     headless_test.cpp (renderiza as cenas em PNG no Linux/CI)
flutter/   engine_bindings.dart (rascunho)
```
