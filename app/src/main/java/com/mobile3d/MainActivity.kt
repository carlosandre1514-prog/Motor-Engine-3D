package com.mobile3d

import android.app.Activity
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.view.MotionEvent
import java.io.File
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class MainActivity : Activity() {
    private lateinit var view: EngineView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        view = EngineView(this, copyShaders())
        setContentView(view)
    }

    override fun onPause() { super.onPause(); view.onPause() }
    override fun onResume() { super.onResume(); view.onResume() }

    // O renderer lê os shaders por caminho de arquivo: copia os assets para o storage interno.
    private fun copyShaders(): String {
        val dir = File(filesDir, "shaders").apply { mkdirs() }
        for (name in listOf("pbr.vert", "pbr.frag", "shadow.vert", "shadow.frag")) {
            assets.open(name).use { input -> File(dir, name).outputStream().use { input.copyTo(it) } }
        }
        return dir.absolutePath
    }
}

class EngineView(activity: Activity, private val shaderDir: String) : GLSurfaceView(activity) {
    init {
        setEGLContextClientVersion(3)
        setEGLConfigChooser(8, 8, 8, 8, 24, 0)
        setRenderer(object : Renderer {
            private var last = 0L
            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                NativeBridge.nativeCreate(shaderDir)
                NativeBridge.nativeLoadDemo()
                last = System.nanoTime()
            }
            override fun onSurfaceChanged(gl: GL10?, w: Int, h: Int) = NativeBridge.nativeResize(w, h)
            override fun onDrawFrame(gl: GL10?) {
                val now = System.nanoTime()
                val dt = ((now - last) / 1e9f).coerceIn(0f, 0.1f)
                last = now
                NativeBridge.nativeFrame(dt)
            }
        })
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    // 1 dedo: orbitar (ou arrastar o gizmo) | 2 dedos: pan + pinça
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
