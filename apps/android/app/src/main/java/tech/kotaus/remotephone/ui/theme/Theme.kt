package tech.kotaus.remotephone.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val RemotePhoneColors = darkColorScheme(
    primary = Mint,
    onPrimary = MintInk,
    secondary = Color(0xFF94CAFF),
    onSecondary = Night,
    background = Night,
    onBackground = TextPrimary,
    surface = Surface,
    onSurface = TextPrimary,
    surfaceVariant = Card,
    onSurfaceVariant = TextSecondary,
    outline = Outline,
    error = Danger,
    onError = Night
)

@Composable
fun RemotePhoneTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = RemotePhoneColors,
        content = content
    )
}
