# Motor-Engine-3D

Motor 3D para celular (Android) em C++20 + OpenGL ES 3.2, com editor por toque.

## Como usar o app
| Ação | Como |
|---|---|
| Orbitar câmera | 1 dedo arrastando no vazio |
| Mover câmera / zoom | 2 dedos (arrastar = pan, pinça = zoom) |
| Selecionar objeto | Toque no objeto (toque de novo para pegar a peça interna de um modelo) |
| Mover / girar / escalar | Botões **Mover / Girar / Escala** e arraste as setas/círculos/quadrados coloridos |
| Valores exatos | Painel inferior (Inspetor): digite e toque em **Aplicar** |
| Adicionar | **+ Objeto**: cubo, esfera, cápsula, pedra (com física) |
| Hierarquia | **Objetos**: lista todos os objetos; tocar seleciona e enquadra |
| Carregar modelo | **Abrir .glb**: escolhe um arquivo do aparelho (ex.: `Fox.glb` dos Khronos Sample Assets) |
| Animação | **Animação**: alterna entre as animações do modelo (rodam no Play) |
| Simular | **▶ Play**: física e animação. **■ Stop** volta tudo ao estado anterior. No Play, toque num objeto para empurrar |
| Visual | **Visual**: exposição, intensidade do sol e grade |
| Cenas prontas | **Cenas**: vitrine de materiais, física, cena vazia |

## Build
- **APK**: GitHub Actions (`.github/workflows/android.yml`) → artefato `mobile3d-debug-apk`.
- **Teste headless**: o mesmo workflow compila o motor no Linux, renderiza as cenas com Mesa e publica PNGs no artefato `screenshots-teste`. Localmente:
  ```
  sudo apt install libegl-dev libgles-dev
  cmake -S . -B build && cmake --build build -j4 --target mobile3d_headless
  ./build/mobile3d_headless shaders saida/ Fox.glb
  ```

Detalhes da arquitetura em [ARCHITECTURE.md](ARCHITECTURE.md).
