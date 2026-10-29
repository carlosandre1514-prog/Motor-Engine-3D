package com.mobile3d

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.math.max
import kotlin.math.min

// O jogo Polícia x Bandidos (APK separado do editor): tela inicial, escolha do lado e da skin, loja de armas e
// skins com moedas, partida 5 x 5 contra robôs no mapa Favela em primeira pessoa, HUD e tela de fim.
// O motor é o mesmo do editor; aqui só entram o projeto pronto da partida e os controles do jogo.
class JogoActivity : Activity() {

    // ---------------- catálogo (preços provisórios: o dono do jogo ainda vai passar os definitivos) ----------------
    // Ordem das skins = ordem da lista do motor (Game.cpp kSkins): o índice vai no nativeTeamLoadout
    class Skin(val key: String, val name: String, val team: Int, val index: Int, val price: Int)
    class Gun(val key: String, val name: String, val team: Int, val price: Int, val info: String)

    private val skins = listOf(
        Skin("pm", "PM", 0, 0, 0), Skin("civil", "Polícia Civil", 0, 1, 500), Skin("exercito", "Exército", 0, 2, 700),
        Skin("choque", "Choque", 0, 3, 1000), Skin("especial", "Especial", 0, 4, 1500),
        Skin("rubro", "Rubro", 1, 0, 0), Skin("alvinegro", "Alvinegro", 1, 1, 0), Skin("tricolor", "Tricolor", 1, 2, 0),
        Skin("canarinho", "Canarinho", 1, 3, 0), Skin("faixa", "Faixa", 1, 4, 0))
    private val guns = listOf(
        Gun("m4a1", "M4A1", 0, 0, "24 de dano · 60 na cabeça · 10 tiros/s · pente 30"),
        Gun("m4colt", "M4 Colt", 0, 1500, "Com luneta · 26 de dano · 65 na cabeça · 9 tiros/s · pente 30"),
        Gun("ak47", "AK-47", 1, 0, "30 de dano · 75 na cabeça · 8 tiros/s · pente 30"),
        Gun("ak74", "AK-74", 1, 1400, "27 de dano · 68 na cabeça · 9 tiros/s · pente 30"),
        Gun("ak74_tatica", "AK-74 tática", 1, 1800, "28 de dano · 70 na cabeça · 9,5 tiros/s · pente 30"),
        Gun("tt33", "TT-33", -1, 0, "Pistola · 25 de dano · 62 na cabeça · 3 tiros/s · pente 8"))

    // Skins de arma (só trocam a textura de cor; o modelo é o mesmo)
    class GunSkin(val gun: String, val key: String, val name: String, val price: Int)
    private val gunSkins = listOf(
        GunSkin("tt33", "crimson_forge", "TT-33 Crimson Forge", 800),
        GunSkin("tt33", "nebula_shadow", "TT-33 Nebula Shadow", 1000),
        GunSkin("tt33", "gold_relic", "TT-33 Gold Relic", 1500))
    private fun gunSkinOf(gun: String) = prefs.getString("skin_arma_$gun", "")!!

    // ---------------- cores (as das telas aprovadas) ----------------
    private val bg = 0xFF0B0F17.toInt()
    private val panel = 0xFF121826.toInt()
    private val gold = 0xFFFBBF24.toInt()
    private val blue = 0xFF3B82F6.toInt()
    private val blueLight = 0xFF93C5FD.toInt()
    private val orange = 0xFFF59E0B.toInt()
    private val yellowLight = 0xFFFCD34D.toInt()
    private val green = 0xFF86EFAC.toInt()
    private val red = 0xFFDC2626.toInt()
    private val dark = 0xB80B0F17.toInt()
    private val textMain = 0xFFF3F4F6.toInt()
    private val textDim = 0xFFC3C8D2.toInt()
    private val condensed: Typeface = Typeface.create("sans-serif-condensed", Typeface.BOLD)

    // ---------------- estado ----------------
    private lateinit var view: EngineView
    private lateinit var screen: FrameLayout
    private val ui = Handler(Looper.getMainLooper())
    private val sound by lazy { SoundManager(this) }
    private val prefs by lazy { getSharedPreferences("jogo", MODE_PRIVATE) }
    private val weather = FloatArray(3)
    @Volatile private var firing = false
    @Volatile private var menuSpin = true
    private val lookLock = Any()
    private var lookDx = 0f
    private var lookDy = 0f
    private var inMatch = false
    private var matchEnded = false
    private var chosenTeam = 0
    private var hud: Hud? = null
    private val images = HashMap<String, Bitmap?>()

    private var coins: Int
        get() = prefs.getInt("moedas", 1000)
        set(v) { prefs.edit().putInt("moedas", v).apply() }

    private fun owns(id: String, price: Int) = price == 0 || prefs.getStringSet("itens", emptySet())!!.contains(id)
    private fun addOwned(id: String) {
        val s = HashSet(prefs.getStringSet("itens", emptySet())!!)
        s.add(id)
        prefs.edit().putStringSet("itens", s).apply()
    }
    private fun ownsSkin(s: Skin) = owns("skin:" + s.key, s.price)
    private fun ownsGun(g: Gun) = owns("arma:" + g.key, g.price)

    // ---------------- ciclo de vida ----------------
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        val splash = label("Preparando o jogo…", 16f, textMain).apply { gravity = Gravity.CENTER; setBackgroundColor(bg) }
        setContentView(splash)
        Thread {
            val dir = EngineFiles.copy(this) { done, total ->
                runOnUiThread { splash.text = "Preparando os arquivos do jogo…\n$done de $total\n(só depois de instalar ou atualizar)" }
            }
            runOnUiThread { if (!isFinishing && !isDestroyed) setup(dir) }
        }.start()
    }

    override fun onPause() {
        super.onPause()
        if (::view.isInitialized) view.onPause()
        sound.pause()
        firing = false
    }

    override fun onResume() {
        super.onResume()
        if (::view.isInitialized) view.onResume()
        sound.resume()
    }

    override fun onDestroy() {
        super.onDestroy()
        sound.release()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        when {
            !::screen.isInitialized -> return
            inMatch && !matchEnded -> showPause()
            screen.childCount > 0 && screen.getChildAt(0).tag != "inicio" -> showHome()
            else -> @Suppress("DEPRECATION") super.onBackPressed()
        }
    }

    private fun setup(shaderDir: String) {
        // 1ª vez: celular com até ~4 GB de RAM (ex.: Galaxy A14) começa no modo leve
        if (!prefs.contains("qualidade")) {
            val mi = android.app.ActivityManager.MemoryInfo()
            (getSystemService(ACTIVITY_SERVICE) as android.app.ActivityManager).getMemoryInfo(mi)
            prefs.edit().putInt("qualidade", if (mi.totalMem < 5_000_000_000L) 0 else 1).apply()
        }
        try { File(shaderDir, "qualidade.txt").writeText(prefs.getInt("qualidade", 1).toString()) } catch (_: Exception) {}
        view = EngineView(this, shaderDir)
        view.touchToEngine = false
        view.onReady = { loadMatchProject() }
        view.afterFrame = { frame ->
            sound.playEvents(NativeBridge.nativePollSounds())
            if (frame % 10 == 0) { NativeBridge.nativeGetWeather(weather); sound.setRain(weather[0]) }
            if (firing) NativeBridge.nativePlayerFire()
            var dx: Float; var dy: Float
            synchronized(lookLock) { dx = lookDx; dy = lookDy; lookDx = 0f; lookDy = 0f }
            if (dx != 0f || dy != 0f) NativeBridge.nativeLook(dx, dy)
            if (menuSpin) NativeBridge.nativeLook(-0.35f, 0f)  // menu: o morro girando devagar ao fundo
        }
        applyQuality()
        val root = FrameLayout(this)
        root.addView(view, FrameLayout.LayoutParams(-1, -1))
        screen = FrameLayout(this)
        root.addView(screen, FrameLayout.LayoutParams(-1, -1))
        setContentView(root)
        showHome()
    }

    private fun applyQuality() {
        when (prefs.getInt("qualidade", 1)) {
            0 -> view.applyQuality(0.5f, 30, 0)
            1 -> view.applyQuality(0.75f, 45, 1)
            else -> view.applyQuality(1f, 60, 2)
        }
    }

    private fun gl(block: () -> Unit) = view.queueEvent(block)

    // Projeto pronto da partida (o mesmo exemplo do editor), sem as barracas de loja: aqui a loja fica no menu
    private fun loadMatchProject() {
        try {
            val json = JSONObject(assets.open("exemplos/5_policia_x_bandidos.json").bufferedReader().readText())
            val objs = json.getJSONArray("objetos")
            val keep = JSONArray()
            for (i in 0 until objs.length()) if (objs.getJSONObject(i).optString("tipo") != "loja") keep.put(objs.get(i))
            json.put("objetos", keep)
            NativeBridge.nativeSetEditorOverlay(false)
            NativeBridge.importProject(json.toString())
            NativeBridge.nativeSelect(-1)
        } catch (e: Exception) {
            runOnUiThread { toast("Não consegui abrir a partida: ${e.message}") }
        }
        overviewCamera()
    }

    private fun overviewCamera() = NativeBridge.nativeSetCamera(0f, 8f, 20f, 3.6f, 0.42f, 115f)

    // ---------------- ajudantes de interface ----------------
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun dpf(v: Float) = v * resources.displayMetrics.density
    private fun toast(msg: String) = android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_SHORT).show()

    private fun label(text: String, sp: Float, color: Int, bold: Boolean = false) = TextView(this).apply {
        this.text = text
        setTextColor(color)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, sp)
        if (bold) typeface = Typeface.DEFAULT_BOLD
    }

    private fun title(text: String, sp: Float, color: Int) = label(text, sp, color).apply {
        typeface = condensed
        letterSpacing = 0.04f
        includeFontPadding = false
    }

    private fun round(color: Int, radius: Float, stroke: Int = 0, strokeColor: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dpf(radius)
        if (stroke > 0) setStroke(dp(stroke), strokeColor)
    }

    private fun button(text: String, fill: Int, fg: Int, sp: Float = 15f, radius: Float = 12f, onClick: () -> Unit) = TextView(this).apply {
        this.text = text
        setTextColor(fg)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, sp)
        typeface = Typeface.DEFAULT_BOLD
        gravity = Gravity.CENTER
        background = round(fill, radius)
        setPadding(dp(14), dp(8), dp(14), dp(8))
        minHeight = dp(44)
        isClickable = true
        setOnClickListener { onClick() }
    }

    private fun lp(w: Int, h: Int, weight: Float = 0f) = LinearLayout.LayoutParams(w, h, weight)
    private fun vbox() = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
    private fun hbox() = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }

    private fun image(path: String): Bitmap? = images.getOrPut(path) {
        try { assets.open(path).use { BitmapFactory.decodeStream(it) } } catch (_: Exception) { null }
    }

    private fun coinChip(): View = hbox().apply {
        background = round(0xD90B0F17.toInt(), 18f, 1, 0x8CFCD34D.toInt())
        setPadding(dp(12), dp(4), dp(12), dp(4))
        addView(View(context).apply { background = round(yellowLight, 8f) }, lp(dp(14), dp(14)))
        addView(label("  ${fmt(coins)} moedas", 14f, yellowLight, true))
    }

    private fun fmt(n: Int) = String.format(java.util.Locale("pt", "BR"), "%,d", n)

    private fun show(v: View, tag: String) {
        v.tag = tag
        screen.removeAllViews()
        screen.addView(v, FrameLayout.LayoutParams(-1, -1))
    }

    // ---------------- tela inicial ----------------
    private fun showHome() {
        menuSpin = true
        val root = FrameLayout(this)
        root.addView(View(this).apply { setBackgroundColor(0xB8080B12.toInt()) }, FrameLayout.LayoutParams(-1, -1))
        val left = vbox().apply { setPadding(dp(28), dp(20), dp(28), dp(20)) }
        left.addView(title("POLÍCIA", 54f, 0xFF60A5FA.toInt()))
        left.addView(hbox().apply {
            addView(title("x  ", 30f, textMain))
            addView(title("BANDIDOS", 54f, gold))
        })
        left.addView(label("FAVELA · 5 CONTRA 5 · PRIMEIRA PESSOA", 12f, 0xFFD1D5DB.toInt(), true).apply { letterSpacing = 0.12f })
        left.addView(View(this), lp(1, 0, 1f))
        left.addView(button("▶  JOGAR", gold, 0xFF111827.toInt(), 24f, 14f) { showSide() }.apply { typeface = condensed }, lp(dp(270), dp(58)))
        val grid = hbox().apply { setPadding(0, dp(8), 0, 0) }
        grid.addView(button("Loja", 0x1AFFFFFF, textMain) { showShop(0) }, lp(0, dp(46), 1f))
        grid.addView(View(this), lp(dp(8), 1))
        grid.addView(button("Gráficos", 0x1AFFFFFF, textMain) { qualityDialog() }, lp(0, dp(46), 1f))
        left.addView(grid, lp(dp(270), -2))
        left.addView(button("Multiplayer (em breve: criar sala / entrar com código)", 0x0DFFFFFF, 0xFF9CA3AF.toInt(), 11f) {
            toast("Multiplayer online: em breve")
        }.apply { minHeight = dp(36) }, lp(dp(270), -2).apply { topMargin = dp(8) })
        root.addView(left, FrameLayout.LayoutParams(-2, -1, Gravity.START))
        root.addView(coinChip(), FrameLayout.LayoutParams(-2, dp(36), Gravity.TOP or Gravity.END).apply { topMargin = dp(16); rightMargin = dp(20) })
        // destaque: a skin mais cara que ainda falta
        val featured = skins.filter { !ownsSkin(it) }.maxByOrNull { it.price }
        if (featured != null) {
            val card = vbox().apply {
                background = round(0xC70B0F17.toInt(), 18f, 1, 0x73FCD34D)
                setPadding(dp(14), dp(10), dp(14), dp(12))
                isClickable = true
                setOnClickListener { showShop(if (featured.team == 0) 2 else 3) }
            }
            card.addView(label("DESTAQUE DA LOJA", 11f, gold, true).apply { letterSpacing = 0.1f })
            card.addView(ImageView(this).apply { setImageBitmap(image("retratos/${featured.key}.png")); scaleType = ImageView.ScaleType.FIT_CENTER }, lp(dp(150), dp(170)))
            card.addView(title(featured.name.uppercase(), 22f, textMain))
            card.addView(label("${fmt(featured.price)} moedas", 13f, yellowLight, true))
            root.addView(card, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.END).apply { bottomMargin = dp(20); rightMargin = dp(24) })
        }
        show(root, "inicio")
    }

    private fun qualityDialog() {
        val items = arrayOf("Leve (celulares de 4 GB, ex.: Galaxy A14)", "Equilibrada", "Alta")
        AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Gráficos")
            .setSingleChoiceItems(items, prefs.getInt("qualidade", 1)) { d, which ->
                prefs.edit().putInt("qualidade", which).apply()
                try { File(filesDir, "shaders/qualidade.txt").writeText(which.toString()) } catch (_: Exception) {}
                applyQuality()
                d.dismiss()
            }.show()
    }

    // ---------------- escolha do lado ----------------
    private fun header(text: String, back: () -> Unit): LinearLayout = hbox().apply {
        addView(button("‹", 0x14FFFFFF, textMain, 22f) { back() }.apply { setPadding(0, 0, 0, dp(4)) }, lp(dp(44), dp(44)))
        addView(title("  $text", 34f, textMain))
        addView(View(context), lp(0, 1, 1f))
        addView(coinChip(), lp(-2, dp(34)))
    }

    private fun showSide() {
        menuSpin = true
        val root = vbox().apply { setBackgroundColor(bg); setPadding(dp(24), dp(16), dp(24), dp(18)) }
        root.addView(header("ESCOLHA SEU LADO") { showHome() })
        val row = hbox().apply { setPadding(0, dp(12), 0, 0) }
        fun card(team: Int): View {
            val c = vbox().apply {
                background = round(if (team == 0) 0xFF0F1D36.toInt() else 0xFF2A1C08.toInt(), 18f, 2, if (team == 0) blue else orange)
                setPadding(dp(20), dp(16), dp(20), dp(16))
                isClickable = true
                setOnClickListener { chosenTeam = team; showSkins(team) }
            }
            c.addView(title(if (team == 0) "POLÍCIA" else "BANDIDOS", 42f, if (team == 0) blueLight else yellowLight))
            c.addView(label(if (team == 0) "Nasce fora da favela, na rua dos prédios. Sobe as escadarias para tomar o morro."
                            else "Nasce na quadra, dentro da favela. Com polícia na quadra, nasce num canto do morro sem policial.",
                            13f, if (team == 0) 0xFFDBEAFE.toInt() else 0xFFFEF3C7.toInt()).apply { setPadding(0, dp(6), 0, 0) })
            c.addView(View(this@JogoActivity), lp(1, 0, 1f))
            val list = if (team == 0) "M4A1 · M4 Colt (com luneta)" else "AK-47 · AK-74 · AK-74 tática"
            c.addView(label(list, 11f, if (team == 0) 0xFFDBEAFE.toInt() else 0xFFFEF3C7.toInt()))
            c.addView(hbox().apply {
                setPadding(0, dp(8), 0, 0)
                addView(label("4 robôs + você", 12f, if (team == 0) 0xFFBFDBFE.toInt() else 0xFFFDE68A.toInt()), lp(0, -2, 1f))
                addView(button("ENTRAR", if (team == 0) blue else orange, if (team == 0) textMain else 0xFF111827.toInt(), 14f) {
                    chosenTeam = team; showSkins(team)
                })
            })
            return c
        }
        row.addView(card(0), lp(0, -1, 1f))
        row.addView(View(this), lp(dp(16), 1))
        row.addView(card(1), lp(0, -1, 1f))
        root.addView(row, lp(-1, 0, 1f))
        show(root, "lado")
    }

    // ---------------- skins do lado + arma principal ----------------
    private fun showSkins(team: Int) {
        menuSpin = true
        val mine = skins.filter { it.team == team }
        var selected = prefs.getString("skin_$team", mine[0].key)!!.let { k -> mine.firstOrNull { it.key == k && ownsSkin(it) } ?: mine.first { ownsSkin(it) } }
        val primaries = guns.filter { it.team == team || it.team == -1 }
        var gun = prefs.getString("arma_$team", "")!!.let { k -> primaries.firstOrNull { it.key == k && ownsGun(it) } ?: primaries.first { ownsGun(it) } }
        var second: Gun? = prefs.getString("arma2_$team", "tt33")!!.let { k -> primaries.firstOrNull { it.key == k && ownsGun(it) && it != gun } }

        val root = vbox().apply { setBackgroundColor(bg); setPadding(dp(20), dp(12), dp(20), dp(12)) }
        root.addView(header(if (team == 0) "SUA FARDA · POLÍCIA" else "SUA ROUPA · BANDIDOS") { showSide() })
        val cards = hbox()
        val cardViews = ArrayList<View>()
        fun refresh() {
            mine.forEachIndexed { i, s ->
                val on = s == selected
                cardViews[i].background = round(if (on) 0xFF16233D.toInt() else panel, 16f, 2, if (on) gold else 0x22FFFFFF)
            }
        }
        for (s in mine) {
            val owned = ownsSkin(s)
            val c = vbox().apply {
                gravity = Gravity.CENTER_HORIZONTAL
                setPadding(dp(6), dp(6), dp(6), dp(8))
                isClickable = true
                setOnClickListener {
                    if (ownsSkin(s)) { selected = s; refresh() }
                    else buyDialog("a skin ${s.name}", s.price, "skin:" + s.key) { showSkins(team) }
                }
            }
            val img = ImageView(this).apply {
                setImageBitmap(image("retratos/${s.key}.png"))
                scaleType = ImageView.ScaleType.FIT_CENTER
                if (!owned) alpha = 0.45f
            }
            c.addView(img, lp(-1, 0, 1f))
            c.addView(label(s.name, 13f, textMain, true).apply { gravity = Gravity.CENTER })
            c.addView(label(if (owned) (if (s.price == 0) "grátis" else "sua") else "🔒 ${fmt(s.price)} moedas", 11f,
                            if (owned) green else yellowLight, true).apply { gravity = Gravity.CENTER })
            cardViews.add(c)
            cards.addView(c, lp(0, -1, 1f).apply { leftMargin = dp(4); rightMargin = dp(4) })
        }
        refresh()
        root.addView(cards, lp(-1, 0, 1f).apply { topMargin = dp(8) })
        // arma principal (as que você tem deste lado)
        val gunRow = hbox().apply { setPadding(0, dp(8), 0, 0) }
        gunRow.addView(label("Arma principal:  ", 13f, textDim, true))
        val chips = HashMap<Gun, TextView>()
        val chips2 = HashMap<Gun?, TextView>()
        fun refreshGuns() {
            if (second == gun) second = null
            for ((g, t) in chips) t.background = round(if (g == gun) 0xFF16233D.toInt() else panel, 12f, 2, if (g == gun) gold else 0x22FFFFFF)
            for ((g, t) in chips2) {
                t.visibility = if (g != null && g == gun) View.GONE else View.VISIBLE
                t.background = round(if (g == second) 0xFF16233D.toInt() else panel, 12f, 2, if (g == second) gold else 0x22FFFFFF)
            }
        }
        val chipRow = hbox()
        for (g in primaries) {
            val owned = ownsGun(g)
            val t = label(if (owned) g.name else "🔒 ${g.name}", 13f, if (owned) textMain else 0xFF9CA3AF.toInt(), true).apply {
                gravity = Gravity.CENTER
                setPadding(dp(12), dp(6), dp(12), dp(6))
                isClickable = true
                setOnClickListener {
                    if (ownsGun(g)) { gun = g; refreshGuns() }
                    else buyDialog("a ${g.name}", g.price, "arma:" + g.key) { showSkins(team) }
                }
            }
            chips[g] = t
            chipRow.addView(t, lp(-2, dp(40)).apply { rightMargin = dp(6) })
        }
        // segunda arma (troca rápida): outra que você tem deste lado, ou nenhuma
        val chipRow2 = hbox()
        for (g in listOf<Gun?>(null) + primaries.filter { ownsGun(it) }) {
            val t = label(g?.name ?: "nenhuma", 13f, textMain, true).apply {
                gravity = Gravity.CENTER
                setPadding(dp(12), dp(6), dp(12), dp(6))
                isClickable = true
                setOnClickListener { second = g; refreshGuns() }
            }
            chips2[g] = t
            chipRow2.addView(t, lp(-2, dp(40)).apply { rightMargin = dp(6) })
        }
        refreshGuns()
        val secondRow = hbox().apply {
            setPadding(0, dp(6), 0, 0)
            addView(label("Segunda arma (troca rápida):  ", 13f, textDim, true))
            addView(HorizontalScrollView(this@JogoActivity).apply { isHorizontalScrollBarEnabled = false; addView(chipRow2) }, lp(0, -2, 1f))
        }
        gunRow.addView(HorizontalScrollView(this).apply { isHorizontalScrollBarEnabled = false; addView(chipRow) }, lp(0, -2, 1f))
        gunRow.addView(button("COMEÇAR  ▶", gold, 0xFF111827.toInt(), 17f) {
            prefs.edit().putString("skin_$team", selected.key).putString("arma_$team", gun.key)
                .putString("arma2_$team", second?.key ?: "").putInt("lado", team).apply()
            startMatch(team, selected, gun, second)
        }.apply { typeface = condensed }, lp(dp(170), dp(50)))
        root.addView(gunRow)
        root.addView(secondRow)
        show(root, "skins")
    }

    private fun buyDialog(what: String, price: Int, id: String, after: () -> Unit) {
        val have = coins
        val b = AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("Comprar $what?")
        if (have >= price) {
            b.setMessage("Custa ${fmt(price)} moedas. Você tem ${fmt(have)}.")
                .setPositiveButton("Comprar") { _, _ -> coins = have - price; addOwned(id); toast("Comprado!"); after() }
                .setNegativeButton("Agora não", null)
        } else {
            b.setMessage("Custa ${fmt(price)} moedas e você tem ${fmt(have)}. Faltam ${fmt(price - have)}: ganhe jogando (abates e vitórias dão moedas).")
                .setPositiveButton("Ok", null)
        }
        b.show()
    }

    // ---------------- loja: armas e skins juntas ----------------
    private fun showShop(tab: Int) {
        menuSpin = true
        val root = vbox().apply { setBackgroundColor(bg); setPadding(dp(18), dp(10), dp(18), dp(12)) }
        root.addView(header("LOJA") { showHome() })
        val tabs = hbox().apply { setPadding(0, dp(6), 0, dp(6)) }
        arrayOf("Armas", "Skins de arma", "Skins Polícia", "Skins Bandidos").forEachIndexed { i, t ->
            tabs.addView(button(t, if (i == tab) gold else 0x14FFFFFF, if (i == tab) 0xFF111827.toInt() else textMain, 13f) { showShop(i) },
                         lp(-2, dp(40)).apply { rightMargin = dp(8) })
        }
        tabs.addView(label("Preços provisórios", 11f, 0xFF9CA3AF.toInt()))
        root.addView(tabs)
        // equip: skin de arma (usar / tirar); null nos outros itens
        class Item(val id: String, val name: String, val price: Int, val img: String, val info: String, val owned: Boolean,
                   val equip: (() -> Unit)? = null, val equipped: Boolean = false)
        val items = when (tab) {
            0 -> guns.map {
                Item("arma:" + it.key, it.name, it.price, "armas/${it.key}.png",
                     (if (it.team == 0) "Polícia · " else if (it.team == 1) "Bandidos · " else "Os dois lados · ") + it.info, ownsGun(it))
            }
            1 -> gunSkins.map { g ->
                val on = gunSkinOf(g.gun) == g.key
                Item("skin_arma:${g.gun}:${g.key}", g.name, g.price, "armas/${g.gun}_${g.key}.png", "Skin de arma · troca só a pintura",
                     owns("skin_arma:${g.gun}:${g.key}", g.price),
                     { prefs.edit().putString("skin_arma_${g.gun}", if (on) "" else g.key).apply(); showShop(1) }, on)
            }
            else -> skins.filter { it.team == tab - 2 }.map {
                Item("skin:" + it.key, it.name, it.price, "retratos/${it.key}.png", if (it.team == 0) "Skin da polícia" else "Skin dos bandidos", ownsSkin(it))
            }
        }
        val body = hbox().apply { gravity = Gravity.TOP }
        val list = vbox()
        val detail = vbox().apply { background = round(panel, 16f, 1, 0x1AFFFFFF); setPadding(dp(16), dp(12), dp(16), dp(12)) }
        fun select(it: Item) {
            detail.removeAllViews()
            detail.addView(hbox().apply {
                addView(title(it.name.uppercase(), 30f, textMain), lp(0, -2, 1f))
                addView(label(if (it.price == 0) "grátis" else "${fmt(it.price)} moedas", 16f, yellowLight, true))
            })
            detail.addView(ImageView(this).apply { setImageBitmap(image(it.img)); scaleType = ImageView.ScaleType.FIT_CENTER; background = round(0xFF1B2232.toInt(), 12f) },
                           lp(-1, 0, 1f).apply { topMargin = dp(6); bottomMargin = dp(6) })
            detail.addView(label(it.info, 12f, textDim))
            val can = !it.owned && coins >= it.price
            val equip = it.equip
            if (it.owned && equip != null) {
                detail.addView(button(if (it.equipped) "USANDO  (tocar para tirar)" else "USAR ESTA SKIN", if (it.equipped) 0x1AFFFFFF else gold,
                                      if (it.equipped) textMain else 0xFF111827.toInt(), 15f) { equip() }, lp(-1, dp(48)).apply { topMargin = dp(8) })
            } else {
                detail.addView(button(if (it.owned) "JÁ É SUA" else if (can) "COMPRAR POR ${fmt(it.price)}" else "FALTAM ${fmt(it.price - coins)} MOEDAS",
                                      if (it.owned) 0x1AFFFFFF else if (can) gold else 0x40EF4444, if (can) 0xFF111827.toInt() else textMain, 15f) {
                    if (can) { coins -= it.price; addOwned(it.id); toast("Comprado: ${it.name}"); showShop(tab) }
                }, lp(-1, dp(48)).apply { topMargin = dp(8) })
            }
        }
        for (it in items) {
            list.addView(hbox().apply {
                background = round(panel, 12f, 1, 0x14FFFFFF)
                setPadding(dp(10), 0, dp(10), 0)
                isClickable = true
                setOnClickListener { _ -> select(it) }
                addView(ImageView(context).apply { setImageBitmap(image(it.img)); scaleType = ImageView.ScaleType.FIT_CENTER }, lp(dp(64), dp(40)))
                addView(label("  " + it.name, 14f, textMain, true), lp(0, -2, 1f))
                addView(label(if (it.equipped) "usando" else if (it.owned) "✓ sua" else if (it.price == 0) "grátis" else fmt(it.price), 12f,
                              if (it.owned) blueLight else green, true))
            }, lp(-1, dp(48)).apply { bottomMargin = dp(5) })
        }
        body.addView(ScrollView(this).apply { addView(list) }, lp(dp(300), -1))
        body.addView(View(this), lp(dp(14), 1))
        body.addView(detail, lp(0, -1, 1f))
        root.addView(body, lp(-1, 0, 1f))
        items.firstOrNull()?.let { select(it) }
        show(root, "loja")
    }

    // ---------------- partida ----------------
    private fun startMatch(team: Int, skin: Skin, gun: Gun, second: Gun?) {
        menuSpin = false
        inMatch = true
        matchEnded = false
        chosenTeam = team
        val h = Hud()
        hud = h
        show(h.root, "hud")
        val chosenSkins = guns.map { it.key to gunSkinOf(it.key) }
        gl {
            for ((g, sk) in chosenSkins) NativeBridge.nativeTeamGunSkin(g, sk)
            NativeBridge.nativeTeamLoadout(team, skin.index, gun.key, second?.key ?: "")
            NativeBridge.nativeSetFirstPerson(true)
            NativeBridge.nativeSetPlay(true)
            // polícia olha para o morro; bandidos, para a rua de baixo
            NativeBridge.nativeSetCamera(0f, 0f, 0f, if (team == 0) 0f else 3.14159f, 0.05f, 3f)
        }
        pollHud()
    }

    private fun pollHud() {
        if (!inMatch) return
        gl {
            val s = String(NativeBridge.nativeHudJson(), Charsets.UTF_8)
            runOnUiThread { try { onHud(JSONObject(s)) } catch (_: Exception) {} }
        }
        ui.postDelayed({ pollHud() }, 100)
    }

    private fun onHud(j: JSONObject) {
        val h = hud ?: return
        if (!inMatch) return
        h.update(j)
        if (!matchEnded && j.optInt("estado", 0) != 0) {
            matchEnded = true
            firing = false
            ui.postDelayed({ endMatch(j) }, 2500)
        }
    }

    private fun stopEngineMatch() {
        firing = false
        gl {
            NativeBridge.nativePlayerInput(0f, 0f, false)
            NativeBridge.nativePlayerAim(false)
            NativeBridge.nativePlayerCrouch(false)
            NativeBridge.nativeSetPlay(false)
            NativeBridge.nativeSetFirstPerson(false)
            overviewCamera()
        }
    }

    private fun showPause() {
        firing = false
        AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Pausa")
            .setItems(arrayOf("Continuar", "Sair da partida")) { _, which ->
                if (which == 1) { inMatch = false; stopEngineMatch(); showHome() }
            }.show()
    }

    private fun endMatch(j: JSONObject) {
        if (!inMatch) return
        inMatch = false
        stopEngineMatch()
        val score = j.optJSONArray("placar")
        val s0 = score?.optInt(0) ?: 0
        val s1 = score?.optInt(1) ?: 0
        val winner = if (s0 > s1) 0 else if (s1 > s0) 1 else -1
        val kills = j.optInt("abates", 0)
        val earned = kills * 25 + if (winner == chosenTeam) 150 else if (winner < 0) 80 else 50
        coins += earned

        val root = vbox().apply { setBackgroundColor(bg); setPadding(dp(26), dp(16), dp(26), dp(16)) }
        root.addView(hbox().apply {
            addView(vbox().apply {
                addView(label(if (winner < 0) "FIM DE JOGO" else "VITÓRIA DOS", 12f, if (winner == 1) yellowLight else blueLight, true).apply { letterSpacing = 0.15f })
                addView(title(if (winner < 0) "EMPATE" else if (winner == 0) "POLÍCIA" else "BANDIDOS", 50f, if (winner == 1) gold else 0xFF60A5FA.toInt()))
            }, lp(0, -2, 1f))
            addView(vbox().apply {
                gravity = Gravity.END
                addView(hbox().apply {
                    background = round(panel, 14f)
                    setPadding(dp(16), dp(4), dp(16), dp(4))
                    addView(title("$s0", 44f, 0xFF60A5FA.toInt()))
                    addView(label("   x   ", 16f, textDim))
                    addView(title("$s1", 44f, gold))
                })
                addView(label("+${fmt(earned)} moedas  (abates ${kills}, ${if (winner == chosenTeam) "vitória" else if (winner < 0) "empate" else "derrota"})", 13f, yellowLight, true).apply { setPadding(0, dp(6), 0, 0) })
            })
        })
        val tables = hbox().apply { setPadding(0, dp(10), 0, dp(10)) }
        val players = j.optJSONArray("jogadores") ?: JSONArray()
        for (team in 0..1) {
            val t = vbox().apply { background = round(if (team == 0) 0xFF0F1D36.toInt() else 0xFF2A1C08.toInt(), 14f); setPadding(dp(12), dp(8), dp(12), dp(8)) }
            t.addView(hbox().apply {
                addView(label(if (team == 0) "POLÍCIA" else "BANDIDOS", 11f, if (team == 0) blueLight else yellowLight, true), lp(0, -2, 1f))
                addView(label("ABATES   MORTES", 11f, textDim, true))
            })
            val list = (0 until players.length()).map { players.getJSONObject(it) }.filter { it.optInt("time") == team }.sortedByDescending { it.optInt("abates") }
            for (p in list) {
                val me = p.optBoolean("voce")
                t.addView(hbox().apply {
                    if (me) background = round(0x1FFFFFFF, 8f)
                    setPadding(dp(6), dp(4), dp(6), dp(4))
                    addView(label(p.optString("nome"), 13f, textMain, me), lp(0, -2, 1f))
                    addView(label("${p.optInt("abates")}", 13f, textMain, true).apply { gravity = Gravity.END }, lp(dp(56), -2))
                    addView(label("${p.optInt("mortes")}", 13f, textDim).apply { gravity = Gravity.END }, lp(dp(56), -2))
                })
            }
            tables.addView(t, lp(0, -2, 1f).apply { if (team == 0) rightMargin = dp(12) })
        }
        root.addView(ScrollView(this).apply { addView(tables) }, lp(-1, 0, 1f))
        root.addView(hbox().apply {
            gravity = Gravity.END
            addView(button("Menu inicial", 0x14FFFFFF, textMain) { showHome() }, lp(-2, dp(46)).apply { rightMargin = dp(10) })
            addView(button("JOGAR DE NOVO", gold, 0xFF111827.toInt()) { showSkins(chosenTeam) }, lp(-2, dp(46)))
        })
        show(root, "fim")
    }

    // ---------------- HUD em primeira pessoa ----------------
    inner class Hud {
        val root = FrameLayout(this@JogoActivity)
        private val hpBar = View(this@JogoActivity)
        private val hpText = label("100", 13f, textMain, true)
        private val kd = label("0 abates · 0 mortes", 12f, 0xFFE5E7EB.toInt())
        private val polScore = title("0", 26f, textMain)
        private val banScore = title("0", 26f, textMain)
        private val clock = label("10:00", 15f, textMain, true)
        private val goal = label("até 25 abates", 11f, textMain)
        private val feed = vbox().apply { gravity = Gravity.END }
        private val crosshair = Crosshair()
        private val slots = arrayOf(SlotView(), SlotView())
        private val nadeCount = label("2", 11f, 0xFF111827.toInt(), true)
        private val center = label("", 18f, textMain, true).apply {
            gravity = Gravity.CENTER; setShadowLayer(4f, 0f, 0f, Color.BLACK); setPadding(dp(16), dp(10), dp(16), dp(10))
        }
        private val hurt = View(this@JogoActivity).apply { setBackgroundColor(0x55DC2626); visibility = View.GONE }
        private val scope = ScopeView().apply { visibility = View.GONE }
        private val interact = button("", gold, 0xFF111827.toInt(), 13f) { gl { NativeBridge.nativeInteract() } }.apply { visibility = View.GONE }
        private val minimap = MiniMap()
        private val swapBtn = button("⇄", dark, textMain, 20f) { gl { NativeBridge.nativePlayerSwap() } }.apply {
            background = round(dark, 12f, 2, 0x80FFFFFF.toInt())
        }
        private var aiming = false
        private var crouching = false
        private var lastMsg = ""
        private var team = chosenTeam

        init {
            // fundo: arrastar em qualquer lugar livre gira o olhar
            root.addView(LookArea(), FrameLayout.LayoutParams(-1, -1))
            root.addView(scope, FrameLayout.LayoutParams(-1, -1))
            root.addView(hurt, FrameLayout.LayoutParams(-1, -1))
            root.addView(crosshair, FrameLayout.LayoutParams(dp(60), dp(60), Gravity.CENTER))

            // canto superior esquerdo: vida, abates, minimapa
            val tl = vbox()
            tl.addView(hbox().apply {
                background = round(dark, 10f)
                setPadding(dp(10), dp(6), dp(10), dp(6))
                addView(label("♥", 14f, 0xFFF87171.toInt(), true))
                val track = FrameLayout(context).apply { background = round(0x2EFFFFFF, 5f) }
                hpBar.background = round(0xFF4ADE80.toInt(), 5f)
                track.addView(hpBar, FrameLayout.LayoutParams(-1, -1))
                addView(track, lp(dp(120), dp(9)).apply { leftMargin = dp(8); rightMargin = dp(8) })
                addView(hpText)
            })
            tl.addView(kd.apply { background = round(dark, 9f); setPadding(dp(9), dp(4), dp(9), dp(4)) }, lp(-2, -2).apply { topMargin = dp(6) })
            tl.addView(minimap, lp(dp(84), dp(84)).apply { topMargin = dp(6) })
            root.addView(tl, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START).apply { leftMargin = dp(14); topMargin = dp(10) })

            // placar no alto, no centro
            val sc = vbox().apply { gravity = Gravity.CENTER_HORIZONTAL }
            sc.addView(hbox().apply {
                background = round(0xCC0B0F17.toInt(), 12f)
                addView(hbox().apply {
                    background = round(0xFF1D4ED8.toInt(), 12f); setPadding(dp(12), dp(2), dp(12), dp(2))
                    addView(label("POLÍCIA  ", 11f, textMain, true)); addView(polScore)
                })
                addView(clock.apply { setPadding(dp(12), 0, dp(12), 0) })
                addView(hbox().apply {
                    background = round(0xFFB45309.toInt(), 12f); setPadding(dp(12), dp(2), dp(12), dp(2))
                    addView(banScore); addView(label("  BANDIDOS", 11f, textMain, true))
                })
            })
            sc.addView(goal.apply { background = round(0x990B0F17.toInt(), 6f); setPadding(dp(8), dp(1), dp(8), dp(1)) }, lp(-2, -2).apply { topMargin = dp(3) })
            root.addView(sc, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply { topMargin = dp(8) })

            // abates recentes e pausa
            root.addView(feed, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.END).apply { topMargin = dp(10); rightMargin = dp(66) })
            root.addView(button("II", dark, textMain, 15f) { showPause() }, FrameLayout.LayoutParams(dp(44), dp(44), Gravity.TOP or Gravity.END).apply { topMargin = dp(10); rightMargin = dp(14) })

            root.addView(center, FrameLayout.LayoutParams(-2, -2, Gravity.CENTER).apply { topMargin = dp(90) })
            root.addView(interact, FrameLayout.LayoutParams(-2, dp(42), Gravity.CENTER).apply { topMargin = dp(150) })

            // armas (embaixo, no centro) e troca rápida
            val wr = hbox().apply { gravity = Gravity.BOTTOM }
            for ((i, s) in slots.withIndex()) {
                s.setOnClickListener { gl { if (currentSlot != i) NativeBridge.nativePlayerSwap() } }
                wr.addView(s, lp(dp(112), dp(58)).apply { rightMargin = dp(6) })
            }
            wr.addView(swapBtn, lp(dp(46), dp(46)))
            root.addView(wr, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply { bottomMargin = dp(10) })

            // joystick (corre sozinho com o dedo no máximo para a frente) e o tiro do lado esquerdo
            val stick = JoystickView(this@JogoActivity) { x, y -> gl { NativeBridge.nativePlayerInput(x, y, y > 0.93f) } }
            root.addView(stick, FrameLayout.LayoutParams(dp(130), dp(130), Gravity.BOTTOM or Gravity.START).apply { leftMargin = dp(26); bottomMargin = dp(16) })
            root.addView(FireButton(dp(56)), FrameLayout.LayoutParams(dp(56), dp(56), Gravity.START or Gravity.CENTER_VERTICAL).apply { leftMargin = dp(46) })

            // lado direito: atirar, mirar, pular, agachar, granada e recarregar
            val pad = FrameLayout(this@JogoActivity)
            fun put(v: View, size: Int, right: Int, bottom: Int) = pad.addView(v, FrameLayout.LayoutParams(dp(size), dp(size), Gravity.BOTTOM or Gravity.END).apply { rightMargin = dp(right); bottomMargin = dp(bottom) })
            put(FireButton(dp(84)), 84, 8, 34)
            val aim = roundButton("MIRA", 0xA60B0F17.toInt()) {}
            aim.setOnClickListener {
                aiming = !aiming
                aim.background = round(if (aiming) 0x59FBBF24 else 0xA60B0F17.toInt(), 27f, 2, if (aiming) gold else 0x8CFFFFFF.toInt())
                gl { NativeBridge.nativePlayerAim(aiming) }
            }
            put(aim, 54, 104, 38)
            put(roundButton("▲\nPULAR", 0xA60B0F17.toInt()) { gl { NativeBridge.nativePlayerJump() } }, 54, 2, 132)
            val crouch = roundButton("▼\nAGACHAR", 0xA60B0F17.toInt()) {}
            crouch.setOnClickListener {
                crouching = !crouching
                crouch.background = round(if (crouching) 0x59FBBF24 else 0xA60B0F17.toInt(), 27f, 2, if (crouching) gold else 0x8CFFFFFF.toInt())
                gl { NativeBridge.nativePlayerCrouch(crouching) }
            }
            put(crouch, 54, 66, 118)
            val nade = FrameLayout(this@JogoActivity)
            nade.addView(roundButton("💣", 0xA60B0F17.toInt(), yellowLight) { gl { NativeBridge.nativePlayerGrenade() } }, FrameLayout.LayoutParams(-1, -1))
            nade.addView(nadeCount.apply { gravity = Gravity.CENTER; background = round(gold, 10f) }, FrameLayout.LayoutParams(dp(20), dp(20), Gravity.TOP or Gravity.END))
            put(nade, 54, 130, 108)
            put(roundButton("⟳", 0xA60B0F17.toInt()) { gl { NativeBridge.nativePlayerReload() } }, 48, 172, 44)
            root.addView(pad, FrameLayout.LayoutParams(dp(250), dp(250), Gravity.BOTTOM or Gravity.END).apply { rightMargin = dp(14); bottomMargin = dp(12) })
        }

        private var currentSlot = 0

        private fun roundButton(text: String, fill: Int, stroke: Int = 0x8CFFFFFF.toInt(), onClick: () -> Unit) = TextView(this@JogoActivity).apply {
            this.text = text
            setTextColor(textMain)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER
            background = round(fill, 27f, 2, stroke)
            isClickable = true
            setOnClickListener { onClick() }
        }

        fun update(j: JSONObject) {
            team = j.optInt("time", team)
            val hp = j.optInt("vida", 100)
            val maxHp = max(1, j.optInt("vidaMax", 100))
            (hpBar.layoutParams as FrameLayout.LayoutParams).apply { width = (hpBar.parent as View).width * hp / maxHp; hpBar.layoutParams = this }
            hpBar.background = round(if (hp > 50) 0xFF4ADE80.toInt() else if (hp > 25) 0xFFFBBF24.toInt() else 0xFFF87171.toInt(), 5f)
            hpText.text = "$hp"
            kd.text = "${j.optInt("abates")} abates · ${j.optInt("mortes")} mortes"
            val sc = j.optJSONArray("placar")
            polScore.text = "${sc?.optInt(0) ?: 0}"
            banScore.text = "${sc?.optInt(1) ?: 0}"
            val t = j.optInt("tempo", -1)
            clock.text = if (t < 0) "--:--" else String.format("%d:%02d", t / 60, t % 60)
            goal.text = "até ${j.optInt("meta", 25)} abates"
            nadeCount.text = "${j.optInt("granadas", 0)}"
            currentSlot = j.optInt("slot", 0)
            val arms = j.optJSONArray("armas")
            swapBtn.visibility = if ((arms?.length() ?: 0) > 1) View.VISIBLE else View.GONE
            for (i in 0..1) {
                val a = arms?.optJSONObject(i)
                slots[i].visibility = if (a == null) View.GONE else View.VISIBLE
                if (a != null) slots[i].set(a, i == currentSlot, j.optBoolean("recarregando") && i == currentSlot)
            }
            val ads = j.optBoolean("mirando")
            val scoped = ads && j.optBoolean("luneta")
            scope.visibility = if (scoped) View.VISIBLE else View.GONE
            // mirando: a cruz vira uma bolinha no ponto exato do tiro (na luneta, a própria luneta tem o ponto)
            crosshair.visibility = if (scoped) View.INVISIBLE else View.VISIBLE
            crosshair.aiming = ads
            crosshair.hit = j.optBoolean("acerto")
            crosshair.head = j.optBoolean("cabeca")
            crosshair.invalidate()
            hurt.visibility = if (j.optBoolean("dano")) View.VISIBLE else View.GONE
            val dead = j.optBoolean("morto")
            center.text = if (dead) "Você foi abatido\nRenasce em ${j.optInt("renasce")} s"
                          else if (j.optInt("estado", 0) != 0) j.optString("msg") else ""
            center.background = if (center.text.isEmpty()) null else round(0xB30B0F17.toInt(), 12f)
            val ia = j.optString("interagir")
            interact.visibility = if (ia.isEmpty() || dead) View.GONE else View.VISIBLE
            interact.text = ia
            // abates recentes
            val list = j.optJSONArray("abatidos") ?: JSONArray()
            val sig = list.toString()
            if (sig != lastMsg) {
                lastMsg = sig
                feed.removeAllViews()
                for (k in max(0, list.length() - 4) until list.length()) {
                    val f = list.getJSONObject(k)
                    val line = TextView(this@JogoActivity).apply {
                        setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                        background = round(if (f.optBoolean("voce")) 0xD91F2937.toInt() else dark, 7f)
                        setPadding(dp(9), dp(3), dp(9), dp(3))
                        val a = f.optString("a")
                        val tc = { tm: Int -> if (tm == 0) "#93C5FD" else "#FCD34D" }
                        val html = if (a.isEmpty()) "<b><font color='${tc(f.optInt("vt"))}'>${f.optString("v")}</font></b> caiu"
                                   else "<b><font color='${tc(f.optInt("at"))}'>$a</font></b> abateu <b><font color='${tc(f.optInt("vt"))}'>${f.optString("v")}</font></b> · ${f.optString("arma")}"
                        @Suppress("DEPRECATION")
                        text = android.text.Html.fromHtml(html)
                        setTextColor(textMain)
                    }
                    feed.addView(line, lp(-2, -2).apply { bottomMargin = dp(4) })
                }
            }
            minimap.points = j.optJSONArray("mapa") ?: JSONArray()
            minimap.myTeam = team
            minimap.invalidate()
        }

        // Fundo do HUD: arrastar gira o olhar (cada dedo separado)
        inner class LookArea : View(this@JogoActivity) {
            private val last = HashMap<Int, Pair<Float, Float>>()
            override fun onTouchEvent(e: MotionEvent): Boolean {
                when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                        val i = e.actionIndex
                        last[e.getPointerId(i)] = e.getX(i) to e.getY(i)
                    }
                    MotionEvent.ACTION_MOVE -> for (i in 0 until e.pointerCount) {
                        val id = e.getPointerId(i)
                        val p = last[id] ?: continue
                        look(e.getX(i) - p.first, e.getY(i) - p.second)
                        last[id] = e.getX(i) to e.getY(i)
                    }
                    MotionEvent.ACTION_POINTER_UP -> last.remove(e.getPointerId(e.actionIndex))
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> last.clear()
                }
                return true
            }
        }

        // Botão de tiro: segurar atira (automático); arrastar com o mesmo dedo também mira
        inner class FireButton(size: Int) : TextView(this@JogoActivity) {
            private var lx = 0f
            private var ly = 0f
            init {
                text = "✛"
                setTextColor(Color.WHITE)
                setTextSize(TypedValue.COMPLEX_UNIT_PX, size * 0.42f)
                gravity = Gravity.CENTER
                background = round(0xD9DC2626.toInt(), size / 2f / resources.displayMetrics.density, 3, 0xBFFFFFFF.toInt())
            }
            override fun onTouchEvent(e: MotionEvent): Boolean {
                when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN -> { firing = true; lx = e.rawX; ly = e.rawY; alpha = 0.7f }
                    MotionEvent.ACTION_MOVE -> { look(e.rawX - lx, e.rawY - ly); lx = e.rawX; ly = e.rawY }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { firing = false; alpha = 1f }
                }
                return true
            }
        }

        inner class SlotView : LinearLayout(this@JogoActivity) {
            private val img = ImageView(this@JogoActivity).apply { scaleType = ImageView.ScaleType.FIT_CENTER }
            private val ammo = label("", 11f, textMain, true)
            private var key = ""
            init {
                orientation = VERTICAL
                gravity = Gravity.CENTER
                addView(img, lp(dp(92), dp(32)))
                addView(ammo)
                isClickable = true
            }
            fun set(a: JSONObject, on: Boolean, reloading: Boolean) {
                val k = a.optString("arma")
                if (k != key) { key = k; img.setImageBitmap(image("armas/$k.png")) }
                val res = a.optInt("reserva", -1)
                ammo.text = if (reloading) "recarregando…" else "${a.optInt("balas")} / ${if (res < 0) "∞" else res}"
                ammo.setTextColor(if (a.optInt("balas") == 0 && !reloading) 0xFFF87171.toInt() else textMain)
                background = round(dark, 12f, 2, if (on) gold else 0x40FFFFFF)
                alpha = if (on) 1f else 0.75f
            }
        }
    }

    // arrastar 100 dp gira uns 50° (igual em qualquer tela)
    private fun look(dx: Float, dy: Float) {
        val k = 2.2f / resources.displayMetrics.density
        synchronized(lookLock) { lookDx += dx * k; lookDy += dy * k }
    }

    // Mira no centro: cruz sem mirar; mirando, uma bolinha onde a bala vai. Marca de acerto (cabeça: maior e vermelha)
    inner class Crosshair : View(this) {
        var hit = false
        var head = false
        var aiming = false
        private val dot = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
        private val p = Paint(Paint.ANTI_ALIAS_FLAG).apply { strokeWidth = dpf(2f); style = Paint.Style.STROKE }
        override fun onDraw(c: Canvas) {
            val cx = width / 2f
            val cy = height / 2f
            val g = dpf(5f)
            val l = dpf(9f)
            p.color = Color.WHITE
            p.setShadowLayer(2f, 0f, 0f, Color.BLACK)
            if (aiming) {
                dot.color = 0xFF000000.toInt(); c.drawCircle(cx, cy, dpf(3.6f), dot)   // contorno escuro: aparece em céu e parede clara
                dot.color = 0xFFFF2A1F.toInt(); c.drawCircle(cx, cy, dpf(2.6f), dot)
            } else {
                c.drawLine(cx, cy - g - l, cx, cy - g, p); c.drawLine(cx, cy + g, cx, cy + g + l, p)
                c.drawLine(cx - g - l, cy, cx - g, cy, p); c.drawLine(cx + g, cy, cx + g + l, cy, p)
                c.drawCircle(cx, cy, dpf(1.2f), p)
            }
            if (hit) {
                p.color = if (head) 0xFFFF3B30.toInt() else 0xFFFFFFFF.toInt()
                val a = dpf(if (head) 13f else 10f)
                val b = dpf(5f)
                c.drawLine(cx - a, cy - a, cx - b, cy - b, p); c.drawLine(cx + a, cy - a, cx + b, cy - b, p)
                c.drawLine(cx - a, cy + a, cx - b, cy + b, p); c.drawLine(cx + a, cy + a, cx + b, cy + b, p)
            }
        }
    }

    // Tela da luneta: preto em volta do vidro, anel do tubo, cruz fina com o ponto vermelho no centro e uma
    // leve sombra na borda do vidro (o balanço da mira vem do motor: a imagem inteira se mexe devagar)
    inner class ScopeView : View(this) {
        private val black = Paint().apply { color = Color.BLACK }
        private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF1A1A1A.toInt(); style = Paint.Style.STROKE }
        private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xF0101010.toInt(); strokeWidth = dpf(1.6f) }
        private val thick = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xF0101010.toInt(); strokeWidth = dpf(4f) }
        private val dot = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFF2A1F.toInt() }
        private val glow = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x55FF2A1F }
        private val path = android.graphics.Path()
        override fun onDraw(c: Canvas) {
            val cx = width / 2f
            val cy = height / 2f
            val r = min(width, height) * 0.47f
            path.reset()
            path.fillType = android.graphics.Path.FillType.EVEN_ODD
            path.addRect(0f, 0f, width.toFloat(), height.toFloat(), android.graphics.Path.Direction.CW)
            path.addCircle(cx, cy, r, android.graphics.Path.Direction.CW)
            c.drawPath(path, black)
            ring.strokeWidth = r * 0.09f
            ring.shader = android.graphics.RadialGradient(cx, cy, r, intArrayOf(0x00000000, 0x00000000, 0xCC000000.toInt()), floatArrayOf(0f, 0.86f, 1f), android.graphics.Shader.TileMode.CLAMP)
            c.drawCircle(cx, cy, r - ring.strokeWidth / 2f, ring)
            // cruz: grossa perto da borda, fina no meio (com um vão em volta do ponto)
            val gap = r * 0.06f
            c.drawLine(cx - r, cy, cx - r * 0.45f, cy, thick); c.drawLine(cx + r * 0.45f, cy, cx + r, cy, thick)
            c.drawLine(cx, cy + r * 0.45f, cx, cy + r, thick)
            c.drawLine(cx - r * 0.45f, cy, cx - gap, cy, line); c.drawLine(cx + gap, cy, cx + r * 0.45f, cy, line)
            c.drawLine(cx, cy + gap, cx, cy + r * 0.45f, line); c.drawLine(cx, cy - r * 0.3f, cx, cy - gap, line)
            // marquinhas de distância embaixo
            for (k in 1..3) { val y = cy + r * 0.12f * k; c.drawLine(cx - r * 0.03f, y, cx + r * 0.03f, y, line) }
            c.drawCircle(cx, cy, dpf(5f), glow)
            c.drawCircle(cx, cy, dpf(2.6f), dot)
        }
    }

    // Minimapa: você no centro (frente para cima), companheiros em azul/amarelo, inimigo que atirou em vermelho
    inner class MiniMap : View(this) {
        var points = JSONArray()
        var myTeam = 0
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        override fun onDraw(c: Canvas) {
            val cx = width / 2f
            val cy = height / 2f
            val r = min(cx, cy)
            p.style = Paint.Style.FILL; p.color = 0xB80B0F17.toInt()
            c.drawCircle(cx, cy, r, p)
            p.style = Paint.Style.STROKE; p.strokeWidth = dpf(2f); p.color = 0x40FFFFFF
            c.drawCircle(cx, cy, r - dpf(1f), p)
            p.style = Paint.Style.FILL
            val k = (r - dpf(5f)) / 45f
            for (i in 0 until points.length()) {
                val a = points.optJSONArray(i) ?: continue
                val enemy = a.optInt(2) == 1
                p.color = if (enemy) 0xFFF87171.toInt() else if (myTeam == 0) 0xFF60A5FA.toInt() else 0xFFFBBF24.toInt()
                c.drawCircle(cx + a.optDouble(0).toFloat() * k, cy - a.optDouble(1).toFloat() * k, dpf(3f), p)
            }
            p.color = Color.WHITE
            val tri = android.graphics.Path().apply {
                moveTo(cx, cy - dpf(6f)); lineTo(cx - dpf(4f), cy + dpf(4f)); lineTo(cx + dpf(4f), cy + dpf(4f)); close()
            }
            c.drawPath(tri, p)
        }
    }
}
