package com.mobile3d

import com.anthropic.client.AnthropicClient
import com.anthropic.client.okhttp.AnthropicOkHttpClient
import com.anthropic.core.JsonValue
import com.anthropic.core.jsonMapper
import com.anthropic.models.messages.ContentBlockParam
import com.anthropic.models.messages.MessageCreateParams
import com.anthropic.models.messages.MessageParam
import com.anthropic.models.messages.OutputConfig
import com.anthropic.models.messages.StopReason
import com.anthropic.models.messages.ThinkingConfigAdaptive
import com.anthropic.models.messages.Tool
import com.anthropic.models.messages.ToolResultBlockParam
import java.time.Duration

// Assistente que monta o jogo usando as ferramentas do motor (engine_tool_call).
// A chave da API fica só no celular (SharedPreferences); as chamadas vão direto para o provedor escolhido.
interface GameAgent {
    var cancelled: Boolean
    fun reset()
    fun send(userText: String, onText: (String) -> Unit)
}

// Ferramenta descrita uma vez e convertida para o formato de cada provedor
class ToolSpec(val name: String, val description: String, val properties: Map<String, Any>, val required: List<String>)

// Claude (API da Anthropic), pelo SDK oficial
class Assistant(
    apiKey: String,
    private val model: String,
    // executa a ferramenta na thread GL e devolve o JSON de resposta
    private val runTool: (name: String, inputJson: String) -> String,
) : GameAgent {
    private val client: AnthropicClient = AnthropicOkHttpClient.builder()
        .apiKey(apiKey)
        .timeout(Duration.ofMinutes(5))
        .maxRetries(2)
        .build()
    private val history = ArrayList<MessageParam>()
    @Volatile override var cancelled = false

    override fun reset() = synchronized(history) { history.clear() }

    // Roda o ciclo pedido -> ferramentas -> resposta. onText recebe os textos do assistente e o andamento.
    override fun send(userText: String, onText: (String) -> Unit) {
        cancelled = false
        synchronized(history) {
            history.add(MessageParam.ofUser(userText))
            for (round in 0 until MAX_ROUNDS) {
                if (cancelled) { onText("⏹ Parado."); return }
                val b = MessageCreateParams.builder()
                    .model(model)
                    .maxTokens(16000L)
                    .system(SYSTEM)
                    .thinking(ThinkingConfigAdaptive.builder().build())
                    .outputConfig(OutputConfig.builder().effort(OutputConfig.Effort.MEDIUM).build())
                    .messages(history)
                TOOLS.forEach { b.addTool(it) }
                if (model == DEFAULT_MODEL) {  // se o modelo principal estiver indisponível, o servidor usa um reserva
                    b.putAdditionalHeader("anthropic-beta", "server-side-fallback-2026-07-01")
                    b.putAdditionalBodyProperty("fallbacks", JsonValue.from("default"))
                }
                val msg = client.messages().create(b.build())
                history.add(msg.toParam())
                msg.content().forEach { block -> block.text().ifPresent { if (it.text().isNotBlank()) onText(it.text()) } }
                val stop = msg.stopReason().orElse(null)
                if (stop == StopReason.REFUSAL) { onText("O assistente recusou esse pedido."); return }
                val uses = msg.content().mapNotNull { it.toolUse().orElse(null) }
                if (uses.isEmpty()) {
                    if (stop == StopReason.MAX_TOKENS) onText("(resposta cortada: peça para continuar)")
                    return
                }
                val results = uses.map { tu ->
                    val input = jsonMapper().writeValueAsString(tu._input())
                    onText("🔧 ${TOOL_LABELS[tu.name()] ?: tu.name()}")
                    val out = try { runTool(tu.name(), input) } catch (e: Exception) { "{\"erro\":\"${e.message}\"}" }
                    ContentBlockParam.ofToolResult(
                        ToolResultBlockParam.builder().toolUseId(tu.id()).content(out).isError(out.contains("\"erro\"")).build())
                }
                history.add(MessageParam.ofUserOfContentBlockParams(results))
            }
            onText("(parei depois de muitas etapas; peça para continuar se faltar algo)")
        }
    }

    companion object {
        const val DEFAULT_MODEL = "claude-opus-5-5"
        val MODELS = arrayOf(DEFAULT_MODEL, "claude-sonnet-5-5", "claude-haiku-5-5")
        private const val MAX_ROUNDS = 40

        val TOOL_LABELS = mapOf(
            "ver_cena" to "olhando a cena", "carregar_mapa" to "carregando mapa", "criar_objetos" to "criando objetos",
            "editar_objeto" to "editando objeto", "remover_objetos" to "removendo objetos", "definir_regras" to "definindo regras",
            "definir_ambiente" to "ajustando ambiente", "altura_terreno" to "medindo terreno", "focar_camera" to "movendo câmera")

        private fun tool(name: String, desc: String, props: Map<String, Any>, required: List<String> = emptyList()) =
            ToolSpec(name, desc, props, required)

        private fun ToolSpec.toClaude(): Tool {
            val p = Tool.InputSchema.Properties.builder()
            properties.forEach { (k, v) -> p.putAdditionalProperty(k, JsonValue.from(v)) }
            return Tool.builder().name(name).description(description)
                .inputSchema(Tool.InputSchema.builder().properties(p.build()).required(required).build())
                .build()
        }
        private val vec3 = mapOf("type" to "array", "items" to mapOf("type" to "number"), "minItems" to 3, "maxItems" to 3)

        val SPECS: List<ToolSpec> = listOf(
            tool("ver_cena", "Mostra o projeto atual em JSON: mapa, ambiente, regras e todos os objetos editáveis com id, " +
                "posição, rotação e receita. Use antes de editar para saber os ids e onde as coisas estão.", emptyMap()),
            tool("carregar_mapa", "Troca o cenário inteiro (apaga os objetos atuais). tipo: vazio (só chão 60x60, para criar do zero), " +
                "cidade, favela, escola, bairro (com limites) ou infinito. Responde os limites do mapa e a posição do jogador.",
                mapOf("tipo" to mapOf("type" to "string", "enum" to listOf("vazio", "cidade", "favela", "escola", "bairro", "infinito")),
                      "semente" to mapOf("type" to "integer", "description" to "variação do mapa (opcional)"),
                      "com_jogador" to mapOf("type" to "boolean", "description" to "cria o personagem do jogador (padrão true)")),
                listOf("tipo")),
            tool("criar_objetos", "Cria vários objetos de uma vez (formato descrito nas instruções). Responde os ids criados e erros.",
                mapOf("objetos" to mapOf("type" to "array", "items" to mapOf("type" to "object"))), listOf("objetos")),
            tool("editar_objeto", "Muda um objeto pelo id. nome/pos/rot/rot_y/escala/cor/material/comportamentos e parâmetros " +
                "(vida, dano, velocidade, alcance, pontos, raio...) mudam no lugar; outros campos (aparencia, modelo, arma, item, efeito...) " +
                "recriam o objeto e ele ganha um id novo (veja a resposta).",
                mapOf("id" to mapOf("type" to "integer")), listOf("id")),
            tool("remover_objetos", "Apaga objetos pelos ids.",
                mapOf("ids" to mapOf("type" to "array", "items" to mapOf("type" to "integer"))), listOf("ids")),
            tool("definir_regras", "Regras do jogo. objetivo: livre, sobreviver (até o tempo acabar), coletar (quantidade ou todos), " +
                "chegar (ao objeto objetivo), eliminar (inimigos: quantidade ou todos) ou pontos (quantidade). tempo em segundos (0 = sem limite).",
                mapOf("objetivo" to mapOf("type" to "string", "enum" to listOf("livre", "sobreviver", "coletar", "chegar", "eliminar", "pontos")),
                      "quantidade" to mapOf("type" to "integer"), "tempo" to mapOf("type" to "number"),
                      "vida_jogador" to mapOf("type" to "number"), "titulo" to mapOf("type" to "string"),
                      "descricao" to mapOf("type" to "string"))),
            tool("definir_ambiente", "Hora do dia (0-24; 19-5 é noite), exposição da câmera, força do sol e neblina (0 a 0.02).",
                mapOf("hora" to mapOf("type" to "number"), "exposicao" to mapOf("type" to "number"),
                      "sol" to mapOf("type" to "number"), "neblina" to mapOf("type" to "number"))),
            tool("altura_terreno", "Altura do chão em pontos [x,z] (favela e infinito têm morros). Use para pousar objetos no chão.",
                mapOf("pontos" to mapOf("type" to "array", "items" to mapOf("type" to "array", "items" to mapOf("type" to "number")))),
                listOf("pontos")),
            tool("focar_camera", "Aponta a câmera do editor para um ponto, para o usuário ver o que foi criado.",
                mapOf("alvo" to vec3, "distancia" to mapOf("type" to "number"))),
        )
        val TOOLS: List<Tool> by lazy { SPECS.map { it.toClaude() } }

        val SYSTEM = """
Você é o assistente do Motor 3D, um editor de jogos 3D para celular. O usuário fala português do Brasil, de forma
direta; responda curto, em português. Você monta e altera o jogo usando as ferramentas; o usuário vê tudo na hora e
depois aperta ▶ Play para jogar (joystick, pular, correr, atirar/bater, magia).

Coordenadas: metros, Y para cima. O jogador anda no chão (Y = altura do terreno; 0 em mapas planos). rot_y em graus.
Personagens olham para +Z com rot_y 0.

Objetos (criar_objetos). Campos comuns opcionais: nome, pos [x,y,z], rot [x,y,z] ou rot_y, escala (número ou [x,y,z]),
cor ("vermelho", "#ff8800" ou [r,g,b] 0..1), material (reboco, tijolo, telha, concreto, asfalto, calcada, grama,
madeira, metal, metal_escuro, plastico, vidro, agua, pintura), comportamentos (lista abaixo).
- {"tipo":"forma","forma":"cubo|esfera|capsula|pedra","fisica":true,"massa":1} (massa 0 = parado/fixo; use para paredes, rampas, plataformas)
- {"tipo":"chao","tamanho":40}
- {"tipo":"construcao","modelo":"casa|sobrado|barraco|predio|arranha_ceu|escola|arvore|pinheiro|palmeira|carro|poste|muro","semente":123}
- {"tipo":"personagem","jogador":true,"arma":"pistola|fuzil|escopeta|sniper|faca|taco","aparencia":"aleatorio" ou
   {"altura":1.75,"corpo":"magro|normal|forte","pele":"clara|morena|parda|negra","camisa":cor,"calca":cor,"tenis":cor,
    "cabelo":cor,"estilo_cabelo":"careca|curto|comprido|bone","manga_comprida":false}}
- {"tipo":"inimigo", ... mesmos campos de personagem, "vida":100,"dano":10,"velocidade":3.2,"alcance":25,"pontos":10}
  (com arma de fogo atira de longe; sem arma ou com faca/taco ataca de perto)
- {"tipo":"arma","arma":"fuzil"} solta no chão, ou {"tipo":"arma","arma":"fuzil","portador":<id do personagem>}
- {"tipo":"coletavel","item":"moeda|cristal|vida|municao","pontos":10} (vida cura 30)
- {"tipo":"objetivo"} ponto de chegada (coluna de luz; regra "chegar")
- {"tipo":"spawner","maximo":5,"total":10,"intervalo":4,"arma_inimigo":"pistola","vida":60} portal que cria inimigos perto do jogador
- {"tipo":"zona_dano","raio":2,"dano":20,"elemento":"fogo|veneno|gelo|trevas|luz|raio|arcano"} dano por segundo dentro
- {"tipo":"efeito","efeito":"fogueira|tocha|fumaca|faiscas|aura|portal|explosao|orbe|coluna_luz","elemento":"fogo|gelo|trevas|luz|veneno|raio|arcano"}
- {"tipo":"luz","cor":"#ffcc88","intensidade":8,"raio":10}
Comportamentos extras: "girar", "flutuar", "explosivo" (explode ao levar dano), "destrutivel" (quebra), "inimigo",
{"tipo":"plataforma","destino":[0,3,0],"velocidade":2} (vai e volta), {"tipo":"zona_dano",...}. Ex.: barril explosivo =
forma cubo/capsula + "comportamentos":["explosivo"].

Como trabalhar: pense no jogo completo (cenário, jogador com arma, desafios, objetivo/regras, clima) e crie em poucas
chamadas, várias coisas por vez. Em mapas prontos (cidade, favela...) use ver_cena/altura_terreno e os limites para
posicionar perto do jogador (até ~40 m) e no chão. Construções têm uns 8-30 m de largura: não sobreponha. Ao terminar,
diga em 2-4 linhas o que fez e como jogar. Se o pedido for ambíguo, faça uma versão boa e diga o que dá para ajustar.
O projeto é salvo automaticamente.
""".trimIndent()
    }
}
