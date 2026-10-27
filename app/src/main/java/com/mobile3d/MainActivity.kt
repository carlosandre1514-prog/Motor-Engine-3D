package com.mobile3d

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.json.JSONArray
import org.json.JSONObject
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

// Estado lido do motor na thread GL e mostrado pela UI (atualizado a cada ~0,25 s)
class EngineSnapshot {
    @Volatile var stats = FloatArray(6)
    @Volatile var selected = -1
    @Volatile var selectedName = ""
    @Volatile var transform = FloatArray(9)
    @Volatile var playing = false
    @Volatile var hasPlayer = false
    @Volatile var playerWeapon = -1
    @Volatile var ready = false
    @Volatile var game = FloatArray(8)
    @Volatile var objective = ""
    @Volatile var message = ""
}

class MainActivity : Activity() {
    private lateinit var view: EngineView
    private val ui = Handler(Looper.getMainLooper())
    private val snap get() = view.snapshot

    // Widgets
    private lateinit var playBtn: TextView
    private lateinit var modeBtns: List<TextView>
    private lateinit var statsText: TextView
    private lateinit var inspector: LinearLayout
    private lateinit var inspTitle: TextView
    private lateinit var fields: List<EditText>
    private lateinit var visualPanel: LinearLayout
    private lateinit var hud: FrameLayout
    private lateinit var joystick: JoystickView
    private lateinit var runBtn: TextView
    private lateinit var fireBtn: TextView
    private var shownSelection = -2
    private var running = false
    private var spellElement = 0
    private val elements = arrayOf("Fogo", "Gelo", "Trevas", "Luz", "Veneno", "Raio", "Arcano")
    private val effects = arrayOf("Fogueira", "Tocha", "Fumaça", "Faíscas", "Aura mágica", "Portal", "Explosão (repete)", "Orbe mágico", "Coluna de luz")
    private var stick = floatArrayOf(0f, 0f)

    // Projeto e salvamento automático
    private lateinit var store: ProjectStore
    @Volatile private var savedRev = -1L          // revisão do motor já gravada no arquivo (lida na thread GL)
    private var projectName = ""
    private lateinit var gameBar: TextView
    private lateinit var gameMsg: TextView
    private lateinit var endPanel: LinearLayout
    private lateinit var endTitle: TextView
    private lateinit var endText: TextView
    private lateinit var hurtOverlay: View
    private var lastMsg = ""
    private var msgUntil = 0L

    // Assistente de IA
    private lateinit var aiPanel: LinearLayout
    private lateinit var aiLog: TextView
    private lateinit var aiScroll: ScrollView
    private lateinit var aiInput: EditText
    private lateinit var aiSend: TextView
    private var assistant: GameAgent? = null
    @Volatile private var aiBusy = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        store = ProjectStore(this)
        view = EngineView(this, copyShaders())
        view.onReady = { openStartupProject() }  // thread GL: motor (re)criado -> abre o projeto salvo

        val root = FrameLayout(this)
        root.addView(view, FrameLayout.LayoutParams(-1, -1))
        root.addView(buildToolbar(), FrameLayout.LayoutParams(-1, -2, Gravity.TOP))

        statsText = TextView(this).apply {
            setTextColor(Color.WHITE)
            setShadowLayer(3f, 0f, 0f, Color.BLACK)
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
        }
        root.addView(statsText, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START).apply {
            topMargin = dp(52); leftMargin = dp(8)
        })

        visualPanel = buildVisualPanel().apply { visibility = View.GONE }
        root.addView(visualPanel, FrameLayout.LayoutParams(dp(260), -2, Gravity.TOP or Gravity.END).apply {
            topMargin = dp(52); rightMargin = dp(8)
        })

        inspector = buildInspector().apply { visibility = View.GONE }
        root.addView(inspector, FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM))

        hud = buildHud().apply { visibility = View.GONE }
        root.addView(hud, FrameLayout.LayoutParams(-1, -1))

        aiPanel = buildAiPanel().apply { visibility = View.GONE }
        root.addView(aiPanel, FrameLayout.LayoutParams(dp(340), -2, Gravity.BOTTOM or Gravity.END).apply {
            rightMargin = dp(8); bottomMargin = dp(8)
        })

        setContentView(root)
        ui.post(refresh)
        ui.postDelayed(autosave, AUTOSAVE_MS)
    }

    // Fechar/minimizar: salva antes de pausar a thread GL (espera até 2 s)
    override fun onPause() {
        saveBlocking()
        super.onPause()
        view.onPause()
    }
    override fun onResume() { super.onResume(); view.onResume() }
    override fun onDestroy() { ui.removeCallbacksAndMessages(null); super.onDestroy() }

    // ---------------- projeto: abrir, salvar, autosave ----------------
    private val autosave = object : Runnable {
        override fun run() {
            gl { saveIfChanged() }
            ui.postDelayed(this, AUTOSAVE_MS)
        }
    }

    // thread GL
    private fun saveIfChanged(force: Boolean = false): Boolean {
        if (!snap.ready || projectName.isEmpty()) return false
        val rev = NativeBridge.nativeProjectRevision()
        if (!force && rev == savedRev) return false
        try {
            store.write(projectName, NativeBridge.exportProject())
            savedRev = rev
            return true
        } catch (e: Exception) {
            toast("Erro ao salvar: ${e.message}")
            return false
        }
    }

    private fun saveBlocking() {
        if (!snap.ready) return
        val latch = CountDownLatch(1)
        view.queueEvent { try { saveIfChanged() } finally { latch.countDown() } }
        latch.await(2, TimeUnit.SECONDS)
    }

    // thread GL
    private fun openStartupProject() {
        val name = store.current
        if (name.isNotEmpty() && openProjectGl(name)) { toast("Projeto \"$name\" aberto (continua de onde parou)"); return }
        newProjectGl(store.uniqueName("Meu jogo"), -1)
    }

    // thread GL
    private fun openProjectGl(name: String): Boolean {
        val json = store.read(name) ?: return false
        if (!NativeBridge.importProject(json)) { toast("Projeto \"$name\" com erro: " + NativeBridge.nativeLastError()); return false }
        setCurrent(name)
        savedRev = NativeBridge.nativeProjectRevision()
        val warn = NativeBridge.nativeLastError()
        if (warn.isNotBlank()) toast("Aviso ao abrir: $warn")
        return true
    }

    // thread GL. map: -1 vazio (chão + jogador), 0..4 mapas prontos
    private fun newProjectGl(name: String, map: Int) {
        setCurrent(name)
        if (map < 0) NativeBridge.toolCall("carregar_mapa", "{\"tipo\":\"vazio\",\"com_jogador\":true}")
        else NativeBridge.nativeLoadMap(map, seed())
        NativeBridge.toolCall("definir_regras", "{\"objetivo\":\"livre\",\"titulo\":\"\",\"descricao\":\"\"}")
        saveIfChanged(force = true)
        toast("Projeto \"$name\" criado (salvamento automático ligado)")
    }

    private fun setCurrent(name: String) {
        projectName = name
        store.current = name
        NativeBridge.nativeSetProjectName(name.toByteArray(Charsets.UTF_8))
        runOnUiThread { if (::playBtn.isInitialized) title = name }
    }

    private fun stopPlayIfNeeded() { if (snap.playing) togglePlay() }

    private fun projectDialog() {
        val items = arrayOf("💾 Salvar agora", "📄 Novo projeto…", "📂 Abrir projeto…", "📑 Duplicar (salvar como)…", "✏ Renomear…",
                            "⭐ Abrir exemplo pronto…", "📤 Exportar arquivo .json…", "📥 Importar arquivo .json…", "🔗 Abrir de link…",
                            "🗑 Excluir projetos…", "🚪 Sair do projeto", "📦 Exportar modelo 3D (.glb)…")
        dialog().setTitle("Projeto: $projectName").setItems(items) { _, which ->
            when (which) {
                0 -> gl { if (saveIfChanged(force = true)) toast("Salvo ✔") }
                1 -> newProjectDialog()
                2 -> openProjectDialog()
                3 -> askText("Duplicar como", store.uniqueName("$projectName cópia")) { n ->
                    val name = store.uniqueName(n)
                    stopPlayIfNeeded()
                    gl { saveIfChanged(); setCurrent(name); saveIfChanged(force = true); toast("Agora editando \"$name\"") }
                }
                4 -> askText("Novo nome", projectName) { n ->
                    val to = ProjectStore.safe(n)
                    gl {
                        saveIfChanged(force = true)
                        if (to.isNotEmpty() && store.rename(projectName, to)) { setCurrent(to); saveIfChanged(force = true); toast("Renomeado") }
                        else toast("Já existe um projeto com esse nome")
                    }
                }
                5 -> examplesDialog()
                6 -> { saveBlocking(); exportProject() }
                7 -> pickFile(REQ_PROJECT)
                8 -> askText("Link do projeto (.json, GitHub ou outro)", "https://") { url -> downloadProject(url) }
                9 -> deleteProjectsDialog()
                11 -> exportGlbDialog()
                else -> { stopPlayIfNeeded(); saveBlocking(); toast("Projeto salvo"); openProjectDialog(exitMode = true) }
            }
        }.show()
    }

    private fun newProjectDialog() {
        val maps = arrayOf("Vazio (criar do zero)", "Cidade", "Favela", "Colégio", "Bairro", "Mundo infinito")
        askText("Nome do novo projeto", store.uniqueName("Meu jogo")) { n ->
            list("Começar com qual mapa?", maps) { m ->
                stopPlayIfNeeded()
                val kind = when (m) { 0 -> -1; 1 -> 1; 2 -> 2; 3 -> 3; 4 -> 4; else -> 0 }
                if (kind >= 0) toast("Gerando mapa…")
                gl { saveIfChanged(); newProjectGl(store.uniqueName(n), kind) }
            }
        }
    }

    private fun openProjectDialog(exitMode: Boolean = false) {
        val names = store.list()
        val items = (listOf("📄 Novo projeto…") + names.map { if (it == projectName) "● $it" else it }).toTypedArray()
        val d = dialog().setTitle(if (exitMode) "Seus projetos (o atual foi salvo)" else "Abrir projeto").setItems(items) { _, which ->
            if (which == 0) { newProjectDialog(); return@setItems }
            val name = names[which - 1]
            stopPlayIfNeeded()
            toast("Abrindo $name…")
            gl { saveIfChanged(); if (openProjectGl(name)) toast("Projeto \"$name\" aberto") }
        }
        if (!exitMode) d.setNegativeButton("Cancelar", null)
        d.show()
    }

    private fun deleteProjectsDialog() {
        val names = store.list().filter { it != projectName }
        if (names.isEmpty()) { toast("Só existe o projeto aberto (não dá para excluir o que está aberto)"); return }
        list("Excluir qual projeto? (não tem volta)", names.toTypedArray()) { i ->
            dialog().setTitle("Excluir \"${names[i]}\"?").setPositiveButton("Excluir") { _, _ -> store.delete(names[i]); toast("Excluído") }
                .setNegativeButton("Cancelar", null).show()
        }
    }

    private fun examplesDialog() {
        val files = assets.list("exemplos").orEmpty().filter { it.endsWith(".json") }.sorted()
        if (files.isEmpty()) { toast("Nenhum exemplo no app"); return }
        val titles = files.map { f ->
            try { JSONObject(assets.open("exemplos/$f").bufferedReader().readText()).optString("nome", f) } catch (e: Exception) { f }
        }
        list("Exemplos (abre como projeto novo)", titles.toTypedArray()) { i ->
            val json = assets.open("exemplos/${files[i]}").bufferedReader().readText()
            importAsNewProject(json, titles[i])
        }
    }

    private fun importAsNewProject(json: String, fallbackName: String) {
        val name = store.uniqueName(try { JSONObject(json).optString("nome", fallbackName) } catch (e: Exception) { fallbackName })
        stopPlayIfNeeded()
        toast("Abrindo $name…")
        gl {
            saveIfChanged()
            if (NativeBridge.importProject(json)) {
                setCurrent(name)
                saveIfChanged(force = true)
                toast("Projeto \"$name\" pronto. Aperte ▶ Play!")
            } else toast("Arquivo inválido: " + NativeBridge.nativeLastError())
        }
    }

    private fun downloadProject(link: String) {
        var url = link.trim()
        // link de página do GitHub -> arquivo cru
        Regex("https://github.com/([^/]+)/([^/]+)/blob/(.+)").matchEntire(url)?.let { m ->
            url = "https://raw.githubusercontent.com/${m.groupValues[1]}/${m.groupValues[2]}/${m.groupValues[3]}"
        }
        toast("Baixando…")
        Thread {
            try {
                val c = URL(url).openConnection() as HttpURLConnection
                c.connectTimeout = 15000; c.readTimeout = 30000
                val json = c.inputStream.bufferedReader().readText()
                runOnUiThread { importAsNewProject(json, url.substringAfterLast('/').removeSuffix(".json")) }
            } catch (e: Exception) { toast("Não consegui baixar: ${e.message}") }
        }.start()
    }

    private fun exportProject() {
        val i = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/json"
            putExtra(Intent.EXTRA_TITLE, "$projectName.json")
        }
        startActivityForResult(i, REQ_EXPORT)
    }

    private var pendingGlb: File? = null

    // .glb = glTF binário: abre no Blender, Unity (glTFast), Godot, Unreal, sites de visualização e de novo aqui
    private fun exportGlbDialog() {
        val items = arrayOf("Objeto selecionado (personagem com animações, construção, arma…)", "Cena / mapa inteiro")
        list("Exportar modelo 3D (.glb)", items) { which ->
            val id = if (which == 0) snap.selected else -1
            if (which == 0 && id < 0) { toast("Toque num objeto primeiro"); return@list }
            stopPlayIfNeeded()
            toast("Gerando .glb…")
            val name = (if (which == 0) snap.selectedName else projectName).ifBlank { "modelo" }.replace(Regex("[^\\p{L}\\p{N} _-]"), "_")
            gl {
                val f = File(cacheDir, "export.glb")
                if (NativeBridge.nativeExportGlb(id, f.absolutePath)) runOnUiThread {
                    pendingGlb = f
                    startActivityForResult(Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                        addCategory(Intent.CATEGORY_OPENABLE)
                        type = "application/octet-stream"
                        putExtra(Intent.EXTRA_TITLE, "$name.glb")
                    }, REQ_GLB)
                } else toast("Não deu: " + NativeBridge.nativeLastError())
            }
        }
    }

    private fun askText(title: String, initial: String, onOk: (String) -> Unit) {
        val et = EditText(this).apply { setText(initial); setSelectAllOnFocus(true); setTextColor(Color.WHITE); setSingleLine() }
        val box = FrameLayout(this).apply { setPadding(dp(20), dp(8), dp(20), 0); addView(et) }
        dialog().setTitle(title).setView(box)
            .setPositiveButton("OK") { _, _ -> val t = et.text.toString().trim(); if (t.isNotEmpty()) onOk(t) }
            .setNegativeButton("Cancelar", null).show()
    }

    // O renderer lê os shaders por caminho de arquivo: copia os assets (.vert/.frag/.glsl) para o storage interno.
    private fun copyShaders(): String {
        val dir = File(filesDir, "shaders").apply { mkdirs() }
        for (name in assets.list("").orEmpty()) {
            if (!(name.endsWith(".vert") || name.endsWith(".frag") || name.endsWith(".glsl"))) continue
            assets.open(name).use { input -> File(dir, name).outputStream().use { input.copyTo(it) } }
        }
        return dir.absolutePath
    }

    private fun gl(block: () -> Unit) = view.queueEvent(block)
    private fun toast(msg: String) = runOnUiThread { Toast.makeText(this, msg, Toast.LENGTH_SHORT).show() }
    private fun seed() = (System.nanoTime() and 0x7fffffff).toInt()

    // ---------------- barra de ferramentas ----------------
    private fun buildToolbar(): View {
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(dp(6), dp(6), dp(6), dp(6))
            gravity = Gravity.CENTER_VERTICAL
        }
        playBtn = button("▶ Play", accent = true) { togglePlay() }
        bar.addView(playBtn)
        bar.addView(button("📁 Projeto") { projectDialog() })
        bar.addView(separator())
        val labels = listOf("Mover", "Girar", "Escala")
        modeBtns = labels.mapIndexed { i, l -> button(l) { setMode(i) } }
        modeBtns.forEach { bar.addView(it) }
        bar.addView(separator())
        bar.addView(button("+ Criar") { createMenu() })
        bar.addView(button("🎮 Jogo") { gameMenu() })
        bar.addView(button("Mapas") { mapsDialog() })
        bar.addView(button("Objetos") { hierarchyDialog() })
        bar.addView(button("Focar") { gl { NativeBridge.nativeFocus() } })
        bar.addView(button("Excluir") { gl { NativeBridge.nativeDeleteSelected() } })
        bar.addView(separator())
        bar.addView(button("Material") { materialDialog() })
        bar.addView(button("Animação") { animationDialog() })
        bar.addView(button("Importar 3D") { pickFile(REQ_MODEL) })
        bar.addView(button("Visual") {
            visualPanel.visibility = if (visualPanel.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        })
        bar.addView(button("🤖 IA", accent = true) { toggleAi() })
        setMode(0)
        return HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            background = GradientDrawable().apply { setColor(0xCC15181D.toInt()) }
            addView(bar)
        }
    }

    private fun togglePlay() {
        val play = !snap.playing
        snap.playing = play
        stick = floatArrayOf(0f, 0f)
        joystick.reset()
        gl { NativeBridge.nativePlayerInput(0f, 0f, false); NativeBridge.nativeSetPlay(play) }
        lastMsg = ""; msgUntil = 0L
        snap.game = FloatArray(8); snap.objective = ""; snap.message = ""
        if (::endPanel.isInitialized) endPanel.visibility = View.GONE
        playBtn.text = if (play) "■ Stop" else "▶ Play"
        (playBtn.background as GradientDrawable).setColor(if (play) 0xFFC0392B.toInt() else 0xFF2E86DE.toInt())
        updateOverlays()
        if (play && !snap.hasPlayer) toast("Sem jogador: crie um personagem (+ Criar → Personagem) para jogar")
    }

    private fun setMode(m: Int) {
        gl { NativeBridge.nativeSetGizmoMode(m) }
        modeBtns.forEachIndexed { i, b ->
            (b.background as GradientDrawable).setColor(if (i == m) 0xFF3D7EAA.toInt() else 0xFF2A2F37.toInt())
        }
    }

    // ---------------- criar ----------------
    private fun createMenu() {
        val items = arrayOf("Personagem", "Arma", "Efeito (fogo, magia, explosão...)", "Construção", "Natureza e objetos", "Formas básicas (com física)")
        dialog().setTitle("Criar").setItems(items) { _, which ->
            when (which) {
                0 -> characterMenu()
                1 -> list("Arma (vai para a mão do personagem selecionado)",
                          arrayOf("Pistola", "Fuzil", "Escopeta", "Sniper", "Faca", "Taco")) { k ->
                    gl {
                        val id = NativeBridge.nativeCreateWeapon(k)
                        if (id < 0) toast("Não foi possível criar a arma")
                    }
                }
                2 -> list("Efeito", effects) { k ->
                    list("Elemento / cor do efeito", elements) { el -> gl { NativeBridge.nativeCreateEffect(k, el) } }
                }
                3 -> list("Construção", arrayOf("Casa", "Sobrado", "Barraco (favela)", "Prédio de apartamentos", "Arranha-céu", "Prédio escolar", "Muro")) { k ->
                    val kind = if (k == 6) 11 else k
                    gl { NativeBridge.nativeCreateBuilding(kind, seed()) }
                }
                4 -> list("Natureza e objetos", arrayOf("Árvore", "Pinheiro", "Palmeira", "Carro", "Poste")) { k ->
                    gl { NativeBridge.nativeCreateBuilding(6 + k, seed()) }
                }
                else -> list("Formas básicas", arrayOf("Cubo", "Esfera", "Cápsula", "Pedra (collider convexo)")) { k ->
                    gl { NativeBridge.nativeAddObject(k, true) }
                }
            }
        }.show()
    }

    private fun characterMenu() {
        val items = arrayOf("Aleatório", "Personalizar…", "✏ Editar o personagem selecionado…",
                            "Usar modelo importado (.glb/.fbx selecionado) como personagem", "Controlar o personagem selecionado no Play",
                            "Ver / trocar animações…")
        dialog().setTitle("Personagem").setItems(items) { _, which ->
            when (which) {
                0 -> gl { NativeBridge.nativeCreateRandomCharacter(seed(), false) }
                1 -> characterEditor()
                2 -> if (snap.selected < 0) toast("Toque num personagem primeiro") else characterEditor(edit = true)
                5 -> animationDialog()
                3 -> gl {
                    val id = NativeBridge.nativeMakeCharacter(true)
                    val msg = NativeBridge.nativeLastError()
                    toast(if (id < 0) "Não deu: $msg" else if (msg.isNotEmpty()) "Personagem pronto ($msg)" else "Personagem pronto! Aperte ▶ Play")
                }
                else -> gl { NativeBridge.nativeSetPlayer(); toast("Esse personagem é o jogador no Play") }
            }
        }.show()
    }

    // Editor de personagem: altura, corpo, cores, cabelo
    private fun hex(c: Int) = String.format("\"#%06x\"", c and 0xFFFFFF)

    private fun characterEditor(edit: Boolean = false) {
        val skins = intArrayOf(0xFFFAD6B9.toInt(), 0xFFE8B48C.toInt(), 0xFFC68C69.toInt(), 0xFF966242.toInt(), 0xFF64402A.toInt(), 0xFF462E20.toInt())
        val clothes = intArrayOf(0xFFC82828.toInt(), 0xFF286EBE.toInt(), 0xFFF0F0F0.toInt(), 0xFF1E1E23.toInt(), 0xFFFAC828.toInt(),
                                 0xFF289650.toInt(), 0xFFF0781E.toInt(), 0xFF7846A0.toInt(), 0xFF5AA0DC.toInt(), 0xFFE678A0.toInt())
        val pants = intArrayOf(0xFF323746.toInt(), 0xFF283C6E.toInt(), 0xFF1E1E20.toInt(), 0xFF786450.toInt(), 0xFF5A5A5F.toInt(), 0xFF465A3C.toInt())
        val hairs = intArrayOf(0xFF14100E.toInt(), 0xFF3C2819.toInt(), 0xFF785028.toInt(), 0xFFC8AA6E.toInt(), 0xFF963C1E.toInt(), 0xFF6E6E6E.toInt())
        val sel = intArrayOf(skins[2], clothes[1], pants[0], clothes[2], hairs[0])
        var build = 1
        var hairStyle = 1
        var height = 1.75f

        val p = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(8), dp(16), dp(8)) }
        p.addView(slider("Altura (m)", 1.4f, 2.1f, height) { v -> height = v })
        p.addView(choice("Corpo", arrayOf("Magro", "Normal", "Forte"), build) { build = it })
        val names = arrayOf("Pele", "Camisa", "Calça", "Tênis", "Cabelo")
        val palettes = arrayOf(skins, clothes, pants, clothes, hairs)
        for (i in names.indices) p.addView(colorRow(names[i], palettes[i], sel[i]) { sel[i] = it })
        p.addView(choice("Cabelo", arrayOf("Careca", "Curto", "Comprido", "Boné"), hairStyle) { hairStyle = it })
        val sleeves = CheckBox(this).apply { text = "Manga comprida"; setTextColor(Color.WHITE) }
        val control = CheckBox(this).apply { text = "Controlar no Play (jogador)"; setTextColor(Color.WHITE); isChecked = !snap.hasPlayer }
        p.addView(sleeves)
        if (!edit) p.addView(control)

        dialog().setTitle(if (edit) "Editar \"${snap.selectedName}\"" else "Criar personagem")
            .setView(ScrollView(this).apply { addView(p) })
            .setPositiveButton(if (edit) "Aplicar" else "Criar") { _, _ ->
                if (edit) {  // recria com a nova aparência, mantendo posição, arma e comportamentos
                    val id = snap.selected
                    val builds = arrayOf("magro", "normal", "forte")
                    val styles = arrayOf("careca", "curto", "comprido", "bone")
                    val json = "{\"id\":$id,\"aparencia\":{\"altura\":${String.format(Locale.US, "%.2f", height)},\"corpo\":\"${builds[build]}\"," +
                        "\"pele\":${hex(sel[0])},\"camisa\":${hex(sel[1])},\"calca\":${hex(sel[2])},\"tenis\":${hex(sel[3])}," +
                        "\"cabelo\":${hex(sel[4])},\"estilo_cabelo\":\"${styles[hairStyle]}\",\"manga_comprida\":${sleeves.isChecked}}}"
                    gl {
                        val r = NativeBridge.toolCall("editar_objeto", json)
                        val nid = try { JSONObject(r).optInt("id", -1) } catch (e: Exception) { -1 }
                        if (nid >= 0) NativeBridge.nativeSelect(nid) else toast("Não deu: $r")
                    }
                    return@setPositiveButton
                }
                val params = FloatArray(20)
                params[0] = height; params[1] = build.toFloat()
                for (i in 0 until 5) {
                    val c = sel[i]
                    params[2 + i * 3] = Color.red(c) / 255f; params[3 + i * 3] = Color.green(c) / 255f; params[4 + i * 3] = Color.blue(c) / 255f
                }
                params[17] = hairStyle.toFloat()
                params[18] = if (sleeves.isChecked) 1f else 0f
                params[19] = if (control.isChecked) 1f else 0f
                gl { NativeBridge.nativeCreateCharacter(params) }
            }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    private fun mapsDialog() {
        val items = arrayOf("🏙 Cidade", "🏘 Favela no morro", "🏫 Colégio", "🏡 Bairro residencial", "🌍 Mundo infinito (sem fim)",
                            "➕ Criar novo mapa / cena vazia (do zero)", "🔭 Visão geral do mapa atual")
        dialog().setTitle("Mapas").setItems(items) { _, which ->
            if (which == 6) { gl { NativeBridge.nativeMapOverview() }; return@setItems }
            dialog().setTitle("Trocar o cenário?")
                .setMessage("Os objetos atuais deste projeto serão substituídos. (Para guardar o atual, use 📁 Projeto → Duplicar antes.)")
                .setPositiveButton("Trocar") { _, _ ->
                    stopPlayIfNeeded()
                    if (which < 5) toast("Gerando mapa…")
                    gl {
                        if (which < 5) {
                            val kind = if (which == 4) 0 else which + 1
                            NativeBridge.nativeLoadMap(kind, seed())
                            toast("Pronto! A borda vermelha é a barreira invisível. Aperte ▶ Play para jogar.")
                        } else {
                            NativeBridge.toolCall("carregar_mapa", "{\"tipo\":\"vazio\",\"com_jogador\":true}")
                            toast("Cena vazia: use + Criar para montar do zero")
                        }
                    }
                }
                .setNegativeButton("Cancelar", null).show()
        }.show()
    }

    private fun hierarchyDialog() {
        gl {
            val list = NativeBridge.nativeObjectList()
            val selected = NativeBridge.nativeSelected()
            runOnUiThread {
                val rows = list.lineSequence().filter { it.isNotBlank() }.map { it.split('\t') }.toList()
                if (rows.isEmpty()) { toast("Nenhum objeto editável nesta cena"); return@runOnUiThread }
                val ids = rows.map { it[0].toInt() }
                val names = rows.map { r ->
                    val depth = r[1].toInt().coerceAtMost(8)
                    val mark = if (r[0].toInt() == selected) "● " else ""
                    "  ".repeat(depth) + (if (depth > 0) "└ " else "") + mark + r.getOrElse(2) { "" }
                }.toTypedArray()
                dialog().setTitle("Hierarquia (${rows.size})")
                    .setItems(names) { _, which -> gl { NativeBridge.nativeSelect(ids[which]); NativeBridge.nativeFocus() } }
                    .setNegativeButton("Fechar", null)
                    .show()
            }
        }
    }

    // ---------------- material / textura ----------------
    private fun materialDialog() {
        if (snap.selected < 0) { toast("Selecione um objeto primeiro (toque nele)"); return }
        gl {
            val names = NativeBridge.nativeMaterialNames().lineSequence().filter { it.isNotBlank() }.toList()
            runOnUiThread {
                val effectItems = elements.map { "✨ Efeito: $it" }
                val items = (listOf("🎨 Cor…", "🖼 Imagem do celular…") + names + effectItems).toTypedArray()
                dialog().setTitle("Material de \"${snap.selectedName}\"").setItems(items) { _, which ->
                    when {
                        which == 0 -> colorDialog()
                        which == 1 -> pickFile(REQ_IMAGE)
                        which < 2 + names.size -> gl { NativeBridge.nativeSetObjectMaterial(which - 2) }
                        else -> gl { NativeBridge.nativeSetEffectElement(which - 2 - names.size) }
                    }
                }.show()
            }
        }
    }

    private fun colorDialog() {
        val colors = intArrayOf(0xFFFFFFFF.toInt(), 0xFFE53935.toInt(), 0xFFFB8C00.toInt(), 0xFFFDD835.toInt(), 0xFF43A047.toInt(), 0xFF00ACC1.toInt(),
                                0xFF1E88E5.toInt(), 0xFF8E24AA.toInt(), 0xFFD81B60.toInt(), 0xFF6D4C41.toInt(), 0xFF757575.toInt(), 0xFF212121.toInt())
        var chosen = colors[0]
        val p = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(8), dp(16), dp(8)) }
        p.addView(colorRow("Cor", colors, chosen) { chosen = it })
        dialog().setTitle("Cor do objeto").setView(p)
            .setPositiveButton("Aplicar") { _, _ ->
                gl { NativeBridge.nativeSetObjectColor(Color.red(chosen) / 255f, Color.green(chosen) / 255f, Color.blue(chosen) / 255f) }
            }
            .setNegativeButton("Cancelar", null).show()
    }

    private var animPreview = true

    // Lista as animações do objeto selecionado (ou do primeiro animado) e toca a escolhida
    private fun animationDialog() {
        gl {
            val names = NativeBridge.animationList()
            val cur = NativeBridge.nativeAnimationIndex()
            runOnUiThread {
                if (names.isEmpty()) {
                    toast("Nenhuma animação aqui. Crie um personagem (+ Criar → Personagem) ou importe um .glb/.fbx com esqueleto e selecione")
                    return@runOnUiThread
                }
                val items = names.mapIndexed { i, n -> (if (i == cur) "▶ " else "    ") + n }.toMutableList()
                items.add(if (animPreview) "⏸ Pausar animações no editor" else "▶ Tocar animações no editor")
                dialog().setTitle("Animações de \"${snap.selectedName.ifEmpty { "cena" }}\"")
                    .setItems(items.toTypedArray()) { _, which ->
                        if (which == names.size) {
                            animPreview = !animPreview
                            val on = animPreview
                            gl { NativeBridge.nativeSetAnimPreview(on) }
                        } else {
                            gl { NativeBridge.nativeSetAnimation(which) }
                            toast("Tocando: ${names[which]}. No Play, andar/correr trocam sozinhos")
                        }
                    }
                    .setNegativeButton("Fechar", null).show()
            }
        }
    }

    // ---------------- menu Jogo: regras, inimigos, itens, comportamentos ----------------
    private fun gameMenu() {
        val items = arrayOf("🎯 Objetivo e regras…", "👾 Inimigo com pistola", "👾 Inimigo com fuzil", "🔪 Inimigo com faca",
                            "👊 Inimigo desarmado", "🌀 Portal que cria inimigos", "🪙 Moeda", "💎 Cristal (50 pontos)", "❤ Vida (cura)",
                            "🏁 Ponto de chegada", "🔥 Zona de dano (fogo)", "☠ Zona de veneno", "🛢 Barril explosivo",
                            "📦 Caixa quebrável", "⚙ Comportamento do objeto selecionado…")
        dialog().setTitle("Jogo").setItems(items) { _, which ->
            val spec = when (which) {
                1 -> "{\"tipo\":\"inimigo\",\"arma\":\"pistola\"}"
                2 -> "{\"tipo\":\"inimigo\",\"arma\":\"fuzil\",\"dano\":8}"
                3 -> "{\"tipo\":\"inimigo\",\"arma\":\"faca\",\"dano\":15,\"velocidade\":4}"
                4 -> "{\"tipo\":\"inimigo\",\"velocidade\":4}"
                5 -> "{\"tipo\":\"spawner\",\"maximo\":4,\"total\":8,\"arma_inimigo\":\"pistola\"}"
                6 -> "{\"tipo\":\"coletavel\",\"item\":\"moeda\"}"
                7 -> "{\"tipo\":\"coletavel\",\"item\":\"cristal\"}"
                8 -> "{\"tipo\":\"coletavel\",\"item\":\"vida\"}"
                9 -> "{\"tipo\":\"objetivo\"}"
                10 -> "{\"tipo\":\"zona_dano\",\"elemento\":\"fogo\"}"
                11 -> "{\"tipo\":\"zona_dano\",\"elemento\":\"veneno\",\"dano\":10,\"raio\":3}"
                12 -> "{\"tipo\":\"forma\",\"forma\":\"capsula\",\"nome\":\"Barril\",\"cor\":\"vermelho\",\"escala\":[0.7,0.6,0.7],\"comportamentos\":[\"explosivo\"]}"
                13 -> "{\"tipo\":\"forma\",\"forma\":\"cubo\",\"nome\":\"Caixa\",\"material\":\"madeira\",\"escala\":0.8,\"comportamentos\":[\"destrutivel\"]}"
                else -> null
            }
            when {
                which == 0 -> rulesDialog()
                which == 14 -> behaviorDialog()
                spec != null -> gl {
                    if (NativeBridge.addSpec(spec) < 0) toast("Erro: " + NativeBridge.nativeLastError())
                    else toast(items[which].drop(2).trim() + " criado no foco da câmera. Mova com as setas.")
                }
            }
        }.show()
    }

    private fun rulesDialog() {
        val goals = arrayOf("livre", "coletar", "eliminar", "chegar", "sobreviver", "pontos")
        val labels = arrayOf("Livre (sem fim)", "Coletar itens", "Eliminar inimigos", "Chegar ao ponto", "Sobreviver (tempo)", "Fazer pontos")
        var goal = 0
        val p = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(8), dp(16), dp(8)) }
        p.addView(choice("Objetivo", labels, 0) { goal = it })
        fun field(hint: String, value: String, numeric: Boolean = true) = EditText(this).apply {
            this.hint = hint; setText(value); setTextColor(Color.WHITE); setHintTextColor(0x88FFFFFF.toInt()); setSingleLine()
            if (numeric) inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
        }
        val qty = field("Quantidade (0 = todos)", "0")
        val time = field("Tempo limite em segundos (0 = sem)", "0")
        val life = field("Vida do jogador", "100")
        val titleF = field("Título do jogo (opcional)", "", numeric = false)
        p.addView(label("Quantidade (coletar/eliminar/pontos; 0 = todos)")); p.addView(qty)
        p.addView(label("Tempo limite em segundos (0 = sem limite)")); p.addView(time)
        p.addView(label("Vida do jogador")); p.addView(life)
        p.addView(label("Título (aparece ao jogar)")); p.addView(titleF)
        dialog().setTitle("Objetivo e regras").setView(ScrollView(this).apply { addView(p) })
            .setPositiveButton("Salvar") { _, _ ->
                val j = JSONObject()
                j.put("objetivo", goals[goal])
                j.put("quantidade", qty.text.toString().toIntOrNull() ?: 0)
                j.put("tempo", time.text.toString().toDoubleOrNull() ?: 0.0)
                j.put("vida_jogador", life.text.toString().toDoubleOrNull() ?: 100.0)
                j.put("titulo", titleF.text.toString())
                gl { val r = NativeBridge.toolCall("definir_regras", j.toString()); toast(if (r.contains("erro")) r else "Regras salvas. Aperte ▶ Play") }
            }
            .setNegativeButton("Cancelar", null).show()
    }

    private fun behaviorDialog() {
        val id = snap.selected
        if (id < 0) { toast("Toque num objeto primeiro"); return }
        val names = arrayOf("girar", "flutuar", "plataforma", "destrutivel", "explosivo", "inimigo")
        val labels = arrayOf("🔄 Girar", "🎈 Flutuar", "⬆ Plataforma (sobe e desce 3 m)", "📦 Destrutível (quebra com tiros)",
                             "💥 Explosivo", "👾 Inimigo (só personagens)", "🧹 Tirar todos os comportamentos")
        list("Comportamento de \"${snap.selectedName}\"", labels) { k ->
            val input = if (k == names.size) "{\"id\":$id,\"comportamentos\":[]}"
                        else "{\"id\":$id,\"comportamento\":\"${names[k]}\"}"
            val tool = if (k == names.size) "editar_objeto" else "adicionar_comportamento"
            gl {
                val r = NativeBridge.toolCall(tool, input)
                toast(if (r.contains("\"erro\"")) r else "Pronto: vale no ▶ Play")
            }
        }
    }

    // ---------------- HUD de jogo ----------------
    private fun buildHud(): FrameLayout {
        val f = FrameLayout(this)
        hurtOverlay = View(this).apply { setBackgroundColor(0x66FF0000); alpha = 0f; isClickable = false }
        f.addView(hurtOverlay, FrameLayout.LayoutParams(-1, -1))
        gameBar = TextView(this).apply {
            setTextColor(Color.WHITE); typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            setPadding(dp(12), dp(6), dp(12), dp(6))
            background = GradientDrawable().apply { setColor(0x99000000.toInt()); cornerRadius = dp(10).toFloat() }
        }
        f.addView(gameBar, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply { topMargin = dp(56) })
        gameMsg = TextView(this).apply {
            setTextColor(0xFFFFE082.toInt()); typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f); setShadowLayer(4f, 0f, 0f, Color.BLACK); alpha = 0f
        }
        f.addView(gameMsg, FrameLayout.LayoutParams(-2, -2, Gravity.CENTER_HORIZONTAL or Gravity.TOP).apply { topMargin = dp(110) })
        endPanel = panel().apply {
            gravity = Gravity.CENTER_HORIZONTAL
            visibility = View.GONE
            endTitle = label("", bold = true).apply { setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f); gravity = Gravity.CENTER }
            endText = label("").apply { gravity = Gravity.CENTER; setPadding(0, dp(6), 0, dp(12)) }
            addView(endTitle); addView(endText)
            val row = LinearLayout(this@MainActivity).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
            row.addView(button("↻ Jogar de novo", accent = true) { restartGame() })
            row.addView(button("✎ Voltar ao editor") { togglePlay() })
            addView(row)
        }
        f.addView(endPanel, FrameLayout.LayoutParams(dp(320), -2, Gravity.CENTER))
        joystick = JoystickView(this) { x, y ->
            stick = floatArrayOf(x, y)
            val r = running
            gl { NativeBridge.nativePlayerInput(x, y, r) }
        }
        f.addView(joystick, FrameLayout.LayoutParams(dp(150), dp(150), Gravity.BOTTOM or Gravity.START).apply {
            leftMargin = dp(24); bottomMargin = dp(24)
        })
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.END }
        fireBtn = roundButton("Atirar", 0xCCC0392B.toInt())
        // segurar = disparo contínuo (o motor respeita a cadência de cada arma)
        val repeat = object : Runnable {
            override fun run() { gl { NativeBridge.nativePlayerFire() }; ui.postDelayed(this, 90) }
        }
        fireBtn.setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { ui.post(repeat); v.alpha = 0.6f }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { ui.removeCallbacks(repeat); v.alpha = 1f }
            }
            true
        }
        val jump = roundButton("Pular", 0xCC2E86DE.toInt()).apply { setOnClickListener { gl { NativeBridge.nativePlayerJump() } } }
        runBtn = roundButton("Correr", 0xCC555555.toInt()).apply {
            setOnClickListener {
                running = !running
                (background as GradientDrawable).setColor(if (running) 0xCC27AE60.toInt() else 0xCC555555.toInt())
                val s = stick
                val r = running
                gl { NativeBridge.nativePlayerInput(s[0], s[1], r) }
            }
        }
        val magic = roundButton("Magia\n" + elements[spellElement], 0xCC8E44AD.toInt())
        magic.setOnClickListener { gl { NativeBridge.nativePlayerCast(spellElement) } }
        magic.setOnLongClickListener {  // segurar troca o elemento
            spellElement = (spellElement + 1) % elements.size
            magic.text = "Magia\n" + elements[spellElement]
            true
        }
        col.addView(magic); col.addView(fireBtn); col.addView(jump); col.addView(runBtn)
        f.addView(col, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.END).apply { rightMargin = dp(20); bottomMargin = dp(20) })
        return f.apply { isClickable = false }
    }

    private fun restartGame() {
        endPanel.visibility = View.GONE
        stick = floatArrayOf(0f, 0f)
        joystick.reset()
        gl { NativeBridge.nativePlayerInput(0f, 0f, false); NativeBridge.nativeSetPlay(false); NativeBridge.nativeSetPlay(true) }
    }

    // Barra de vida/pontos/tempo, avisos e tela de fim (atualizado pelo refresh)
    private fun updateGameHud() {
        if (!snap.playing) { endPanel.visibility = View.GONE; return }
        val g = snap.game
        val sb = StringBuilder()
        val hp = g[1].toInt(); val max = g[2].toInt().coerceAtLeast(1)
        val hearts = (hp * 10 / max).coerceIn(0, 10)
        sb.append("❤ ").append("█".repeat(hearts)).append("░".repeat(10 - hearts)).append(" $hp")
        sb.append("   ⭐ ").append(g[3].toInt())
        val t = if (g[5] > 0f) (g[5] - g[4]).coerceAtLeast(0f) else g[4]
        sb.append("   ⏱ ").append(String.format(Locale.US, "%d:%02d", t.toInt() / 60, t.toInt() % 60))
        if (snap.objective.isNotEmpty()) sb.append("\n").append(snap.objective)
        gameBar.text = sb.toString()
        hurtOverlay.alpha = if (g[6] > 0f) 0.6f else 0f
        val now = System.currentTimeMillis()
        if (snap.message.isNotEmpty() && snap.message != lastMsg) { lastMsg = snap.message; msgUntil = now + 1800; gameMsg.text = lastMsg }
        gameMsg.alpha = if (now < msgUntil && g[0] == 0f) 1f else 0f
        if (g[0] != 0f && endPanel.visibility != View.VISIBLE) {
            endTitle.text = if (g[0] == 1f) "🏆 Vitória!" else "💀 Fim de jogo"
            endTitle.setTextColor(if (g[0] == 1f) 0xFFFFD54F.toInt() else 0xFFEF5350.toInt())
            endText.text = snap.message + String.format(Locale.US, "\nPontos: %d   Tempo: %d:%02d", g[3].toInt(), g[4].toInt() / 60, g[4].toInt() % 60)
            endPanel.visibility = View.VISIBLE
        } else if (g[0] == 0f) endPanel.visibility = View.GONE
    }

    private fun roundButton(text: String, color: Int) = TextView(this).apply {
        this.text = text
        setTextColor(Color.WHITE)
        typeface = Typeface.DEFAULT_BOLD
        gravity = Gravity.CENTER
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
        background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(color) }
        layoutParams = LinearLayout.LayoutParams(dp(66), dp(66)).apply { topMargin = dp(8) }
    }

    // ---------------- assistente de IA ----------------
    private val prefs by lazy { getSharedPreferences("motor3d", MODE_PRIVATE) }

    private fun buildAiPanel(): LinearLayout {
        val p = panel()
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        header.addView(label("🤖 Assistente", bold = true).apply { layoutParams = LinearLayout.LayoutParams(0, -2, 1f) })
        header.addView(button("⚙") { aiSettings() })
        header.addView(button("🗑") { assistant?.reset(); aiLog.text = ""; aiSay("Conversa nova. O que vamos criar?") })
        header.addView(button("✕") { aiPanel.visibility = View.GONE; hideKeyboard() })
        p.addView(header)
        aiLog = label("").apply { setTextIsSelectable(true); setLineSpacing(0f, 1.1f) }
        aiScroll = ScrollView(this).apply { addView(aiLog) }
        p.addView(aiScroll, LinearLayout.LayoutParams(-1, dp(220)))
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        aiInput = EditText(this).apply {
            hint = "Ex.: faça um jogo de sobrevivência na favela à noite"
            setHintTextColor(0x88FFFFFF.toInt()); setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            maxLines = 4
            layoutParams = LinearLayout.LayoutParams(0, -2, 1f)
        }
        row.addView(aiInput)
        aiSend = button("Enviar", accent = true) { aiSendClicked() }
        row.addView(aiSend)
        p.addView(row)
        return p
    }

    private fun toggleAi() {
        if (aiPanel.visibility == View.VISIBLE) { aiPanel.visibility = View.GONE; hideKeyboard(); return }
        if (!aiConfigured()) { aiSettings(openAfter = true); return }
        aiPanel.visibility = View.VISIBLE
        if (aiLog.text.isEmpty()) aiSay("Oi! Diga o jogo que você quer e eu monto aqui no editor. Ex.: \"cidade à noite com 5 inimigos de pistola e 10 moedas para coletar\".")
    }

    private fun aiConfigured() = if (prefs.getInt("provedor", 0) == 0) !prefs.getString("api_key", "").isNullOrBlank()
                                 else !prefs.getString("outra_url", "").isNullOrBlank() && !prefs.getString("outra_modelo", "").isNullOrBlank()

    private fun newAgent(): GameAgent =
        if (prefs.getInt("provedor", 0) == 0)
            Assistant(prefs.getString("api_key", "") ?: "", prefs.getString("modelo", Assistant.DEFAULT_MODEL) ?: Assistant.DEFAULT_MODEL, ::runToolOnGl)
        else
            OpenAiAgent(prefs.getString("outra_url", "") ?: "", prefs.getString("outra_key", "") ?: "",
                        prefs.getString("outra_modelo", "") ?: "", ::runToolOnGl)

    // Provedor: Claude (Anthropic) ou qualquer API no padrão OpenAI (OpenAI, Gemini, DeepSeek, Groq, OpenRouter...)
    private fun aiSettings(openAfter: Boolean = false) {
        val p = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(8), dp(20), dp(8)) }
        fun field(value: String?, hint: String, secret: Boolean = false) = EditText(this).apply {
            setText(value ?: ""); setTextColor(Color.WHITE); setHintTextColor(0x88FFFFFF.toInt()); setSingleLine(); this.hint = hint
            if (secret) inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        var provider = prefs.getInt("provedor", 0)
        val claudeBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val otherBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        p.addView(choice("Qual IA", arrayOf("Claude (Anthropic)", "Outra IA (padrão OpenAI)"), provider) {
            provider = it
            claudeBox.visibility = if (it == 0) View.VISIBLE else View.GONE
            otherBox.visibility = if (it == 1) View.VISIBLE else View.GONE
        })
        // Claude
        claudeBox.addView(label("Chave da API (console.anthropic.com → API Keys)"))
        val key = field(prefs.getString("api_key", ""), "sk-ant-...", secret = true)
        claudeBox.addView(key)
        val models = Assistant.MODELS
        var model = models.indexOf(prefs.getString("modelo", Assistant.DEFAULT_MODEL)).coerceAtLeast(0)
        claudeBox.addView(choice("Modelo", arrayOf("Opus 5.5 (melhor)", "Sonnet 5.5", "Haiku 5.5 (rápido)"), model) { model = it })
        // Outra IA
        val url = field(prefs.getString("outra_url", ""), "https://.../v1")
        val okey = field(prefs.getString("outra_key", ""), "chave da API", secret = true)
        val omodel = field(prefs.getString("outra_modelo", ""), "nome do modelo (veja na sua conta)")
        otherBox.addView(choice("Atalhos de endereço", OpenAiAgent.PRESETS.map { it.first }.toTypedArray(), -1) { url.setText(OpenAiAgent.PRESETS[it].second) })
        otherBox.addView(label("Endereço da API")); otherBox.addView(url)
        otherBox.addView(label("Chave da API")); otherBox.addView(okey)
        otherBox.addView(label("Modelo (precisa aceitar ferramentas / function calling)")); otherBox.addView(omodel)
        claudeBox.visibility = if (provider == 0) View.VISIBLE else View.GONE
        otherBox.visibility = if (provider == 1) View.VISIBLE else View.GONE
        p.addView(claudeBox); p.addView(otherBox)
        p.addView(label("As chaves ficam salvas só neste celular.").apply { alpha = 0.7f })
        dialog().setTitle("Assistente de IA").setView(ScrollView(this).apply { addView(p) })
            .setPositiveButton("Salvar") { _, _ ->
                prefs.edit().putInt("provedor", provider)
                    .putString("api_key", key.text.toString().trim()).putString("modelo", models[model])
                    .putString("outra_url", url.text.toString().trim()).putString("outra_key", okey.text.toString().trim())
                    .putString("outra_modelo", omodel.text.toString().trim()).apply()
                assistant = null  // recria com a configuração nova (a conversa recomeça)
                if (openAfter && aiConfigured()) toggleAi()
            }
            .setNegativeButton("Cancelar", null).show()
    }

    private fun aiSay(text: String) = runOnUiThread {
        if (aiLog.text.isNotEmpty()) aiLog.append("\n\n")
        aiLog.append(text)
        aiScroll.post { aiScroll.fullScroll(View.FOCUS_DOWN) }
    }

    // Ferramenta do motor na thread GL (o assistente roda numa thread de rede e espera a resposta)
    private fun runToolOnGl(name: String, input: String): String {
        var out = "{\"erro\":\"tempo esgotado\"}"
        val latch = CountDownLatch(1)
        view.queueEvent { try { out = NativeBridge.toolCall(name, input) } finally { latch.countDown() } }
        latch.await(60, TimeUnit.SECONDS)
        return out
    }

    private fun aiSendClicked() {
        if (aiBusy) { assistant?.cancelled = true; aiSay("(parando…)"); return }
        val text = aiInput.text.toString().trim()
        if (text.isEmpty()) return
        if (!aiConfigured()) { aiSettings(); return }
        stopPlayIfNeeded()
        aiInput.setText("")
        hideKeyboard()
        aiSay("🧑 $text")
        val a = assistant ?: newAgent().also { assistant = it }
        aiBusy = true
        aiSend.text = "Parar"
        Thread {
            try {
                a.send(text) { msg -> aiSay(if (msg.startsWith("🔧")) msg else "🤖 $msg") }
            } catch (e: Exception) {
                val m = e.message ?: e.toString()
                aiSay("⚠ Erro: " + when {
                    m.contains("401") || m.contains("authentication", true) -> "chave da API inválida (⚙ para trocar)"
                    m.contains("429") -> "limite de uso atingido; tente em instantes"
                    m.contains("credit", true) || m.contains("billing", true) -> "sem créditos na conta da API"
                    m.contains("Unable to resolve host") || m.contains("timeout", true) -> "sem internet"
                    else -> m.take(300)
                })
            } finally {
                aiBusy = false
                runOnUiThread { aiSend.text = "Enviar" }
                view.queueEvent { saveIfChanged() }
            }
        }.start()
    }

    private fun hideKeyboard() =
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(view.windowToken, 0)

    // ---------------- painel visual ----------------
    private fun buildVisualPanel(): LinearLayout {
        val p = panel()
        p.addView(label("Visual", bold = true))
        p.addView(slider("Exposição", 0.1f, 3f, 1f) { v -> gl { NativeBridge.nativeSetExposure(v) } })
        p.addView(slider("Hora do dia", 0f, 24f, 13f) { v -> gl { NativeBridge.nativeSetTimeOfDay(v) } })
        p.addView(slider("Sol", 0f, 8f, 3.2f) { v -> gl { NativeBridge.nativeSetSunIntensity(v) } })
        p.addView(CheckBox(this).apply {
            text = "Grade do editor"
            setTextColor(Color.WHITE)
            isChecked = true
            setOnCheckedChangeListener { _, c -> gl { NativeBridge.nativeSetGrid(c) } }
        })
        return p
    }

    private fun slider(name: String, min: Float, max: Float, initial: Float, onChange: (Float) -> Unit): View {
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val lbl = label(String.format(Locale.US, "%s: %.2f", name, initial))
        val bar = SeekBar(this).apply {
            this.max = 1000
            progress = ((initial - min) / (max - min) * 1000).toInt()
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar?, p: Int, fromUser: Boolean) {
                    val v = min + (max - min) * p / 1000f
                    lbl.text = String.format(Locale.US, "%s: %.2f", name, v)
                    if (fromUser) onChange(v)
                }
                override fun onStartTrackingTouch(s: SeekBar?) {}
                override fun onStopTrackingTouch(s: SeekBar?) {}
            })
        }
        col.addView(lbl)
        col.addView(bar)
        return col
    }

    private fun choice(name: String, options: Array<String>, initial: Int, onPick: (Int) -> Unit): View {
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(0, dp(6), 0, dp(6)) }
        col.addView(label(name))
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val btns = ArrayList<TextView>()
        options.forEachIndexed { i, o ->
            val b = button(o) {
                onPick(i)
                btns.forEachIndexed { j, bb -> (bb.background as GradientDrawable).setColor(if (j == i) 0xFF3D7EAA.toInt() else 0xFF2A2F37.toInt()) }
            }
            (b.background as GradientDrawable).setColor(if (i == initial) 0xFF3D7EAA.toInt() else 0xFF2A2F37.toInt())
            btns.add(b); row.addView(b)
        }
        col.addView(HorizontalScrollView(this).apply { addView(row) })
        return col
    }

    private fun colorRow(name: String, colors: IntArray, initial: Int, onPick: (Int) -> Unit): View {
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(0, dp(6), 0, dp(6)) }
        col.addView(label(name))
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val chips = ArrayList<View>()
        fun paint(v: View, c: Int, on: Boolean) {
            v.background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL; setColor(c)
                setStroke(dp(if (on) 3 else 1), if (on) Color.WHITE else 0x66FFFFFF)
            }
        }
        for (c in colors) {
            val v = View(this)
            paint(v, c, c == initial)
            v.setOnClickListener {
                onPick(c)
                chips.forEachIndexed { j, ch -> paint(ch, colors[j], colors[j] == c) }
            }
            chips.add(v)
            row.addView(v, LinearLayout.LayoutParams(dp(34), dp(34)).apply { setMargins(dp(3), dp(3), dp(3), dp(3)) })
        }
        col.addView(HorizontalScrollView(this).apply { addView(row) })
        return col
    }

    // ---------------- inspetor ----------------
    private fun buildInspector(): LinearLayout {
        val p = panel()
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        inspTitle = label("", bold = true).apply { layoutParams = LinearLayout.LayoutParams(0, -2, 1f) }
        header.addView(inspTitle)
        header.addView(button("Aplicar") { applyTransform() })
        p.addView(header)
        val all = ArrayList<EditText>()
        for ((row, name) in listOf("Posição", "Rotação", "Escala").withIndex()) {
            val line = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            line.addView(label(name).apply { layoutParams = LinearLayout.LayoutParams(dp(64), -2) })
            for (axis in 0 until 3) {
                val et = EditText(this).apply {
                    setTextColor(Color.WHITE)
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                    inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
                    imeOptions = EditorInfo.IME_ACTION_DONE
                    setSingleLine()
                    setPadding(dp(6), dp(4), dp(6), dp(4))
                    background = GradientDrawable().apply {
                        setColor(0xFF22262C.toInt()); cornerRadius = dp(6).toFloat()
                        setStroke(dp(1), listOf(0xFFE74C3C, 0xFF2ECC71, 0xFF3498DB)[axis].toInt())
                    }
                    setOnEditorActionListener { _, _, _ -> applyTransform(); true }
                    layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(dp(3), dp(2), dp(3), dp(2)) }
                }
                all.add(et)
                line.addView(et)
            }
            p.addView(line)
            if (row == 2) p.addView(label("Toque num objeto para selecionar. Arraste as setas ou digite os valores.").apply {
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f); alpha = 0.7f
            })
        }
        fields = all
        return p
    }

    private fun applyTransform() {
        val id = snap.selected
        if (id < 0) return
        val v = FloatArray(9) { i -> fields[i].text.toString().replace(',', '.').toFloatOrNull() ?: snap.transform[i] }
        gl { NativeBridge.nativeSetTransform(id, v) }
        fields.forEach { it.clearFocus() }
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(inspector.windowToken, 0)
    }

    private fun updateOverlays() {
        inspector.visibility = if (snap.selected >= 0 && !snap.playing) View.VISIBLE else View.GONE
        hud.visibility = if (snap.playing) View.VISIBLE else View.GONE
        val controls = if (snap.hasPlayer) View.VISIBLE else View.GONE
        if (::joystick.isInitialized) { joystick.visibility = controls; (fireBtn.parent as View).visibility = controls }
        if (::fireBtn.isInitialized) fireBtn.text = if (snap.playerWeapon in 0..3) "Atirar" else "Bater"
    }

    // Atualiza estatísticas e inspetor a partir do snapshot da thread GL
    private val refresh = object : Runnable {
        override fun run() {
            val s = snap.stats
            if (snap.ready) {
                statsText.text = String.format(Locale.US, "%.0f FPS  %.1f ms (CPU %.1f)\n%d draws  %s tris  %d objetos",
                    s[0], s[1], s[2], s[3].toInt(), compact(s[4]), s[5].toInt())
            }
            updateOverlays()
            updateGameHud()
            if (inspector.visibility == View.VISIBLE) {
                if (shownSelection != snap.selected) { inspTitle.text = snap.selectedName; shownSelection = snap.selected }
                val editing = fields.any { it.hasFocus() }
                if (!editing) {
                    val t = snap.transform
                    for (i in 0 until 9) {
                        val txt = String.format(Locale.US, if (i in 3..5) "%.1f" else "%.3f", t[i])
                        if (fields[i].text.toString() != txt) fields[i].setText(txt)
                    }
                }
            }
            ui.postDelayed(this, if (snap.playing) 150 else 250)
        }
    }

    private fun compact(v: Float) = when {
        v >= 1e6f -> String.format(Locale.US, "%.1fM", v / 1e6f)
        v >= 1e3f -> String.format(Locale.US, "%.1fk", v / 1e3f)
        else -> v.toInt().toString()
    }

    // ---------------- arquivos do aparelho (Storage Access Framework) ----------------
    private fun pickFile(req: Int) {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = if (req == REQ_IMAGE) "image/*" else "*/*"  // .glb não tem MIME confiável em todos os aparelhos
        }
        startActivityForResult(i, req)
    }

    @Deprecated("Activity API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        if (requestCode == REQ_GLB) {
            val f = pendingGlb ?: return
            Thread {
                try {
                    contentResolver.openOutputStream(uri)!!.use { out -> f.inputStream().use { it.copyTo(out) } }
                    toast(String.format(Locale.US, "Modelo salvo (%.1f MB)", f.length() / 1048576.0))
                } catch (e: Exception) { toast("Erro ao salvar: ${e.message}") }
            }.start()
            return
        }
        if (requestCode == REQ_EXPORT) {
            gl {
                val json = NativeBridge.exportProject()
                Thread {
                    try {
                        contentResolver.openOutputStream(uri)!!.use { it.write(json.toByteArray(Charsets.UTF_8)) }
                        toast("Projeto exportado")
                    } catch (e: Exception) { toast("Erro ao exportar: ${e.message}") }
                }.start()
            }
            return
        }
        if (requestCode == REQ_PROJECT) {
            Thread {
                try {
                    val json = contentResolver.openInputStream(uri)!!.bufferedReader().readText()
                    runOnUiThread { importAsNewProject(json, displayName(uri).removeSuffix(".json")) }
                } catch (e: Exception) { toast("Erro ao ler: ${e.message}") }
            }.start()
            return
        }
        val name = displayName(uri)
        val lower = name.lowercase(Locale.US)
        if (requestCode == REQ_MODEL && !lower.endsWith(".glb") && !lower.endsWith(".gltf") && !lower.endsWith(".fbx")) {
            toast("Escolha um arquivo .glb, .gltf ou .fbx")
            return
        }
        toast("Carregando $name…")
        Thread {
            try {
                val dir = File(filesDir, if (requestCode == REQ_IMAGE) "textures" else "models").apply { mkdirs() }
                val dst = File(dir, name.replace('/', '_'))
                contentResolver.openInputStream(uri)!!.use { input -> dst.outputStream().use { input.copyTo(it) } }
                gl {
                    if (requestCode == REQ_IMAGE) {
                        if (NativeBridge.nativeSetObjectTexture(dst.absolutePath)) toast("Textura aplicada")
                        else toast("Erro: " + NativeBridge.nativeLastError())
                    } else {
                        val id = NativeBridge.nativeLoadModel(dst.absolutePath)
                        if (id < 0) toast("Erro ao carregar: " + NativeBridge.nativeLastError())
                        else toast("$name carregado. Se for personagem: + Criar → Personagem → Usar modelo importado")
                    }
                }
            } catch (e: Exception) {
                toast("Erro ao copiar arquivo: ${e.message}")
            }
        }.start()
    }

    private fun displayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0)?.let { return it }
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "arquivo"
    }

    // ---------------- helpers de UI ----------------
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun dialog() = AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)

    private fun list(title: String, items: Array<String>, onPick: (Int) -> Unit) {
        dialog().setTitle(title).setItems(items) { _, which -> onPick(which) }.show()
    }

    private fun button(text: String, accent: Boolean = false, onClick: () -> Unit) = TextView(this).apply {
        this.text = text
        setTextColor(Color.WHITE)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
        typeface = Typeface.DEFAULT_BOLD
        gravity = Gravity.CENTER
        minWidth = dp(48)
        setPadding(dp(12), dp(8), dp(12), dp(8))
        background = GradientDrawable().apply {
            setColor(if (accent) 0xFF2E86DE.toInt() else 0xFF2A2F37.toInt())
            cornerRadius = dp(8).toFloat()
        }
        layoutParams = LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(3), 0, dp(3), 0) }
        setOnClickListener { onClick() }
    }

    private fun separator() = View(this).apply {
        setBackgroundColor(0x33FFFFFF)
        layoutParams = LinearLayout.LayoutParams(dp(1), dp(24)).apply { setMargins(dp(4), 0, dp(4), 0) }
    }

    private fun label(text: String, bold: Boolean = false) = TextView(this).apply {
        this.text = text
        setTextColor(Color.WHITE)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, if (bold) 15f else 12f)
        if (bold) typeface = Typeface.DEFAULT_BOLD
    }

    private fun panel() = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(12), dp(10), dp(12), dp(10))
        isClickable = true  // não deixa o toque atravessar para a cena 3D
        background = GradientDrawable().apply { setColor(0xE015181D.toInt()); cornerRadius = dp(12).toFloat() }
        layoutParams = ViewGroup.LayoutParams(-1, -2)
    }

    companion object {
        private const val REQ_MODEL = 42
        private const val REQ_IMAGE = 43
        private const val REQ_PROJECT = 44
        private const val REQ_EXPORT = 45
        private const val REQ_GLB = 46
        private const val AUTOSAVE_MS = 30_000L
    }
}

class EngineView(activity: Activity, private val shaderDir: String) : GLSurfaceView(activity) {
    val snapshot = EngineSnapshot()
    var onReady: (() -> Unit)? = null  // chamado na thread GL quando o motor é (re)criado

    init {
        setEGLContextClientVersion(3)
        setEGLConfigChooser(8, 8, 8, 8, 24, 0)
        preserveEGLContextOnPause = true  // evita recriar tudo ao voltar do segundo plano
        setRenderer(object : Renderer {
            private var last = 0L
            private var frame = 0
            private val stats = FloatArray(6)
            private val tr = FloatArray(9)
            private var lastSel = -2

            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                // Contexto novo (1ª vez ou contexto perdido): recria o motor e a cena
                if (NativeBridge.nativeCreate(shaderDir)) {
                    snapshot.ready = true
                    onReady?.invoke() ?: NativeBridge.nativeNewScene()
                } else {
                    post { Toast.makeText(context, "Falha ao iniciar o motor (shaders). Veja o logcat: Mobile3D", Toast.LENGTH_LONG).show() }
                }
                snapshot.playing = false
                lastSel = -2
                last = System.nanoTime()
            }
            override fun onSurfaceChanged(gl: GL10?, w: Int, h: Int) = NativeBridge.nativeResize(w, h)
            override fun onDrawFrame(gl: GL10?) {
                val now = System.nanoTime()
                val dt = ((now - last) / 1e9f).coerceIn(0f, 0.1f)
                last = now
                NativeBridge.nativeFrame(dt)
                if (++frame % 15 == 0) publish()
            }

            private fun publish() {
                NativeBridge.nativeGetStats(stats)
                snapshot.stats = stats.copyOf()
                val sel = NativeBridge.nativeSelected()
                if (sel != lastSel) { snapshot.selectedName = if (sel >= 0) NativeBridge.nativeObjectName(sel) else ""; lastSel = sel }
                if (sel >= 0 && NativeBridge.nativeGetTransform(sel, tr)) snapshot.transform = tr.copyOf()
                snapshot.selected = sel
                snapshot.hasPlayer = NativeBridge.nativeHasPlayer()
                snapshot.playerWeapon = NativeBridge.nativePlayerWeapon()
                if (snapshot.playing) {
                    val g = FloatArray(8)
                    NativeBridge.nativeGameStatus(g)
                    snapshot.game = g
                    snapshot.objective = NativeBridge.gameText(0)
                    snapshot.message = NativeBridge.gameText(1)
                }
            }
        })
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    // 1 dedo: orbitar, arrastar o gizmo ou tocar para selecionar | 2 dedos: pan + pinça
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val action = when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> 0
            MotionEvent.ACTION_MOVE -> 1
            else -> 2
        }
        val skip = if (e.actionMasked == MotionEvent.ACTION_POINTER_UP) e.actionIndex else -1
        val ids = ArrayList<Int>()
        val xy = ArrayList<Float>()
        for (i in 0 until minOf(e.pointerCount, 3)) {
            if (i == skip) continue
            ids.add(e.getPointerId(i)); xy.add(e.getX(i)); xy.add(e.getY(i))
        }
        val idArr = ids.toIntArray()
        val xyArr = xy.toFloatArray()
        queueEvent { NativeBridge.nativeTouch(action, idArr, xyArr) }
        return true
    }
}
