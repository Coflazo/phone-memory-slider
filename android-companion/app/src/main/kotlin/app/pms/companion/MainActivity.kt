package app.pms.companion

import android.Manifest
import android.app.Activity
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
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
import androidx.compose.foundation.layout.weight
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.scale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            CompanionTheme {
                val catalog = remember { MediaStoreCatalogSource(applicationContext) }
                val pairing = remember { ForegroundPairingSession(applicationContext) }
                CompanionRoute(catalog, pairing)
            }
        }
    }
}

private enum class Stage { Permissions, Pairing, Connected, Trash }

@Composable
private fun CompanionRoute(catalog: MediaCatalogSource, pairing: ForegroundPairingSession) {
    var permissionRefresh by remember { mutableIntStateOf(0) }
    val coverage = remember(permissionRefresh) { catalog.permissionCoverage() }
    val pairingState by pairing.state.collectAsState()
    var stage by remember(coverage) {
        mutableStateOf(if (coverage == PermissionCoverage.Denied) Stage.Permissions else Stage.Pairing)
    }
    val pendingTrash by TrashConfirmationBus.pending.collectAsState()
    val trashLauncher = rememberLauncherForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) {
        TrashConfirmationBus.finish(it.resultCode == Activity.RESULT_OK)
    }
    LaunchedEffect(pendingTrash?.token) {
        pendingTrash?.let {
            stage = Stage.Trash
            trashLauncher.launch(IntentSenderRequest.Builder(it.prepared.confirmation.intentSender).build())
        }
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) {
        permissionRefresh += 1
        stage = if (catalog.permissionCoverage() == PermissionCoverage.Denied) Stage.Permissions else Stage.Pairing
    }

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(
            modifier = Modifier.fillMaxSize().padding(horizontal = 24.dp, vertical = 36.dp),
            verticalArrangement = Arrangement.spacedBy(24.dp),
        ) {
            BrandBar(stage)
            AnimatedContent(
                targetState = stage,
                transitionSpec = {
                    (slideInHorizontally(tween(520, easing = FastOutSlowInEasing)) { it / 3 } + fadeIn(tween(360)))
                        .togetherWith(slideOutHorizontally(tween(420)) { -it / 4 } + fadeOut(tween(240)))
                },
                label = "companion-stage",
                modifier = Modifier.weight(1f),
            ) { current ->
                when (current) {
                    Stage.Permissions -> PermissionScreen {
                        permissionLauncher.launch(requiredPermissions())
                    }
                    Stage.Pairing -> PairingScreen(
                        coverage = coverage,
                        onStart = {
                            pairing.start()
                            stage = Stage.Connected
                        },
                    )
                    Stage.Connected -> ConnectedScreen(
                        coverage = coverage,
                        pairingState = pairingState,
                        onPreviewTrash = { stage = Stage.Trash },
                        onStop = {
                            pairing.stop()
                            stage = Stage.Pairing
                        },
                    )
                    Stage.Trash -> TrashConfirmationScreen { stage = Stage.Connected }
                }
            }
        }
    }
}

@Composable
private fun BrandBar(stage: Stage) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier.background(MaterialTheme.colorScheme.primary, CircleShape).padding(7.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text("P", fontWeight = FontWeight.Black, color = MaterialTheme.colorScheme.onPrimary)
        }
        Text(
            "  Pocket Relay",
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.weight(1f),
        )
        Text(
            text = "0${stage.ordinal + 1} / 04",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 12.sp,
        )
    }
}

@Composable
private fun PermissionScreen(onGrant: () -> Unit) {
    StageLayout(
        eyebrow = "PRIVATE BY DESIGN",
        title = "Choose what the desktop can review.",
        body = "Your media stays between this phone and your computer. Selected access works; full access gives better ranking coverage.",
    ) {
        PrimaryButton("Choose gallery access", onGrant)
    }
}

@Composable
private fun PairingScreen(coverage: PermissionCoverage, onStart: () -> Unit) {
    StageLayout(
        eyebrow = if (coverage == PermissionCoverage.Partial) "SELECTED LIBRARY" else "GALLERY READY",
        title = "Bring both screens close.",
        body = "Start a local session, then enter the six-digit code shown here in the desktop app.",
    ) {
        PrimaryButton("Start local pairing", onStart)
    }
}

@Composable
private fun ConnectedScreen(
    coverage: PermissionCoverage,
    pairingState: PairingState,
    onPreviewTrash: () -> Unit,
    onStop: () -> Unit,
) {
    val code = when (pairingState) {
        is PairingState.Advertising -> pairingState.code
        is PairingState.Connected -> pairingState.code
        else -> null
    }
    val address = when (pairingState) {
        is PairingState.Advertising -> pairingState.addresses.firstOrNull()?.let { "$it:${pairingState.port}" }
        is PairingState.Connected -> pairingState.addresses.firstOrNull()?.let { "$it:${pairingState.port}" }
        else -> null
    }
    val desktopName = (pairingState as? PairingState.Connected)?.desktopName
    val pulse = rememberInfiniteTransition(label = "connection-pulse")
    val scale by pulse.animateFloat(
        initialValue = 0.94f,
        targetValue = 1.08f,
        animationSpec = infiniteRepeatable(tween(1_400), RepeatMode.Reverse),
        label = "connection-scale",
    )
    StageLayout(
        eyebrow = if (desktopName == null) "LOCAL SESSION READY" else "CONNECTED LOCALLY",
        title = desktopName?.let { "$it can review ${if (coverage == PermissionCoverage.Full) "your gallery" else "your selection"}." }
            ?: "Enter the endpoint and code on your desktop.",
        body = when (pairingState) {
            PairingState.Starting -> "Creating the encrypted local session…"
            is PairingState.Failure -> "${pairingState.message}. End the session and try again."
            else -> "${address ?: "Finding a local address"}\nCode ${code ?: "—— ——"}\nAndroid will ask again before anything moves to trash."
        },
        signal = {
            Box(
                Modifier.scale(scale).alpha(0.75f).background(MaterialTheme.colorScheme.primary, CircleShape)
                    .padding(16.dp),
            )
        },
    ) {
        PrimaryButton("Preview trash confirmation", onPreviewTrash)
        OutlinedButton(onClick = onStop, modifier = Modifier.fillMaxWidth()) { Text("End session") }
    }
}

@Composable
private fun TrashConfirmationScreen(onBack: () -> Unit) {
    StageLayout(
        eyebrow = "ANDROID HAS FINAL SAY",
        title = "One clear confirmation. Nothing silent.",
        body = "The desktop prepares a batch, favorites are checked again here, then Android shows its recoverable system-trash prompt.",
    ) {
        Surface(
            color = MaterialTheme.colorScheme.surfaceVariant,
            shape = RoundedCornerShape(20.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Row(Modifier.padding(18.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Example batch", color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text("Waiting for desktop batch", fontWeight = FontWeight.Bold)
            }
        }
        PrimaryButton("Back to connection", onBack)
    }
}

@Composable
private fun StageLayout(
    eyebrow: String,
    title: String,
    body: String,
    signal: @Composable () -> Unit = {},
    actions: @Composable () -> Unit,
) {
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center) {
        signal()
        Text(
            eyebrow,
            color = MaterialTheme.colorScheme.primary,
            fontSize = 12.sp,
            fontWeight = FontWeight.Bold,
            letterSpacing = 1.6.sp,
        )
        Spacer(Modifier.height(16.dp))
        Text(title, fontSize = 40.sp, lineHeight = 44.sp, fontWeight = FontWeight.Black)
        Spacer(Modifier.height(18.dp))
        Text(
            body,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 17.sp,
            lineHeight = 25.sp,
        )
        Spacer(Modifier.height(36.dp))
        Column(verticalArrangement = Arrangement.spacedBy(12.dp), content = { actions() })
    }
}

@Composable
private fun PrimaryButton(label: String, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth().height(56.dp),
        shape = RoundedCornerShape(18.dp),
        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary),
    ) {
        Text(label, fontWeight = FontWeight.Bold, textAlign = TextAlign.Center)
    }
}

private fun requiredPermissions(): Array<String> = buildList {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        add(Manifest.permission.READ_MEDIA_IMAGES)
        add(Manifest.permission.READ_MEDIA_VIDEO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            add(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)
        }
        add(Manifest.permission.POST_NOTIFICATIONS)
    } else {
        add(Manifest.permission.READ_EXTERNAL_STORAGE)
    }
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
