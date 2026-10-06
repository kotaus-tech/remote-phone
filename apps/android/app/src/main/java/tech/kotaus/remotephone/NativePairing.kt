package tech.kotaus.remotephone

/** Rust-backed OPAQUE bridge. PINs and session keys stay inside native Rust. */
object NativePairing {
    init {
        System.loadLibrary("remote_phone_jni")
    }

    external fun phoneCreate(): ByteArray?
    external fun phoneStartConnection(handle: Long): ByteArray?
    external fun phoneHandleFrame(handle: Long, frame: ByteArray): ByteArray?
    external fun phoneIsAuthenticated(handle: Long): Boolean
    external fun phoneAttemptsUsed(handle: Long): Int
    external fun phoneAbortConnection(handle: Long)
    external fun phoneDestroy(handle: Long)
    external fun phoneEncryptSignal(handle: Long, plaintext: ByteArray): ByteArray?
    external fun phoneDecryptSignal(handle: Long, frame: ByteArray): ByteArray?
}
