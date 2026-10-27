package com.mobile3d

// Funções nativas (libmobile3d.so). Chamar SEMPRE na thread GL (GLSurfaceView.queueEvent / onDrawFrame).
object NativeBridge {
    init { System.loadLibrary("mobile3d") }

    @JvmStatic external fun nativeCreate(shaderDir: String): Boolean
    @JvmStatic external fun nativeDestroy()
    @JvmStatic external fun nativeResize(w: Int, h: Int)
    @JvmStatic external fun nativeFrame(dt: Float)
    @JvmStatic external fun nativeTouch(action: Int, ids: IntArray, xy: FloatArray)

    @JvmStatic external fun nativeSetPlay(playing: Boolean)
    @JvmStatic external fun nativeIsPlaying(): Boolean

    @JvmStatic external fun nativeNewScene()
    @JvmStatic external fun nativeLoadDemo()
    @JvmStatic external fun nativeLoadPhysicsDemo()
    @JvmStatic external fun nativeLoadModel(path: String): Int
    @JvmStatic external fun nativeLastError(): String
    @JvmStatic external fun nativeAddObject(kind: Int, physics: Boolean): Int
    @JvmStatic external fun nativeDeleteSelected()
    @JvmStatic external fun nativeFocus()

    @JvmStatic external fun nativeObjectList(): String
    @JvmStatic external fun nativeObjectName(id: Int): String
    @JvmStatic external fun nativeSelect(id: Int)
    @JvmStatic external fun nativeSelected(): Int
    @JvmStatic external fun nativeGetTransform(id: Int, out9: FloatArray): Boolean
    @JvmStatic external fun nativeSetTransform(id: Int, in9: FloatArray)
    @JvmStatic external fun nativeSetGizmoMode(mode: Int)

    @JvmStatic external fun nativeSetExposure(x: Float)
    @JvmStatic external fun nativeSetSunIntensity(x: Float)
    @JvmStatic external fun nativeSetGrid(visible: Boolean)
    @JvmStatic external fun nativeCycleAnimation(): String
    @JvmStatic external fun nativeGetStats(out6: FloatArray)

    // Criação procedural
    @JvmStatic external fun nativeCreateCharacter(params20: FloatArray): Int
    @JvmStatic external fun nativeCreateRandomCharacter(seed: Int, controllable: Boolean): Int
    @JvmStatic external fun nativeCreateWeapon(kind: Int): Int
    @JvmStatic external fun nativeCreateBuilding(kind: Int, seed: Int): Int
    @JvmStatic external fun nativeLoadMap(kind: Int, seed: Int)
    @JvmStatic external fun nativeMapOverview()

    // Materiais
    @JvmStatic external fun nativeMaterialNames(): String
    @JvmStatic external fun nativeSetObjectMaterial(index: Int)
    @JvmStatic external fun nativeSetObjectColor(r: Float, g: Float, b: Float)
    @JvmStatic external fun nativeSetObjectTexture(path: String): Boolean

    // Jogador
    @JvmStatic external fun nativeHasPlayer(): Boolean
    @JvmStatic external fun nativeSetPlayer()
    @JvmStatic external fun nativePlayerInput(x: Float, y: Float, run: Boolean)
    @JvmStatic external fun nativePlayerJump()
    @JvmStatic external fun nativePlayerFire()
    @JvmStatic external fun nativePlayerWeapon(): Int
}
