package tech.kotaus.remotephone

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import tech.kotaus.remotephone.ui.theme.Card as CardColor
import tech.kotaus.remotephone.ui.theme.Mint
import tech.kotaus.remotephone.ui.theme.Outline
import tech.kotaus.remotephone.ui.theme.TextPrimary
import tech.kotaus.remotephone.ui.theme.TextSecondary

@Composable
internal fun PhonePairingCard(state: PhonePairingState) {
    val authenticated = state.phase == PhonePairingPhase.AUTHENTICATED
    val accent = when {
        authenticated && !state.awaitingResume -> Mint
        else -> Color(0xFF94CAFF)
    }
    val heading = when {
        authenticated && state.streaming -> "Трансляция идёт"
        authenticated && state.awaitingResume -> "Соединение прервано — ожидание компьютера"
        authenticated -> "Сопряжение подтверждено"
        else -> "Локальное сопряжение"
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = CardColor),
        border = BorderStroke(1.dp, if (authenticated) Mint.copy(alpha = 0.36f) else Outline.copy(alpha = 0.75f))
    ) {
        Column(
            modifier = Modifier.padding(15.dp),
            verticalArrangement = Arrangement.spacedBy(7.dp)
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Surface(
                    shape = CircleShape,
                    color = accent.copy(alpha = 0.12f),
                    border = BorderStroke(1.dp, accent.copy(alpha = 0.25f))
                ) {
                    Text(
                        text = if (authenticated) "✓" else "⌁",
                        modifier = Modifier.padding(horizontal = 9.dp, vertical = 5.dp),
                        color = accent,
                        fontSize = 13.sp,
                        fontWeight = FontWeight.Bold
                    )
                }
                Spacer(Modifier.width(10.dp))
                Column {
                    Text(
                        text = heading,
                        color = TextPrimary,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold
                    )
                    Text(state.message, color = TextSecondary, fontSize = 10.sp, lineHeight = 15.sp)
                }
            }

            state.pin?.let { pin ->
                Spacer(Modifier.height(3.dp))
                Text("Временный PIN — введите его на компьютере", color = TextSecondary, fontSize = 10.sp)
                Text(
                    text = pin.chunked(4).joinToString(" "),
                    color = Mint,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 31.sp,
                    letterSpacing = 3.sp,
                    fontWeight = FontWeight.SemiBold
                )
                Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Text(
                        "Истекает через ${formatRemaining(state.secondsRemaining)}",
                        color = TextSecondary,
                        fontSize = 10.sp
                    )
                    Text(
                        "Попытки: ${state.attemptsUsed} из 5",
                        color = TextSecondary,
                        fontSize = 10.sp
                    )
                }
                val port = state.port
                if (port != null) {
                    val automatic = if (state.discoveryAvailable) {
                        "Компьютер может найти телефон автоматически."
                    } else {
                        "Автоматический поиск недоступен. Используйте адрес ниже."
                    }
                    Text(automatic, color = TextSecondary, fontSize = 10.sp, lineHeight = 15.sp)
                    if (state.addresses.isNotEmpty()) {
                        Text(
                            "Адрес для ручного подключения: ${state.addresses.joinToString { "$it:$port" }}",
                            color = TextSecondary,
                            fontSize = 10.sp,
                            lineHeight = 15.sp
                        )
                    } else {
                        Text(
                            "Порт для ручного подключения: $port. Узнайте IP телефона в настройках Wi‑Fi.",
                            color = TextSecondary,
                            fontSize = 10.sp,
                            lineHeight = 15.sp
                        )
                    }
                }
            }
        }
    }
}

private fun formatRemaining(totalSeconds: Int): String {
    val safeSeconds = totalSeconds.coerceAtLeast(0)
    return "%02d:%02d".format(safeSeconds / 60, safeSeconds % 60)
}
