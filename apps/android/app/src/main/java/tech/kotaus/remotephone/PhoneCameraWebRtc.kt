package tech.kotaus.remotephone

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Handler
import android.os.HandlerThread
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
import org.webrtc.RtpReceiver
import org.webrtc.RtpTransceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

private const val MAX_RTC_CANDIDATES = 128
private const val MAX_RTC_CANDIDATE_BYTES = 8192
private const val MAX_RTC_MID_BYTES = 256
private const val MAX_CAPTURE_FPS_MILLI = 30_000

/** Camera2 → WebRTC sender. It never creates a screen capturer, microphone, or audio track. */
internal class PhoneCameraWebRtc(
    context: Context,
    private val sendSignal: (JSONObject) -> Unit,
    private val onStatus: (String) -> Unit
) : AutoCloseable {
    private val applicationContext = context.applicationContext
    private val thread = HandlerThread("remote-phone-webrtc-camera").apply { start() }
    private val handler = Handler(thread.looper)
    private val closeLock = Any()

    @Volatile private var closed = false
    @Volatile private var failureStarted = false
    private var peerConnection: PeerConnection? = null
    private var factory: PeerConnectionFactory? = null
    private var eglBase: EglBase? = null
    private var textureHelper: SurfaceTextureHelper? = null
    private var cameraCapturer: CameraVideoCapturer? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var controlChannel: DataChannel? = null
    private var remoteDescriptionSet = false
    private var answerSent = false
    private var remoteCandidateCount = 0
    private var localCandidateCount = 0
    private val pendingRemoteCandidates = mutableListOf<IceCandidate>()
    private val pendingLocalCandidates = mutableListOf<IceCandidate>()

    fun handleSignal(signal: JSONObject) {
        if (closed) return
        handler.post {
            if (closed) return@post
            when (signal.optString("type")) {
                "offer" -> handleOffer(signal)
                "ice" -> handleIce(signal)
                "bye", "error" -> closeInternal()
                else -> fail("Телефон получил неизвестный тип WebRTC-сигнализации.")
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

    private fun handleOffer(signal: JSONObject) {
        if (peerConnection != null) return
        val sdp = signal.optString("sdp", "")
        if (sdp.isBlank() || sdp.toByteArray(Charsets.UTF_8).size > 160 * 1024) {
            reject("RTC_SDP_INVALID", "Получено некорректное SDP-предложение.")
            return
        }
        if (ContextCompat.checkSelfPermission(applicationContext, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED) {
            reject("CAMERA_NOT_ENABLED", "Разрешите доступ к камере и выберите режим «Веб-камера».")
            return
        }
        if (!Camera2Enumerator.isSupported(applicationContext)) {
            reject("CAMERA2_UNAVAILABLE", "На этом устройстве недоступен захват камеры Camera2.")
            return
        }

        try {
            createPeerConnection()
            val peer = peerConnection ?: throw IllegalStateException("Не удалось создать WebRTC peer.")
            peer.setRemoteDescription(object : SdpObserver {
                override fun onCreateSuccess(description: SessionDescription) = Unit
                override fun onSetSuccess() {
                    handler.post {
                        if (closed || peerConnection !== peer) return@post
                        try {
                            remoteDescriptionSet = true
                            if (!startCameraTrack(peer)) return@post
                            for (candidate in pendingRemoteCandidates.toList()) {
                                if (!peer.addIceCandidate(candidate)) {
                                    fail("Не удалось применить ICE-кандидат компьютера.")
                                    return@post
                                }
                            }
                            pendingRemoteCandidates.clear()
                            createAnswer(peer)
                        } catch (_: Exception) {
                            fail("Не удалось запустить камеру телефона для WebRTC.")
                        }
                    }
                }
                override fun onCreateFailure(error: String) = fail("Не удалось принять SDP-предложение.")
                override fun onSetFailure(error: String) = fail("Не удалось применить SDP-предложение.")
            }, SessionDescription(SessionDescription.Type.OFFER, sdp))
        } catch (_: Exception) {
            fail("Не удалось подготовить защищённый видеосеанс.")
        }
    }

    private fun createPeerConnection() {
        if (factory == null) {
            synchronized(FACTORY_LOCK) {
                if (!factoryInitialized) {
                    PeerConnectionFactory.initialize(
                        PeerConnectionFactory.InitializationOptions.builder(applicationContext)
                            .createInitializationOptions()
                    )
                    factoryInitialized = true
                }
            }
            val egl = EglBase.create()
            eglBase = egl
            factory = PeerConnectionFactory.builder()
                .setVideoEncoderFactory(DefaultVideoEncoderFactory(egl.eglBaseContext, true, true))
                .setVideoDecoderFactory(DefaultVideoDecoderFactory(egl.eglBaseContext))
                .createPeerConnectionFactory()
        }

        val configuration = PeerConnection.RTCConfiguration(emptyList()).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
        }
        peerConnection = factory?.createPeerConnection(configuration, observer())
            ?: throw IllegalStateException("PeerConnection creation returned null")
    }

    private fun startCameraTrack(peer: PeerConnection): Boolean {
        if (videoTrack != null) return true
        val enumerator = Camera2Enumerator(applicationContext)
        val cameraName = enumerator.deviceNames.firstOrNull { enumerator.isBackFacing(it) }
            ?: enumerator.deviceNames.firstOrNull()
        if (cameraName == null) {
            reject("CAMERA_UNAVAILABLE", "Не найдена доступная камера телефона.")
            return false
        }

        val captureFormat = enumerator.getSupportedFormats(cameraName)
            .orEmpty()
            .asSequence()
            .filter { format ->
                format.width in 2..3840 && format.height in 2..2160
                    && format.width % 2 == 0 && format.height % 2 == 0
                    && format.framerate.max >= 15_000
            }
            .maxWithOrNull(
                compareBy<CameraEnumerationAndroid.CaptureFormat> {
                    if (it.framerate.max >= MAX_CAPTURE_FPS_MILLI) 1 else 0
                }
                    .thenBy { it.width.toLong() * it.height }
                    .thenBy { minOf(MAX_CAPTURE_FPS_MILLI, it.framerate.max) }
            )
        if (captureFormat == null) {
            reject("CAMERA_FORMAT_UNAVAILABLE", "Камера не сообщила режим не ниже 15 fps в пределах 3840×2160.")
            return false
        }

        // WebRTC CaptureFormat framerates are milli-fps; VideoCapturer.startCapture expects fps.
        val targetFps = (minOf(MAX_CAPTURE_FPS_MILLI, captureFormat.framerate.max) / 1000)
            .coerceAtLeast(15)
        val eglContext = eglBase?.eglBaseContext
            ?: throw IllegalStateException("Не создан EGL context для Camera2.")
        val helper = SurfaceTextureHelper.create("remote-phone-camera2", eglContext)
            ?: throw IllegalStateException("Не создан SurfaceTextureHelper.")
        textureHelper = helper
        val source = factory?.createVideoSource(false)
            ?: throw IllegalStateException("Не создан WebRTC video source.")
        videoSource = source
        val capturer = enumerator.createCapturer(cameraName, cameraEventsHandler())
            ?: throw IllegalStateException("Не создан Camera2 capturer.")
        cameraCapturer = capturer
        capturer.initialize(helper, applicationContext, source.capturerObserver)
        capturer.startCapture(captureFormat.width, captureFormat.height, targetFps)

        val track = factory?.createVideoTrack("remote-phone-camera-track", source)
            ?: throw IllegalStateException("Не создан WebRTC video track.")
        videoTrack = track
        if (peer.addTrack(track, listOf("remote-phone-camera")) == null) {
            reject("VIDEO_TRACK_REJECTED", "WebRTC не принял видеотрек камеры.")
            return false
        }
        onStatus("Camera2 запущена: ${captureFormat.width}×${captureFormat.height}, до $targetFps fps. Ожидаем защищённый канал.")
        return true
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
                                for (candidate in pendingLocalCandidates.toList()) sendIceCandidate(candidate)
                                pendingLocalCandidates.clear()
                                onStatus("SDP-ответ отправлен. Медиапоток WebRTC использует DTLS-SRTP.")
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
            override fun onSetFailure(error: String) = Unit
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

    private fun observer() = object : PeerConnection.Observer {
        override fun onSignalingChange(state: PeerConnection.SignalingState) = Unit
        override fun onIceConnectionChange(state: PeerConnection.IceConnectionState) {
            when (state) {
                PeerConnection.IceConnectionState.CONNECTED,
                PeerConnection.IceConnectionState.COMPLETED -> onStatus("WebRTC подключён; медиаданные защищены DTLS-SRTP.")
                PeerConnection.IceConnectionState.FAILED -> fail("Локальный ICE-маршрут не установился.")
                PeerConnection.IceConnectionState.DISCONNECTED -> onStatus("Локальный WebRTC-маршрут восстанавливается…")
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
                if (closed || channel.label() != "remote-phone-control") {
                    channel.close()
                    channel.dispose()
                    return@post
                }
                controlChannel?.close()
                controlChannel?.dispose()
                controlChannel = channel
                channel.registerObserver(object : DataChannel.Observer {
                    override fun onBufferedAmountChange(previousAmount: Long) = Unit
                    override fun onStateChange() = Unit
                    override fun onMessage(buffer: DataChannel.Buffer) {
                        // The separate channel intentionally accepts no text, input, or device commands.
                        if (buffer.data.hasRemaining()) onStatus("Сообщения управления пока не принимаются.")
                    }
                })
            }
        }
        override fun onRenegotiationNeeded() = Unit
        override fun onAddTrack(receiver: RtpReceiver, mediaStreams: Array<org.webrtc.MediaStream>) = Unit
        override fun onTrack(transceiver: RtpTransceiver) = Unit
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
            handler.post { onStatus("Первый кадр Camera2 получен; проверяем его передачу через WebRTC.") }
        }
        override fun onCameraClosed() = Unit
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

    private fun sendProtocolError(code: String) {
        runCatching {
            sendSignal(JSONObject().put("v", 1).put("type", "error").put("code", code))
        }
    }

    private fun reject(code: String, message: String) {
        if (closed) return
        failureStarted = true
        sendProtocolError(code)
        onStatus(message)
        closeInternal()
    }

    private fun fail(message: String) {
        if (closed || failureStarted) return
        failureStarted = true
        onStatus(message)
        sendProtocolError("WEBRTC_FAILED")
        handler.post { closeInternal() }
    }

    private fun closeInternal() {
        if (!markClosed()) return
        closeResources()
        thread.quitSafely()
    }

    private fun closeResources() {
        runCatching { cameraCapturer?.stopCapture() }
        runCatching { cameraCapturer?.dispose() }
        cameraCapturer = null

        runCatching { controlChannel?.unregisterObserver() }
        runCatching { controlChannel?.close() }
        runCatching { controlChannel?.dispose() }
        controlChannel = null

        runCatching { peerConnection?.close() }
        runCatching { peerConnection?.dispose() }
        peerConnection = null
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
        pendingRemoteCandidates.clear()
        pendingLocalCandidates.clear()
        remoteDescriptionSet = false
        answerSent = false
    }

    companion object {
        private val FACTORY_LOCK = Any()
        @Volatile private var factoryInitialized = false
    }
}
