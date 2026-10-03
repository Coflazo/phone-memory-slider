package app.pms.companion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.os.Build
import android.provider.Settings
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

class ForegroundPairingSession(private val context: Context) : PairingSession {
    override val state: StateFlow<PairingState> = ConnectionRuntime.state

    override fun start() {
        ConnectionRuntime.publish(PairingState.Starting)
        ContextCompat.startForegroundService(context, Intent(context, PairingForegroundService::class.java))
    }

    override fun stop() {
        context.stopService(Intent(context, PairingForegroundService::class.java))
        ConnectionRuntime.publish(PairingState.Idle)
    }
}

class PairingForegroundService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var server: LocalGalleryServer? = null
    private var advertiser: LocalServiceAdvertiser? = null
    private var authenticator: SessionAuthenticator? = null

    override fun onCreate() {
        super.onCreate()
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Desktop connection", NotificationManager.IMPORTANCE_LOW),
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, notification("Starting local-only session"))
        if (server != null) return START_NOT_STICKY
        scope.launch {
            runCatching { startLocalSession() }
                .onFailure {
                    ConnectionRuntime.publish(PairingState.Failure("Could not start the local session"))
                    stopSelf()
                }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        advertiser?.close()
        server?.close()
        authenticator?.invalidate()
        TrashRequestRuntime.shared.invalidateUncommitted()
        scope.cancel()
        ConnectionRuntime.publish(PairingState.Idle)
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun startLocalSession() {
        val identity = TlsIdentityStore.loadOrCreate()
        val pairingCode = SessionAuthenticator.newPairingCode()
        val sessionAuthenticator = SessionAuthenticator(pairingCode)
        val catalog = MediaStoreCatalogSource(this)
        val galleryApi = GalleryApi(
            catalog = catalog,
            deviceId = opaqueAssetId(Settings.Secure.getString(contentResolver, Settings.Secure.ANDROID_ID).orEmpty()),
            deviceName = "${Build.MANUFACTURER} ${Build.MODEL}".trim(),
            trashCoordinator = AndroidTrashCoordinator(contentResolver),
            requestConfirmation = TrashConfirmationBus::request,
        )
        val addresses = localNetworkAddresses()
        lateinit var endpoint: PairingState.Advertising
        val localServer = LocalGalleryServer(identity, sessionAuthenticator, onPaired = { desktopName ->
            ConnectionRuntime.publish(
                PairingState.Connected(
                    desktopName = desktopName,
                    code = pairingCode,
                    addresses = endpoint.addresses,
                    port = endpoint.port,
                ),
            )
        }, authenticatedHandler = galleryApi::handle)
        val port = localServer.start()
        endpoint = PairingState.Advertising(pairingCode, addresses, port)
        val localAdvertiser = LocalServiceAdvertiser(this).also { it.register(port) }
        authenticator = sessionAuthenticator
        server = localServer
        advertiser = localAdvertiser
        ConnectionRuntime.publish(endpoint)
        getSystemService(NotificationManager::class.java).notify(
            NOTIFICATION_ID,
            notification("Code $pairingCode · ${addresses.firstOrNull() ?: "local network"}:$port"),
        )
    }

    private fun notification(message: String): Notification = NotificationCompat.Builder(this, CHANNEL_ID)
        .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
        .setContentTitle("Phone Memory Slider is ready")
        .setContentText(message)
        .setOngoing(true)
        .build()

    companion object {
        private const val CHANNEL_ID = "pms_connection"
        private const val NOTIFICATION_ID = 41
    }
}
