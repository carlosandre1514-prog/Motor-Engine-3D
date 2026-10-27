package com.mobile3d

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.MotionEvent
import android.view.View
import kotlin.math.min
import kotlin.math.sqrt

// Joystick virtual: entrega (x, y) em -1..1, com y positivo = para frente (para cima na tela)
class JoystickView(context: Context, private val onMove: (Float, Float) -> Unit) : View(context) {
    private val base = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x44FFFFFF; style = Paint.Style.FILL }
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x99FFFFFF.toInt(); style = Paint.Style.STROKE; strokeWidth = 4f }
    private val knob = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; alpha = 200 }
    private var kx = 0f
    private var ky = 0f

    override fun onDraw(c: Canvas) {
        val cx = width / 2f
        val cy = height / 2f
        val r = min(cx, cy) * 0.9f
        c.drawCircle(cx, cy, r, base)
        c.drawCircle(cx, cy, r, ring)
        c.drawCircle(cx + kx * r, cy + ky * r, r * 0.38f, knob)
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        val cx = width / 2f
        val cy = height / 2f
        val r = min(cx, cy) * 0.9f
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                var dx = (e.x - cx) / r
                var dy = (e.y - cy) / r
                val len = sqrt(dx * dx + dy * dy)
                if (len > 1f) { dx /= len; dy /= len }
                kx = dx; ky = dy
                onMove(dx, -dy)
            }
            else -> { kx = 0f; ky = 0f; onMove(0f, 0f) }
        }
        invalidate()
        return true
    }

    fun reset() { kx = 0f; ky = 0f; invalidate() }
}
