package com.mobile3d

import android.content.Context
import android.media.AudioAttributes
import android.media.SoundPool
import kotlin.random.Random

// Sons do jogo (assets/sons/*.wav, gerados por tools/gerar_sons.py): tiros de cada arma, recarga, impacto, golpes,
// trovão e a chuva em loop. O motor pede os sons (nativePollSounds) e diz quanto está chovendo (nativeGetWeather).
class SoundManager(context: Context) {
    private val pool = SoundPool.Builder()
        .setMaxStreams(16)
        .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
        .build()
    private val ids = HashMap<String, Int>()
    private val ready = HashSet<Int>()
    private var rainStream = 0
    private var rainVolume = 0f
    var enabled = true

    init {
        pool.setOnLoadCompleteListener { _, id, status -> if (status == 0) synchronized(ready) { ready.add(id) } }
        val am = context.assets
        for (f in am.list("sons").orEmpty()) {
            if (!f.endsWith(".wav")) continue
            try { am.openFd("sons/$f").use { ids[f.removeSuffix(".wav")] = pool.load(it, 1) } } catch (_: Exception) {}
        }
    }

    private fun isReady(id: Int) = synchronized(ready) { ready.contains(id) }

    // Linhas "nome\tvolume\tpan" vindas do motor. De longe: usa "<nome>_longe" se existir.
    fun playEvents(text: String) {
        if (!enabled || text.isEmpty()) return
        for (line in text.split('\n')) {
            val p = line.split('\t')
            if (p.size < 3) continue
            val name = if (ids.containsKey(p[0])) p[0] else p[0].removeSuffix("_longe")
            val id = ids[name] ?: continue
            if (!isReady(id)) continue
            val vol = p[1].toFloatOrNull() ?: continue
            val pan = (p[2].toFloatOrNull() ?: 0f).coerceIn(-1f, 1f)
            val l = vol * (if (pan > 0) 1f - pan * 0.7f else 1f)
            val r = vol * (if (pan < 0) 1f + pan * 0.7f else 1f)
            // cada tiro um pouco diferente (como na vida real)
            val rate = if (name.startsWith("tiro") || name.startsWith("golpe")) 0.94f + Random.nextFloat() * 0.12f else 1f
            pool.play(id, l, r, if (name.startsWith("tiro")) 2 else 1, 0, rate)
        }
    }

    // Chuva em loop: volume acompanha a intensidade (0..1)
    fun setRain(level: Float) {
        val v = if (enabled) (level * 0.85f).coerceIn(0f, 1f) else 0f
        if (v < 0.01f) {
            if (rainStream != 0) { pool.stop(rainStream); rainStream = 0 }
            rainVolume = 0f
            return
        }
        val id = ids["chuva"] ?: return
        if (rainStream == 0 && isReady(id)) rainStream = pool.play(id, v, v, 0, -1, 1f)
        if (rainStream != 0 && kotlin.math.abs(v - rainVolume) > 0.02f) { pool.setVolume(rainStream, v, v); rainVolume = v }
    }

    fun pause() { pool.autoPause() }
    fun resume() { pool.autoResume() }
    fun release() { pool.release() }
}
