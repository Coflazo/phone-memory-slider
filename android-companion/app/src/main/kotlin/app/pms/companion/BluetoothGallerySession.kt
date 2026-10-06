package app.pms.companion

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.os.Build
import androidx.core.content.edit
import java.io.EOFException
import java.io.IOException
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

sealed interface SessionState {
    data object Idle : SessionState
    data class Connecting(val computerName: String) : SessionState
    data class Connected(val computerName: String, val requestsServed: Int) : SessionState
    data class Transferring(val computerName: String, val operation: String, val requestsServed: Int) : SessionState
    data class Failure(val message: String) : SessionState
}

class BluetoothGallerySession(
    private val context: Context,
    private val catalog: MediaCatalogSource,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val mutableState = MutableStateFlow<SessionState>(SessionState.Idle)
    val state = mutableState.asStateFlow()
    private var connectionJob: Job? = null
    @Volatile private var socket: BluetoothSocket? = null

    @SuppressLint("MissingPermission")
    fun connect(payload: PairingPayload) {
        stop()
        mutableState.value = SessionState.Connecting(payload.computerName)
        connectionJob = scope.launch {
            try {
                val adapter = context.getSystemService(BluetoothManager::class.java).adapter
                    ?: error("This phone has no Bluetooth adapter")
                check(adapter.isEnabled) { "Turn on Bluetooth, then scan the code again" }
                val remote = adapter.getRemoteDevice(payload.address)
                val connectedSocket = remote.createRfcommSocketToServiceRecord(payload.serviceUuid)
                socket = connectedSocket
                connectedSocket.connect()
                connectedSocket.outputStream.run {
                    write("PMS/1 AUTH ${payload.secret}\r\n".toByteArray(StandardCharsets.US_ASCII))
                    flush()
                }
                serve(connectedSocket, payload)
            } catch (failure: SecurityException) {
                mutableState.value = SessionState.Failure("Allow nearby-device access, then try again")
            } catch (failure: Exception) {
                if (currentCoroutineContext().isActive) {
                    mutableState.value = SessionState.Failure(
                        failure.message?.take(160) ?: "The Bluetooth session could not start",
                    )
                }
            } finally {
                socket?.runCatching { close() }
                socket = null
            }
        }
    }

    fun stop() {
        socket?.runCatching { close() }
        socket = null
        connectionJob?.cancel()
        connectionJob = null
        TrashRequestRuntime.shared.invalidateUncommitted()
        mutableState.value = SessionState.Idle
    }

    fun close() {
        stop()
        scope.cancel()
    }

    private fun serve(socket: BluetoothSocket, payload: PairingPayload) {
        val parser = HttpRequestParser(maxBodyBytes = 1024 * 1024)
        val api = GalleryApi(
            catalog = catalog,
            deviceId = installationId(),
            deviceName = listOf(Build.MANUFACTURER, Build.MODEL).filter(String::isNotBlank).joinToString(" "),
            trashCoordinator = AndroidTrashCoordinator(context.contentResolver),
            requestConfirmation = TrashConfirmationBus::request,
        )
        var served = 0
        mutableState.value = SessionState.Connected(payload.computerName, served)
        while (true) {
            val request = try {
                parser.parse(socket.inputStream)
            } catch (_: EOFException) {
                break
            } catch (failure: HttpProtocolException) {
                if (!socket.isConnected) break
                HttpResponse(failure.status).writeTo(socket.outputStream)
                continue
            } catch (_: IOException) {
                break
            }
            if (!authorized(request, payload.secret)) {
                HttpResponse(401).writeTo(socket.outputStream)
                continue
            }
            mutableState.value = SessionState.Transferring(
                payload.computerName,
                operationName(request.target),
                served,
            )
            api.handle(request).writeTo(socket.outputStream)
            served += 1
            mutableState.value = SessionState.Connected(payload.computerName, served)
        }
    }

    private fun authorized(request: HttpRequest, secret: String): Boolean {
        val supplied = request.headers["authorization"]?.toByteArray(StandardCharsets.US_ASCII) ?: return false
        val expected = "Bearer $secret".toByteArray(StandardCharsets.US_ASCII)
        return MessageDigest.isEqual(supplied, expected)
    }

    private fun installationId(): String {
        val preferences = context.getSharedPreferences("local_identity", Context.MODE_PRIVATE)
        return preferences.getString("installation_id", null)
            ?: java.util.UUID.randomUUID().toString().also {
                preferences.edit { putString("installation_id", it) }
            }
    }

    private fun operationName(target: String): String = when {
        target.startsWith("/v1/catalog") -> "Sending gallery catalog"
        target.startsWith("/v1/thumbnail") -> "Sending a private preview"
        target.startsWith("/v1/content") -> "Sending a video for review"
        target.startsWith("/v1/hash") -> "Verifying media"
        target.startsWith("/v1/trash") -> "Preparing recoverable trash"
        else -> "Checking the private session"
    }
}
