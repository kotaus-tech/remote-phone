#include <jni.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "remote_phone_pairing.h"

namespace {
constexpr size_t kHandleLength = sizeof(uint64_t);
constexpr size_t kSessionResultLength = kHandleLength + RP_PIN_LENGTH + RP_SESSION_ID_LENGTH;

void SecureZero(uint8_t* bytes, size_t length) {
    volatile uint8_t* target = bytes;
    while (length-- != 0) {
        *target++ = 0;
    }
}

void ThrowPairingError(JNIEnv* env) {
    jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
    if (exceptionClass != nullptr) {
        env->ThrowNew(exceptionClass, "PAIRING_FAILED");
        env->DeleteLocalRef(exceptionClass);
    }
}

bool CopyInput(JNIEnv* env, jbyteArray input, size_t maximum, std::vector<uint8_t>* output) {
    if (input == nullptr || output == nullptr) {
        ThrowPairingError(env);
        return false;
    }
    const jsize length = env->GetArrayLength(input);
    if (length < 0 || static_cast<size_t>(length) > maximum) {
        ThrowPairingError(env);
        return false;
    }
    output->resize(static_cast<size_t>(length));
    if (length != 0) {
        env->GetByteArrayRegion(input, 0, length, reinterpret_cast<jbyte*>(output->data()));
    }
    return !env->ExceptionCheck();
}

jbyteArray MakeOutput(JNIEnv* env, const uint8_t* bytes, size_t length) {
    if (length > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
        ThrowPairingError(env);
        return nullptr;
    }
    jbyteArray output = env->NewByteArray(static_cast<jsize>(length));
    if (output == nullptr) {
        return nullptr;
    }
    if (length != 0) {
        env->SetByteArrayRegion(
            output,
            0,
            static_cast<jsize>(length),
            reinterpret_cast<const jbyte*>(bytes));
    }
    return env->ExceptionCheck() ? nullptr : output;
}

bool ValidHandle(JNIEnv* env, jlong handle) {
    if (handle <= 0) {
        ThrowPairingError(env);
        return false;
    }
    return true;
}

jbyteArray FrameCallResult(JNIEnv* env, int64_t result, const std::vector<uint8_t>& output,
                           bool zeroMeansNoReply) {
    if (result < 0) {
        ThrowPairingError(env);
        return nullptr;
    }
    if (result == 0 && zeroMeansNoReply) {
        return nullptr;
    }
    const size_t length = static_cast<size_t>(result);
    if (length > output.size()) {
        ThrowPairingError(env);
        return nullptr;
    }
    return MakeOutput(env, output.data(), length);
}

int32_t CheckSimpleResult(JNIEnv* env, int32_t result) {
    if (result < 0) {
        ThrowPairingError(env);
    }
    return result;
}
}  // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneCreate(JNIEnv* env, jobject) {
    uint8_t pin[RP_PIN_LENGTH] = {};
    uint8_t sessionId[RP_SESSION_ID_LENGTH] = {};
    const uint64_t handle = rp_phone_create(pin, sizeof(pin), sessionId, sizeof(sessionId));
    if (handle == 0) {
        SecureZero(pin, sizeof(pin));
        SecureZero(sessionId, sizeof(sessionId));
        ThrowPairingError(env);
        return nullptr;
    }

    uint8_t result[kSessionResultLength] = {};
    std::memcpy(result, &handle, kHandleLength);
    std::memcpy(result + kHandleLength, pin, sizeof(pin));
    std::memcpy(result + kHandleLength + sizeof(pin), sessionId, sizeof(sessionId));
    jbyteArray output = MakeOutput(env, result, sizeof(result));
    SecureZero(pin, sizeof(pin));
    SecureZero(sessionId, sizeof(sessionId));
    SecureZero(result, sizeof(result));
    if (output == nullptr) {
        rp_phone_destroy(handle);
    }
    return output;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneStartConnection(JNIEnv* env, jobject,
                                                                 jlong handle) {
    if (!ValidHandle(env, handle)) {
        return nullptr;
    }
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_phone_start_connection(
        static_cast<uint64_t>(handle), output.data(), output.size());
    return FrameCallResult(env, result, output, false);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneResumeConnection(JNIEnv* env, jobject,
                                                                  jlong handle) {
    if (!ValidHandle(env, handle)) {
        return nullptr;
    }
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_phone_resume_connection(
        static_cast<uint64_t>(handle), output.data(), output.size());
    return FrameCallResult(env, result, output, false);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneHandleFrame(JNIEnv* env, jobject,
                                                             jlong handle,
                                                             jbyteArray frameArray) {
    if (!ValidHandle(env, handle)) {
        return nullptr;
    }
    std::vector<uint8_t> frame;
    if (!CopyInput(env, frameArray, RP_MAX_FRAME_BYTES, &frame)) {
        return nullptr;
    }
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_phone_handle_frame(
        static_cast<uint64_t>(handle), frame.data(), frame.size(), output.data(), output.size());
    return FrameCallResult(env, result, output, true);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneIsAuthenticated(JNIEnv* env, jobject,
                                                                 jlong handle) {
    if (!ValidHandle(env, handle)) {
        return JNI_FALSE;
    }
    const int32_t result = rp_phone_is_authenticated(static_cast<uint64_t>(handle));
    if (result < 0) {
        ThrowPairingError(env);
        return JNI_FALSE;
    }
    return result == 1 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneAttemptsUsed(JNIEnv* env, jobject,
                                                             jlong handle) {
    if (!ValidHandle(env, handle)) {
        return -1;
    }
    const int32_t result = rp_phone_attempts_used(static_cast<uint64_t>(handle));
    if (result < 0) {
        ThrowPairingError(env);
    }
    return static_cast<jint>(result);
}

extern "C" JNIEXPORT void JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneAbortConnection(JNIEnv* env, jobject,
                                                                 jlong handle) {
    if (ValidHandle(env, handle)) {
        CheckSimpleResult(env, rp_phone_abort_connection(static_cast<uint64_t>(handle)));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneDestroy(JNIEnv* env, jobject, jlong handle) {
    if (ValidHandle(env, handle)) {
        CheckSimpleResult(env, rp_phone_destroy(static_cast<uint64_t>(handle)));
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneEncryptSignal(JNIEnv* env, jobject,
                                                               jlong handle,
                                                               jbyteArray plaintextArray) {
    if (!ValidHandle(env, handle)) {
        return nullptr;
    }
    std::vector<uint8_t> plaintext;
    if (!CopyInput(env, plaintextArray, RP_MAX_FRAME_BYTES, &plaintext)) {
        return nullptr;
    }
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_phone_encrypt_signal(
        static_cast<uint64_t>(handle), plaintext.data(), plaintext.size(), output.data(),
        output.size());
    return FrameCallResult(env, result, output, false);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairing_phoneDecryptSignal(JNIEnv* env, jobject,
                                                               jlong handle,
                                                               jbyteArray frameArray) {
    if (!ValidHandle(env, handle)) {
        return nullptr;
    }
    std::vector<uint8_t> frame;
    if (!CopyInput(env, frameArray, RP_MAX_FRAME_BYTES, &frame)) {
        return nullptr;
    }
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_phone_decrypt_signal(
        static_cast<uint64_t>(handle), frame.data(), frame.size(), output.data(), output.size());
    return FrameCallResult(env, result, output, false);
}

#ifdef REMOTE_PHONE_ENABLE_TEST_PEER
// Debug-only desktop-peer JNI wrappers let the Android emulator exercise the
// same full OPAQUE handshake and encrypted SIGNAL exchange as the Windows peer.
extern "C" JNIEXPORT jlong JNICALL
Java_tech_kotaus_remotephone_NativePairingTestPeer_pcStart(JNIEnv* env, jobject,
                                                            jbyteArray pinArray,
                                                            jbyteArray helloArray) {
    std::vector<uint8_t> pin;
    std::vector<uint8_t> hello;
    if (!CopyInput(env, pinArray, RP_PIN_LENGTH, &pin)
        || !CopyInput(env, helloArray, RP_MAX_FRAME_BYTES, &hello)
        || pin.size() != RP_PIN_LENGTH) {
        return 0;
    }
    const uint64_t handle = rp_pc_start(pin.data(), pin.size(), hello.data(), hello.size());
    if (!pin.empty()) SecureZero(pin.data(), pin.size());
    if (handle == 0) ThrowPairingError(env);
    return static_cast<jlong>(handle);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairingTestPeer_pcTakeInitialFrame(JNIEnv* env, jobject,
                                                                       jlong handle) {
    if (!ValidHandle(env, handle)) return nullptr;
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_pc_take_initial_frame(
        static_cast<uint64_t>(handle), output.data(), output.size());
    return FrameCallResult(env, result, output, false);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairingTestPeer_pcHandleFrame(JNIEnv* env, jobject,
                                                                  jlong handle,
                                                                  jbyteArray frameArray) {
    if (!ValidHandle(env, handle)) return nullptr;
    std::vector<uint8_t> frame;
    if (!CopyInput(env, frameArray, RP_MAX_FRAME_BYTES, &frame)) return nullptr;
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_pc_handle_frame(
        static_cast<uint64_t>(handle), frame.data(), frame.size(), output.data(), output.size());
    return FrameCallResult(env, result, output, true);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_tech_kotaus_remotephone_NativePairingTestPeer_pcIsAuthenticated(JNIEnv* env, jobject,
                                                                      jlong handle) {
    if (!ValidHandle(env, handle)) return JNI_FALSE;
    const int32_t result = rp_pc_is_authenticated(static_cast<uint64_t>(handle));
    if (result < 0) {
        ThrowPairingError(env);
        return JNI_FALSE;
    }
    return result == 1 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_tech_kotaus_remotephone_NativePairingTestPeer_pcDestroy(JNIEnv* env, jobject,
                                                              jlong handle) {
    if (ValidHandle(env, handle)) {
        CheckSimpleResult(env, rp_pc_destroy(static_cast<uint64_t>(handle)));
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairingTestPeer_pcEncryptSignal(JNIEnv* env, jobject,
                                                                    jlong handle,
                                                                    jbyteArray plaintextArray) {
    if (!ValidHandle(env, handle)) return nullptr;
    std::vector<uint8_t> plaintext;
    if (!CopyInput(env, plaintextArray, RP_MAX_FRAME_BYTES, &plaintext)) return nullptr;
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_pc_encrypt_signal(
        static_cast<uint64_t>(handle), plaintext.data(), plaintext.size(), output.data(),
        output.size());
    return FrameCallResult(env, result, output, false);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tech_kotaus_remotephone_NativePairingTestPeer_pcDecryptSignal(JNIEnv* env, jobject,
                                                                    jlong handle,
                                                                    jbyteArray frameArray) {
    if (!ValidHandle(env, handle)) return nullptr;
    std::vector<uint8_t> frame;
    if (!CopyInput(env, frameArray, RP_MAX_FRAME_BYTES, &frame)) return nullptr;
    std::vector<uint8_t> output(RP_MAX_FRAME_BYTES);
    const int64_t result = rp_pc_decrypt_signal(
        static_cast<uint64_t>(handle), frame.data(), frame.size(), output.data(), output.size());
    return FrameCallResult(env, result, output, false);
}
#endif
