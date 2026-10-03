package app.pms.companion

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import java.net.InetAddress
import java.net.NetworkInterface
import java.nio.charset.StandardCharsets
import java.util.Collections
import javax.net.ssl.SSLServerSocket
import javax.net.ssl.SSLSocket
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONObject

class LocalGalleryServer(
    private val identity: TlsIdentity,
    private val authenticator: SessionAuthenticator,
    private val onPaired: (desktopName: String) -> Unit,
    private val authenticatedHandler: (HttpRequest) -> HttpResponse = {
        jsonResponse(404, "error" to "not_found")
    },
) : AutoCloseable {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var serverSocket: SSLServerSocket? = null
    private var acceptJob: Job? = null

    fun start(): Int {
        check(serverSocket == null) { "Server is already running" }
        val socket = identity.sslContext.serverSocketFactory.createServerSocket(0) as SSLServerSocket
        socket.reuseAddress = true
        socket.needClientAuth = false
        socket.enabledProtocols = socket.supportedProtocols
            .filter { it == "TLSv1.3" || it == "TLSv1.2" }
            .toTypedArray()
        serverSocket = socket
        acceptJob = scope.launch {
            while (isActive) {
                val client = runCatching { socket.accept() as SSLSocket }.getOrNull() ?: break
                launch { handle(client) }
            }
        }
        return socket.localPort
    }

    override fun close() {
        authenticator.invalidate()
        runCatching { serverSocket?.close() }
        acceptJob?.cancel()
        scope.cancel()
        serverSocket = null
    }

    private fun handle(socket: SSLSocket) {
        socket.use { client ->
            client.soTimeout = 15_000
            if (!LocalPeerPolicy.isAllowed(client.inetAddress)) return
            runCatching {
                client.startHandshake()
                val request = HttpRequestParser().parse(client.inputStream)
                route(request).writeTo(client.outputStream)
            }.onFailure { failure ->
                if (failure is HttpProtocolException) {
                    runCatching {
                        jsonResponse(failure.status, "error" to "invalid_request").writeTo(client.outputStream)
                    }
                }
            }
        }
    }

    private fun route(request: HttpRequest): HttpResponse {
        if (!SessionAuthenticator.requiresAuthentication(request.target)) return pair(request)
        if (!authenticator.authorize(request.headers["authorization"])) {
            return jsonResponse(401, "error" to "unauthorized")
        }
        return authenticatedHandler(request)
    }

    private fun pair(request: HttpRequest): HttpResponse {
        if (request.method != "POST" || request.target != "/v1/pair") {
            return jsonResponse(400, "error" to "invalid_pair_request")
        }
        val payload = runCatching {
            JSONObject(String(request.body, StandardCharsets.UTF_8))
        }.getOrNull() ?: return jsonResponse(400, "error" to "invalid_json")
        val token = authenticator.pair(payload.optString("code")).orElse(null)
            ?: return jsonResponse(403, "error" to "pairing_rejected")
        val desktopName = payload.optString("desktop_name").trim().take(80).ifEmpty { "Desktop" }
        onPaired(desktopName)
        return jsonResponse(200, "token" to token, "protocol_version" to 1)
    }

    companion object {
        fun jsonResponse(status: Int, vararg values: Pair<String, Any>): HttpResponse {
            val json = JSONObject()
            values.forEach { (key, value) -> json.put(key, value) }
            return HttpResponse(status = status, body = json.toString().toByteArray(StandardCharsets.UTF_8))
        }
    }
}

class LocalServiceAdvertiser(context: Context) : AutoCloseable {
    private val manager = context.getSystemService(NsdManager::class.java)
    private var listener: NsdManager.RegistrationListener? = null

    fun register(port: Int) {
        if (listener != null) return
        val registration = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(serviceInfo: NsdServiceInfo) = Unit
            override fun onRegistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = Unit
            override fun onServiceUnregistered(serviceInfo: NsdServiceInfo) = Unit
            override fun onUnregistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = Unit
        }
        listener = registration
        manager.registerService(
            NsdServiceInfo().apply {
                serviceName = "Phone Memory Slider"
                serviceType = "_pms._tcp."
                setPort(port)
                setAttribute("v", "1")
            },
            NsdManager.PROTOCOL_DNS_SD,
            registration,
        )
    }

    override fun close() {
        listener?.let { runCatching { manager.unregisterService(it) } }
        listener = null
    }
}

fun localNetworkAddresses(): List<String> {
    val interfaces = NetworkInterface.getNetworkInterfaces() ?: return emptyList()
    return Collections.list(interfaces)
        .asSequence()
        .filter { runCatching { it.isUp && !it.isLoopback }.getOrDefault(false) }
        .flatMap { Collections.list(it.inetAddresses).asSequence() }
        .filter(LocalPeerPolicy::isAllowed)
        .filterNot(InetAddress::isLoopbackAddress)
        .map { it.hostAddress.substringBefore('%') }
        .distinct()
        .sorted()
        .toList()
}
