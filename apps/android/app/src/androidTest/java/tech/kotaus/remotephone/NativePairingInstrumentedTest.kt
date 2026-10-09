package tech.kotaus.remotephone

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import androidx.test.ext.junit.runners.AndroidJUnit4

@RunWith(AndroidJUnit4::class)
class NativePairingInstrumentedTest {
    @Test
    fun nativeLibrariesLoadAndCompleteOpaqueHandshakeAndSignalExchange() {
        // This enters the production Kotlin -> JNI -> statically linked Rust path.
        val session = NativePairing.phoneCreate()
        assertNotNull("phoneCreate returned no native session", session)
        val sessionBytes = requireNotNull(session)
        assertEquals(8 + 8 + 16, sessionBytes.size)

        val decoded = ByteBuffer.wrap(sessionBytes).order(ByteOrder.LITTLE_ENDIAN)
        val phoneHandle = decoded.long
        val pin = ByteArray(8)
        decoded.get(pin)
        assertTrue("native phone handle is invalid", phoneHandle > 0L)
        assertTrue("native PIN is not eight ASCII digits", pin.all { it.toInt() in '0'.code..'9'.code })

        var pcHandle = 0L
        try {
            val hello = requireNotNull(NativePairing.phoneStartConnection(phoneHandle))
            pcHandle = NativePairingTestPeer.pcStart(pin, hello)
            assertTrue("native desktop peer did not start", pcHandle > 0L)

            val login1 = requireNotNull(NativePairingTestPeer.pcTakeInitialFrame(pcHandle))
            val login2 = requireNotNull(NativePairing.phoneHandleFrame(phoneHandle, login1))
            val login3 = requireNotNull(NativePairingTestPeer.pcHandleFrame(pcHandle, login2))
            val authOk = requireNotNull(NativePairing.phoneHandleFrame(phoneHandle, login3))
            val authAck = requireNotNull(NativePairingTestPeer.pcHandleFrame(pcHandle, authOk))

            assertTrue("desktop side did not authenticate", NativePairingTestPeer.pcIsAuthenticated(pcHandle))
            assertNull("phone should accept AUTH_ACK without sending a reply",
                NativePairing.phoneHandleFrame(phoneHandle, authAck))
            assertTrue("phone side did not authenticate", NativePairing.phoneIsAuthenticated(phoneHandle))

            val plaintext = "emulator-signal-smoke-test".toByteArray(Charsets.UTF_8)
            val encrypted = requireNotNull(NativePairingTestPeer.pcEncryptSignal(pcHandle, plaintext))
            assertArrayEquals(
                plaintext,
                requireNotNull(NativePairing.phoneDecryptSignal(phoneHandle, encrypted))
            )
        } finally {
            if (pcHandle > 0L) runCatching { NativePairingTestPeer.pcDestroy(pcHandle) }
            if (phoneHandle > 0L) runCatching { NativePairing.phoneDestroy(phoneHandle) }
            pin.fill(0)
            sessionBytes.fill(0)
        }
    }

    @Test
    fun phoneResumeConnectionReadvertisesSameSessionAndKeepsSignalCipher() {
        val session = NativePairing.phoneCreate()
        val sessionBytes = requireNotNull(session)
        val decoded = ByteBuffer.wrap(sessionBytes).order(ByteOrder.LITTLE_ENDIAN)
        val phoneHandle = decoded.long
        val pin = ByteArray(8)
        decoded.get(pin)
        val sessionId = ByteArray(16)
        decoded.get(sessionId)

        // Resume is impossible before authentication: the JNI bridge throws.
        var resumeRejected = false
        try {
            NativePairing.phoneResumeConnection(phoneHandle)
        } catch (expected: IllegalStateException) {
            resumeRejected = true
        }
        assertTrue("resume before authentication must be rejected", resumeRejected)

        var pcHandle = 0L
        try {
            val hello = requireNotNull(NativePairing.phoneStartConnection(phoneHandle))
            pcHandle = NativePairingTestPeer.pcStart(pin, hello)
            val login1 = requireNotNull(NativePairingTestPeer.pcTakeInitialFrame(pcHandle))
            val login2 = requireNotNull(NativePairing.phoneHandleFrame(phoneHandle, login1))
            val login3 = requireNotNull(NativePairingTestPeer.pcHandleFrame(pcHandle, login2))
            val authOk = requireNotNull(NativePairing.phoneHandleFrame(phoneHandle, login3))
            val authAck = requireNotNull(NativePairingTestPeer.pcHandleFrame(pcHandle, authOk))
            assertNull(NativePairing.phoneHandleFrame(phoneHandle, authAck))
            assertTrue(NativePairing.phoneIsAuthenticated(phoneHandle))

            // Advance the signal sequence before the simulated transport drop.
            val beforeDrop = "before-drop".toByteArray(Charsets.UTF_8)
            val encryptedBefore = requireNotNull(NativePairingTestPeer.pcEncryptSignal(pcHandle, beforeDrop))
            assertArrayEquals(beforeDrop, requireNotNull(NativePairing.phoneDecryptSignal(phoneHandle, encryptedBefore)))

            // The resumed HELLO carries the same session_id in the frame header (bytes 5..21).
            val resumedHello = requireNotNull(NativePairing.phoneResumeConnection(phoneHandle))
            assertTrue("resumed HELLO is too short", resumedHello.size >= 29)
            assertEquals('R'.code.toByte(), resumedHello[0])
            assertEquals('V'.code.toByte(), resumedHello[1])
            assertEquals('P'.code.toByte(), resumedHello[2])
            assertEquals('1'.code.toByte(), resumedHello[3])
            assertEquals(0x01.toByte(), resumedHello[4])
            val resumedSessionId = resumedHello.copyOfRange(5, 21)
            assertArrayEquals(sessionId, resumedSessionId)

            // The protected signal exchange continues with sequence continuity.
            val afterResume = "after-resume".toByteArray(Charsets.UTF_8)
            val encryptedAfter = requireNotNull(NativePairingTestPeer.pcEncryptSignal(pcHandle, afterResume))
            assertArrayEquals(afterResume, requireNotNull(NativePairing.phoneDecryptSignal(phoneHandle, encryptedAfter)))

            val phoneSignal = "phone-after-resume".toByteArray(Charsets.UTF_8)
            val phoneEncrypted = requireNotNull(NativePairing.phoneEncryptSignal(phoneHandle, phoneSignal))
            assertArrayEquals(phoneSignal, requireNotNull(NativePairingTestPeer.pcDecryptSignal(pcHandle, phoneEncrypted)))
        } finally {
            if (pcHandle > 0L) runCatching { NativePairingTestPeer.pcDestroy(pcHandle) }
            if (phoneHandle > 0L) runCatching { NativePairing.phoneDestroy(phoneHandle) }
            pin.fill(0)
            sessionId.fill(0)
            sessionBytes.fill(0)
        }
    }
}
