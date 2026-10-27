package com.mobile3d

import android.content.Context
import java.io.File

// Projetos salvos no armazenamento interno do app: files/projects/<nome>.json
// O nome do projeto aberto fica nas preferências: ao abrir o app, ele continua de onde parou.
class ProjectStore(private val ctx: Context) {
    private val dir = File(ctx.filesDir, "projects").apply { mkdirs() }
    private val prefs = ctx.getSharedPreferences("motor3d", Context.MODE_PRIVATE)

    var current: String
        get() = prefs.getString("projeto_atual", "") ?: ""
        set(v) { prefs.edit().putString("projeto_atual", v).apply() }

    fun file(name: String) = File(dir, safe(name) + ".json")
    fun exists(name: String) = file(name).exists()

    // Mais recentes primeiro
    fun list(): List<String> = dir.listFiles { f -> f.name.endsWith(".json") }.orEmpty()
        .sortedByDescending { it.lastModified() }.map { it.name.removeSuffix(".json") }

    fun read(name: String): String? = file(name).takeIf { it.exists() }?.readText(Charsets.UTF_8)

    // Grava num temporário e renomeia: se o celular desligar no meio, o arquivo antigo continua inteiro
    @Synchronized fun write(name: String, json: String) {
        val f = file(name)
        val tmp = File(dir, f.name + ".tmp")
        tmp.writeText(json, Charsets.UTF_8)
        if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
    }

    fun delete(name: String) { file(name).delete() }
    fun rename(from: String, to: String): Boolean {
        val dst = file(to)
        if (dst.exists()) return false
        return file(from).renameTo(dst).also { if (it && current == from) current = safe(to) }
    }

    fun uniqueName(base: String): String {
        val b = safe(base).ifBlank { "Projeto" }
        if (!exists(b)) return b
        var i = 2
        while (exists("$b $i")) i++
        return "$b $i"
    }

    companion object {
        fun safe(name: String) = name.trim().replace(Regex("[\\\\/:*?\"<>|\\n\\r\\t]"), "_").take(60)
    }
}
