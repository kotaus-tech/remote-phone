package tech.kotaus.remotephone

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

private const val NOTIFICATION_ID = 41
private const val CHANNEL_ID = "remote-phone-session"
private const val ACTION_START = "tech.kotaus.remotephone.action.START_SESSION"
private const val ACTION_STOP = "tech.kotaus.remotephone.action.STOP_SESSION"
private const val EXTRA_MODE = "mode"
private const val EXTRA_QUALITY = "quality"
private const val EXTRA_ALLOW_CONTROL = "allowControl"
private const val EXTRA_PROJECTION = "projection"

/**
 * Application-scoped bridge between the foreground service and Compose UI.
 * The service owns the pairing host and the media session; the activity only
 * renders its published state and issues start/stop requests.
 */
object PhoneSessionController {
    private val _uiState = MutableStateFlow(PhonePairingState())
    val uiState: StateFlow<PhonePairingState> = _uiState.asStateFlow()

    fun publish(state: PhonePairingState) {
        _uiState.value = state
    }

    fun start(
        context: Context,
        mode: PhoneStreamMode,
        quality: PhoneQualityProfile,
        allowControl: Boolean,
        projectionData: Intent?
    ) {
        val intent = Intent(context, PhoneSessionService::class.java).apply {
            action = ACTION_START
            putExtra(EXTRA_MODE, mode.wireName)
            putExtra(EXTRA_QUALITY, quality.wireName)
            putExtra(EXTRA_ALLOW_CONTROL, allowControl)
            projectionData?.let { putExtra(EXTRA_PROJECTION, it) }
        }
        ContextCompat.startForegroundService(context, intent)
    }

    fun stop(context: Context) {
        val intent = Intent(context, PhoneSessionService::class.java).apply {
            action = ACTION_STOP
        }
        ContextCompat.startForegroundService(context, intent)
    }
}

/**
 * Long-lived foreground service that owns one pairing session and one media
 * session (camera or screen). It survives activity minimization, app
 * switching, rotation and screen off; a persistent notification can stop it.
 */
class PhoneSessionService : Service() {
    private var pairingHost: PhonePairingHost? = null
    private var lastState = PhonePairingState()
    private var thermalDegraded = false
    private val mainHandler = Handler(Looper.getMainLooper())

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) = Unit
        override fun onDisplayRemoved(displayId: Int) = Unit
        override fun onDisplayChanged(displayId: Int) {
            if (displayId == android.view.Display.DEFAULT_DISPLAY) {
                pairingHost?.rotateMediaCapture()
            }
        }
    }

    private val thermalListener = object : PowerManager.OnThermalStatusChangedListener {
        override fun onThermalStatusChanged(status: Int) {
            mainHandler.post { handleThermalStatus(status) }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val manager = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Активный сеанс «Видоискателя»",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Состояние трансляции и кнопка остановки"
                setShowBadge(false)
            }
            manager.createNotificationChannel(channel)
        }
        getSystemService(DisplayManager::class.java)?.registerDisplayListener(
            displayListener, mainHandler
        )
        val powerManager = getSystemService(PowerManager::class.java)
        if (Build.VERSION.SDK_INT >= 30 && powerManager != null) {
            runCatching {
                powerManager.addThermalStatusListener(mainHandler::post, thermalListener)
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> startSession(intent)
            ACTION_STOP -> stopSession("Сеанс остановлен. PIN и временные ключи удалены.")
            else -> stopSession("Сеанс остановлен.")
        }
        return START_NOT_STICKY
    }

    private fun startSession(intent: Intent) {
        val mode = PhoneStreamMode.fromWire(intent.getStringExtra(EXTRA_MODE))
            ?: PhoneStreamMode.CAMERA
        val quality = PhoneQualityProfile.fromWire(intent.getStringExtra(EXTRA_QUALITY))
            ?: PhoneQualityProfile.AUTO
        val allowControl = intent.getBooleanExtra(EXTRA_ALLOW_CONTROL, false)
        val projection = runCatching {
            if (Build.VERSION.SDK_INT >= 33) {
                intent.getParcelableExtra(EXTRA_PROJECTION, Intent::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(EXTRA_PROJECTION)
            }
        }.getOrNull()

        if (pairingHost != null) {
            // A session is already running; ignore duplicate start requests.
            return
        }

        val foregroundType = when (mode) {
            PhoneStreamMode.SCREEN -> {
                if (projection == null) {
                    stopSelf()
                    return
                }
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            }
            PhoneStreamMode.CAMERA -> ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
        }
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification("Подготовка сеанса…"),
            foregroundType
        )

        val host = PhonePairingHost(
            this,
            mode,
            quality,
            allowControl,
            projection,
            onStateChanged = { state ->
                lastState = state
                PhoneSessionController.publish(state)
                updateNotification(state)
                if (state.phase.isTerminal()) {
                    stopHost()
                }
            },
            onMediaTelemetry = { telemetry ->
                lastState = lastState.copy(telemetry = telemetry)
                PhoneSessionController.publish(lastState)
            },
            onMediaEnded = { message ->
                lastState = lastState.copy(
                    streaming = false,
                    telemetry = null,
                    message = message
                )
                PhoneSessionController.publish(lastState)
                updateNotification(lastState)
            }
        )
        pairingHost = host
        host.start()
    }

    private fun handleThermalStatus(status: Int) {
        val host = pairingHost ?: return
        when {
            status >= PowerManager.THERMAL_STATUS_EMERGENCY -> {
                stopStreamOnly("Аварийный перегрев: трансляция остановлена для защиты телефона.")
            }
            status >= PowerManager.THERMAL_STATUS_SEVERE -> {
                if (!thermalDegraded) {
                    thermalDegraded = true
                    host.applyQualityToMedia(PhoneQualityProfile.ECONOMY)
                    lastState = lastState.copy(
                        message = "Телефон сильно нагревается: качество снижено, чтобы трансляция не прервалась."
                    )
                    PhoneSessionController.publish(lastState)
                    updateNotification(lastState)
                }
            }
            status >= PowerManager.THERMAL_STATUS_MODERATE -> {
                if (!thermalDegraded) {
                    thermalDegraded = true
                    host.applyQualityToMedia(PhoneQualityProfile.ECONOMY)
                    lastState = lastState.copy(
                        message = "Телефон нагревается: включён экономный режим качества."
                    )
                    PhoneSessionController.publish(lastState)
                    updateNotification(lastState)
                }
            }
            else -> {
                if (thermalDegraded && status <= PowerManager.THERMAL_STATUS_LIGHT) {
                    thermalDegraded = false
                    host.applyQualityToMedia(PhoneQualityProfile.AUTO)
                    lastState = lastState.copy(message = "Температура нормализовалась: качество «Авто».")
                    PhoneSessionController.publish(lastState)
                }
            }
        }
    }

    private fun stopStreamOnly(message: String) {
        pairingHost?.stopMedia(message)
    }

    private fun stopSession(message: String) {
        stopHost()
        lastState = PhonePairingState(
            phase = PhonePairingPhase.CLOSED,
            message = message
        )
        PhoneSessionController.publish(lastState)
        stopForegroundCompat()
        stopSelf()
    }

    private fun stopHost() {
        val host = pairingHost
        pairingHost = null
        host?.close()
    }

    private fun stopForegroundCompat() {
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
    }

    override fun onDestroy() {
        stopHost()
        getSystemService(DisplayManager::class.java)?.unregisterDisplayListener(displayListener)
        val powerManager = getSystemService(PowerManager::class.java)
        if (Build.VERSION.SDK_INT >= 30 && powerManager != null) {
            runCatching { powerManager.removeThermalStatusListener(thermalListener) }
        }
        stopForegroundCompat()
        super.onDestroy()
    }

    private fun updateNotification(state: PhonePairingState) {
        getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, buildNotification(state))
    }

    private fun buildNotification(state: PhonePairingState): Notification =
        buildNotification(state.message)

    private fun buildNotification(text: String): Notification {
        val modeTitle = lastState.mode?.title
        val headline = if (modeTitle != null) "Видоискатель — ${modeTitle}" else "Видоискатель"
        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val stopIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, PhoneSessionService::class.java).apply { action = ACTION_STOP },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentTitle(headline)
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .addAction(0, "Остановить", stopIntent)
            .build()
    }
}

internal fun PhonePairingPhase.isTerminal(): Boolean = this in setOf(
    PhonePairingPhase.FAILED,
    PhonePairingPhase.LOCKED,
    PhonePairingPhase.EXPIRED,
    PhonePairingPhase.CLOSED
)
