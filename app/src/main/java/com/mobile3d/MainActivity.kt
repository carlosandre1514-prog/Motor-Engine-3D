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
    private var shownSelection = -2

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
        bar.addView(button("+ Objeto") { addObjectDialog() })
        bar.addView(button("Objetos") { hierarchyDialog() })
        bar.addView(button("Focar") { gl { NativeBridge.nativeFocus() } })
        bar.addView(button("Excluir") { gl { NativeBridge.nativeDeleteSelected() } })
        bar.addView(separator())
        bar.addView(button("Abrir .glb") { pickModel() })
        bar.addView(button("Cenas") { scenesDialog() })
        bar.addView(button("Animação") { cycleAnimation() })
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
        gl { NativeBridge.nativeSetPlay(play) }
        playBtn.text = if (play) "■ Stop" else "▶ Play"
        (playBtn.background as GradientDrawable).setColor(if (play) 0xFFC0392B.toInt() else 0xFF2E86DE.toInt())
        updateInspectorVisibility()
    }

    private fun setMode(m: Int) {
        gl { NativeBridge.nativeSetGizmoMode(m) }
        modeBtns.forEachIndexed { i, b ->
            (b.background as GradientDrawable).setColor(if (i == m) 0xFF3D7EAA.toInt() else 0xFF2A2F37.toInt())
        }
    }

    private fun addObjectDialog() {
        val items = arrayOf("Cubo", "Esfera", "Cápsula", "Pedra (collider convexo)")
        dialog().setTitle("Adicionar objeto (com física)")
            .setItems(items) { _, which -> gl { NativeBridge.nativeAddObject(which, true) } }
            .show()
    }

    private fun scenesDialog() {
        val items = arrayOf("Vitrine de materiais (PBR)", "Física: pilha e chuva de objetos", "Cena vazia")
        dialog().setTitle("Carregar cena")
            .setItems(items) { _, which ->
                if (snap.playing) togglePlay()
                gl {
                    when (which) {
                        0 -> NativeBridge.nativeLoadDemo()
                        1 -> NativeBridge.nativeLoadPhysicsDemo()
                        else -> NativeBridge.nativeNewScene()
                    }
                }
                if (which == 1) toast("Aperte ▶ Play para soltar. No Play, toque nos objetos para empurrar.")
            }
            .show()
    }

    private fun hierarchyDialog() {
        gl {
            val list = NativeBridge.nativeObjectList()
            val selected = NativeBridge.nativeSelected()
            runOnUiThread {
                val rows = list.lineSequence().filter { it.isNotBlank() }.map { it.split('\t') }.toList()
                if (rows.isEmpty()) { toast("Cena vazia"); return@runOnUiThread }
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

    private fun cycleAnimation() {
        gl {
            val name = NativeBridge.nativeCycleAnimation()
            toast(if (name.isEmpty()) "Nenhuma animação (carregue um .glb com esqueleto, ex.: Fox)" else "Animação: $name (aperte ▶ Play)")
        }
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
            if (row == 2) p.addView(label("Arraste as setas na tela ou digite os valores. Toque num objeto para selecionar.").apply {
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

    private fun updateInspectorVisibility() {
        inspector.visibility = if (snap.selected >= 0 && !snap.playing) View.VISIBLE else View.GONE
    }

    // Atualiza estatísticas e inspetor a partir do snapshot da thread GL
    private val refresh = object : Runnable {
        override fun run() {
            val s = snap.stats
            if (snap.ready) {
                statsText.text = String.format(Locale.US, "%.0f FPS  %.1f ms (CPU %.1f)\n%d draws  %s tris  %d objetos",
                    s[0], s[1], s[2], s[3].toInt(), compact(s[4]), s[5].toInt())
            }
            updateInspectorVisibility()
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

    // ---------------- abrir .glb (Storage Access Framework) ----------------
    private fun pickModel() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"  // .glb não tem MIME confiável em todos os aparelhos
        }
        startActivityForResult(i, REQ_PICK)
    }

    @Deprecated("Activity API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_PICK || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        val name = displayName(uri)
        if (!name.lowercase(Locale.US).endsWith(".glb") && !name.lowercase(Locale.US).endsWith(".gltf")) {
            toast("Escolha um arquivo .glb (ou .gltf embutido)")
            return
        }
        toast("Carregando $name…")
        Thread {
            try {
                val dir = File(filesDir, "models").apply { mkdirs() }
                val dst = File(dir, name.replace('/', '_'))
                contentResolver.openInputStream(uri)!!.use { input -> dst.outputStream().use { input.copyTo(it) } }
                gl {
                    val id = NativeBridge.nativeLoadModel(dst.absolutePath)
                    if (id < 0) toast("Erro ao carregar: " + NativeBridge.nativeLastError())
                    else toast("$name carregado")
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
        return uri.lastPathSegment?.substringAfterLast('/') ?: "modelo.glb"
    }

    // ---------------- helpers de UI ----------------
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun dialog() = AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)

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

    companion object { private const val REQ_PICK = 42 }
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
