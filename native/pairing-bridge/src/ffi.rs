use std::collections::HashMap;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::str;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use remote_phone_pairing_core::{
    HandshakeError, PairingError, PcHandshake, PhonePairingSession, TransportError,
    MAX_FRAME_BYTES, MAX_OPAQUE_PAYLOAD_BYTES, PIN_LENGTH,
};
use zeroize::Zeroize;

const ERR_INVALID_HANDLE: i64 = -1;
const ERR_INVALID_ARGUMENT: i64 = -2;
const ERR_BUFFER_TOO_SMALL: i64 = -3;
const ERR_PROTOCOL: i64 = -4;
const ERR_INVALID_STATE: i64 = -5;
const ERR_PANIC: i64 = -127;
const SESSION_ID_LENGTH: usize = 16;

static NEXT_HANDLE: AtomicU64 = AtomicU64::new(1);
static ENDPOINTS: OnceLock<Mutex<HashMap<u64, Arc<Mutex<Endpoint>>>>> = OnceLock::new();

enum Endpoint {
    Phone(PhonePairingSession),
    Pc(PcEndpoint),
}

struct PcEndpoint {
    handshake: PcHandshake,
    initial_frame: Option<Vec<u8>>,
}

fn endpoint_map() -> &'static Mutex<HashMap<u64, Arc<Mutex<Endpoint>>>> {
    ENDPOINTS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn insert_endpoint(endpoint: Endpoint) -> Result<u64, i64> {
    let handle = NEXT_HANDLE
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |current| {
            current.checked_add(1)
        })
        .map_err(|_| ERR_INVALID_STATE)?;
    if handle == 0 {
        return Err(ERR_INVALID_STATE);
    }
    let mut endpoints = endpoint_map().lock().map_err(|_| ERR_PANIC)?;
    endpoints.insert(handle, Arc::new(Mutex::new(endpoint)));
    Ok(handle)
}

fn lookup_endpoint(handle: u64) -> Result<Arc<Mutex<Endpoint>>, i64> {
    endpoint_map()
        .lock()
        .map_err(|_| ERR_PANIC)?
        .get(&handle)
        .cloned()
        .ok_or(ERR_INVALID_HANDLE)
}

fn with_endpoint<T>(
    handle: u64,
    operation: impl FnOnce(&mut Endpoint) -> Result<T, i64>,
) -> Result<T, i64> {
    let endpoint = lookup_endpoint(handle)?;
    let mut endpoint = endpoint.lock().map_err(|_| ERR_PANIC)?;
    operation(&mut endpoint)
}

fn remove_endpoint(handle: u64) -> Result<(), i64> {
    endpoint_map()
        .lock()
        .map_err(|_| ERR_PANIC)?
        .remove(&handle)
        .map(|_| ())
        .ok_or(ERR_INVALID_HANDLE)
}

fn map_pairing_error(_error: PairingError) -> i64 {
    ERR_PROTOCOL
}

fn map_handshake_error(_error: HandshakeError) -> i64 {
    ERR_PROTOCOL
}

fn map_transport_error(_error: TransportError) -> i64 {
    ERR_PROTOCOL
}

unsafe fn read_input<'a>(
    input: *const u8,
    length: usize,
    maximum: usize,
) -> Result<&'a [u8], i64> {
    if length > maximum || (length != 0 && input.is_null()) {
        return Err(ERR_INVALID_ARGUMENT);
    }
    if length == 0 {
        return Ok(&[]);
    }
    // SAFETY: callers of the C ABI must provide a readable buffer of `length`
    // bytes. The length is bounded above before the pointer is dereferenced.
    Ok(unsafe { slice::from_raw_parts(input, length) })
}

unsafe fn write_output(output: *mut u8, capacity: usize, bytes: &[u8]) -> Result<i64, i64> {
    if bytes.len() > capacity || (!bytes.is_empty() && output.is_null()) {
        return Err(ERR_BUFFER_TOO_SMALL);
    }
    if !bytes.is_empty() {
        // SAFETY: callers of the C ABI must provide a writable buffer of at
        // least `capacity` bytes. `bytes.len()` was checked against capacity.
        unsafe { ptr::copy(bytes.as_ptr(), output, bytes.len()) };
    }
    i64::try_from(bytes.len()).map_err(|_| ERR_BUFFER_TOO_SMALL)
}

fn require_frame_capacity(output: *mut u8, capacity: usize) -> Result<(), i64> {
    if output.is_null() || capacity < MAX_FRAME_BYTES {
        Err(ERR_BUFFER_TOO_SMALL)
    } else {
        Ok(())
    }
}

/// Creates a phone PIN session and writes its eight-digit PIN and 16-byte
/// session identifier to caller-owned output buffers. Returns zero on failure.
/// The two output ranges must be valid, writable and non-overlapping.
#[no_mangle]
pub unsafe extern "C" fn rp_phone_create(
    pin_out: *mut u8,
    pin_capacity: usize,
    session_id_out: *mut u8,
    session_id_capacity: usize,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if pin_out.is_null()
            || pin_capacity < PIN_LENGTH
            || session_id_out.is_null()
            || session_id_capacity < SESSION_ID_LENGTH
        {
            return 0;
        }
        let session = match PhonePairingSession::create() {
            Ok(session) => session,
            Err(error) => {
                let _ = map_pairing_error(error);
                return 0;
            }
        };
        let pin = session.pin().expose_for_display().as_bytes();
        if pin.len() != PIN_LENGTH {
            return 0;
        }
        let session_id = session.session_id();
        if write_output(pin_out, pin_capacity, pin).is_err()
            || write_output(session_id_out, session_id_capacity, session_id).is_err()
        {
            return 0;
        }
        insert_endpoint(Endpoint::Phone(session)).unwrap_or(0)
    }))
    .unwrap_or(0)
}

/// Begins a WebSocket attempt and returns the encoded HELLO frame length.
#[no_mangle]
pub unsafe extern "C" fn rp_phone_start_connection(
    handle: u64,
    frame_out: *mut u8,
    frame_capacity: usize,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if frame_out.is_null() || frame_capacity < 34 {
            return ERR_BUFFER_TOO_SMALL;
        }
        with_endpoint(handle, |endpoint| {
            let Endpoint::Phone(session) = endpoint else {
                return Err(ERR_INVALID_HANDLE);
            };
            let frame = session
                .start_connection()
                .map_err(map_handshake_error)?;
            write_output(frame_out, frame_capacity, &frame)
        })
        .unwrap_or_else(|error| error)
    }))
    .unwrap_or(ERR_PANIC)
}

/// Feeds one complete binary frame to the phone state machine. Returns zero
/// after a valid AUTH_ACK, a positive reply length, or a negative error code.
#[no_mangle]
pub unsafe extern "C" fn rp_phone_handle_frame(
    handle: u64,
    frame: *const u8,
    frame_length: usize,
    reply_out: *mut u8,
    reply_capacity: usize,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if require_frame_capacity(reply_out, reply_capacity).is_err() {
            return ERR_BUFFER_TOO_SMALL;
        }
        let frame = match read_input(frame, frame_length, MAX_FRAME_BYTES) {
            Ok(frame) => frame,
            Err(error) => return error,
        };
        with_endpoint(handle, |endpoint| {
            let Endpoint::Phone(session) = endpoint else {
                return Err(ERR_INVALID_HANDLE);
            };
            match session.handle_handshake_frame(frame) {
                Ok(Some(reply)) => write_output(reply_out, reply_capacity, &reply),
                Ok(None) => Ok(0),
                Err(error) => Err(map_handshake_error(error)),
            }
        })
        .unwrap_or_else(|error| error)
    }))
    .unwrap_or(ERR_PANIC)
}

#[no_mangle]
pub extern "C" fn rp_phone_is_authenticated(handle: u64) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        with_endpoint(handle, |endpoint| match endpoint {
            Endpoint::Phone(session) => Ok(if session.is_authenticated() { 1 } else { 0 }),
            Endpoint::Pc(_) => Err(ERR_INVALID_HANDLE),
        })
        .unwrap_or_else(|error| error as i32)
    }))
    .unwrap_or(ERR_PANIC as i32)
}

#[no_mangle]
pub extern "C" fn rp_phone_attempts_used(handle: u64) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        with_endpoint(handle, |endpoint| match endpoint {
            Endpoint::Phone(session) => Ok(i32::from(session.attempts_used())),
            Endpoint::Pc(_) => Err(ERR_INVALID_HANDLE),
        })
        .unwrap_or_else(|error| error as i32)
    }))
    .unwrap_or(ERR_PANIC as i32)
}

#[no_mangle]
pub extern "C" fn rp_phone_abort_connection(handle: u64) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        with_endpoint(handle, |endpoint| match endpoint {
            Endpoint::Phone(session) => {
                session.abort_connection();
                Ok(0)
            }
            Endpoint::Pc(_) => Err(ERR_INVALID_HANDLE),
        })
        .unwrap_or_else(|error| error as i32)
    }))
    .unwrap_or(ERR_PANIC as i32)
}

#[no_mangle]
pub extern "C" fn rp_phone_destroy(handle: u64) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        remove_endpoint(handle).map(|_| 0).unwrap_or_else(|error| error as i32)
    }))
    .unwrap_or(ERR_PANIC as i32)
}

/// Encrypts one signaling payload after mutual authentication.
#[no_mangle]
pub unsafe extern "C" fn rp_phone_encrypt_signal(
    handle: u64,
    plaintext: *const u8,
    plaintext_length: usize,
    frame_out: *mut u8,
    frame_capacity: usize,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if require_frame_capacity(frame_out, frame_capacity).is_err() {
            return ERR_BUFFER_TOO_SMALL;
        }
        let plaintext = match read_input(plaintext, plaintext_length, MAX_FRAME_BYTES) {
            Ok(plaintext) => plaintext,
            Err(error) => return error,
        };
        with_endpoint(handle, |endpoint| {
            let Endpoint::Phone(session) = endpoint else {
                return Err(ERR_INVALID_HANDLE);
            };
            let frame = session.encrypt_signal(plaintext).map_err(map_transport_error)?;
            write_output(frame_out, frame_capacity, &frame)
        })
        .unwrap_or_else(|error| error)
    }))
    .unwrap_or(ERR_PANIC)
}

/// Authenticates and decrypts one signal frame after mutual authentication.
#[no_mangle]
pub unsafe extern "C" fn rp_phone_decrypt_signal(
    handle: u64,
    frame: *const u8,
    frame_length: usize,
    plaintext_out: *mut u8,
    plaintext_capacity: usize,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if require_frame_capacity(plaintext_out, plaintext_capacity).is_err() {
            return ERR_BUFFER_TOO_SMALL;
        }
        let frame = match read_input(frame, frame_length, MAX_FRAME_BYTES) {
            Ok(frame) => frame,
            Err(error) => return error,
        };
        with_endpoint(handle, |endpoint| {
            let Endpoint::Phone(session) = endpoint else {
                return Err(ERR_INVALID_HANDLE);
            };
            let plaintext = session.decrypt_signal(frame).map_err(map_transport_error)?;
            write_output(plaintext_out, plaintext_capacity, &plaintext)
        })
        .unwrap_or_else(|error| error)
    }))
    .unwrap_or(ERR_PANIC)
}

/// Starts the desktop OPAQUE client from the received HELLO frame. The PIN is
/// copied into a zeroizing buffer before it reaches the safe Rust core.
#[no_mangle]
pub unsafe extern "C" fn rp_pc_start(
    pin: *const u8,
    pin_length: usize,
    hello_frame: *const u8,
    hello_length: usize,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if pin_length != PIN_LENGTH {
            return 0;
        }
        let pin_bytes = match read_input(pin, pin_length, PIN_LENGTH) {
            Ok(pin) => pin,
            Err(_) => return 0,
        };
        let hello = match read_input(
            hello_frame,
            hello_length,
            MAX_OPAQUE_PAYLOAD_BYTES + 29,
        ) {
            Ok(hello) => hello,
            Err(_) => return 0,
        };
        let mut pin = match str::from_utf8(pin_bytes) {
            Ok(pin) => pin.to_owned(),
            Err(_) => return 0,
        };
        let result = PcHandshake::start(&pin, hello);
        pin.zeroize();
        match result {
            Ok((handshake, initial_frame)) => insert_endpoint(Endpoint::Pc(PcEndpoint {
                handshake,
                initial_frame: Some(initial_frame),
            }))
            .unwrap_or(0),
            Err(error) => {
                let _ = map_handshake_error(error);
                0
            }
        }
    }))
    .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn rp_pc_take_initial_frame(
    handle: u64,
    frame_out: *mut u8,
    frame_capacity: usize,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if require_frame_capacity(frame_out, frame_capacity).is_err() {
            return ERR_BUFFER_TOO_SMALL;
        }
        with_endpoint(handle, |endpoint| {
            let Endpoint::Pc(client) = endpoint else {
                return Err(ERR_INVALID_HANDLE);
            };
            let frame = client
                .initial_frame
                .take()
                .ok_or(ERR_INVALID_STATE)?;
            write_output(frame_out, frame_capacity, &frame)
        })
        .unwrap_or_else(|error| error)
    }))
    .unwrap_or(ERR_PANIC)
}

#[no_mangle]
pub unsafe extern "C" fn rp_pc_handle_frame(
    handle: u64,
    frame: *const u8,
    frame_length: usize,
    reply_out: *mut u8,
    reply_capacity: usize,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if require_frame_capacity(reply_out, reply_capacity).is_err() {
            return ERR_BUFFER_TOO_SMALL;
        }
        let frame = match read_input(frame, frame_length, MAX_FRAME_BYTES) {
            Ok(frame) => frame,
            Err(error) => return error,
        };
        with_endpoint(handle, |endpoint| {
            let Endpoint::Pc(client) = endpoint else {
                return Err(ERR_INVALID_HANDLE);
            };
            match client.handshake.handle_handshake_frame(frame) {
                Ok(Some(reply)) => write_output(reply_out, reply_capacity, &reply),
                Ok(None) => Ok(0),
                Err(error) => Err(map_handshake_error(error)),
            }
        })
        .unwrap_or_else(|error| error)
    }))
    .unwrap_or(ERR_PANIC)
}

#[no_mangle]
pub extern "C" fn rp_pc_is_authenticated(handle: u64) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        with_endpoint(handle, |endpoint| match endpoint {
            Endpoint::Pc(client) => Ok(if client.handshake.is_authenticated() { 1 } else { 0 }),
            Endpoint::Phone(_) => Err(ERR_INVALID_HANDLE),
        })
        .unwrap_or_else(|error| error as i32)
    }))
    .unwrap_or(ERR_PANIC as i32)
}

#[no_mangle]
pub extern "C" fn rp_pc_destroy(handle: u64) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        remove_endpoint(handle).map(|_| 0).unwrap_or_else(|error| error as i32)
    }))
    .unwrap_or(ERR_PANIC as i32)
}

#[no_mangle]
pub unsafe extern "C" fn rp_pc_encrypt_signal(
    handle: u64,
    plaintext: *const u8,
    plaintext_length: usize,
    frame_out: *mut u8,
    frame_capacity: usize,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if require_frame_capacity(frame_out, frame_capacity).is_err() {
            return ERR_BUFFER_TOO_SMALL;
        }
        let plaintext = match read_input(plaintext, plaintext_length, MAX_FRAME_BYTES) {
            Ok(plaintext) => plaintext,
            Err(error) => return error,
        };
        with_endpoint(handle, |endpoint| {
            let Endpoint::Pc(client) = endpoint else {
                return Err(ERR_INVALID_HANDLE);
            };
            let frame = client
                .handshake
                .encrypt_signal(plaintext)
                .map_err(map_transport_error)?;
            write_output(frame_out, frame_capacity, &frame)
        })
        .unwrap_or_else(|error| error)
    }))
    .unwrap_or(ERR_PANIC)
}

#[no_mangle]
pub unsafe extern "C" fn rp_pc_decrypt_signal(
    handle: u64,
    frame: *const u8,
    frame_length: usize,
    plaintext_out: *mut u8,
    plaintext_capacity: usize,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if require_frame_capacity(plaintext_out, plaintext_capacity).is_err() {
            return ERR_BUFFER_TOO_SMALL;
        }
        let frame = match read_input(frame, frame_length, MAX_FRAME_BYTES) {
            Ok(frame) => frame,
            Err(error) => return error,
        };
        with_endpoint(handle, |endpoint| {
            let Endpoint::Pc(client) = endpoint else {
                return Err(ERR_INVALID_HANDLE);
            };
            let plaintext = client
                .handshake
                .decrypt_signal(frame)
                .map_err(map_transport_error)?;
            write_output(plaintext_out, plaintext_capacity, &plaintext)
        })
        .unwrap_or_else(|error| error)
    }))
    .unwrap_or(ERR_PANIC)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn take_frame(result: i64, output: &mut [u8]) -> Vec<u8> {
        assert!(result >= 0, "C bridge error code: {result}");
        let length = usize::try_from(result).unwrap();
        output[..length].to_vec()
    }

    #[test]
    fn c_api_completes_pairing_and_keeps_signal_keys_inside_native_handles() {
        let mut pin = [0_u8; PIN_LENGTH];
        let mut session_id = [0_u8; SESSION_ID_LENGTH];
        // SAFETY: the fixed-size arrays are valid, writable, and non-overlapping.
        let phone = unsafe {
            rp_phone_create(
                pin.as_mut_ptr(),
                pin.len(),
                session_id.as_mut_ptr(),
                session_id.len(),
            )
        };
        assert_ne!(phone, 0);
        assert!(pin.iter().all(|digit| digit.is_ascii_digit()));

        let mut frame_buffer = vec![0_u8; MAX_FRAME_BYTES];
        // SAFETY: the output buffer is writable and has the advertised capacity.
        let hello_len = unsafe {
            rp_phone_start_connection(phone, frame_buffer.as_mut_ptr(), frame_buffer.len())
        };
        let hello = take_frame(hello_len, &mut frame_buffer);

        // SAFETY: `pin` and `hello` remain readable for the duration of this call.
        let pc = unsafe {
            rp_pc_start(
                pin.as_ptr(),
                pin.len(),
                hello.as_ptr(),
                hello.len(),
            )
        };
        assert_ne!(pc, 0);
        pin.zeroize();

        // SAFETY: the output buffer is writable and has the advertised capacity.
        let login1_len = unsafe {
            rp_pc_take_initial_frame(pc, frame_buffer.as_mut_ptr(), frame_buffer.len())
        };
        let login1 = take_frame(login1_len, &mut frame_buffer);

        // SAFETY: input and output buffers are valid and do not overlap.
        let login2_len = unsafe {
            rp_phone_handle_frame(
                phone,
                login1.as_ptr(),
                login1.len(),
                frame_buffer.as_mut_ptr(),
                frame_buffer.len(),
            )
        };
        let login2 = take_frame(login2_len, &mut frame_buffer);

        // SAFETY: input and output buffers are valid and do not overlap.
        let login3_len = unsafe {
            rp_pc_handle_frame(
                pc,
                login2.as_ptr(),
                login2.len(),
                frame_buffer.as_mut_ptr(),
                frame_buffer.len(),
            )
        };
        let login3 = take_frame(login3_len, &mut frame_buffer);

        // SAFETY: input and output buffers are valid and do not overlap.
        let auth_ok_len = unsafe {
            rp_phone_handle_frame(
                phone,
                login3.as_ptr(),
                login3.len(),
                frame_buffer.as_mut_ptr(),
                frame_buffer.len(),
            )
        };
        let auth_ok = take_frame(auth_ok_len, &mut frame_buffer);

        // SAFETY: input and output buffers are valid and do not overlap.
        let auth_ack_len = unsafe {
            rp_pc_handle_frame(
                pc,
                auth_ok.as_ptr(),
                auth_ok.len(),
                frame_buffer.as_mut_ptr(),
                frame_buffer.len(),
            )
        };
        let auth_ack = take_frame(auth_ack_len, &mut frame_buffer);
        assert_eq!(rp_pc_is_authenticated(pc), 1);

        // SAFETY: input and output buffers are valid and do not overlap.
        let no_reply = unsafe {
            rp_phone_handle_frame(
                phone,
                auth_ack.as_ptr(),
                auth_ack.len(),
                frame_buffer.as_mut_ptr(),
                frame_buffer.len(),
            )
        };
        assert_eq!(no_reply, 0);
        assert_eq!(rp_phone_is_authenticated(phone), 1);

        let mut signal_frame = vec![0_u8; MAX_FRAME_BYTES];
        // SAFETY: input and output buffers are valid and do not overlap.
        let signal_len = unsafe {
            rp_pc_encrypt_signal(
                pc,
                b"offer".as_ptr(),
                b"offer".len(),
                signal_frame.as_mut_ptr(),
                signal_frame.len(),
            )
        };
        let signal = take_frame(signal_len, &mut signal_frame);
        let mut plaintext = vec![0_u8; MAX_FRAME_BYTES];
        // SAFETY: input and output buffers are valid and do not overlap.
        let plaintext_len = unsafe {
            rp_phone_decrypt_signal(
                phone,
                signal.as_ptr(),
                signal.len(),
                plaintext.as_mut_ptr(),
                plaintext.len(),
            )
        };
        assert_eq!(take_frame(plaintext_len, &mut plaintext), b"offer");

        assert_eq!(rp_phone_destroy(phone), 0);
        assert_eq!(rp_pc_destroy(pc), 0);
        assert_eq!(rp_phone_is_authenticated(phone), ERR_INVALID_HANDLE as i32);
        session_id.zeroize();
    }

    #[test]
    fn c_api_validates_handles_before_dereferencing_input() {
        // SAFETY: a null pointer is supplied with length zero and no handle
        // exists, so this call must fail before it can read or write a buffer.
        let result = unsafe {
            rp_phone_handle_frame(0xffff, ptr::null(), 0, ptr::null_mut(), 0)
        };
        assert_eq!(result, ERR_BUFFER_TOO_SMALL);
        assert_eq!(rp_phone_is_authenticated(0xffff), ERR_INVALID_HANDLE as i32);
    }
}
