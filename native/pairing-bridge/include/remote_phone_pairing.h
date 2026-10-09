#ifndef REMOTE_PHONE_PAIRING_H
#define REMOTE_PHONE_PAIRING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RP_PIN_LENGTH 8
#define RP_SESSION_ID_LENGTH 16
#define RP_MAX_FRAME_BYTES (256 * 1024)

/* Zero is reserved as the invalid handle. Frame functions return a positive
 * byte count on output, zero when the handshake expects no reply, and a
 * negative status code on failure. */
uint64_t rp_phone_create(uint8_t *pin_out, size_t pin_capacity,
                         uint8_t *session_id_out, size_t session_id_capacity);
int64_t rp_phone_start_connection(uint64_t handle, uint8_t *frame_out,
                                  size_t frame_capacity);
int64_t rp_phone_resume_connection(uint64_t handle, uint8_t *frame_out,
                                   size_t frame_capacity);
int64_t rp_phone_handle_frame(uint64_t handle, const uint8_t *frame,
                              size_t frame_length, uint8_t *reply_out,
                              size_t reply_capacity);
int32_t rp_phone_is_authenticated(uint64_t handle);
int32_t rp_phone_attempts_used(uint64_t handle);
int32_t rp_phone_abort_connection(uint64_t handle);
int32_t rp_phone_destroy(uint64_t handle);
int64_t rp_phone_encrypt_signal(uint64_t handle, const uint8_t *plaintext,
                                size_t plaintext_length, uint8_t *frame_out,
                                size_t frame_capacity);
int64_t rp_phone_decrypt_signal(uint64_t handle, const uint8_t *frame,
                                size_t frame_length, uint8_t *plaintext_out,
                                size_t plaintext_capacity);

uint64_t rp_pc_start(const uint8_t *pin, size_t pin_length,
                     const uint8_t *hello_frame, size_t hello_length);
int64_t rp_pc_take_initial_frame(uint64_t handle, uint8_t *frame_out,
                                 size_t frame_capacity);
int64_t rp_pc_handle_frame(uint64_t handle, const uint8_t *frame,
                           size_t frame_length, uint8_t *reply_out,
                           size_t reply_capacity);
int32_t rp_pc_is_authenticated(uint64_t handle);
int32_t rp_pc_destroy(uint64_t handle);
int64_t rp_pc_encrypt_signal(uint64_t handle, const uint8_t *plaintext,
                             size_t plaintext_length, uint8_t *frame_out,
                             size_t frame_capacity);
int64_t rp_pc_decrypt_signal(uint64_t handle, const uint8_t *frame,
                             size_t frame_length, uint8_t *plaintext_out,
                             size_t plaintext_capacity);

#ifdef __cplusplus
}
#endif

#endif /* REMOTE_PHONE_PAIRING_H */
