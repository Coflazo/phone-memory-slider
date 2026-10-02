package app.pms.companion

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import java.security.SecureRandom
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class ForegroundPairingSession(private val context: Context) : PairingSession {
    private val mutableState = MutableStateFlow<PairingState>(PairingState.Idle)
    override val state: StateFlow<PairingState> = mutableState.asStateFlow()

    override fun start() {
        val code = "%06d".format(SecureRandom().nextInt(1_000_000))
        mutableState.value = PairingState.Advertising(code)
        ContextCompat.startForegroundService(
            context,
            Intent(context, PairingForegroundService::class.java)
                .putExtra(PairingForegroundService.EXTRA_CODE, code),
        )
    }

    override fun stop() {
        context.stopService(Intent(context, PairingForegroundService::class.java))
        mutableState.value = PairingState.Idle
    }
}

class PairingForegroundService : Service() {
    override fun onCreate() {
        super.onCreate()
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Desktop connection", NotificationManager.IMPORTANCE_LOW),
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val code = intent?.getStringExtra(EXTRA_CODE).orEmpty()
        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Phone Memory Slider is ready")
            .setContentText("Pairing code $code · local network only")
            .setOngoing(true)
            .build()
        startForeground(NOTIFICATION_ID, notification)
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        const val EXTRA_CODE = "pairing_code"
        private const val CHANNEL_ID = "pms_connection"
        private const val NOTIFICATION_ID = 41
    }
}
