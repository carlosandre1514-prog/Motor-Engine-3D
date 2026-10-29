package com.mobile3d

import android.content.Context
import java.io.File

// Arquivos do motor (shaders, texturas, modelos, animações): copiados dos assets do APK para o armazenamento do app
// na 1ª abertura depois de instalar/atualizar (o motor lê arquivos comuns). Usado pelo editor e pelo jogo.
object EngineFiles {
    fun copy(ctx: Context, progress: (Int, Int) -> Unit = { _, _ -> }): String {
        val assets = ctx.assets
        val dir = File(ctx.filesDir, "shaders").apply { mkdirs() }
        for (name in assets.list("").orEmpty()) {
            if (!(name.endsWith(".vert") || name.endsWith(".frag") || name.endsWith(".glsl") || name.endsWith(".jpg"))) continue
            assets.open(name).use { input -> File(dir, name).outputStream().use { input.copyTo(it) } }
        }
        // texturas do terreno e dos materiais (alguns MB): copiadas só na instalação/atualização do app
        val installed = ctx.packageManager.getPackageInfo(ctx.packageName, 0).lastUpdateTime.toString()
        // lista primeiro (para mostrar o progresso), depois copia só o que mudou de tamanho
        val pending = ArrayList<Pair<String, File>>()
        fun listTree(path: String, to: File) {
            for (name in assets.list(path).orEmpty()) {
                val child = "$path/$name"
                val sub = assets.list(child).orEmpty()
                if (sub.isNotEmpty()) listTree(child, File(to, name)) else pending.add(child to File(to, name))
            }
        }
        val stamps = ArrayList<File>()
        for (sub in listOf("terrain", "materials", "kit", "anim")) {
            val folder = File(dir, sub)
            val stamp = File(folder, ".versao")
            if (stamp.exists() && stamp.readText() == installed) continue
            listTree(sub, folder)
            stamps.add(stamp)
        }
        pending.forEachIndexed { i, (asset, dst) ->
            if (i % 10 == 0) progress(i, pending.size)
            val size = try { assets.openFd(asset).use { it.length } } catch (_: Exception) { -1L }  // -1: comprimido no APK
            if (size >= 0 && dst.exists() && dst.length() == size) return@forEachIndexed
            dst.parentFile?.mkdirs()
            assets.open(asset).use { input -> dst.outputStream().use { input.copyTo(it, 1 shl 16) } }
        }
        for (st in stamps) { st.parentFile?.mkdirs(); st.writeText(installed) }
        return dir.absolutePath
    }
}
