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
1. Input → EditorCamera/Gizmo (Edit) ou gameplay (Play)
2. `Physics::step` (1/60, até 2 substeps) → sincroniza Transform3D
3. `Animator::update` → paleta de joints
4. `Scene::updateTransforms` (só nós sujos)
5. Frustum culling (câmera + cada cascata) → `RenderQueue`
6. Shadow pass: 3 cascatas em `GL_TEXTURE_2D_ARRAY`
7. Opaque pass ordenado por (material, mesh) → instancing
8. Skinned: draw individual com UBO de joints
9. Gizmos/overlay (apenas editor)

## Orçamento (celular intermediário)
- ≤ 150 draw calls, ≤ 300k triângulos/frame
- Sombras: 3 cascatas × 1024², PCF por hardware (`sampler2DArrayShadow`)
- ≤ 8 point lights (UBO std140)
- Texturas ASTC 6x6 (albedo/normal) e 8x8 (ORM), sempre com mipmaps
- `glInvalidateFramebuffer` no depth após os passes (economiza banda em GPUs tiled)

## Ordem sugerida de implementação
1. Pool/Scene → 2. Renderer PBR sem sombra → 3. glTF → 4. CSM → 5. Instancing → 6. Animator → 7. Bullet → 8. Editor touch + gizmos → 9. Flutter UI → 10. Backends Vulkan/Metal, IBL, culling na GPU.

## Estrutura
```
CMakeLists.txt
shaders/   pbr.vert pbr.frag shadow.vert shadow.frag
src/core/  Pool.h
src/scene/ Scene.h Animator.h
src/render/ Camera.h Lights.h Renderer.h Renderer.cpp
src/assets/ GltfLoader.h GltfLoader.cpp TextureManager.h
src/physics/ Physics.h
src/editor/ EditorCamera.h Gizmo.h
src/platform/ engine_c_api.h engine_c_api.cpp android_jni.cpp
flutter/   engine_bindings.dart
```

> Observação: o código é um esqueleto estrutural compilável em conceito, não foi compilado nem testado em dispositivo. Espere ajustes de includes, loader GL e detalhes de plataforma ao integrar.
