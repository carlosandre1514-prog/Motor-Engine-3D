package com.mobile3d

import java.net.HttpURLConnection
import java.net.URL
import org.json.JSONArray
import org.json.JSONObject

// Outras IAs: qualquer API no formato "chat completions" com ferramentas (padrão da OpenAI).
// Serve para OpenAI, Google Gemini, DeepSeek, Groq, OpenRouter, Mistral, servidores locais (LM Studio, Ollama)...
class OpenAiAgent(
    private val baseUrl: String,          // ex.: https://api.openai.com/v1
    private val apiKey: String,
    private val model: String,
    private val runTool: (name: String, inputJson: String) -> String,
) : GameAgent {
    private val history = ArrayList<JSONObject>()
    @Volatile override var cancelled = false

    override fun reset() = synchronized(history) { history.clear() }

    private val tools: JSONArray by lazy {
        JSONArray().apply {
            for (t in Assistant.SPECS) put(JSONObject().put("type", "function").put("function", JSONObject()
                .put("name", t.name).put("description", t.description)
                .put("parameters", JSONObject().put("type", "object").put("properties", JSONObject(t.properties))
                    .put("required", JSONArray(t.required)))))
        }
    }

    private fun post(body: JSONObject): JSONObject {
        val url = baseUrl.trimEnd('/') + "/chat/completions"
        val c = URL(url).openConnection() as HttpURLConnection
        c.requestMethod = "POST"
        c.connectTimeout = 20000
        c.readTimeout = 300000
        c.doOutput = true
        c.setRequestProperty("Content-Type", "application/json")
        if (apiKey.isNotBlank()) c.setRequestProperty("Authorization", "Bearer $apiKey")
        c.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
        val code = c.responseCode
        val text = (if (code in 200..299) c.inputStream else c.errorStream)?.bufferedReader()?.readText() ?: ""
        if (code !in 200..299) throw RuntimeException("HTTP $code: ${text.take(400)}")
        return JSONObject(text)
    }

    override fun send(userText: String, onText: (String) -> Unit) {
        cancelled = false
        synchronized(history) {
            history.add(JSONObject().put("role", "user").put("content", userText))
            repeat(40) {
                if (cancelled) { onText("⏹ Parado."); return }
                val msgs = JSONArray().put(JSONObject().put("role", "system").put("content", Assistant.SYSTEM))
                history.forEach { msgs.put(it) }
                val body = JSONObject().put("model", model).put("messages", msgs).put("tools", tools)
                val resp = post(body)
                val choice = resp.getJSONArray("choices").getJSONObject(0)
                val msg = choice.getJSONObject("message")
                history.add(msg)
                val text = msg.optString("content", "")
                if (text.isNotBlank() && text != "null") onText(text)
                val calls = msg.optJSONArray("tool_calls")
                if (calls == null || calls.length() == 0) {
                    if (choice.optString("finish_reason") == "length") onText("(resposta cortada: peça para continuar)")
                    return
                }
                for (i in 0 until calls.length()) {
                    val call = calls.getJSONObject(i)
                    val fn = call.getJSONObject("function")
                    val name = fn.getString("name")
                    onText("🔧 ${Assistant.TOOL_LABELS[name] ?: name}")
                    val out = try { runTool(name, fn.optString("arguments", "{}").ifBlank { "{}" }) }
                              catch (e: Exception) { "{\"erro\":\"${e.message}\"}" }
                    history.add(JSONObject().put("role", "tool").put("tool_call_id", call.optString("id")).put("content", out))
                }
            }
            onText("(parei depois de muitas etapas; peça para continuar se faltar algo)")
        }
    }

    companion object {
        // Endereços prontos (o modelo é digitado pelo usuário, conforme a conta dele)
        val PRESETS = arrayOf(
            "OpenAI" to "https://api.openai.com/v1",
            "Google Gemini" to "https://generativelanguage.googleapis.com/v1beta/openai",
            "DeepSeek" to "https://api.deepseek.com/v1",
            "Groq" to "https://api.groq.com/openai/v1",
            "OpenRouter (vários modelos)" to "https://openrouter.ai/api/v1",
            "Mistral" to "https://api.mistral.ai/v1",
        )
    }
}
