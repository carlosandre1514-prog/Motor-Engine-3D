package com.mobile3d

import org.json.JSONArray
import org.json.JSONObject

// Sala do multiplayer no Firebase (salas/<CÓDIGO>):
//   host: quem criou (comanda os robôs) · estado: "espera" | "jogo" · jogadores/<id>: {nome, time, skin, arma, arma2}
//   est/<id>: posição e estado de cada lutador (10 vezes por segundo) · ev/<chave>: dano, abate, granada
// Até 10 pessoas; quem entra escolhe o lado e a skin na sala de espera; quem criou aperta Iniciar.
class OnlineRoom private constructor(val fb: Firebase, val code: String, val isHost: Boolean) {
    private val path = "salas/$code"
    @Volatile var closed = false
        private set
    @Volatile private var syncing = false
    private val players = LinkedHashMap<String, JSONObject>()
    @Volatile var estado = "espera"
        private set

    // avisos para a tela (rodam na thread da rede: a tela passa para a thread da interface)
    var onPlayers: ((List<Pair<String, JSONObject>>) -> Unit)? = null
    var onEstado: ((String) -> Unit)? = null
    var onGone: (() -> Unit)? = null
    var onLeft: ((String) -> Unit)? = null  // alguém saiu durante a partida

    companion object {
        const val MAX = 10
        private const val LETTERS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"  // sem 0/O, 1/I/L (fácil de ditar)

        fun newCode() = (1..6).map { LETTERS[(Math.random() * LETTERS.length).toInt()] }.joinToString("")

        // Cria a sala com um código que ainda não existe; devolve a sala ou o motivo do erro
        fun create(fb: Firebase, me: JSONObject): Pair<OnlineRoom?, String> {
            repeat(6) {
                val code = newCode()
                val g = fb.get("salas/$code")
                if (!g.ok) return null to "o Firebase recusou (${g.code}). Veja as regras do banco."
                if (g.body.trim() != "null") return@repeat
                val room = JSONObject()
                    .put("host", fb.uid)
                    .put("estado", "espera")
                    .put("criada", JSONObject().put(".sv", "timestamp"))
                    .put("jogadores", JSONObject().put(fb.uid, me))
                val r = fb.put("salas/$code", room.toString())
                if (!r.ok) return null to "não consegui criar a sala (${r.code})"
                return OnlineRoom(fb, code, true) to ""
            }
            return null to "não achei um código livre, tente de novo"
        }

        fun join(fb: Firebase, code: String, me: JSONObject): Pair<OnlineRoom?, String> {
            val c = code.trim().uppercase()
            val g = fb.get("salas/$c")
            if (!g.ok) return null to "o Firebase recusou (${g.code})"
            if (g.body.trim() == "null") return null to "não existe sala com o código $c"
            val room = JSONObject(g.body)
            if (room.optString("estado") != "espera") return null to "essa sala já está jogando"
            val js = room.optJSONObject("jogadores") ?: JSONObject()
            if (js.length() >= MAX && !js.has(fb.uid)) return null to "a sala está cheia (10 pessoas)"
            val r = fb.put("salas/$c/jogadores/${fb.uid}", me.toString())
            if (!r.ok) return null to "não consegui entrar (${r.code})"
            return OnlineRoom(fb, c, room.optString("host") == fb.uid) to ""
        }
    }

    // ---------------- sala de espera ----------------
    fun listenRoom() {
        fb.listen("$path/jogadores", { closed }) { ev, sub, data ->
            synchronized(players) {
                if (sub == "/") {
                    if (ev == "put") players.clear()
                    if (data is JSONObject) for (k in data.keys()) {
                        val v = data.opt(k)
                        if (v is JSONObject) players[k] = v else players.remove(k)
                    }
                } else {
                    val id = sub.trim('/').substringBefore('/')
                    if (sub.trim('/').contains('/')) {  // mudou um campo só
                        val field = sub.trim('/').substringAfter('/')
                        players[id]?.put(field, data ?: JSONObject.NULL)
                    } else if (data is JSONObject) {
                        val old = players[id]
                        if (ev == "patch" && old != null) for (k in data.keys()) old.put(k, data.opt(k))  // só os campos mudados
                        else players[id] = data
                    } else {
                        players.remove(id)
                        if (estado == "jogo") onLeft?.invoke(id)
                    }
                }
                Unit
            }
            onPlayers?.invoke(snapshot())
        }
        fb.listen("$path/estado", { closed }) { _, _, data ->
            if (data == null) { closed = true; onGone?.invoke(); return@listen }
            val s = data.toString()
            if (s != estado) { estado = s; onEstado?.invoke(s) }
        }
    }

    fun snapshot(): List<Pair<String, JSONObject>> = synchronized(players) { players.entries.map { it.key to JSONObject(it.value.toString()) } }

    // Lista dos jogadores para o motor: [{id, nome, time, skin, arma}]
    fun roster(): String {
        val a = JSONArray()
        for ((id, p) in snapshot())
            a.put(JSONObject().put("id", id).put("nome", p.optString("nome", "Jogador")).put("time", p.optInt("time"))
                    .put("skin", p.optInt("skin")).put("arma", p.optString("arma")))
        return a.toString()
    }

    fun updateMe(me: JSONObject) = fb.patch("$path/jogadores/${fb.uid}", me.toString())

    // Quem criou: limpa o que sobrou da partida anterior e começa
    fun start(): Boolean {
        fb.delete("$path/est"); fb.delete("$path/ev")
        return fb.patch(path, JSONObject().put("estado", "jogo").put("inicio", JSONObject().put(".sv", "timestamp")).toString()).ok
    }

    fun backToLobby() {
        if (!isHost) return
        fb.delete("$path/est"); fb.delete("$path/ev")
        fb.patch(path, JSONObject().put("estado", "espera").toString())
    }

    // Sair: quem criou fecha a sala para todos; os outros só saem da lista
    fun leave() {
        syncing = false
        if (isHost) fb.delete(path) else fb.delete("$path/jogadores/${fb.uid}")
        closed = true
    }

    // ---------------- durante a partida ----------------
    // collect(): pega no motor (thread GL) o estado dos meus lutadores e os eventos a enviar.
    // apply(states) e event(ev): entregam ao motor o que chegou dos outros celulares.
    fun startSync(collect: () -> Pair<String, String>?, apply: (String) -> Unit, event: (String) -> Unit) {
        syncing = true
        val stop = { !syncing || closed }
        fb.listen("$path/est", stop) { ev, sub, data ->
            val out = JSONObject()
            if (sub == "/") {
                if (data is JSONObject) for (k in data.keys()) data.optJSONObject(k)?.let { out.put(k, it) }
            } else {
                val id = sub.trim('/').substringBefore('/')
                if (!sub.trim('/').contains('/') && data is JSONObject) out.put(id, data)
            }
            if (out.length() > 0 && ev.isNotEmpty()) apply(out.toString())
        }
        val seen = HashSet<String>()
        fb.listen("$path/ev", stop) { _, sub, data ->
            val items = ArrayList<Pair<String, JSONObject>>()
            if (sub == "/") {
                if (data is JSONObject) for (k in data.keys().asSequence().sorted()) data.optJSONObject(k)?.let { items.add(k to it) }
            } else if (data is JSONObject) items.add(sub.trim('/') to data)
            for ((k, v) in items) {
                if (!seen.add(k)) continue
                if (v.optString("cel") == fb.uid) continue  // o meu próprio aviso
                event(v.toString())
            }
        }
        Thread {
            while (!stop()) {
                val t0 = System.currentTimeMillis()
                val got = try { collect() } catch (_: Exception) { null }
                if (got != null) {
                    val (states, events) = got
                    try {
                        if (states.length > 2) fb.patch("$path/est", states)
                        val arr = JSONArray(events)
                        for (i in 0 until arr.length()) fb.post("$path/ev", arr.getJSONObject(i).put("cel", fb.uid).toString())
                    } catch (_: Exception) {}
                }
                val wait = 100 - (System.currentTimeMillis() - t0)  // 10 vezes por segundo
                if (wait > 0) try { Thread.sleep(wait) } catch (_: InterruptedException) { break }
            }
        }.apply { isDaemon = true; start() }
    }

    fun stopSync() { syncing = false }
}
