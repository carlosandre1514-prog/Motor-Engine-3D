package com.mobile3d

import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

// Firebase Realtime Database pela API REST (sem SDK): ler/gravar com GET/PUT/PATCH/POST/DELETE e acompanhar
// mudanças ao vivo com streaming (text/event-stream). Login anônimo pela API do Firebase Auth; sem ele ligado no
// console, entra sem token (só funciona se as regras do banco deixarem).
// A configuração (apiKey e databaseURL) é colada pelo jogador no celular e fica só no aparelho.
class Firebase(private val apiKey: String, dbUrl: String) {
    private val db = dbUrl.trim().trimEnd('/')
    @Volatile private var token: String? = null
    private var refresh: String? = null
    private var expiresAt = 0L
    var uid: String = ""
        private set
    var anonymous = false
        private set

    companion object {
        // Aceita o trecho de código do console (const firebaseConfig = {...}) ou o JSON
        fun parseConfig(text: String): Pair<String, String>? {
            val key = Regex("apiKey\"?\\s*[:=]\\s*[\"']([^\"']+)[\"']").find(text)?.groupValues?.get(1)
            val url = Regex("databaseURL\"?\\s*[:=]\\s*[\"']([^\"']+)[\"']").find(text)?.groupValues?.get(1)
            return if (key != null && url != null && url.startsWith("https://")) key to url else null
        }

        fun randomId(n: Int = 20): String {
            val a = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
            return (1..n).map { a[(Math.random() * a.length).toInt()] }.joinToString("")
        }
    }

    class Reply(val code: Int, val body: String) {
        val ok get() = code in 200..299
    }

    private fun request(method: String, url: String, body: String?, contentType: String = "application/json"): Reply {
        val c = URL(url).openConnection() as HttpURLConnection
        try {
            c.connectTimeout = 10_000
            c.readTimeout = 15_000
            // HttpURLConnection não tem PATCH: o Firebase aceita POST com X-HTTP-Method-Override
            if (method == "PATCH") { c.requestMethod = "POST"; c.setRequestProperty("X-HTTP-Method-Override", "PATCH") }
            else c.requestMethod = method
            if (body != null) {
                c.doOutput = true
                c.setRequestProperty("Content-Type", "$contentType; charset=utf-8")
                c.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            }
            val code = c.responseCode
            val stream = if (code in 200..299) c.inputStream else c.errorStream
            val text = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() } ?: ""
            return Reply(code, text)
        } finally {
            c.disconnect()
        }
    }

    // Login anônimo; devolve null se deu certo ou o motivo de ter entrado sem login
    fun signIn(fallbackUid: String): String? {
        return try {
            val r = request("POST", "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=$apiKey", "{\"returnSecureToken\":true}")
            if (r.ok) {
                val j = JSONObject(r.body)
                token = j.getString("idToken")
                refresh = j.optString("refreshToken")
                uid = j.getString("localId")
                expiresAt = System.currentTimeMillis() + j.optString("expiresIn", "3600").toLong() * 1000L - 300_000L
                anonymous = true
                null
            } else {
                uid = fallbackUid
                token = null
                anonymous = false
                "login anônimo desligado no Firebase (${r.code})"
            }
        } catch (e: Exception) {
            uid = fallbackUid
            "sem conexão com o Firebase: ${e.message}"
        }
    }

    // O token do login vale 1 hora: renova antes de vencer
    @Synchronized private fun freshToken(): String? {
        val r = refresh
        if (r != null && System.currentTimeMillis() > expiresAt) {
            try {
                val rep = request("POST", "https://securetoken.googleapis.com/v1/token?key=$apiKey",
                                  "grant_type=refresh_token&refresh_token=" + URLEncoder.encode(r, "UTF-8"), "application/x-www-form-urlencoded")
                if (rep.ok) {
                    val j = JSONObject(rep.body)
                    token = j.getString("id_token")
                    refresh = j.optString("refresh_token", r)
                    expiresAt = System.currentTimeMillis() + j.optString("expires_in", "3600").toLong() * 1000L - 300_000L
                }
            } catch (_: Exception) {}
        }
        return token
    }

    private fun url(path: String): String {
        val t = freshToken()
        return "$db/$path.json" + if (t != null) "?auth=" + URLEncoder.encode(t, "UTF-8") else ""
    }

    fun get(path: String): Reply = request("GET", url(path), null)
    fun put(path: String, json: String): Reply = request("PUT", url(path), json)
    fun patch(path: String, json: String): Reply = request("PATCH", url(path), json)
    fun post(path: String, json: String): Reply = request("POST", url(path), json)
    fun delete(path: String): Reply = request("DELETE", url(path), null)

    // Acompanha um caminho ao vivo. onChange(evento "put"/"patch", subcaminho, dado) roda na thread do stream.
    // Cai a conexão: tenta de novo em 1 s até stop() dizer para parar. Devolve a thread (já rodando).
    fun listen(path: String, stop: () -> Boolean, onChange: (String, String, Any?) -> Unit): Thread {
        val t = Thread {
            while (!stop()) {
                var c: HttpURLConnection? = null
                try {
                    c = URL(url(path)).openConnection() as HttpURLConnection
                    c.setRequestProperty("Accept", "text/event-stream")
                    c.connectTimeout = 10_000
                    c.readTimeout = 70_000  // o Firebase manda keep-alive a cada 30 s
                    c.instanceFollowRedirects = true
                    val reader = BufferedReader(InputStreamReader(c.inputStream, Charsets.UTF_8))
                    var event = ""
                    while (!stop()) {
                        val line = reader.readLine() ?: break
                        when {
                            line.startsWith("event:") -> event = line.substring(6).trim()
                            line.startsWith("data:") -> {
                                val data = line.substring(5).trim()
                                if (event == "put" || event == "patch") {
                                    val j = JSONObject(data)
                                    val v = j.opt("data")
                                    onChange(event, j.optString("path", "/"), if (v == JSONObject.NULL) null else v)
                                } else if (event == "cancel" || event == "auth_revoked") break
                            }
                        }
                    }
                } catch (_: Exception) {
                } finally {
                    try { c?.disconnect() } catch (_: Exception) {}
                }
                if (!stop()) try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
            }
        }
        t.isDaemon = true
        t.start()
        return t
    }
}
