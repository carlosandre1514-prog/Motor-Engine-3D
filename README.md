# Motor-Engine-3D

Motor 3D para celular (Android) em C++20 + OpenGL ES 3.2, com editor por toque.

## Como usar o app
| Ação | Como |
|---|---|
| Projetos | **📁 Projeto**: salvar, novo, abrir, duplicar, renomear, exemplos prontos, exportar/importar `.json`, abrir de link, excluir, sair. **Salva sozinho** a cada 30 s com mudanças, ao minimizar/fechar o app e ao sair do projeto; ao abrir o app, continua de onde parou |
| Jogo | **🎮 Jogo**: objetivo e regras (coletar, eliminar, chegar, sobreviver, pontos; tempo e vida), inimigos (pistola, fuzil, faca, desarmado), portal de inimigos, moeda, cristal, vida, ponto de chegada, zonas de dano, barril explosivo, caixa quebrável e comportamentos do objeto selecionado (girar, flutuar, plataforma, destrutível, explosivo) |
| Assistente de IA | **🤖 IA**: escreva o jogo que quer (ex.: "favela à noite, sobreviver 2 minutos contra inimigos de pistola") e o assistente monta no editor. Em ⚙ escolha Claude (chave da Anthropic) ou outra IA no padrão OpenAI (OpenAI, Gemini, DeepSeek, Groq, OpenRouter, Mistral...). As chaves ficam só no celular |
| Exportar 3D | **📁 Projeto → 📦 Exportar modelo 3D (.glb)**: o objeto selecionado (personagem com esqueleto e animações, construção, arma) ou a cena/mapa inteiro, com cores e texturas. Abre no Blender, Unity (glTFast), Godot, Unreal e aqui mesmo (Importar 3D). Para FBX, abra o .glb no Blender e exporte |
| Orbitar câmera | 1 dedo arrastando no vazio |
| Mover câmera / zoom | 2 dedos (arrastar = pan, pinça = zoom) |
| Selecionar objeto | Toque no objeto (toque de novo para pegar a peça interna de um modelo) |
| Mover / girar / escalar | Botões **Mover / Girar / Escala** e arraste as setas/círculos/quadrados coloridos |
| Valores exatos | Painel inferior (Inspetor): digite e toque em **Aplicar** |
| Criar | **+ Criar**: personagem (aleatório ou personalizado), arma, construção (casa, sobrado, barraco, prédio, arranha-céu, escola, muro), natureza/objetos (árvores, carro, poste), formas com física |
| Mapas | **Mapas**: cidade, favela, colégio, bairro (com limite e barreira invisível — borda vermelha no editor), mundo infinito, ou **criar mapa/cena vazia** para montar do zero |
| Material | **Material**: troca o material do objeto selecionado (tijolo, concreto, vidro, madeira...), cor ou uma imagem do celular |
| Jogar | **▶ Play** com um personagem jogador: joystick (esquerda), Pular / Correr / Atirar (direita), arraste a tela para girar a câmera |
| Hierarquia | **Objetos**: lista todos os objetos; tocar seleciona e enquadra |
| Importar 3D | **Importar 3D**: `.fbx`, `.glb`, `.gltf` ou `.obj` do aparelho (Mixamo, Unity Asset Store, Sketchfab, Blender). Pode misturar formatos no mesmo jogo. Outros formatos (.dae, .blend, .3ds): abra no Blender e exporte `.glb` |
| Personagem de arquivo | **+ Criar → Personagem → 📂 Personagem de um arquivo 3D**: escolha o `.fbx`/`.glb`/`.gltf`/`.obj` e ele já vira personagem jogável (física, anda/corre/pula). Humanoides (ossos tipo Mixamo/Unity) usam as animações do motor se não tiverem as próprias; modelos sem esqueleto andam sem animação |
| Efeitos | **+ Criar → Efeito**: fogueira, tocha, fumaça, faíscas, aura, portal, explosão, orbe, coluna de luz; elementos fogo, gelo, trevas, luz, veneno, raio, arcano ou cor livre (Material → Cor) |
| Magia | No Play: botão **Magia** lança o elemento escolhido (segure para trocar); **Atirar** mostra clarão, bala e impacto |
| Animação | **Animação**: lista as animações do personagem/modelo selecionado (inclusive `.fbx`/`.glb` importados: humanoides ganham também as do motor, marcadas "(motor)") e toca a escolhida, também no editor. No Play, parado/andar/correr trocam sozinhos |
| Criar animação | **Animação → 🎬 Criar animação personalizada**: escolha o osso, gire em X/Y/Z, avance o tempo e grave quadros; ▶ Prévia; 💾 Salvar com nome. Fica no projeto e vai junto no `.glb` exportado |
| Editar mapa | **Mapas → ✏ Editar este mapa peça por peça**: cada construção, árvore, poste, carro, rua e calçada vira objeto (mover, girar, apagar, cor, material, textura; toque de novo pega a parte: parede, telhado...). Vira o SEU mapa (projeto separado); o mapa base continua igual. O projeto guarda só as mudanças |
| Trocar modelos/texturas do mapa | **Mapas → 🌳 Trocar árvores/pedras/casas por arquivo 3D** e **🖼 Trocar a textura de um material** (grama, telha, tijolo...). Vale também no mundo infinito e fica salvo no projeto |
| Peças de construção | **+ Criar → Construção**: rua, cruzamento, calçada, escada, rampa (dá para subir), piso/laje, parede, cerca, banco, caixa d'água, além das casas e prédios |
| Editar personagem | Selecione o personagem → **+ Criar → Personagem → Editar o personagem selecionado** (altura, corpo, cores, cabelo) |
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
