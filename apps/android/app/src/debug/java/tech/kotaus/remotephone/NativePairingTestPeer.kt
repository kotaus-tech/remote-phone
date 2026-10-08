package tech.kotaus.remotephone

/** Test-only desktop endpoint backed by the same Rust core as the shipped peer. */
internal object NativePairingTestPeer {
    external fun pcStart(pin: ByteArray, hello: ByteArray): Long
    external fun pcTakeInitialFrame(handle: Long): ByteArray?
    external fun pcHandleFrame(handle: Long, frame: ByteArray): ByteArray?
    external fun pcIsAuthenticated(handle: Long): Boolean
    external fun pcDestroy(handle: Long)
    external fun pcEncryptSignal(handle: Long, plaintext: ByteArray): ByteArray?
}
