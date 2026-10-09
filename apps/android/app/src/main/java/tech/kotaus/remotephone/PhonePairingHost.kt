package tech.kotaus.remotephone

import android.content.Context
import android.content.Intent
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import org.java_websocket.WebSocket
import org.java_websocket.drafts.Draft
import org.java_websocket.drafts.Draft_6455
import org.java_websocket.extensions.IExtension
import org.java_websocket.handshake.ClientHandshake
import org.java_websocket.server.WebSocketServer
import org.json.JSONObject
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets
import java.util.Collections
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

private const val MAX_FRAME_BYTES = 256 * 1024
private const val MAX_RTC_SIGNAL_BYTES = 192 * 1024
private const val MAX_RTC_SDP_BYTES = 160 * 1024
private const val MAX_RTC_CANDIDATE_BYTES = 8192
private const val MAX_RTC_MID_BYTES = 256
private const val MAX_DIAGNOSTIC_CHARS = 12_000
private const val SESSION_LIFETIME_SECONDS = 5 * 60
private const val SERVICE_TYPE = "_remotephone._tcp."
private const val LOG_TAG = "RemotePhonePairing"
private const val RESUME_TIMEOUT_MS = 10_000L

internal enum class PhonePairingPhase {
    IDLE,
    STARTING,
    WAITING,
    VERIFYING,
    AUTHENTICATED,
    FAILED,
    LOCKED,
    EXPIRED,
    CLOSED
}

internal data class PhonePairingState(
    val phase: PhonePairingPhase = PhonePairingPhase.IDLE,
    val mode: PhoneStreamMode? = null,
    val quality: PhoneQualityProfile = PhoneQualityProfile.AUTO,
    val allowControl: Boolean = false,
    val streaming: Boolean = false,
    val telemetry: PhoneTelemetry? = null,
    val pin: String? = null,
    val message: String = "Сеанс сопряжения не запущен.",
    val addresses: List<String> = emptyList(),
    val port: Int? = null,
    val attemptsUsed: Int = 0,
    val secondsRemaining: Int = 0,
    val discoveryAvailable: Boolean = false,
    val awaitingResume: Boolean = false,
    val diagnosticText: String? = null
)

/**
 * Owns one temporary phone-side PIN session and its local WebSocket endpoint.
 * Lives inside the foreground service: it must not be tied to an activity.
 *
 * After a successful OPAQUE authentication the session stays resumable: the
 * WebSocket endpoint keeps listening, the signal cipher and its sequence
 * counters survive a transport drop, and the peer can reconnect with the same
 * session id without a new PIN while the phone keeps the session running.
 */
internal class PhonePairingHost(
    context: Context,
    val mode: PhoneStreamMode,
    val quality: PhoneQualityProfile,
    val allowControl: Boolean,
    private val projectionData: Intent?,
    private val onStateChanged: (PhonePairingState) -> Unit,
    private val onMediaTelemetry: (PhoneTelemetry) -> Unit,
    private val onMediaEnded: (String) -> Unit
) : AutoCloseable {
    private val applicationContext = context.applicationContext
    private val nsdManager = applicationContext.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val mainHandler = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "remote-phone-pairing").apply { isDaemon = true }
    }
    private val lock = Any()
    private val mediaLock = Any()
    private val signalCipherLock = Any()

    @Volatile private var state = PhonePairingState(
        mode = mode,
        quality = quality,
        allowControl = allowControl
    )
    @Volatile private var handle = 0L
    @Volatile private var pinForUi: String? = null
    @Volatile private var server: PairingWebSocketServer? = null
    @Volatile private var activeConnection: WebSocket? = null
    @Volatile private var registrationListener: NsdManager.RegistrationListener? = null
    @Volatile private var serviceRegistered = false
    @Volatile private var expiryAtElapsedRealtime = 0L
    @Volatile private var lastFailureMessage: String? = null
    @Volatile private var closed = false
    @Volatile private var mediaSession: PhoneMediaSession? = null
    @Volatile private var resumeDeadlineTask: Runnable? = null

    private val countdownTask = object : Runnable {
        override fun run() {
            if (closed || handle == 0L || expiryAtElapsedRealtime == 0L) return
            // Once authenticated the PIN window no longer bounds the session.
            if (state.phase == PhonePairingPhase.AUTHENTICATED) return
            val remainingMillis = (expiryAtElapsedRealtime - SystemClock.elapsedRealtime()).coerceAtLeast(0L)
            val remainingSeconds = ((remainingMillis + 999L) / 1000L).toInt()
            if (remainingSeconds == 0) {
                worker.execute { stopInternal(PhonePairingPhase.EXPIRED, "Срок действия PIN истёк. Создайте новый сеанс.") }
            } else {
                publish(state.copy(secondsRemaining = remainingSeconds))
                mainHandler.postDelayed(this, 1000L)
            }
        }
    }

    fun currentState(): PhonePairingState = state

    fun start() {
        if (closed || state.phase !in setOf(PhonePairingPhase.IDLE, PhonePairingPhase.CLOSED, PhonePairingPhase.FAILED, PhonePairingPhase.EXPIRED, PhonePairingPhase.LOCKED)) {
            return
        }
        publish(PhonePairingState(
            phase = PhonePairingPhase.STARTING,
            mode = mode,
            quality = quality,
            allowControl = allowControl,
            message = if (mode == PhoneStreamMode.CAMERA) {
                "Создаём PIN и готовим передачу с камеры…"
            } else {
                "Создаём PIN и готовим трансляцию экрана…"
            }
        ))
        worker.execute {
            val createStarted = SystemClock.elapsedRealtime()
            try {
                val nativeSession = NativePairing.phoneCreate()
                    ?: throw IllegalStateException("PAIRING_FAILED")
                if (nativeSession.size != 8 + 8 + 16) {
                    nativeSession.fill(0)
                    throw IllegalStateException("PAIRING_FAILED")
                }

                val decoded = ByteBuffer.wrap(nativeSession).order(ByteOrder.LITTLE_ENDIAN)
                val createdHandle = decoded.long
                val pinBytes = ByteArray(8)
                decoded.get(pinBytes)
                nativeSession.fill(0)

                if (createdHandle <= 0L) {
                    pinBytes.fill(0)
                    throw IllegalStateException("PAIRING_FAILED")
                }
                val pin = String(pinBytes, StandardCharsets.US_ASCII)
                pinBytes.fill(0)
                val suffix = UUID.randomUUID().toString().replace("-", "").take(6)

                handle = createdHandle
                pinForUi = pin
                expiryAtElapsedRealtime = createStarted + TimeUnit.SECONDS.toMillis(SESSION_LIFETIME_SECONDS.toLong())
                val createdServer = PairingWebSocketServer(createdHandle, "Видоискатель-$suffix")
                server = createdServer
                createdServer.start()
            } catch (error: LinkageError) {
                Log.e(LOG_TAG, "Нативный модуль сопряжения недоступен", error)
                stopInternal(
                    PhonePairingPhase.FAILED,
                    "Не удалось загрузить модуль сопряжения. Откройте «Диагностику» для просмотра причины.",
                    diagnosticDetails("Загрузка нативного модуля сопряжения", error)
                )
            } catch (error: Exception) {
                Log.e(LOG_TAG, "Не удалось запустить сеанс сопряжения", error)
                stopInternal(
                    PhonePairingPhase.FAILED,
                    "Не удалось запустить сопряжение. Откройте «Диагностику» для просмотра причины.",
                    diagnosticDetails("Запуск сеанса сопряжения", error)
                )
            }
        }
    }

    fun stop() {
        if (closed) return
        mainHandler.removeCallbacks(countdownTask)
        worker.execute {
            stopInternal(PhonePairingPhase.CLOSED, "Сеанс остановлен. PIN и временные ключи удалены.")
        }
    }

    override fun close() {
        if (closed) return
        stop()
        closed = true
        worker.shutdown()
    }

    // ------------------------------------------------------------------
    // Media session control (called by the foreground service)
    // ------------------------------------------------------------------

    fun applyQualityToMedia(next: PhoneQualityProfile) {
        mediaSession?.applyQuality(next)
        state = state.copy(quality = next)
    }

    fun rotateMediaCapture() {
        mediaSession?.rotateCapture()
    }

    fun stopMedia(message: String) {
        val session = synchronized(mediaLock) {
            mediaSession.also { mediaSession = null }
        }
        session?.close()
        if (handle != 0L && state.phase == PhonePairingPhase.AUTHENTICATED) {
            publish(state.copy(streaming = false, telemetry = null, message = message))
        }
        onMediaEnded(message)
    }

    private fun getOrCreateMediaSession(connection: WebSocket, pairingHandle: Long): PhoneMediaSession {
        synchronized(mediaLock) {
            mediaSession?.let { return it }
            var createdSession: PhoneMediaSession? = null
            val created = PhoneMediaSession(
                applicationContext,
                mode,
                quality,
                projectionData,
                sendSignal = { outgoing ->
                    val current = activeConnection
                    if (current != null && handle == pairingHandle) {
                        sendEncryptedRtcSignal(current, pairingHandle, outgoing)
                    }
                },
                onStatus = { message ->
                    if (handle == pairingHandle && state.phase == PhonePairingPhase.AUTHENTICATED) {
                        publish(state.copy(streaming = true, message = message))
                    }
                },
                onTelemetry = { telemetry ->
                    if (handle == pairingHandle && state.phase == PhonePairingPhase.AUTHENTICATED) {
                        onMediaTelemetry(telemetry)
                    }
                },
                onEnded = { message ->
                    synchronized(mediaLock) {
                        if (mediaSession != null && mediaSession === createdSession) mediaSession = null
                    }
                    if (handle == pairingHandle && state.phase == PhonePairingPhase.AUTHENTICATED) {
                        publish(state.copy(streaming = false, telemetry = null, message = message))
                    }
                    onMediaEnded(message)
                }
            )
            createdSession = created
            mediaSession = created
            return created
        }
    }

    private fun closeMediaQuietly() {
        val session = synchronized(mediaLock) { mediaSession.also { mediaSession = null } }
        session?.close()
    }

    // ------------------------------------------------------------------
    // State helpers
    // ------------------------------------------------------------------

    private fun diagnosticDetails(stage: String, error: Throwable): String {
        val heading = "$stage\n"
        val stack = error.stackTraceToString()
        val combined = heading + stack
        return if (combined.length <= MAX_DIAGNOSTIC_CHARS) combined
        else combined.take(MAX_DIAGNOSTIC_CHARS) + "\n… журнал ошибки усечён"
    }

    private fun safeAttemptsUsed(): Int {
        val currentHandle = handle
        if (currentHandle == 0L) return 0
        return synchronized(signalCipherLock) {
            runCatching { NativePairing.phoneAttemptsUsed(currentHandle).coerceAtLeast(0) }
                .getOrDefault(0)
        }
    }

    private fun publish(next: PhonePairingState) {
        state = next
        mainHandler.post {
            if (!closed || next.phase == PhonePairingPhase.CLOSED) onStateChanged(next)
        }
    }

    private fun localIpv4Addresses(): List<String> = runCatching {
        Collections.list(NetworkInterface.getNetworkInterfaces())
            .asSequence()
            .filter { it.isUp && !it.isLoopback && isLanInterface(it.name) }
            .flatMap { network -> Collections.list(network.inetAddresses).asSequence() }
            .filterIsInstance<Inet4Address>()
            .filter { it.isSiteLocalAddress && !it.isLoopbackAddress }
            .map { it.hostAddress }
            .distinct()
            .toList()
    }.getOrDefault(emptyList())

    private fun isLanInterface(name: String): Boolean {
        val normalized = name.lowercase()
        return normalized.startsWith("wlan")
            || normalized.startsWith("wifi")
            || normalized.startsWith("swlan")
            || normalized.startsWith("ap")
            || normalized.startsWith("eth")
    }

    private fun isPeerOnLocalLan(remoteSocketAddress: java.net.SocketAddress?): Boolean = runCatching {
        val remote = (remoteSocketAddress as? InetSocketAddress)?.address as? Inet4Address ?: return false
        if (!remote.isSiteLocalAddress && !remote.isLinkLocalAddress) return false
        Collections.list(NetworkInterface.getNetworkInterfaces())
            .asSequence()
            .filter { it.isUp && !it.isLoopback && isLanInterface(it.name) }
            .flatMap { network -> network.interfaceAddresses.asSequence() }
            .any { interfaceAddress ->
                val local = interfaceAddress.address as? Inet4Address ?: return@any false
                val prefixLength = interfaceAddress.networkPrefixLength.toInt()
                prefixLength in 1..32 && sameIpv4Subnet(local.address, remote.address, prefixLength)
            }
    }.getOrDefault(false)

    private fun sameIpv4Subnet(local: ByteArray, remote: ByteArray, prefixLength: Int): Boolean {
        for (bit in 0 until prefixLength) {
            val byteIndex = bit / 8
            val mask = 0x80 ushr (bit % 8)
            if ((local[byteIndex].toInt() and 0xff and mask)
                != (remote[byteIndex].toInt() and 0xff and mask)
            ) {
                return false
            }
        }
        return true
    }

    // ------------------------------------------------------------------
    // Protected SIGNAL parsing and routing
    // ------------------------------------------------------------------

    private fun parseRtcSignal(payload: ByteArray): JSONObject {
        if (payload.isEmpty() || payload.size > MAX_RTC_SIGNAL_BYTES) {
            throw IllegalArgumentException("RTC_SIGNAL_SIZE")
        }
        val jsonText = StandardCharsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(payload))
            .toString()
        val signal = JSONObject(jsonText)
        val version = signal.opt("v") as? Number
        if (version == null || version.toDouble() != 1.0) throw IllegalArgumentException("RTC_SIGNAL_VERSION")

        when (signal.optString("type")) {
            "offer" -> {
                val sdp = signal.opt("sdp") as? String ?: throw IllegalArgumentException("RTC_SDP")
                if (sdp.isBlank() || sdp.toByteArray(StandardCharsets.UTF_8).size > MAX_RTC_SDP_BYTES) {
                    throw IllegalArgumentException("RTC_SDP_SIZE")
                }
            }
            "ice" -> {
                val candidate = signal.opt("candidate") as? String ?: throw IllegalArgumentException("RTC_ICE")
                if (candidate.isBlank() || candidate.toByteArray(StandardCharsets.UTF_8).size > MAX_RTC_CANDIDATE_BYTES) {
                    throw IllegalArgumentException("RTC_ICE_SIZE")
                }
                val mid = signal.opt("sdpMid")
                if (mid != null && mid !== JSONObject.NULL
                    && (mid !is String || mid.toByteArray(StandardCharsets.UTF_8).size > MAX_RTC_MID_BYTES)) {
                    throw IllegalArgumentException("RTC_MID")
                }
                val lineIndex = signal.opt("sdpMLineIndex")
                if (lineIndex != null && lineIndex !== JSONObject.NULL) {
                    val numeric = lineIndex as? Number ?: throw IllegalArgumentException("RTC_MLINE")
                    val index = numeric.toInt()
                    if (index !in 0..255 || numeric.toDouble() != index.toDouble()) {
                        throw IllegalArgumentException("RTC_MLINE")
                    }
                }
            }
            "bye" -> Unit
            "error" -> {
                val code = signal.opt("code") as? String ?: throw IllegalArgumentException("RTC_ERROR")
                if (!code.matches(Regex("[A-Z0-9_]{1,48}"))) throw IllegalArgumentException("RTC_ERROR")
            }
            else -> throw IllegalArgumentException("RTC_SIGNAL_TYPE")
        }
        return signal
    }

    private fun handleRtcSignal(connection: WebSocket, pairingHandle: Long, signal: JSONObject) {
        val type = signal.optString("type")
        if (type == "bye" || type == "error") {
            val code = signal.optString("code")
            val message = when {
                type == "bye" -> "Компьютер завершил видеосеанс."
                code == "CAMERA_NOT_ENABLED" -> "Компьютер сообщил: камера не разрешена."
                code.isNotBlank() -> "Компьютер сообщил об ошибке видеосеанса ($code)."
                else -> "Компьютер сообщил об ошибке видеосеанса."
            }
            cancelResumeDeadline()
            closeMediaQuietly()
            if (connection === activeConnection || activeConnection == null) {
                if (state.phase == PhonePairingPhase.AUTHENTICATED) {
                    publish(state.copy(streaming = false, telemetry = null, message = message))
                }
            }
            return
        }

        synchronized(mediaLock) {
            if (connection !== activeConnection || handle != pairingHandle) return
            cancelResumeDeadline()
            val session = getOrCreateMediaSession(connection, pairingHandle)
            session.handleSignal(signal)
        }
    }

    private fun sendEncryptedRtcSignal(connection: WebSocket, pairingHandle: Long, signal: JSONObject) {
        val plaintext = signal.toString().toByteArray(StandardCharsets.UTF_8)
        if (plaintext.isEmpty() || plaintext.size > MAX_RTC_SIGNAL_BYTES) {
            plaintext.fill(0)
            throw IllegalArgumentException("RTC_SIGNAL_SIZE")
        }
        val encrypted = try {
            synchronized(signalCipherLock) {
                if (handle != pairingHandle
                    || !NativePairing.phoneIsAuthenticated(pairingHandle)) {
                    throw IllegalStateException("RTC_SIGNAL_SESSION_CLOSED")
                }
                NativePairing.phoneEncryptSignal(pairingHandle, plaintext)
                    ?: throw IllegalStateException("RTC_SIGNAL_ENCRYPTION_FAILED")
            }
        } finally {
            plaintext.fill(0)
        }
        // Java-WebSocket may retain the backing array in its asynchronous write queue.
        // This buffer is ciphertext; do not overwrite it before the network write completes.
        connection.send(encrypted)
    }

    private fun scheduleResumeDeadline(connection: WebSocket, pairingHandle: Long) {
        cancelResumeDeadline()
        val task = Runnable {
            resumeDeadlineTask = null
            if (closed || handle != pairingHandle) return@Runnable
            if (connection !== activeConnection) return@Runnable
            // The resumed peer never proved possession of the session keys.
            connection.close(1008, "Возобновление не подтверждено")
            activeConnection = null
            publish(state.copy(
                awaitingResume = true,
                message = "Повторное подключение не подтверждено. Компьютер может попробовать снова."
            ))
        }
        resumeDeadlineTask = task
        mainHandler.postDelayed(task, RESUME_TIMEOUT_MS)
    }

    private fun cancelResumeDeadline() {
        resumeDeadlineTask?.let { mainHandler.removeCallbacks(it) }
        resumeDeadlineTask = null
    }

    // ------------------------------------------------------------------
    // WebSocket endpoint
    // ------------------------------------------------------------------

    private inner class PairingWebSocketServer(
        private val pairingHandle: Long,
        val serviceName: String
    ) : WebSocketServer(
        InetSocketAddress(0),
        1,
        listOf<Draft>(Draft_6455(emptyList<IExtension>(), MAX_FRAME_BYTES))
    ) {
        init {
            connectionLostTimeout = 30
        }

        override fun onStart() {
            onServerStarted(this, port)
        }

        override fun onOpen(connection: WebSocket, handshake: ClientHandshake) {
            if (!isPeerOnLocalLan(connection.remoteSocketAddress)) {
                connection.close(1008, "Только локальная сеть")
                return
            }
            val resuming = synchronized(lock) {
                if (closed || handle != pairingHandle || activeConnection != null) {
                    connection.close(1008, "Сеанс занят")
                    return
                }
                if (state.phase == PhonePairingPhase.AUTHENTICATED) {
                    activeConnection = connection
                    true
                } else {
                    activeConnection = connection
                    lastFailureMessage = null
                    false
                }
            }
            if (resuming) {
                try {
                    val resumedHello = synchronized(signalCipherLock) {
                        if (connection !== activeConnection || handle != pairingHandle) {
                            throw IllegalStateException("PAIRING_FAILED")
                        }
                        NativePairing.phoneResumeConnection(pairingHandle)
                            ?: throw IllegalStateException("PAIRING_FAILED")
                    }
                    connection.send(resumedHello)
                    publish(state.copy(
                        awaitingResume = true,
                        message = "Соединение восстанавливается без нового PIN…"
                    ))
                    scheduleResumeDeadline(connection, pairingHandle)
                } catch (error: Exception) {
                    Log.e(LOG_TAG, "Не удалось возобновить защищённый сеанс", error)
                    activeConnection = null
                    failConnection(connection, "Не удалось возобновить защищённый сеанс.", error)
                }
                return
            }
            try {
                val hello = synchronized(signalCipherLock) {
                    if (connection !== activeConnection || handle != pairingHandle) {
                        throw IllegalStateException("PAIRING_FAILED")
                    }
                    NativePairing.phoneStartConnection(pairingHandle)
                        ?: throw IllegalStateException("PAIRING_FAILED")
                }
                connection.send(hello)
                publish(state.copy(phase = PhonePairingPhase.VERIFYING, message = "Компьютер подключён. Проверяем код…"))
            } catch (error: LinkageError) {
                Log.e(LOG_TAG, "Ошибка вызова нативного модуля при начале сопряжения", error)
                failConnection(
                    connection,
                    "Не удалось запустить нативное сопряжение. Откройте «Диагностику» для просмотра причины.",
                    error
                )
            } catch (error: Exception) {
                Log.e(LOG_TAG, "Не удалось начать защищённое сопряжение", error)
                failConnection(connection, "Не удалось начать защищённое сопряжение.", error)
            }
        }

        override fun onMessage(connection: WebSocket, message: ByteBuffer) {
            if (connection !== activeConnection || handle != pairingHandle) return
            if (!message.hasRemaining() || message.remaining() > MAX_FRAME_BYTES) {
                failConnection(connection, "Получено некорректное сообщение. Попробуйте создать новый сеанс.")
                return
            }
            val frame = ByteArray(message.remaining())
            message.get(frame)
            try {
                val alreadyAuthenticated = synchronized(signalCipherLock) {
                    if (connection !== activeConnection || handle != pairingHandle) return
                    NativePairing.phoneIsAuthenticated(pairingHandle)
                }
                if (alreadyAuthenticated) {
                    val plaintext = synchronized(signalCipherLock) {
                        if (connection !== activeConnection || handle != pairingHandle) return
                        NativePairing.phoneDecryptSignal(pairingHandle, frame)
                            ?: throw IllegalStateException("PAIRING_FAILED")
                    }
                    try {
                        val signal = parseRtcSignal(plaintext)
                        cancelResumeDeadline()
                        handleRtcSignal(connection, pairingHandle, signal)
                    } finally {
                        plaintext.fill(0)
                    }
                    return
                }
                val handshakeResult = synchronized(signalCipherLock) {
                    if (connection !== activeConnection || handle != pairingHandle) return
                    val reply = NativePairing.phoneHandleFrame(pairingHandle, frame)
                    val attempts = NativePairing.phoneAttemptsUsed(pairingHandle).coerceAtLeast(0)
                    val authenticated = NativePairing.phoneIsAuthenticated(pairingHandle)
                    Triple(reply, attempts, authenticated)
                }
                val reply = handshakeResult.first
                val attempts = handshakeResult.second
                if (reply != null) connection.send(reply)
                if (handshakeResult.third) {
                    mainHandler.removeCallbacks(countdownTask)
                    publish(state.copy(
                        phase = PhonePairingPhase.AUTHENTICATED,
                        pin = null,
                        attemptsUsed = attempts,
                        awaitingResume = false,
                        message = "Защищённое сопряжение подтверждено. Трансляция переживёт сворачивание приложения и блокировку экрана."
                    ))
                } else {
                    publish(state.copy(attemptsUsed = attempts))
                }
            } catch (error: LinkageError) {
                Log.e(LOG_TAG, "Ошибка вызова нативного модуля при обработке кадра", error)
                connection.close(1008, "Нативный модуль недоступен")
                val details = diagnosticDetails("Обработка кадра нативным модулем", error)
                worker.execute {
                    stopInternal(
                        PhonePairingPhase.FAILED,
                        "Нативный модуль сопряжения недоступен. Откройте «Диагностику» для просмотра причины.",
                        details
                    )
                }
                return
            } catch (error: Exception) {
                Log.e(LOG_TAG, "Не удалось обработать кадр сопряжения", error)
                val details = diagnosticDetails("Обработка кадра сопряжения", error)
                if (state.phase == PhonePairingPhase.AUTHENTICATED) {
                    connection.close(1008, "Защищённый кадр отклонён")
                    worker.execute {
                        stopInternal(
                            PhonePairingPhase.CLOSED,
                            "Защищённый кадр не прошёл проверку. Откройте «Диагностику» для просмотра причины.",
                            details
                        )
                    }
                    return
                }
                val attempts = safeAttemptsUsed()
                if (attempts >= 5) {
                    worker.execute {
                        stopInternal(
                            PhonePairingPhase.LOCKED,
                            "Достигнут предел попыток. Создайте новый PIN вручную.",
                            details
                        )
                    }
                    connection.close(1008, "Сеанс заблокирован")
                } else {
                    failConnection(
                        connection,
                        "Не удалось подтвердить код. Проверьте PIN на телефоне и попробуйте снова.",
                        error
                    )
                }
            } finally {
                frame.fill(0)
            }
        }

        override fun onMessage(connection: WebSocket, message: String) {
            failConnection(connection, "Принимаются только двоичные сообщения защищённого протокола.")
        }

        override fun onClose(connection: WebSocket, code: Int, reason: String, remote: Boolean) {
            synchronized(lock) {
                if (connection !== activeConnection) return
                activeConnection = null
            }
            val authenticated = synchronized(signalCipherLock) {
                if (handle != pairingHandle) return
                runCatching { NativePairing.phoneIsAuthenticated(pairingHandle) }.getOrDefault(false)
            }
            if (authenticated) {
                cancelResumeDeadline()
                // Keep the session, the media stream and the endpoint alive so
                // the computer can reconnect without a new PIN.
                publish(state.copy(
                    awaitingResume = true,
                    message = "Соединение прервано. Компьютер может переподключиться без нового PIN, пока сеанс активен."
                ))
            } else {
                synchronized(signalCipherLock) {
                    if (handle == pairingHandle) runCatching { NativePairing.phoneAbortConnection(pairingHandle) }
                }
                val attempts = safeAttemptsUsed()
                if (attempts >= 5) {
                    worker.execute {
                        stopInternal(PhonePairingPhase.LOCKED, "Достигнут предел попыток. Создайте новый PIN вручную.")
                    }
                } else if (!closed) {
                    val failure = lastFailureMessage
                    lastFailureMessage = null
                    publish(state.copy(
                        phase = PhonePairingPhase.WAITING,
                        attemptsUsed = attempts,
                        message = if (failure == null) {
                            "Ожидаем повторное подключение. Использовано попыток: $attempts из 5."
                        } else {
                            "$failure Использовано попыток: $attempts из 5."
                        }
                    ))
                }
            }
        }

        override fun onError(connection: WebSocket?, error: Exception) {
            if (connection != null && connection === activeConnection) {
                failConnection(
                    connection,
                    "Ошибка локального соединения. Откройте «Диагностику» для просмотра причины.",
                    error
                )
            }
        }

        private fun failConnection(connection: WebSocket, message: String, error: Throwable? = null) {
            if (connection !== activeConnection) return
            synchronized(signalCipherLock) {
                if (handle == pairingHandle) runCatching { NativePairing.phoneAbortConnection(pairingHandle) }
            }
            lastFailureMessage = message
            publish(state.copy(
                phase = if (state.phase == PhonePairingPhase.AUTHENTICATED) state.phase else PhonePairingPhase.WAITING,
                attemptsUsed = safeAttemptsUsed(),
                message = message,
                diagnosticText = error?.let { diagnosticDetails("Обработка соединения", it) } ?: state.diagnosticText
            ))
            connection.close(1008, "Сопряжение отклонено")
        }
    }

    private fun onServerStarted(webSocketServer: PairingWebSocketServer, port: Int) {
        if (closed || server !== webSocketServer) return
        val addresses = localIpv4Addresses()
        val currentPin = pinForUi
        publish(PhonePairingState(
            phase = PhonePairingPhase.WAITING,
            mode = mode,
            quality = quality,
            allowControl = allowControl,
            pin = currentPin,
            message = "Ожидаем компьютер в этой же локальной сети.",
            addresses = addresses,
            port = port,
            attemptsUsed = safeAttemptsUsed(),
            secondsRemaining = ((expiryAtElapsedRealtime - SystemClock.elapsedRealtime() + 999L) / 1000L)
                .coerceAtLeast(0L).toInt()
        ))
        registerService(webSocketServer.serviceName, port)
        mainHandler.removeCallbacks(countdownTask)
        mainHandler.post(countdownTask)
    }

    private fun registerService(serviceName: String, port: Int) {
        val info = NsdServiceInfo().apply {
            this.serviceName = serviceName
            serviceType = SERVICE_TYPE
            this.port = port
            setAttribute("version", "1")
            setAttribute("profile", "0001")
        }
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(registeredServiceInfo: NsdServiceInfo) {
                if (closed || handle == 0L || registrationListener !== this) {
                    runCatching { nsdManager.unregisterService(this) }
                    return
                }
                serviceRegistered = true
                publish(state.copy(discoveryAvailable = true))
            }

            override fun onRegistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                if (registrationListener !== this) return
                serviceRegistered = false
                publish(state.copy(
                    discoveryAvailable = false,
                    message = "Поиск телефона может быть недоступен; используйте адрес и порт ниже."
                ))
            }

            override fun onServiceUnregistered(serviceInfo: NsdServiceInfo) {
                if (registrationListener === this) serviceRegistered = false
            }

            override fun onUnregistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                if (registrationListener === this) serviceRegistered = false
            }
        }
        registrationListener = listener
        runCatching {
            nsdManager.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
        }.onFailure {
            serviceRegistered = false
            publish(state.copy(
                discoveryAvailable = false,
                message = "Поиск телефона недоступен; используйте адрес и порт ниже."
            ))
        }
    }

    private fun stopInternal(
        finalPhase: PhonePairingPhase,
        finalMessage: String,
        diagnosticText: String? = null
    ) {
        mainHandler.removeCallbacks(countdownTask)
        cancelResumeDeadline()
        closeMediaQuietly()
        val currentServer = server
        server = null
        if (currentServer != null) {
            runCatching { currentServer.stop(1000) }
        }
        activeConnection = null

        val listener = registrationListener
        registrationListener = null
        if (listener != null && serviceRegistered) {
            runCatching { nsdManager.unregisterService(listener) }
        }
        serviceRegistered = false

        val oldHandle = handle
        handle = 0L
        pinForUi = null
        expiryAtElapsedRealtime = 0L
        if (oldHandle != 0L) runCatching {
            synchronized(signalCipherLock) { NativePairing.phoneDestroy(oldHandle) }
        }
        publish(PhonePairingState(
            phase = finalPhase,
            mode = mode,
            quality = quality,
            allowControl = allowControl,
            message = finalMessage,
            diagnosticText = diagnosticText
        ))
    }
}
