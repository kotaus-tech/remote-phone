package tech.kotaus.remotephone

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.media.projection.MediaProjection
import android.os.BatteryManager
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.view.WindowManager
import androidx.core.content.ContextCompat
import org.json.JSONObject
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraEnumerationAndroid
import org.webrtc.CameraVideoCapturer
import org.webrtc.DataChannel
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpParameters
import org.webrtc.RtpTransceiver
import org.webrtc.RtpReceiver
import org.webrtc.ScreenCapturerAndroid
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoCapturer
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Product modes; exactly one media session can run per phone at a time. */
internal enum class PhoneStreamMode(val wireName: String, val title: String) {
    CAMERA("camera", "Веб-камера"),
    SCREEN("screen", "Экран");

    companion object {
        fun fromWire(value: String?): PhoneStreamMode? =
            entries.firstOrNull { it.wireName == value }
    }
}

/**
 * Live quality profile. Limits are applied to the WebRTC sender and to the
 * capture format; they never introduce artificial buffering latency.
 */
internal enum class PhoneQualityProfile(
    val wireName: String,
    val title: String,
    val maxBitrateBps: Int,
    val maxFpsMilli: Int,
    val maxPixelCount: Long
) {
    AUTO("auto", "Авто", 4_000_000, 30_000, 2_073_600L),
    MAX("max", "Максимум", 12_000_000, 60_000, 8_294_400L),
    ECONOMY("economy", "Экономия", 1_500_000, 15_000, 921_600L);

    companion object {
        fun fromWire(value: String?): PhoneQualityProfile? =
            entries.firstOrNull { it.wireName == value }
    }
}

/** One telemetry snapshot published to the Android UI and to the Windows app. */
internal data class PhoneTelemetry(
    val fps: Double = 0.0,
    val bitrateKbps: Double = 0.0,
    val width: Int = 0,
    val height: Int = 0,
    val framesTotal: Long = 0,
    val framesDropped: Long = 0,
    val batteryPercent: Int = -1,
    val charging: Boolean = false,
    val thermalLabel: String = "Норма",
    val thermalLevel: Int = 0
)

internal fun thermalStatusLabel(status: Int): String = when (status) {
    PowerManager.THERMAL_STATUS_NONE -> "Норма"
    PowerManager.THERMAL_STATUS_LIGHT -> "Лёгкий нагрев"
    PowerManager.THERMAL_STATUS_MODERATE -> "Умеренный нагрев"
    PowerManager.THERMAL_STATUS_SEVERE -> "Сильный нагрев"
    PowerManager.THERMAL_STATUS_CRITICAL -> "Критический нагрев"
    PowerManager.THERMAL_STATUS_EMERGENCY,
    PowerManager.THERMAL_STATUS_SHUTDOWN -> "Аварийный перегрев"
    else -> "Норма"
}

private const val MAX_RTC_CANDIDATES = 128
private const val MAX_RTC_CANDIDATE_BYTES = 8192
private const val MAX_RTC_MID_BYTES = 256
private const val MAX_DATA_MESSAGE_BYTES = 4096
private const val MAX_CONTROL_MESSAGES = 512
private const val TELEMETRY_PERIOD_MS = 2000L
private const val CONTROL_CHANNEL_LABEL = "remote-phone-control"

/**
 * One live media session (camera or screen) transported over WebRTC.
 * The video source and its track outlive transport renegotiations: a fresh
 * desktop offer attaches to the same track, so Wi-Fi reconnects never restart
 * the camera or the MediaProjection capture.
 */
internal class PhoneMediaSession(
    private val context: Context,
    val mode: PhoneStreamMode,
    quality: PhoneQualityProfile,
    private val projectionData: Intent?,
    private val sendSignal: (JSONObject) -> Unit,
    private val onStatus: (String) -> Unit,
    private val onTelemetry: (PhoneTelemetry) -> Unit,
    private val onEnded: (String) -> Unit
) : AutoCloseable {
    @Volatile var quality: PhoneQualityProfile = quality
        private set

    private val thread = HandlerThread("remote-phone-media").apply { start() }
    private val handler = Handler(thread.looper)
    private val closeLock = Any()

    @Volatile private var closed = false
    @Volatile private var ended = false
    @Volatile private var streamingStarted = false

    private var factory: PeerConnectionFactory? = null
    private var eglBase: EglBase? = null
    private var textureHelper: SurfaceTextureHelper? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var capturer: CameraVideoCapturer? = null
    private var captureWidth = 0
    private var captureHeight = 0
    private var captureFps = 0

    private var peerConnection: PeerConnection? = null
    private var senderMaxBitrateApplied = false
    private var controlChannel: DataChannel? = null
    private var controlMessageCount = 0
    private var remoteDescriptionSet = false
    private var answerSent = false
    private var remoteCandidateCount = 0
    private var localCandidateCount = 0
    private val pendingRemoteCandidates = mutableListOf<IceCandidate>()
    private val pendingLocalCandidates = mutableListOf<IceCandidate>()

    private var statsScheduled = false
    private var prevStatsBytes: Long? = null
    private var prevStatsTimeMs: Long? = null

    fun handleSignal(signal: JSONObject) {
        if (closed) return
        handler.post {
            if (closed) return@post
            when (signal.optString("type")) {
                "offer" -> handleOffer(signal)
                "ice" -> handleIce(signal)
                "bye", "error" -> endSession("Компьютер завершил видеосеанс.")
                else -> fail("Телефон получил неизвестный тип WebRTC-сигнализации.")
            }
        }
    }

    /** Applies a quality change requested live from the Windows app. */
    fun applyQuality(next: PhoneQualityProfile) {
        handler.post {
            if (closed) return@post
            quality = next
            applySenderBitrate()
            onStatus("Качество: ${next.title}.")
        }
    }

    /** Re-targets capture dimensions after a display rotation; capture survives. */
    fun rotateCapture() {
        handler.post {
            if (closed || mode != PhoneStreamMode.SCREEN) return@post
            val activeCapturer = capturer ?: return@post
            val (nextWidth, nextHeight) = screenCaptureDimensions(quality)
            if (nextWidth == captureWidth && nextHeight == captureHeight) return@post
            captureWidth = nextWidth
            captureHeight = nextHeight
            runCatching {
                activeCapturer.changeCaptureFormat(nextWidth, nextHeight, captureFps)
            }.onFailure {
                fail("Не удалось применить поворот экрана к трансляции.")
            }
        }
    }

    override fun close() {
        if (!markClosed()) return
        if (Thread.currentThread() === thread) {
            closeResources()
            thread.quitSafely()
            return
        }
        val latch = CountDownLatch(1)
        val posted = handler.post {
            try {
                closeResources()
            } finally {
                latch.countDown()
                thread.quitSafely()
            }
        }
        if (!posted) {
            thread.quitSafely()
            return
        }
        try {
            if (!latch.await(5, TimeUnit.SECONDS)) thread.quitSafely()
        } catch (_: InterruptedException) {
            thread.quitSafely()
            Thread.currentThread().interrupt()
        }
    }

    private fun markClosed(): Boolean = synchronized(closeLock) {
        if (closed) false else {
            closed = true
            true
        }
    }

    // ------------------------------------------------------------------
    // Negotiation
    // ------------------------------------------------------------------

    private fun handleOffer(signal: JSONObject) {
        val sdp = signal.optString("sdp", "")
        if (sdp.isBlank() || sdp.toByteArray(Charsets.UTF_8).size > 160 * 1024) {
            reject("RTC_SDP_INVALID", "Получено некорректное SDP-предложение.")
            return
        }
        if (mode == PhoneStreamMode.CAMERA && ContextCompat.checkSelfPermission(
                context, Manifest.permission.CAMERA
            ) != PackageManager.PERMISSION_GRANTED
        ) {
            reject("CAMERA_NOT_ENABLED", "Разрешите доступ к камере телефона.")
            return
        }

        try {
            ensureVideoPipeline()
            // A reconnecting desktop sends a fresh offer; attach it to the
            // same video track instead of restarting capture.
            disposeTransport()
            val peer = createPeerConnection()
            peerConnection = peer
            peer.setRemoteDescription(object : SdpObserver {
                override fun onCreateSuccess(description: SessionDescription) = Unit
                override fun onSetSuccess() {
                    handler.post {
                        if (closed || peerConnection !== peer) return@post
                        try {
                            remoteDescriptionSet = true
                            val track = videoTrack
                            if (track == null) {
                                fail("Видеотрек сеанса не готов.")
                                return@post
                            }
                            if (peer.addTrack(track, listOf("remote-phone-${mode.wireName}")) == null) {
                                reject("VIDEO_TRACK_REJECTED", "WebRTC не принял видеотрек.")
                                return@post
                            }
                            for (candidate in pendingRemoteCandidates.toList()) {
                                if (!peer.addIceCandidate(candidate)) {
                                    fail("Не удалось применить ICE-кандидат компьютера.")
                                    return@post
                                }
                            }
                            pendingRemoteCandidates.clear()
                            createAnswer(peer)
                        } catch (_: Exception) {
                            fail("Не удалось подготовить видеосеанс.")
                        }
                    }
                }
                override fun onCreateFailure(error: String) = fail("Не удалось принять SDP-предложение.")
                override fun onSetFailure(error: String) = fail("Не удалось применить SDP-предложение.")
            }, SessionDescription(SessionDescription.Type.OFFER, sdp))
        } catch (_: Exception) {
            fail("Не удалось запустить источник видео (${mode.title}).")
        }
    }

    /** Creates factory, EGL context, video source, track and starts capture once. */
    private fun ensureVideoPipeline() {
        if (videoTrack != null) return
        val factoryLocal = factory ?: synchronized(FACTORY_LOCK) {
            if (!factoryInitialized) {
                PeerConnectionFactory.initialize(
                    PeerConnectionFactory.InitializationOptions.builder(context.applicationContext)
                        .createInitializationOptions()
                )
                factoryInitialized = true
            }
            PeerConnectionFactory.builder()
                .setVideoEncoderFactory(DefaultVideoEncoderFactory(eglContext().eglBaseContext, true, true))
                .setVideoDecoderFactory(DefaultVideoDecoderFactory(eglContext().eglBaseContext))
                .createPeerConnectionFactory()
                .also { factory = it }
        }

        val helper = SurfaceTextureHelper.create("remote-phone-capture", eglContext())
            ?: throw IllegalStateException("Не создан SurfaceTextureHelper.")
        textureHelper = helper
        val source = factoryLocal.createVideoSource(mode == PhoneStreamMode.SCREEN)
            ?: throw IllegalStateException("Не создан WebRTC video source.")
        videoSource = source

        when (mode) {
            PhoneStreamMode.CAMERA -> startCameraCapture(factoryLocal, helper, source)
            PhoneStreamMode.SCREEN -> startScreenCapture(helper, source)
        }

        val track = factoryLocal.createVideoTrack("remote-phone-${mode.wireName}-track", source)
            ?: throw IllegalStateException("Не создан WebRTC video track.")
        videoTrack = track
    }

    private fun eglContext(): EglBase {
        eglBase?.let { return it }
        val created = EglBase.create()
        eglBase = created
        return created
    }

    private fun startCameraCapture(
        factoryLocal: PeerConnectionFactory,
        helper: SurfaceTextureHelper,
        source: VideoSource
    ) {
        val enumerator = Camera2Enumerator(context.applicationContext)
        val cameraName = enumerator.deviceNames.firstOrNull { enumerator.isBackFacing(it) }
            ?: enumerator.deviceNames.firstOrNull()
        if (cameraName == null) {
            reject("CAMERA_UNAVAILABLE", "Не найдена доступная камера телефона.")
            return
        }
        val maxFpsMilli = quality.maxFpsMilli
        val captureFormat = enumerator.getSupportedFormats(cameraName)
            .orEmpty()
            .asSequence()
            .filter { format ->
                format.width.toLong() * format.height <= quality.maxPixelCount
                    && format.width in 2..3840 && format.height in 2..2160
                    && format.width % 2 == 0 && format.height % 2 == 0
                    && format.framerate.max >= 10_000
            }
            .maxWithOrNull(
                compareBy<CameraEnumerationAndroid.CaptureFormat> {
                    if (it.framerate.max >= maxFpsMilli) 1 else 0
                }
                    .thenBy { it.width.toLong() * it.height }
                    .thenBy { minOf(maxFpsMilli, it.framerate.max) }
            )
        if (captureFormat == null) {
            reject("CAMERA_FORMAT_UNAVAILABLE", "Камера не сообщила подходящий режим для качества «${quality.title}».")
            return
        }
        val targetFps = (minOf(maxFpsMilli, captureFormat.framerate.max) / 1000).coerceAtLeast(10)
        captureWidth = captureFormat.width
        captureHeight = captureFormat.height
        captureFps = targetFps

        val capturerLocal = enumerator.createCapturer(cameraName, cameraEventsHandler())
            ?: throw IllegalStateException("Не создан Camera2 capturer.")
        capturer = capturerLocal
        capturerLocal.initialize(helper, context.applicationContext, source.capturerObserver)
        capturerLocal.startCapture(captureFormat.width, captureFormat.height, targetFps)
        onStatus(
            "Camera2 запущена: ${captureFormat.width}×${captureFormat.height}, до $targetFps fps, качество «${quality.title}»."
        )
    }

    private fun startScreenCapture(helper: SurfaceTextureHelper, source: VideoSource) {
        val projectionIntent = projectionData
            ?: throw IllegalStateException("Нет данных разрешения MediaProjection.")
        val dimensions = screenCaptureDimensions(quality)
        captureWidth = dimensions.first
        captureHeight = dimensions.second
        captureFps = quality.maxFpsMilli / 1000

        val projectionCallback = object : MediaProjection.Callback() {
            override fun onProjectionStopped() {
                handler.post {
                    if (!closed && !ended) {
                        endSession("Трансляция экрана остановлена на телефоне.")
                    }
                }
            }
        }
        val screenCapturer = ScreenCapturerAndroid(projectionIntent, projectionCallback)
        capturer = object : CameraVideoCapturer by screenCapturer {}
        screenCapturer.initialize(helper, context.applicationContext, source.capturerObserver)
        screenCapturer.startCapture(captureWidth, captureHeight, captureFps)
        onStatus(
            "Экран транслируется: ${captureWidth}×${captureHeight}, качество «${quality.title}»."
        )
    }

    /** Preserves the display aspect ratio while capping resolution per quality. */
    private fun screenCaptureDimensions(profile: PhoneQualityProfile): Pair<Int, Int> {
        val display: android.view.Display? = context.getSystemService(DisplayManager::class.java)
            ?.getDisplay(android.view.Display.DEFAULT_DISPLAY)
        val metrics = if (android.os.Build.VERSION.SDK_INT >= 30) {
            display?.let { context.display?.realSizeCompat() }
        } else null
        val real = metrics ?: android.util.DisplayMetrics().also { m ->
            @Suppress("DEPRECATION")
            (context.getSystemService(android.view.WindowManager::class.java)?.defaultDisplay
                ?: display)?.getRealMetrics(m)
        }
        var width = maxOf(real.widthPixels, real.heightPixels)
        var height = minOf(real.widthPixels, real.heightPixels)
        val maxPixels = profile.maxPixelCount
        if (width.toLong() * height > maxPixels) {
            val scale = kotlin.math.sqrt(maxPixels.toDouble() / (width.toLong() * height))
            width = (width * scale).toInt().coerceAtLeast(2)
            height = (height * scale).toInt().coerceAtLeast(2)
        }
        width -= width % 2
        height -= height % 2
        return width to height
    }

    private fun android.view.Display.realSizeCompat(): android.util.DisplayMetrics {
        val metrics = android.util.DisplayMetrics()
        @Suppress("DEPRECATION")
        getRealMetrics(metrics)
        return metrics
    }

    private fun createPeerConnection(): PeerConnection {
        val configuration = PeerConnection.RTCConfiguration(emptyList()).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
        }
        return factory?.createPeerConnection(configuration, observer())
            ?: throw IllegalStateException("PeerConnection creation returned null")
    }

    private fun createAnswer(peer: PeerConnection) {
        peer.createAnswer(object : SdpObserver {
            override fun onCreateSuccess(description: SessionDescription) {
                peer.setLocalDescription(object : SdpObserver {
                    override fun onCreateSuccess(localDescription: SessionDescription) = Unit
                    override fun onSetSuccess() {
                        handler.post {
                            if (closed || peerConnection !== peer) return@post
                            try {
                                val answerSdp = description.description
                                if (answerSdp.toByteArray(Charsets.UTF_8).size > 160 * 1024) {
                                    fail("Сформированный WebRTC-ответ превышает допустимый размер.")
                                    return@post
                                }
                                sendSignal(
                                    JSONObject()
                                        .put("v", 1)
                                        .put("type", "answer")
                                        .put("sdp", answerSdp)
                                )
                                answerSent = true
                                streamingStarted = true
                                for (candidate in pendingLocalCandidates.toList()) sendIceCandidate(candidate)
                                pendingLocalCandidates.clear()
                                applySenderBitrate()
                                scheduleStatsCollection()
                                onStatus("SDP-ответ отправлен; медиапоток защищён DTLS-SRTP.")
                            } catch (_: Exception) {
                                fail("Не удалось отправить защищённый SDP-ответ.")
                            }
                        }
                    }
                    override fun onCreateFailure(error: String) = fail("Не удалось создать локальное SDP-описание.")
                    override fun onSetFailure(error: String) = fail("Не удалось применить локальное SDP-описание.")
                }, description)
            }
            override fun onSetSuccess() = Unit
            override fun onCreateFailure(error: String) = fail("Не удалось создать WebRTC-ответ.")
        }, MediaConstraints())
    }

    private fun handleIce(signal: JSONObject) {
        val candidateText = signal.optString("candidate", "")
        if (candidateText.isBlank() || candidateText.toByteArray(Charsets.UTF_8).size > MAX_RTC_CANDIDATE_BYTES) {
            fail("Получен некорректный ICE-кандидат.")
            return
        }
        remoteCandidateCount += 1
        if (remoteCandidateCount > MAX_RTC_CANDIDATES) {
            fail("Компьютер прислал слишком много ICE-кандидатов.")
            return
        }
        val midValue = signal.opt("sdpMid")
        val mid = if (midValue == null || midValue === JSONObject.NULL) null else midValue as? String
        if (midValue != null && midValue !== JSONObject.NULL
            && (mid == null || mid.toByteArray(Charsets.UTF_8).size > MAX_RTC_MID_BYTES)) {
            fail("Получен некорректный идентификатор SDP media.")
            return
        }
        val lineValue = signal.opt("sdpMLineIndex")
        val lineIndex = if (lineValue == null || lineValue === JSONObject.NULL) null else (lineValue as? Number)?.toInt()
        if (lineValue != null && lineValue !== JSONObject.NULL && (lineIndex == null || lineIndex !in 0..255)) {
            fail("Получен некорректный индекс SDP media.")
            return
        }
        val candidate = IceCandidate(mid, lineIndex ?: 0, candidateText)
        val peer = peerConnection
        if (!remoteDescriptionSet || peer == null) {
            pendingRemoteCandidates += candidate
        } else if (!peer.addIceCandidate(candidate)) {
            fail("Не удалось применить ICE-кандидат компьютера.")
        }
    }

    // ------------------------------------------------------------------
    // Bitrate / stats / data channel
    // ------------------------------------------------------------------

    private fun applySenderBitrate() {
        val peer = peerConnection ?: return
        val sender = peer.senders.firstOrNull { it.track() is VideoTrack } ?: return
        runCatching {
            val parameters: RtpParameters = sender.parameters
            val encodings = parameters.encodings
            if (encodings.isEmpty()) return
            encodings.forEach { encoding -> encoding.maxBitrateBps = quality.maxBitrateBps }
            sender.setParameters(parameters)
            senderMaxBitrateApplied = true
        }
    }

    private fun scheduleStatsCollection() {
        if (statsScheduled) return
        statsScheduled = true
        val runnable = object : Runnable {
            override fun run() {
                if (closed) return
                val peer = peerConnection
                if (peer == null) {
                    handler.postDelayed(this, TELEMETRY_PERIOD_MS)
                    return
                }
                peer.getStats { report ->
                    if (closed) return@getStats
                    var fps = 0.0
                    var bitrateKbps = 0.0
                    var width = captureWidth
                    var height = captureHeight
                    var framesTotal = 0L
                    var framesDropped = 0L
                    for ((_, stats) in report.statsMap) {
                        if (stats.type != "outbound-rtp") continue
                        val kind = stats.members["kind"] as? String ?: continue
                        if (kind != "video") continue
                        (stats.members["framesPerSecond"] as? Number)?.let { fps = it.toDouble() }
                        (stats.members["frameWidth"] as? Number)?.let { width = it.toInt() }
                        (stats.members["frameHeight"] as? Number)?.let { height = it.toInt() }
                        (stats.members["framesEncoded"] as? Number)?.let { framesTotal = it.toLong() }
                        (stats.members["bytesSent"] as? Number)?.let { sent ->
                            val now = System.currentTimeMillis()
                            val previousBytes = prevStatsBytes
                            val previousTime = prevStatsTimeMs
                            if (previousBytes != null && previousTime != null && now > previousTime) {
                                bitrateKbps = (sent.toLong() - previousBytes) * 8.0 / (now - previousTime) / 1000.0
                            }
                            prevStatsBytes = sent.toLong()
                            prevStatsTimeMs = now
                        }
                    }
                    onTelemetry(
                        PhoneTelemetry(
                            fps = fps,
                            bitrateKbps = bitrateKbps,
                            width = if (width > 0) width else captureWidth,
                            height = if (height > 0) height else captureHeight,
                            framesTotal = framesTotal,
                            framesDropped = framesDropped,
                            batteryPercent = readBatteryPercent(),
                            charging = readCharging(),
                            thermalLabel = thermalStatusLabel(currentThermalStatus()),
                            thermalLevel = currentThermalStatus()
                        )
                    )
                    publishTelemetryOverDataChannel(fps, bitrateKbps, width, height)
                }
                handler.postDelayed(this, TELEMETRY_PERIOD_MS)
            }
        }
        handler.post(runnable)
    }

    private fun publishTelemetryOverDataChannel(fps: Double, bitrateKbps: Double, width: Int, height: Int) {
        val channel = controlChannel ?: return
        if (channel.state() != DataChannel.State.OPEN) return
        val payload = JSONObject()
            .put("v", 1)
            .put("type", "telemetry")
            .put("mode", mode.wireName)
            .put("quality", quality.wireName)
            .put("fps", Math.round(fps * 10) / 10.0)
            .put("bitrateKbps", Math.round(bitrateKbps))
            .put("width", width)
            .put("height", height)
            .put("batteryPercent", readBatteryPercent())
            .put("charging", readCharging())
            .put("thermal", thermalStatusLabel(currentThermalStatus()))
            .put("thermalLevel", currentThermalStatus())
        val bytes = payload.toString().toByteArray(Charsets.UTF_8)
        if (bytes.size > MAX_DATA_MESSAGE_BYTES) return
        runCatching { channel.send(DataChannel.Buffer(java.nio.ByteBuffer.wrap(bytes), true)) }
    }

    private fun readBatteryPercent(): Int = runCatching {
        val manager = context.getSystemService(BatteryManager::class.java) ?: return -1
        manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
    }.getOrDefault(-1)

    private fun readCharging(): Boolean = runCatching {
        val batteryStatus = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            ?: return false
        val status = batteryStatus.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL
    }.getOrDefault(false)

    private fun currentThermalStatus(): Int = runCatching {
        (context.getSystemService(PowerManager::class.java))?.currentThermalStatus
            ?: PowerManager.THERMAL_STATUS_NONE
    }.getOrDefault(PowerManager.THERMAL_STATUS_NONE)

    // ------------------------------------------------------------------
    // PeerConnection observer and data channel
    // ------------------------------------------------------------------

    private fun observer() = object : PeerConnection.Observer {
        override fun onSignalingChange(state: PeerConnection.SignalingState) = Unit
        override fun onIceConnectionChange(state: PeerConnection.IceConnectionState) {
            when (state) {
                PeerConnection.IceConnectionState.CONNECTED,
                PeerConnection.IceConnectionState.COMPLETED ->
                    onStatus("WebRTC подключён; медиаданные защищены DTLS-SRTP.")
                PeerConnection.IceConnectionState.FAILED ->
                    onStatus("Локальный ICE-маршрут не установился; ожидаем повторную попытку компьютера.")
                PeerConnection.IceConnectionState.DISCONNECTED ->
                    onStatus("Локальный WebRTC-маршрут восстанавливается…")
                else -> Unit
            }
        }
        override fun onIceConnectionReceivingChange(receiving: Boolean) = Unit
        override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) = Unit
        override fun onIceCandidate(candidate: IceCandidate) {
            handler.post {
                if (closed || peerConnection == null) return@post
                localCandidateCount += 1
                if (localCandidateCount > MAX_RTC_CANDIDATES
                    || candidate.sdp.toByteArray(Charsets.UTF_8).size > MAX_RTC_CANDIDATE_BYTES) {
                    fail("Слишком много или слишком большой локальный ICE-кандидат.")
                    return@post
                }
                if (answerSent) sendIceCandidate(candidate) else pendingLocalCandidates += candidate
            }
        }
        override fun onIceCandidatesRemoved(candidates: Array<IceCandidate>) = Unit
        override fun onAddStream(stream: org.webrtc.MediaStream) = Unit
        override fun onRemoveStream(stream: org.webrtc.MediaStream) = Unit
        override fun onDataChannel(channel: DataChannel) {
            handler.post {
                if (closed || channel.label() != CONTROL_CHANNEL_LABEL) {
                    channel.close()
                    channel.dispose()
                    return@post
                }
                controlChannel?.close()
                controlChannel?.dispose()
                controlChannel = channel
                controlMessageCount = 0
                channel.registerObserver(object : DataChannel.Observer {
                    override fun onBufferedAmountChange(previousAmount: Long) = Unit
                    override fun onStateChange() {
                        handler.post {
                            if (closed || channel !== controlChannel) return@post
                            if (channel.state() == DataChannel.State.OPEN) {
                                onStatus("Защищённый канал управления открыт.")
                                publishTelemetryOverDataChannel(0.0, 0.0, captureWidth, captureHeight)
                            }
                        }
                    }
                    override fun onMessage(buffer: DataChannel.Buffer) {
                        handler.post { handleControlMessage(buffer) }
                    }
                })
            }
        }
        override fun onRenegotiationNeeded() = Unit
        override fun onAddTrack(receiver: RtpReceiver, mediaStreams: Array<org.webrtc.MediaStream>) = Unit
        override fun onTrack(transceiver: RtpTransceiver) = Unit
    }

    /** Strict, small session-control protocol. No text input is ever accepted. */
    private fun handleControlMessage(buffer: DataChannel.Buffer) {
        if (closed) return
        if (controlMessageCount >= MAX_CONTROL_MESSAGES) return
        if (!buffer.data.hasRemaining() || buffer.data.remaining() > MAX_DATA_MESSAGE_BYTES) return
        val bytes = ByteArray(buffer.data.remaining())
        buffer.data.get(bytes)
        controlMessageCount += 1
        val signal = runCatching {
            JSONObject(String(bytes, Charsets.UTF_8))
        }.getOrNull() ?: return
        when (signal.optString("type")) {
            "stop-stream" -> endSession("Трансляция остановлена с компьютера.")
            "quality" -> PhoneQualityProfile.fromWire(signal.optString("value"))?.let { applyQuality(it) }
            else -> Unit // Reserved for the later control milestone; unknown is ignored.
        }
    }

    private fun sendIceCandidate(candidate: IceCandidate) {
        if (candidate.sdp.isBlank() || candidate.sdp.toByteArray(Charsets.UTF_8).size > MAX_RTC_CANDIDATE_BYTES
            || candidate.sdpMid?.toByteArray(Charsets.UTF_8)?.size?.let { it > MAX_RTC_MID_BYTES } == true) {
            fail("Локальный ICE-кандидат не прошёл проверку размера.")
            return
        }
        try {
            sendSignal(
                JSONObject()
                    .put("v", 1)
                    .put("type", "ice")
                    .put("candidate", candidate.sdp)
                    .put("sdpMid", candidate.sdpMid ?: JSONObject.NULL)
                    .put("sdpMLineIndex", candidate.sdpMLineIndex)
            )
        } catch (_: Exception) {
            fail("Не удалось отправить ICE-кандидат.")
        }
    }

    private fun cameraEventsHandler() = object : CameraVideoCapturer.CameraEventsHandler {
        override fun onCameraError(errorDescription: String) {
            handler.post { fail("Ошибка Camera2. Проверьте, не занята ли камера другим приложением.") }
        }
        override fun onCameraDisconnected() {
            handler.post { fail("Камера телефона отключилась.") }
        }
        override fun onCameraFreezed(errorDescription: String) {
            handler.post { onStatus("Камера не присылает новые кадры.") }
        }
        override fun onCameraOpening(cameraName: String) = Unit
        override fun onFirstFrameAvailable() {
            handler.post { onStatus("Первый кадр камеры получен.") }
        }
        override fun onCameraClosed() = Unit
    }

    private fun sendProtocolError(code: String) {
        runCatching {
            sendSignal(JSONObject().put("v", 1).put("type", "error").put("code", code))
        }
    }

    private fun reject(code: String, message: String) {
        if (closed || ended) return
        ended = true
        sendProtocolError(code)
        onEnded(message)
    }

    private fun fail(message: String) {
        if (closed || ended) return
        ended = true
        onStatus(message)
        sendProtocolError("WEBRTC_FAILED")
        handler.post { onEnded(message) }
    }

    private fun endSession(message: String) {
        if (closed || ended) return
        ended = true
        onStatus(message)
        handler.post { onEnded(message) }
    }

    /** Closes only the WebRTC transport; the video source and track survive. */
    private fun disposeTransport() {
        runCatching { controlChannel?.unregisterObserver() }
        runCatching { controlChannel?.close() }
        runCatching { controlChannel?.dispose() }
        controlChannel = null
        runCatching { peerConnection?.close() }
        runCatching { peerConnection?.dispose() }
        peerConnection = null
        remoteDescriptionSet = false
        answerSent = false
        remoteCandidateCount = 0
        localCandidateCount = 0
        pendingRemoteCandidates.clear()
        pendingLocalCandidates.clear()
        prevStatsBytes = null
        prevStatsTimeMs = null
    }

    private fun closeResources() {
        disposeTransport()
        runCatching { capturer?.stopCapture() }
        runCatching { capturer?.dispose() }
        capturer = null
        runCatching { videoTrack?.setEnabled(false) }
        runCatching { videoTrack?.dispose() }
        videoTrack = null
        runCatching { videoSource?.dispose() }
        videoSource = null
        runCatching { textureHelper?.dispose() }
        textureHelper = null
        runCatching { factory?.dispose() }
        factory = null
        runCatching { eglBase?.release() }
        eglBase = null
    }

    companion object {
        private val FACTORY_LOCK = Any()
        @Volatile private var factoryInitialized = false
    }
}
