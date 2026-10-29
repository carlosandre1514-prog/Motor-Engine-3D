package com.mobile3d

import android.app.Activity
import android.opengl.GLSurfaceView
import android.view.MotionEvent
import android.widget.Toast
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
    @Volatile var interact = ""
    @Volatile var region = ""
    @Volatile var teamMode = false
}

class EngineView(activity: Activity, private val shaderDir: String) : GLSurfaceView(activity) {
    var touchToEngine = true  // jogo: o toque vira olhar pelos controles da tela, não vai direto ao motor
    var afterFrame: ((Int) -> Unit)? = null
    val snapshot = EngineSnapshot()
    var onReady: (() -> Unit)? = null  // chamado na thread GL quando o motor é (re)criado
    // Qualidade: resolução do desenho (fração da tela) e limite de quadros (30 = esquenta menos e não engasga)
    @Volatile var renderScale = 1f
    @Volatile var maxFps = 60
    @Volatile var quality = 2

    fun applyQuality(scale: Float, fps: Int, q: Int) {
        renderScale = scale; maxFps = fps; quality = q
        if (width > 0 && height > 0) holder.setFixedSize((width * scale).toInt().coerceAtLeast(1), (height * scale).toInt().coerceAtLeast(1))
        queueEvent { NativeBridge.nativeSetQuality(q) }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (renderScale < 1f) post { holder.setFixedSize((w * renderScale).toInt().coerceAtLeast(1), (h * renderScale).toInt().coerceAtLeast(1)) }
    }

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
                    NativeBridge.nativeSetQuality(quality)
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
                if (maxFps < 60) {  // limite de quadros: espera o resto do intervalo
                    val wait = 1_000_000_000L / maxFps - (System.nanoTime() - last)
                    if (wait > 1_000_000L) Thread.sleep(wait / 1_000_000L)
                }
                val now = System.nanoTime()
                val dt = ((now - last) / 1e9f).coerceIn(0f, 0.1f)
                last = now
                NativeBridge.nativeFrame(dt)
                afterFrame?.invoke(frame)   // sons (tiros, chuva...) na thread GL
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
                snapshot.region = String(NativeBridge.nativeRegionName(), Charsets.UTF_8)
                snapshot.teamMode = NativeBridge.nativeTeamMode()
                if (snapshot.playing) {
                    val g = FloatArray(8)
                    NativeBridge.nativeGameStatus(g)
                    snapshot.game = g
                    snapshot.objective = NativeBridge.gameText(0)
                    snapshot.message = NativeBridge.gameText(1)
                    snapshot.interact = String(NativeBridge.nativeInteractLabel(), Charsets.UTF_8)
                }
            }
        })
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    // 1 dedo: orbitar, arrastar o gizmo ou tocar para selecionar | 2 dedos: pan + pinça
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!touchToEngine) return false
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
            // a tela pode estar desenhada em resolução menor (qualidade): o toque vai na escala do desenho
            ids.add(e.getPointerId(i)); xy.add(e.getX(i) * renderScale); xy.add(e.getY(i) * renderScale)
        }
        val idArr = ids.toIntArray()
        val xyArr = xy.toFloatArray()
        queueEvent { NativeBridge.nativeTouch(action, idArr, xyArr) }
        return true
    }
}
