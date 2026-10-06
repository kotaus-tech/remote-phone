package tech.kotaus.remotephone

import android.graphics.Color as AndroidColor
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
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
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
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
                StageTwoHome()
            }
        }
    }
}

private enum class PhoneMode(val title: String, val description: String, val glyph: String) {
    SCREEN("Экран", "Показывать экран телефона и управлять им с ПК", "▣"),
    CAMERA("Веб-камера", "Использовать камеру телефона в приложениях на ПК", "◉")
}

@Composable
private fun StageTwoHome() {
    val context = LocalContext.current
    var selectedMode by remember { mutableStateOf(PhoneMode.SCREEN) }
    var allowControl by remember { mutableStateOf(false) }
    var pairingState by remember { mutableStateOf(PhonePairingState()) }
    val pairingHost = remember {
        PhonePairingHost(context.applicationContext) { nextState -> pairingState = nextState }
    }
    DisposableEffect(pairingHost) {
        onDispose { pairingHost.close() }
    }

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
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(
                        modifier = Modifier
                            .size(38.dp)
                            .clip(RoundedCornerShape(13.dp))
                            .then(Modifier),
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
                    color = Mint.copy(alpha = 0.09f),
                    border = BorderStroke(1.dp, Mint.copy(alpha = 0.16f))
                ) {
                    Text(
                        text = "Этап 3",
                        modifier = Modifier.padding(horizontal = 11.dp, vertical = 7.dp),
                        color = Mint,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Medium
                    )
                }
            }

            Spacer(Modifier.height(28.dp))
            Text("ВАШ ТЕЛЕФОН — ВАША КАМЕРА", color = Mint, fontSize = 9.sp, letterSpacing = 1.2.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            Text("Что хотите показать?", color = TextPrimary, fontSize = 27.sp, lineHeight = 32.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.height(7.dp))
            Text(
                "Создайте локальный PIN-сеанс, чтобы проверить защищённое сопряжение с компьютером.",
                color = TextSecondary,
                fontSize = 13.sp,
                lineHeight = 19.sp
            )
            Spacer(Modifier.height(22.dp))

            PhoneMode.entries.forEach { mode ->
                ModeCard(
                    mode = mode,
                    selected = selectedMode == mode,
                    onClick = { selectedMode = mode }
                )
                Spacer(Modifier.height(10.dp))
            }

            if (selectedMode == PhoneMode.SCREEN) {
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
                            Text("Управление появится на следующем этапе", color = TextSecondary, fontSize = 10.sp)
                        }
                        Switch(
                            checked = allowControl,
                            onCheckedChange = { allowControl = it },
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
                Spacer(Modifier.height(12.dp))
            }

            PhonePairingCard(pairingState)
            Spacer(Modifier.height(14.dp))
            val sessionActive = pairingState.phase in setOf(
                PhonePairingPhase.STARTING,
                PhonePairingPhase.WAITING,
                PhonePairingPhase.VERIFYING,
                PhonePairingPhase.AUTHENTICATED
            )
            Button(
                onClick = { if (sessionActive) pairingHost.stop() else pairingHost.start() },
                enabled = pairingState.phase != PhonePairingPhase.STARTING,
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
                "Тёмная тема · Русский язык · Без записи и передачи звука",
                modifier = Modifier.fillMaxWidth(),
                color = TextSecondary.copy(alpha = 0.78f),
                fontSize = 10.sp,
                lineHeight = 15.sp
            )
            Spacer(Modifier.height(18.dp))
        }
    }
}

@Composable
private fun ModeCard(mode: PhoneMode, selected: Boolean, onClick: () -> Unit) {
    val borderColor = if (selected) Mint.copy(alpha = 0.42f) else Outline
    val containerColor = if (selected) Mint.copy(alpha = 0.065f) else CardColor
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick),
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
                    Text(mode.glyph, color = if (selected) Mint else TextSecondary, fontSize = 19.sp, fontWeight = FontWeight.SemiBold)
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
