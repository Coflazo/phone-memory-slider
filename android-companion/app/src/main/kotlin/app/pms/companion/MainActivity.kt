package app.pms.companion

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.google.zxing.client.android.Intents
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContent {
            CompanionTheme {
                CompanionRoute()
            }
        }
    }
}

@Composable
private fun MainActivity.CompanionRoute() {
    val catalog = remember { MediaStoreCatalogSource(applicationContext) }
    val session = remember { BluetoothGallerySession(applicationContext, catalog) }
    val sessionState by session.state.collectAsState()
    var payload by remember { mutableStateOf<PairingPayload?>(null) }
    var localError by remember { mutableStateOf<String?>(null) }

    DisposableEffect(session) { onDispose(session::close) }

    val trashLauncher = rememberLauncherForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) {
        TrashConfirmationBus.finish(it.resultCode == Activity.RESULT_OK)
    }
    val pendingTrash by TrashConfirmationBus.pending.collectAsState()
    LaunchedEffect(pendingTrash?.token) {
        pendingTrash?.let {
            trashLauncher.launch(IntentSenderRequest.Builder(it.prepared.confirmation.intentSender).build())
        }
    }

    fun connectWhenReady(candidate: PairingPayload) {
        val adapter = getSystemService(BluetoothManager::class.java).adapter
        if (adapter?.isEnabled == true) session.connect(candidate)
        else localError = "Turn on Bluetooth, then tap Connect again"
    }

    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        val candidate = payload ?: return@rememberLauncherForActivityResult
        if (requiredPermissions().all { permission ->
                ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
            }) {
            connectWhenReady(candidate)
        } else {
            localError = "Bluetooth and full photo access are needed to read the gallery"
        }
    }
    val bluetoothLauncher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        payload?.let(::connectWhenReady)
    }
    val scanLauncher = rememberLauncherForActivityResult(ScanContract()) { scan ->
        val raw = scan.contents ?: return@rememberLauncherForActivityResult
        runCatching { PairingPayload.parse(raw) }
            .onSuccess { parsed ->
                payload = parsed
                localError = null
                permissionLauncher.launch(requiredPermissions())
            }
            .onFailure { localError = it.message ?: "The QR code could not be read" }
    }

    val steps = listOf(
        PairingStep("Computer code scanned", payload != null),
        PairingStep("Nearby-device access allowed", bluetoothPermissionGranted()),
        PairingStep("Photos and videos allowed", catalog.permissionCoverage() != PermissionCoverage.Denied),
        PairingStep("Private Bluetooth session connected", sessionState is SessionState.Connected || sessionState is SessionState.Transferring),
        PairingStep("Gallery is visible on the computer", (sessionState as? SessionState.Connected)?.requestsServed?.let { it > 0 } == true),
    )

    Surface(color = MaterialTheme.colorScheme.background, modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier.fillMaxSize().safeDrawingPadding().padding(horizontal = 24.dp, vertical = 20.dp),
            verticalArrangement = Arrangement.spacedBy(24.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.background(MaterialTheme.colorScheme.primary, RoundedCornerShape(5.dp)).padding(7.dp))
                Text(
                    "Phone Memory Slider",
                    modifier = Modifier.padding(start = 12.dp),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
            }

            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.Center) {
                Text(
                    if (sessionState is SessionState.Connected || sessionState is SessionState.Transferring)
                        "Your gallery is connected privately."
                    else "Scan the code on your computer.",
                    style = MaterialTheme.typography.headlineLarge,
                    fontWeight = FontWeight.Bold,
                    lineHeight = 42.sp,
                )
                Spacer(Modifier.height(12.dp))
                Text(
                    "${Build.MANUFACTURER.replaceFirstChar(Char::uppercase)} ${Build.MODEL} uses Bluetooth only. Keep this screen open while the computer reviews your gallery.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodyLarge,
                    lineHeight = 25.sp,
                )
                Spacer(Modifier.height(28.dp))
                Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    steps.forEachIndexed { index, step -> PairingStepRow(index + 1, step) }
                }
                Spacer(Modifier.height(28.dp))
                AnimatedContent(
                    targetState = statusText(sessionState, localError),
                    transitionSpec = { fadeIn(tween(180)).togetherWith(fadeOut(tween(120))) },
                    label = "session-status",
                ) { message ->
                    Text(message, color = if (localError != null || sessionState is SessionState.Failure) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }

            when {
                sessionState is SessionState.Connected || sessionState is SessionState.Transferring ->
                    OutlinedButton(onClick = session::stop, modifier = Modifier.fillMaxWidth().height(54.dp)) {
                        Text("End private session")
                    }
                payload != null && localError?.startsWith("Turn on Bluetooth") == true ->
                    Button(
                        onClick = { bluetoothLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) },
                        modifier = Modifier.fillMaxWidth().height(54.dp),
                    ) { Text("Turn on Bluetooth") }
                payload != null && localError != null ->
                    Button(onClick = { permissionLauncher.launch(requiredPermissions()) }, modifier = Modifier.fillMaxWidth().height(54.dp)) {
                        Text("Allow and connect")
                    }
                else ->
                    Button(
                        onClick = {
                            val options = ScanOptions()
                                .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                                .setPrompt("Point the camera at the code on your computer")
                                .setBeepEnabled(false)
                                .setOrientationLocked(false)
                            options.addExtra(Intents.Scan.SCAN_TYPE, Intents.Scan.MIXED_SCAN)
                            scanLauncher.launch(options)
                        },
                        modifier = Modifier.fillMaxWidth().height(54.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary),
                    ) { Text("Scan computer code", fontWeight = FontWeight.SemiBold) }
            }
        }
    }
}

private data class PairingStep(val label: String, val complete: Boolean)

@Composable
private fun PairingStepRow(number: Int, step: PairingStep) {
    val scale by animateFloatAsState(if (step.complete) 1f else 0.92f, tween(180), label = "step-check")
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier.scale(scale).background(
                if (step.complete) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceVariant,
                CircleShape,
            ).padding(horizontal = 11.dp, vertical = 7.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                number.toString(),
                color = if (step.complete) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.Bold,
            )
        }
        Text(
            step.label,
            modifier = Modifier.padding(start = 14.dp),
            color = if (step.complete) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface,
            textDecoration = if (step.complete) TextDecoration.LineThrough else TextDecoration.None,
            style = MaterialTheme.typography.bodyLarge,
        )
    }
}

@Composable
private fun MainActivity.bluetoothPermissionGranted(): Boolean =
    Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
        ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

private fun statusText(state: SessionState, localError: String?): String = localError ?: when (state) {
    SessionState.Idle -> "No Internet permission is present in this app."
    is SessionState.Connecting -> "Connecting to ${state.computerName}. Android may ask you to confirm pairing."
    is SessionState.Connected -> if (state.requestsServed == 0) "Connected. Waiting for the computer." else "Connected. ${state.requestsServed} private requests served."
    is SessionState.Transferring -> state.operation
    is SessionState.Failure -> state.message
}

private fun requiredPermissions(): Array<String> = buildList {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) add(Manifest.permission.BLUETOOTH_CONNECT)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        add(Manifest.permission.READ_MEDIA_IMAGES)
        add(Manifest.permission.READ_MEDIA_VIDEO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            add(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)
        }
    } else add(Manifest.permission.READ_EXTERNAL_STORAGE)
}.toTypedArray()

private val DarkColors: ColorScheme = darkColorScheme(
    primary = PmsTokens.AccentDark,
    onPrimary = PmsTokens.CanvasDark,
    background = PmsTokens.CanvasDark,
    surface = PmsTokens.SurfaceDark,
    surfaceVariant = PmsTokens.RaisedDark,
    outline = PmsTokens.BorderDark,
    error = PmsTokens.DeleteDark,
    onSurface = PmsTokens.TextPrimaryDark,
    onSurfaceVariant = PmsTokens.TextSecondaryDark,
)

private val LightColors: ColorScheme = lightColorScheme(
    primary = PmsTokens.AccentLight,
    onPrimary = PmsTokens.SurfaceLight,
    background = PmsTokens.CanvasLight,
    surface = PmsTokens.SurfaceLight,
    surfaceVariant = PmsTokens.RaisedLight,
    outline = PmsTokens.BorderLight,
    error = PmsTokens.DeleteLight,
    onSurface = PmsTokens.TextPrimaryLight,
    onSurfaceVariant = PmsTokens.TextSecondaryLight,
)

@Composable
private fun CompanionTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (androidx.compose.foundation.isSystemInDarkTheme()) DarkColors else LightColors,
        content = content,
    )
}
