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

## Estado
Feito: Pool/Scene, PBR, glTF (meshes, materiais, AO, alpha mask, double-sided, skins, animações), CSM com culling
por cascata, instancing, Animator, Bullet (Box/Sphere/Capsule/ConvexMesh), editor por toque (gizmo mover/girar/escalar,
seleção, hierarquia, inspetor), céu + IBL, estatísticas.

Próximos: compressão de texturas ASTC/ETC2 no CI (toktx), UI em Flutter, transparência real (BLEND),
morph targets, animação de nós sem skin, salvar/carregar cena, backends Vulkan/Metal.

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
src/platform/ engine_c_api.h engine_c_api.cpp android_jni.cpp
app/       Android (Kotlin): GLSurfaceView + UI do editor
tools/     headless_test.cpp (renderiza as cenas em PNG no Linux/CI)
flutter/   engine_bindings.dart (rascunho)
```
