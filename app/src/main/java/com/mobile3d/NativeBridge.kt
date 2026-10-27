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

    // Efeitos e personagens importados
    @JvmStatic external fun nativeCreateEffect(kind: Int, element: Int): Int
    @JvmStatic external fun nativeSetEffectElement(element: Int)
    @JvmStatic external fun nativePlayerCast(element: Int)
    @JvmStatic external fun nativeMakeCharacter(controllable: Boolean): Int

    // Projeto (JSON em bytes UTF-8)
    @JvmStatic external fun nativeExportProject(): ByteArray
    @JvmStatic external fun nativeImportProject(json: ByteArray): Boolean
    @JvmStatic external fun nativeProjectRevision(): Long
    @JvmStatic external fun nativeSetProjectName(name: ByteArray)
    @JvmStatic external fun nativeToolCall(name: String, input: ByteArray): ByteArray
    @JvmStatic external fun nativeAddSpec(json: ByteArray): Int
    @JvmStatic external fun nativeSetTimeOfDay(hour: Float)
    @JvmStatic external fun nativeGetTimeOfDay(): Float
    @JvmStatic external fun nativeExportGlb(objectId: Int, path: String): Boolean

    // Jogo
    @JvmStatic external fun nativeGameStatus(out8: FloatArray)
    @JvmStatic external fun nativeGameText(which: Int): ByteArray

    // Animações
    @JvmStatic external fun nativeAnimationList(): ByteArray
    @JvmStatic external fun nativeAnimationIndex(): Int
    @JvmStatic external fun nativeSetAnimation(index: Int)
    @JvmStatic external fun nativeSetAnimPreview(on: Boolean)

    // Atalhos com String
    fun exportProject() = String(nativeExportProject(), Charsets.UTF_8)
    fun importProject(json: String) = nativeImportProject(json.toByteArray(Charsets.UTF_8))
    fun toolCall(name: String, input: String) = String(nativeToolCall(name, input.toByteArray(Charsets.UTF_8)), Charsets.UTF_8)
    fun addSpec(json: String) = nativeAddSpec(json.toByteArray(Charsets.UTF_8))
    fun gameText(which: Int) = String(nativeGameText(which), Charsets.UTF_8)
    fun animationList() = String(nativeAnimationList(), Charsets.UTF_8).lines().filter { it.isNotEmpty() }
}
