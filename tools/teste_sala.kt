// Teste do multiplayer sem internet: dois "celulares" (Firebase.kt + OnlineRoom.kt) numa sala do Firebase de mentira.
// 1) python3 tools/firebase_falso.py 8765 &
// 2) compile com kotlinc junto de app/src/jogo/java/com/mobile3d/{Firebase,OnlineRoom}.kt (org.json no classpath)
// 3) rode TesteSalaKt: imprime o que cada lado viu (sala, estados 10x/s, dano/abate, sair, sala fechando)
import com.mobile3d.Firebase
import com.mobile3d.OnlineRoom
import org.json.JSONObject

// Dois "celulares" na mesma sala do Firebase de mentira: cria, entra, começa, troca estado e eventos, sai
fun main() {
    val url = "http://127.0.0.1:8765"
    val a = Firebase("chave", url); println("A login: " + a.signIn("celA"))
    val b = Firebase("chave", url); println("B login: " + b.signIn("celB"))
    val (ra, ea) = OnlineRoom.create(a, JSONObject().put("nome", "Ana").put("time", 0).put("skin", 0).put("arma", "m4a1"))
    println("A criou: " + (ra?.code ?: ea))
    val (rbBad, eBad) = OnlineRoom.join(b, "ZZZZZZ", JSONObject().put("nome", "Beto"))
    println("B código errado: " + (rbBad?.code ?: eBad))
    val (rb, eb) = OnlineRoom.join(b, ra!!.code, JSONObject().put("nome", "Beto").put("time", 1).put("skin", 2).put("arma", "ak47"))
    println("B entrou: " + (rb?.code ?: eb) + " host? " + rb?.isHost)
    var bStarted = false
    ra.listenRoom(); rb!!.listenRoom()
    rb.onEstado = { st -> println("B viu estado: $st"); if (st == "jogo") bStarted = true }
    var gone = false
    rb.onGone = { gone = true }
    Thread.sleep(800)
    println("A vê na sala: " + ra.snapshot().map { it.second.optString("nome") + "/" + it.second.optInt("time") })
    rb.updateMe(JSONObject().put("time", 0))
    Thread.sleep(500)
    println("B mudou de lado, A vê: " + ra.snapshot().map { it.second.optString("nome") + "/" + it.second.optInt("time") })
    println("roster: " + ra.roster())
    println("A inicia: " + ra.start())
    Thread.sleep(800)
    println("B começou? $bStarted")
    val gotA = ArrayList<String>(); val gotB = ArrayList<String>()
    val evA = ArrayList<String>(); val evB = ArrayList<String>()
    var nA = 0; var nB = 0
    ra.startSync({ nA++; "{\"celA\":{\"p\":[1,2,$nA]},\"b0_2\":{\"p\":[5,5,5]}}" to (if (nA == 3) "[{\"t\":\"dano\",\"alvo\":\"celB\",\"dano\":24}]" else "[]") },
                 { synchronized(gotA) { gotA.add(it) } }, { synchronized(evA) { evA.add(it) } })
    rb.startSync({ nB++; "{\"celB\":{\"p\":[9,9,$nB]}}" to (if (nB == 5) "[{\"t\":\"abate\",\"v\":\"celB\",\"a\":\"celA\"}]" else "[]") },
                 { synchronized(gotB) { gotB.add(it) } }, { synchronized(evB) { evB.add(it) } })
    Thread.sleep(1500)
    ra.stopSync(); rb.stopSync()
    println("A recebeu estados: ${gotA.size}, último: ${gotA.lastOrNull()}")
    println("B recebeu estados: ${gotB.size}, último: ${gotB.lastOrNull()}")
    println("A recebeu eventos: $evA")
    println("B recebeu eventos: $evB")
    rb.leave()
    Thread.sleep(500)
    println("B saiu, A vê: " + ra.snapshot().map { it.second.optString("nome") })
    val (rb2, _) = OnlineRoom.join(b, ra.code, JSONObject().put("nome", "Beto"))
    println("B tenta voltar com a partida rolando: " + (rb2?.code ?: "recusado (certo)"))
    ra.backToLobby()
    val (rb3, _) = OnlineRoom.join(b, ra.code, JSONObject().put("nome", "Beto"))
    rb3!!.listenRoom(); rb3.onGone = { gone = true }
    Thread.sleep(500)
    ra.leave()
    Thread.sleep(800)
    println("A (quem criou) saiu: B foi avisado que a sala fechou? $gone")
    System.exit(0)
}
