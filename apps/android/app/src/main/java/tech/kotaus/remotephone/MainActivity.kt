package tech.kotaus.remotephone

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Color as AndroidColor
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import tech.kotaus.remotephone.ui.theme.Card as CardColor
import tech.kotaus.remotephone.ui.theme.Elevated
import tech.kotaus.remotephone.ui.theme.Mint
import tech.kotaus.remotephone.ui.theme.MintInk
import tech.kotaus.remotephone.ui.theme.Night
import tech.kotaus.remotephone.ui.theme.Outline
import tech.kotaus.remotephone.ui.theme.RemotePhoneTheme
import tech.kotaus.remotephone.ui.theme.TextPrimary
import tech.kotaus.remotephone.ui.theme.TextSecondary

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = AndroidColor.rgb(11, 14, 18)
        window.navigationBarColor = AndroidColor.rgb(11, 14, 18)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            isAppearanceLightStatusBars = false
            isAppearanceLightNavigationBars = false
        }

        setContent {
            RemotePhoneTheme {
                SessionHome()
            }
        }
    }
}

private enum class PhoneModeUi(
    val streamMode: PhoneStreamMode,
    val title: String,
    val description: String,
    val glyph: String
) {
    SCREEN(
        PhoneStreamMode.SCREEN,
        "Экран",
        "Транслировать весь экран телефона на компьютер",
        "▣"
    ),
    CAMERA(
        PhoneStreamMode.CAMERA,
        "Веб-камера",
        "Использовать камеру телефона в приложениях на ПК",
        "◉"
    )
}

@Composable
private fun SessionHome() {
    val context = LocalContext.current
    val state by PhoneSessionController.uiState.collectAsState()
    var selectedMode by remember { mutableStateOf(PhoneModeUi.SCREEN) }
    var allowControl by remember { mutableStateOf(false) }
    var showDiagnostics by remember { mutableStateOf(false) }

    val cameraPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            PhoneSessionController.start(
                context,
                PhoneModeUi.CAMERA.streamMode,
                state.quality,
                allowControl,
                null
            )
        }
    }
    val notificationPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { }
    val projectionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK && result.data != null) {
            PhoneSessionController.start(
                context,
                PhoneModeUi.SCREEN.streamMode,
                state.quality,
                allowControl,
                result.data
            )
        } else {
            PhoneSessionController.publish(
                PhonePairingState(
                    phase = PhonePairingPhase.IDLE,
                    mode = PhoneModeUi.SCREEN.streamMode,
                    quality = state.quality,
                    allowControl = allowControl,
                    message = "Трансляция экрана требует разрешения на запись экрана. Нажмите «Начать» и подтвердите его."
                )
            )
        }
    }

    if (showDiagnostics) {
        DiagnosticsScreen(state.message, state.diagnosticText) { showDiagnostics = false }
        return
    }

    val sessionActive = state.phase in setOf(
        PhonePairingPhase.STARTING,
        PhonePairingPhase.WAITING,
        PhonePairingPhase.VERIFYING,
        PhonePairingPhase.AUTHENTICATED
    )

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = Night
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 22.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(0.dp)
        ) {
            SessionHeader(state)
            Spacer(Modifier.height(28.dp))
            Text(
                "ЧТО ПОКАЗАТЬ КОМПЬЮТЕРУ?",
                color = Mint,
                fontSize = 9.sp,
                letterSpacing = 1.2.sp,
                fontWeight = FontWeight.Bold
            )
            Spacer(Modifier.height(8.dp))
            Text(
                "Что хотите показать?",
                color = TextPrimary,
                fontSize = 27.sp,
                lineHeight = 32.sp,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(Modifier.height(7.dp))
            Text(
                if (sessionActive) "Сеанс активен. Трансляция продолжится, если свернуть приложение или заблокировать экран."
                else "Выберите режим и создайте PIN-сеанс для защищённого подключения компьютера.",
                color = TextSecondary,
                fontSize = 13.sp,
                lineHeight = 19.sp
            )
            Spacer(Modifier.height(22.dp))

            PhoneModeUi.entries.forEach { mode ->
                ModeCard(
                    mode = mode,
                    selected = if (sessionActive) state.mode == mode.streamMode else selectedMode == mode,
                    enabled = !sessionActive,
                    onClick = {
                        selectedMode = mode
                        if (mode == PhoneModeUi.SCREEN && Build.VERSION.SDK_INT >= 33) {
                            notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                        }
                    }
                )
                Spacer(Modifier.height(10.dp))
            }

            if (if (sessionActive) state.mode == PhoneModeUi.SCREEN.streamMode else selectedMode == PhoneModeUi.SCREEN) {
                ControlToggleCard(allowControl) { allowControl = it }
                Spacer(Modifier.height(12.dp))
            }
            if (!sessionActive) {
                QualityCard(state.quality) { next ->
                    PhoneSessionController.publish(state.copy(quality = next))
                }
                Spacer(Modifier.height(12.dp))
            }

            PhonePairingCard(state)
            if (state.streaming) {
                Spacer(Modifier.height(8.dp))
                StreamingCard(state)
            }
            Spacer(Modifier.height(8.dp))
            Button(
                onClick = { showDiagnostics = true },
                modifier = Modifier.fillMaxWidth().height(42.dp),
                shape = RoundedCornerShape(13.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF1D2630),
                    contentColor = TextPrimary
                )
            ) {
                Text("Диагностика", fontSize = 12.sp, fontWeight = FontWeight.Medium)
            }
            Spacer(Modifier.height(14.dp))
            Button(
                onClick = {
                    if (sessionActive) {
                        PhoneSessionController.stop(context)
                    } else {
                        startSelectedMode(
                            context,
                            if (sessionActive) state.mode else selectedMode.streamMode,
                            state.quality,
                            allowControl,
                            projectionLauncher,
                            cameraPermissionLauncher
                        )
                    }
                },
                enabled = state.phase != PhonePairingPhase.STARTING,
                modifier = Modifier.fillMaxWidth().height(50.dp),
                shape = RoundedCornerShape(14.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (sessionActive) Color(0xFF26313A) else Mint,
                    contentColor = if (sessionActive) TextPrimary else MintInk,
                    disabledContainerColor = Color(0xFF26313A),
                    disabledContentColor = TextSecondary
                )
            ) {
                Text(
                    if (sessionActive) "Завершить сеанс" else "Создать PIN и ждать подключения",
                    fontSize = 12.sp,
                    fontWeight = FontWeight.SemiBold
                )
            }
            Spacer(Modifier.height(14.dp))
            Text(
                "Тёмная тема · Русский язык · Без записи, звука и чтения экрана",
                modifier = Modifier.fillMaxWidth(),
                color = TextSecondary.copy(alpha = 0.78f),
                fontSize = 10.sp,
                lineHeight = 15.sp
            )
            Spacer(Modifier.height(18.dp))
        }
    }
}

private fun startSelectedMode(
    context: Context,
    mode: PhoneStreamMode?,
    quality: PhoneQualityProfile,
    allowControl: Boolean,
    projectionLauncher: androidx.activity.result.ActivityResultLauncher<android.content.Intent>,
    cameraPermissionLauncher: androidx.activity.result.ActivityResultLauncher<String>
) {
    when (mode) {
        PhoneStreamMode.SCREEN -> {
            val projectionManager = context.getSystemService(MediaProjectionManager::class.java)
            if (projectionManager == null) {
                PhoneSessionController.publish(
                    PhonePairingState(
                        phase = PhonePairingPhase.IDLE,
                        mode = mode,
                        quality = quality,
                        allowControl = allowControl,
                        message = "На этом телефоне нет службы записи экрана (MediaProjection)."
                    )
                )
                return
            }
            projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
        }
        PhoneStreamMode.CAMERA -> {
            val granted = ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED
            if (granted) {
                PhoneSessionController.start(context, PhoneStreamMode.CAMERA, quality, allowControl, null)
            } else {
                cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
            }
        }
        null -> Unit
    }
}

@Composable
private fun SessionHeader(state: PhonePairingState) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                modifier = Modifier
                    .size(38.dp)
                    .clip(RoundedCornerShape(13.dp)),
                contentAlignment = Alignment.Center
            ) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    shape = RoundedCornerShape(13.dp),
                    color = Mint.copy(alpha = 0.12f),
                    border = BorderStroke(1.dp, Mint.copy(alpha = 0.28f))
                ) {}
                Text("◉", color = Mint, fontSize = 19.sp, fontWeight = FontWeight.Medium)
            }
            Spacer(Modifier.width(11.dp))
            Column {
                Text("Видоискатель", color = TextPrimary, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                Text("ПОДКЛЮЧЕНИЕ ТЕЛЕФОНА", color = TextSecondary, fontSize = 9.sp, letterSpacing = 1.1.sp)
            }
        }
        Surface(
            shape = CircleShape,
            color = if (state.phase == PhonePairingPhase.AUTHENTICATED) Mint.copy(alpha = 0.14f) else Mint.copy(alpha = 0.09f),
            border = BorderStroke(
                1.dp,
                if (state.phase == PhonePairingPhase.AUTHENTICATED) Mint.copy(alpha = 0.4f) else Mint.copy(alpha = 0.16f)
            )
        ) {
            Text(
                text = when {
                    state.phase == PhonePairingPhase.AUTHENTICATED && state.streaming -> "В эфире"
                    state.phase == PhonePairingPhase.AUTHENTICATED -> "Подключено"
                    else -> "Ожидание"
                },
                modifier = Modifier.padding(horizontal = 11.dp, vertical = 7.dp),
                color = Mint,
                fontSize = 10.sp,
                fontWeight = FontWeight.Medium
            )
        }
    }
}

@Composable
private fun ModeCard(
    mode: PhoneModeUi,
    selected: Boolean,
    enabled: Boolean,
    onClick: () -> Unit
) {
    val borderColor = if (selected) Mint.copy(alpha = 0.42f) else Outline
    val containerColor = if (selected) Mint.copy(alpha = 0.065f) else CardColor
    Card(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth(),
        enabled = enabled,
        shape = RoundedCornerShape(17.dp),
        colors = CardDefaults.cardColors(containerColor = containerColor),
        border = BorderStroke(1.dp, borderColor)
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Surface(
                modifier = Modifier.size(44.dp),
                shape = RoundedCornerShape(14.dp),
                color = if (selected) Mint.copy(alpha = 0.1f) else Elevated
            ) {
                Box(contentAlignment = Alignment.Center) {
                    Text(
                        mode.glyph,
                        color = if (selected) Mint else TextSecondary,
                        fontSize = 19.sp,
                        fontWeight = FontWeight.SemiBold
                    )
                }
            }
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(mode.title, color = TextPrimary, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.height(4.dp))
                Text(
                    mode.description,
                    color = TextSecondary,
                    fontSize = 10.sp,
                    lineHeight = 15.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
            }
            Spacer(Modifier.width(8.dp))
            Surface(
                modifier = Modifier.size(19.dp),
                shape = CircleShape,
                color = if (selected) Mint else Color.Transparent,
                border = BorderStroke(1.dp, if (selected) Mint else Outline)
            ) {
                if (selected) {
                    Box(contentAlignment = Alignment.Center) {
                        Text("✓", color = MintInk, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }
    }
}

@Composable
private fun ControlToggleCard(allowControl: Boolean, onChange: (Boolean) -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = CardColor)
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 15.dp, vertical = 13.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text("Разрешить управление с ПК", color = TextPrimary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                Spacer(Modifier.height(3.dp))
                Text(
                    "Жесты мыши появятся на следующем этапе; экран никогда не считывается",
                    color = TextSecondary,
                    fontSize = 10.sp,
                    lineHeight = 14.sp
                )
            }
            Switch(
                checked = allowControl,
                onCheckedChange = onChange,
                enabled = false,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = MintInk,
                    checkedTrackColor = Mint,
                    uncheckedThumbColor = TextSecondary,
                    uncheckedTrackColor = Elevated,
                    uncheckedBorderColor = Outline
                )
            )
        }
    }
}

@Composable
private fun QualityCard(current: PhoneQualityProfile, onSelect: (PhoneQualityProfile) -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = CardColor)
    ) {
        Column(modifier = Modifier.padding(horizontal = 15.dp, vertical = 13.dp)) {
            Text("Качество", color = TextPrimary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
            Spacer(Modifier.height(3.dp))
            Text(
                "«Авто» балансирует плавность и трафик; «Экономия» бережёт батарею и от нагрева",
                color = TextSecondary,
                fontSize = 10.sp,
                lineHeight = 14.sp
            )
            Spacer(Modifier.height(10.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PhoneQualityProfile.entries.forEach { profile ->
                    val selected = profile == current
                    Surface(
                        shape = RoundedCornerShape(11.dp),
                        color = if (selected) Mint else Elevated,
                        border = BorderStroke(1.dp, if (selected) Mint else Outline),
                        modifier = Modifier
                            .clip(RoundedCornerShape(11.dp))
                            .clickable { onSelect(profile) }
                    ) {
                        Text(
                            profile.title,
                            modifier = Modifier.padding(horizontal = 13.dp, vertical = 7.dp),
                            color = if (selected) MintInk else TextPrimary,
                            fontSize = 11.sp,
                            fontWeight = FontWeight.Medium
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun StreamingCard(state: PhonePairingState) {
    val telemetry = state.telemetry
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = CardColor),
        border = BorderStroke(1.dp, Mint.copy(alpha = 0.22f))
    ) {
        Column(modifier = Modifier.padding(15.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Text("Состояние трансляции", color = TextPrimary, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            if (telemetry != null) {
                TelemetryRow("Разрешение", "${telemetry.width}×${telemetry.height}")
                TelemetryRow("Частота кадров", if (telemetry.fps > 0) "%.1f fps".format(telemetry.fps) else "—")
                TelemetryRow("Битрейт", if (telemetry.bitrateKbps > 0) "${telemetry.bitrateKbps.toInt()} кбит/с" else "—")
                TelemetryRow("Батарея", if (telemetry.batteryPercent >= 0) "${telemetry.batteryPercent}%" else "—")
                TelemetryRow(
                    "Питание",
                    if (telemetry.charging) "Заряжается" else "От батареи"
                )
                TelemetryRow("Температура", telemetry.thermalLabel)
            } else {
                Text(
                    "Ожидание видеопотока: подключите компьютер и дождитесь первого кадра.",
                    color = TextSecondary,
                    fontSize = 10.sp,
                    lineHeight = 15.sp
                )
            }
        }
    }
}

@Composable
private fun TelemetryRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(label, color = TextSecondary, fontSize = 10.sp)
        Text(value, color = TextPrimary, fontSize = 10.sp, fontWeight = FontWeight.Medium)
    }
}

private val LocalContext = androidx.compose.ui.platform.LocalContext
