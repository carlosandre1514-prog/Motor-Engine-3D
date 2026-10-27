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
import java.util.Locale
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
    private var stick = floatArrayOf(0f, 0f)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        view = EngineView(this, copyShaders())

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

        setContentView(root)
        ui.post(refresh)
    }

    override fun onPause() { super.onPause(); view.onPause() }
    override fun onResume() { super.onResume(); view.onResume() }
    override fun onDestroy() { ui.removeCallbacksAndMessages(null); super.onDestroy() }

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
        bar.addView(separator())
        val labels = listOf("Mover", "Girar", "Escala")
        modeBtns = labels.mapIndexed { i, l -> button(l) { setMode(i) } }
        modeBtns.forEach { bar.addView(it) }
        bar.addView(separator())
        bar.addView(button("+ Criar") { createMenu() })
        bar.addView(button("Mapas") { mapsDialog() })
        bar.addView(button("Objetos") { hierarchyDialog() })
        bar.addView(button("Focar") { gl { NativeBridge.nativeFocus() } })
        bar.addView(button("Excluir") { gl { NativeBridge.nativeDeleteSelected() } })
        bar.addView(separator())
        bar.addView(button("Material") { materialDialog() })
        bar.addView(button("Animação") { cycleAnimation() })
        bar.addView(button("Abrir .glb") { pickFile(REQ_MODEL) })
        bar.addView(button("Visual") {
            visualPanel.visibility = if (visualPanel.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        })
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
        val items = arrayOf("Personagem", "Arma", "Construção", "Natureza e objetos", "Formas básicas (com física)")
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
                2 -> list("Construção", arrayOf("Casa", "Sobrado", "Barraco (favela)", "Prédio de apartamentos", "Arranha-céu", "Prédio escolar", "Muro")) { k ->
                    val kind = if (k == 6) 11 else k
                    gl { NativeBridge.nativeCreateBuilding(kind, seed()) }
                }
                3 -> list("Natureza e objetos", arrayOf("Árvore", "Pinheiro", "Palmeira", "Carro", "Poste")) { k ->
                    gl { NativeBridge.nativeCreateBuilding(6 + k, seed()) }
                }
                else -> list("Formas básicas", arrayOf("Cubo", "Esfera", "Cápsula", "Pedra (collider convexo)")) { k ->
                    gl { NativeBridge.nativeAddObject(k, true) }
                }
            }
        }.show()
    }

    private fun characterMenu() {
        val items = arrayOf("Aleatório", "Personalizar…", "Controlar o personagem selecionado no Play")
        dialog().setTitle("Personagem").setItems(items) { _, which ->
            when (which) {
                0 -> gl { NativeBridge.nativeCreateRandomCharacter(seed(), false) }
                1 -> characterEditor()
                else -> gl { NativeBridge.nativeSetPlayer(); toast("Esse personagem é o jogador no Play") }
            }
        }.show()
    }

    // Editor de personagem: altura, corpo, cores, cabelo
    private fun characterEditor() {
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
        p.addView(sleeves); p.addView(control)

        dialog().setTitle("Criar personagem")
            .setView(ScrollView(this).apply { addView(p) })
            .setPositiveButton("Criar") { _, _ ->
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
        val items = arrayOf("Mundo infinito (procedural, sem fim)", "Cidade (com limite)", "Favela no morro (com limite)",
                            "Colégio (com limite)", "Bairro residencial (com limite)", "Vitrine de materiais", "Física: pilha de objetos",
                            "Cena vazia", "Visão geral do mapa atual")
        dialog().setTitle("Mapas e cenas").setItems(items) { _, which ->
            if (which < 8 && snap.playing) togglePlay()
            if (which < 5) toast("Gerando mapa…")
            gl {
                when (which) {
                    in 0..4 -> NativeBridge.nativeLoadMap(which, seed())
                    5 -> NativeBridge.nativeLoadDemo()
                    6 -> NativeBridge.nativeLoadPhysicsDemo()
                    7 -> NativeBridge.nativeNewScene()
                    else -> NativeBridge.nativeMapOverview()
                }
                if (which in 0..4) toast("Pronto! Aperte ▶ Play para jogar com o personagem. A borda vermelha é a barreira invisível.")
            }
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
                val items = (listOf("🎨 Cor…", "🖼 Imagem do celular…") + names).toTypedArray()
                dialog().setTitle("Material de \"${snap.selectedName}\"").setItems(items) { _, which ->
                    when (which) {
                        0 -> colorDialog()
                        1 -> pickFile(REQ_IMAGE)
                        else -> gl { NativeBridge.nativeSetObjectMaterial(which - 2) }
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

    private fun cycleAnimation() {
        gl {
            val name = NativeBridge.nativeCycleAnimation()
            toast(if (name.isEmpty()) "Nenhuma animação (crie um personagem ou carregue um .glb com esqueleto)" else "Animação: $name")
        }
    }

    // ---------------- HUD de jogo ----------------
    private fun buildHud(): FrameLayout {
        val f = FrameLayout(this)
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
        col.addView(fireBtn); col.addView(jump); col.addView(runBtn)
        f.addView(col, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.END).apply { rightMargin = dp(20); bottomMargin = dp(20) })
        return f.apply { isClickable = false }
    }

    private fun roundButton(text: String, color: Int) = TextView(this).apply {
        this.text = text
        setTextColor(Color.WHITE)
        typeface = Typeface.DEFAULT_BOLD
        gravity = Gravity.CENTER
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
        background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(color) }
        layoutParams = LinearLayout.LayoutParams(dp(78), dp(78)).apply { topMargin = dp(10) }
    }

    // ---------------- painel visual ----------------
    private fun buildVisualPanel(): LinearLayout {
        val p = panel()
        p.addView(label("Visual", bold = true))
        p.addView(slider("Exposição", 0.1f, 3f, 1f) { v -> gl { NativeBridge.nativeSetExposure(v) } })
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
        hud.visibility = if (snap.playing && snap.hasPlayer) View.VISIBLE else View.GONE
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
            ui.postDelayed(this, 250)
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
        val name = displayName(uri)
        val lower = name.lowercase(Locale.US)
        if (requestCode == REQ_MODEL && !lower.endsWith(".glb") && !lower.endsWith(".gltf")) {
            toast("Escolha um arquivo .glb (ou .gltf embutido)")
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
                        if (id < 0) toast("Erro ao carregar: " + NativeBridge.nativeLastError()) else toast("$name carregado")
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
    }
}

class EngineView(activity: Activity, private val shaderDir: String) : GLSurfaceView(activity) {
    val snapshot = EngineSnapshot()

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
                    NativeBridge.nativeLoadDemo()
                    snapshot.ready = true
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
