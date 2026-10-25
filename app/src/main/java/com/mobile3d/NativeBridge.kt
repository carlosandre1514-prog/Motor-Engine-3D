package com.mobile3d

object NativeBridge {
    init { System.loadLibrary("mobile3d") }

    @JvmStatic external fun nativeCreate(shaderDir: String)
    @JvmStatic external fun nativeLoadDemo()
    @JvmStatic external fun nativeResize(w: Int, h: Int)
    @JvmStatic external fun nativeFrame(dt: Float)
    @JvmStatic external fun nativeTouch(action: Int, ids: IntArray, xy: FloatArray)
}
