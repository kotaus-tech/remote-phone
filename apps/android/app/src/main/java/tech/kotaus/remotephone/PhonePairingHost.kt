package tech.kotaus.remotephone

import android.content.Context
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
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.util.Collections
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

private const val MAX_FRAME_BYTES = 256 * 1024
private const val SESSION_LIFETIME_SECONDS = 5 * 60
private const val SERVICE_TYPE = "_remotephone._tcp."
private const val LOG_TAG = "RemotePhonePairing"

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
    val pin: String? = null,
    val message: String = "Сеанс сопряжения не запущен.",
    val addresses: List<String> = emptyList(),
    val port: Int? = null,
    val attemptsUsed: Int = 0,
    val secondsRemaining: Int = 0,
    val discoveryAvailable: Boolean = false
)

/** Owns one temporary phone-side PIN session and its local WebSocket endpoint. */
internal class PhonePairingHost(
    context: Context,
    private val onStateChanged: (PhonePairingState) -> Unit
) : AutoCloseable {
    private val applicationContext = context.applicationContext
    private val nsdManager = applicationContext.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val mainHandler = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "remote-phone-pairing").apply { isDaemon = true }
    }
    private val lock = Any()

    @Volatile private var state = PhonePairingState()
    @Volatile private var handle = 0L
    @Volatile private var pinForUi: String? = null
    @Volatile private var server: PairingWebSocketServer? = null
    @Volatile private var activeConnection: WebSocket? = null
    @Volatile private var registrationListener: NsdManager.RegistrationListener? = null
    @Volatile private var serviceRegistered = false
    @Volatile private var expiryAtElapsedRealtime = 0L
    @Volatile private var lastFailureMessage: String? = null
    @Volatile private var closed = false

    private val countdownTask = object : Runnable {
        override fun run() {
            if (closed || handle == 0L || expiryAtElapsedRealtime == 0L) return
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
        publish(PhonePairingState(phase = PhonePairingPhase.STARTING, message = "Создаём PIN и защищённый сеанс…"))
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
                    "Не удалось загрузить модуль сопряжения. Перезапустите приложение; техническая причина записана в журнал Android."
                )
            } catch (error: Exception) {
                Log.e(LOG_TAG, "Не удалось запустить сеанс сопряжения", error)
                stopInternal(
                    PhonePairingPhase.FAILED,
                    "Не удалось запустить сопряжение. Техническая причина записана в журнал Android; повторите попытку."
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

    private fun onServerStarted(webSocketServer: PairingWebSocketServer, port: Int) {
        if (closed || server !== webSocketServer) return
        val addresses = localIpv4Addresses()
        val currentPin = pinForUi
        publish(PhonePairingState(
            phase = PhonePairingPhase.WAITING,
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

    private fun stopInternal(finalPhase: PhonePairingPhase, finalMessage: String) {
        mainHandler.removeCallbacks(countdownTask)
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
        if (oldHandle != 0L) runCatching { NativePairing.phoneDestroy(oldHandle) }
        publish(PhonePairingState(phase = finalPhase, message = finalMessage))
    }

    private fun safeAttemptsUsed(): Int {
        val currentHandle = handle
        if (currentHandle == 0L) return 0
        return runCatching { NativePairing.phoneAttemptsUsed(currentHandle).coerceAtLeast(0) }
            .getOrDefault(0)
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
            synchronized(lock) {
                if (closed || handle != pairingHandle || activeConnection != null || state.phase == PhonePairingPhase.AUTHENTICATED) {
                    connection.close(1008, "Сеанс занят")
                    return
                }
                activeConnection = connection
                lastFailureMessage = null
            }
            try {
                val hello = NativePairing.phoneStartConnection(pairingHandle)
                    ?: throw IllegalStateException("PAIRING_FAILED")
                connection.send(hello)
                publish(state.copy(phase = PhonePairingPhase.VERIFYING, message = "Компьютер подключён. Проверяем код…"))
            } catch (error: LinkageError) {
                Log.e(LOG_TAG, "Ошибка вызова нативного модуля при начале сопряжения", error)
                failConnection(connection, "Не удалось запустить нативное сопряжение. Проверьте журнал Android.")
            } catch (_: Exception) {
                failConnection(connection, "Не удалось начать защищённое сопряжение.")
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
                if (NativePairing.phoneIsAuthenticated(pairingHandle)) {
                    val plaintext = NativePairing.phoneDecryptSignal(pairingHandle, frame)
                        ?: throw IllegalStateException("PAIRING_FAILED")
                    plaintext.fill(0)
                    return
                }
                val reply = NativePairing.phoneHandleFrame(pairingHandle, frame)
                if (reply != null) connection.send(reply)
                val attempts = safeAttemptsUsed()
                if (NativePairing.phoneIsAuthenticated(pairingHandle)) {
                    publish(state.copy(
                        phase = PhonePairingPhase.AUTHENTICATED,
                        pin = null,
                        attemptsUsed = attempts,
                        message = "Защищённое сопряжение подтверждено. Не закрывайте экран до остановки сеанса."
                    ))
                } else {
                    publish(state.copy(attemptsUsed = attempts))
                }
            } catch (error: LinkageError) {
                Log.e(LOG_TAG, "Ошибка вызова нативного модуля при обработке кадра", error)
                connection.close(1008, "Нативный модуль недоступен")
                worker.execute {
                    stopInternal(PhonePairingPhase.FAILED, "Нативный модуль сопряжения недоступен. Остановите сеанс и попробуйте позже.")
                }
                return
            } catch (_: Exception) {
                if (state.phase == PhonePairingPhase.AUTHENTICATED) {
                    connection.close(1008, "Защищённый кадр отклонён")
                    worker.execute {
                        stopInternal(PhonePairingPhase.CLOSED, "Защищённый кадр не прошёл проверку. Создайте новый PIN.")
                    }
                    return
                }
                val attempts = safeAttemptsUsed()
                if (attempts >= 5) {
                    worker.execute {
                        stopInternal(PhonePairingPhase.LOCKED, "Достигнут предел попыток. Создайте новый PIN вручную.")
                    }
                    connection.close(1008, "Сеанс заблокирован")
                } else {
                    failConnection(connection, "Не удалось подтвердить код. Проверьте PIN на телефоне и попробуйте снова.")
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
            if (handle != pairingHandle) return
            if (runCatching { NativePairing.phoneIsAuthenticated(pairingHandle) }.getOrDefault(false)) {
                worker.execute {
                    stopInternal(PhonePairingPhase.CLOSED, "Защищённое соединение закрыто. Создайте новый PIN для следующего сеанса.")
                }
            } else {
                runCatching { NativePairing.phoneAbortConnection(pairingHandle) }
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
                failConnection(connection, "Ошибка локального соединения. Попробуйте подключиться ещё раз.")
            }
        }

        private fun failConnection(connection: WebSocket, message: String) {
            if (connection !== activeConnection) return
            runCatching { NativePairing.phoneAbortConnection(pairingHandle) }
            lastFailureMessage = message
            publish(state.copy(
                phase = PhonePairingPhase.WAITING,
                attemptsUsed = safeAttemptsUsed(),
                message = message
            ))
            connection.close(1008, "Сопряжение отклонено")
        }
    }
}
