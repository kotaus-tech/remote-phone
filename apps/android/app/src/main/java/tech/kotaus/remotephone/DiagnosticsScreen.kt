package tech.kotaus.remotephone

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.widget.Toast
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import tech.kotaus.remotephone.ui.theme.Card as CardColor
import tech.kotaus.remotephone.ui.theme.Mint
import tech.kotaus.remotephone.ui.theme.MintInk
import tech.kotaus.remotephone.ui.theme.Night
import tech.kotaus.remotephone.ui.theme.Outline
import tech.kotaus.remotephone.ui.theme.TextPrimary
import tech.kotaus.remotephone.ui.theme.TextSecondary

@Composable
internal fun DiagnosticsScreen(currentStatus: String, diagnosticText: String?, onBack: () -> Unit) {
    val context = LocalContext.current
    val details = buildString {
        appendLine("Текущее состояние:")
        appendLine(currentStatus)
        appendLine()
        appendLine("Последняя техническая ошибка:")
        append(diagnosticText ?: "Не зарегистрирована.")
    }

    Surface(modifier = Modifier.fillMaxSize(), color = Night) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 22.dp, vertical = 20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Text("Диагностика", color = TextPrimary, fontSize = 25.sp, fontWeight = FontWeight.SemiBold)
            Text(
                "Здесь показан текущий этап соединения и причина последней технической ошибки. Эти сведения остаются локально и не отправляются автоматически.",
                color = TextSecondary,
                fontSize = 13.sp,
                lineHeight = 19.sp
            )

            Button(
                onClick = {
                    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    clipboard.setPrimaryClip(ClipData.newPlainText("Диагностика Видоискателя", details))
                    Toast.makeText(context, "Диагностика скопирована", Toast.LENGTH_SHORT).show()
                },
                enabled = true,
                modifier = Modifier.fillMaxWidth().height(48.dp),
                shape = RoundedCornerShape(14.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = Mint,
                    contentColor = MintInk,
                    disabledContainerColor = Outline,
                    disabledContentColor = TextSecondary
                )
            ) {
                Text("Скопировать", fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }

            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = CardDefaults.cardColors(containerColor = CardColor),
                border = BorderStroke(1.dp, Outline)
            ) {
                SelectionContainer {
                    Text(
                        text = details,
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(15.dp),
                        color = TextPrimary,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp,
                        lineHeight = 16.sp
                    )
                }
            }

            Spacer(Modifier.height(2.dp))
            Button(
                onClick = onBack,
                modifier = Modifier.fillMaxWidth().height(48.dp),
                shape = RoundedCornerShape(14.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = CardColor,
                    contentColor = TextPrimary
                )
            ) {
                Text("Назад", fontSize = 13.sp, fontWeight = FontWeight.Medium)
            }
        }
    }
}
