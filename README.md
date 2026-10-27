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
| Criar | **+ Criar**: personagem (aleatório ou personalizado), arma, construção (casa, sobrado, barraco, prédio, arranha-céu, escola, muro), natureza/objetos (árvores, carro, poste), formas com física |
| Mapas | **Mapas**: mundo infinito procedural, cidade, favela, colégio, bairro (com limite e barreira invisível — borda vermelha no editor) |
| Material | **Material**: troca o material do objeto selecionado (tijolo, concreto, vidro, madeira...), cor ou uma imagem do celular |
| Jogar | **▶ Play** com um personagem jogador: joystick (esquerda), Pular / Correr / Atirar (direita), arraste a tela para girar a câmera |
| Hierarquia | **Objetos**: lista todos os objetos; tocar seleciona e enquadra |
| Importar 3D | **Importar 3D**: `.glb`, `.gltf` ou `.fbx` do aparelho (Mixamo, Unity Asset Store, Sketchfab, Blender) |
| Personagem importado | Selecione o modelo → **+ Criar → Personagem → Usar modelo importado**: ganha física e anda/corre/pula. Humanoides (ossos tipo Mixamo/Unity) usam as animações do motor se não tiverem as próprias |
| Efeitos | **+ Criar → Efeito**: fogueira, tocha, fumaça, faíscas, aura, portal, explosão, orbe, coluna de luz; elementos fogo, gelo, trevas, luz, veneno, raio, arcano ou cor livre (Material → Cor) |
| Magia | No Play: botão **Magia** lança o elemento escolhido (segure para trocar); **Atirar** mostra clarão, bala e impacto |
| Animação | **Animação**: alterna entre as animações do modelo (rodam no Play) |
| Simular | **▶ Play**: física e animação. **■ Stop** volta tudo ao estado anterior. No Play, toque num objeto para empurrar |
| Visual | **Visual**: exposição, intensidade do sol e grade |

## Build
- **APK**: GitHub Actions (`.github/workflows/android.yml`) → artefato `mobile3d-debug-apk`.
- **Teste headless**: o mesmo workflow compila o motor no Linux, renderiza as cenas com Mesa e publica PNGs no artefato `screenshots-teste`. Localmente:
  ```
  sudo apt install libegl-dev libgles-dev
  cmake -S . -B build && cmake --build build -j4 --target mobile3d_headless
  ./build/mobile3d_headless shaders saida/ Fox.glb
  ```

Detalhes da arquitetura em [ARCHITECTURE.md](ARCHITECTURE.md).
