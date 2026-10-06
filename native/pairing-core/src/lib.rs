#![forbid(unsafe_code)]

//! Shared OPAQUE pairing primitives. Network adapters must pass only bounded
//! OPAQUE payloads here; PINs and session keys must never be logged or exported.

use std::fmt;
use std::time::{Duration, Instant};

use hkdf::Hkdf;
use hmac::{Hmac, Mac};
use opaque_ke::argon2::{Algorithm, Argon2, Params, Version};
use opaque_ke::ciphersuite::CipherSuite;
use opaque_ke::rand::{CryptoRng, RngCore};
use rand::rngs::OsRng;
use opaque_ke::{
    ClientLogin, ClientLoginFinishParameters, ClientRegistration,
    ClientRegistrationFinishParameters, CredentialFinalization, CredentialRequest,
    CredentialResponse, Identifiers, ServerLogin, ServerLoginParameters, ServerRegistration,
    ServerSetup,
};
use sha2::Sha512;
use subtle::ConstantTimeEq;
use thiserror::Error;
use zeroize::{Zeroize, Zeroizing};

pub const PIN_LENGTH: usize = 8;
pub const PIN_LIFETIME: Duration = Duration::from_secs(5 * 60);
pub const MAX_LOGIN_ATTEMPTS: u8 = 5;
pub const MAX_OPAQUE_PAYLOAD_BYTES: usize = 16 * 1024;

const ARGON2_MEMORY_COST_KIB: u32 = 65_536;
const ARGON2_TIME_COST: u32 = 3;
const ARGON2_LANES: u32 = 4;
const ARGON2_SALT_LEN: usize = opaque_ke::argon2::RECOMMENDED_SALT_LEN;
const OPAQUE_HASH_LEN: usize = 64;

mod handshake;
mod transport;
pub use handshake::{HandshakeError, PcHandshake, PhonePairingSession};
pub use transport::{
    AuthenticatedSession, Frame, FrameSender, HandshakeTranscript, Hello, MessageType,
    SignalCipher, TransportError, AEAD_TAG_LEN, AUTH_TAG_LEN, FRAME_HEADER_LEN,
    HELLO_PAYLOAD_LEN, MAX_FRAME_BYTES, MAX_SESSION_LIFETIME_SECONDS,
    MAX_UNAUTHENTICATED_PAYLOAD_BYTES, PROFILE_ID, PROTOCOL_VERSION,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum EndpointRole {
    Phone,
    Pc,
}

const PIN_SPACE: u64 = 100_000_000;
const PIN_PREFIX: &[u8] = b"remote-phone/pairing/v1/";
const CLIENT_ID: &[u8] = b"remote-phone/windows";
const SERVER_ID: &[u8] = b"remote-phone/android";
const USER_ID_PREFIX: &[u8] = b"remote-phone/pairing-user/v1/";
const CONFIRM_INFO: &[u8] = b"remote-phone/v1/confirm";
const PC_TO_PHONE_INFO: &[u8] = b"remote-phone/v1/signal/pc-to-phone";
const PHONE_TO_PC_INFO: &[u8] = b"remote-phone/v1/signal/phone-to-pc";
const SERVER_FINISHED_LABEL: &[u8] = b"RVP1/server-finished\0";
const CLIENT_FINISHED_LABEL: &[u8] = b"RVP1/client-finished\0";

/// The only OPAQUE ciphersuite supported by protocol version 1.
pub struct RemotePhoneCipherSuite;

impl CipherSuite for RemotePhoneCipherSuite {
    type OprfCs = opaque_ke::Ristretto255;
    type KeyExchange = opaque_ke::TripleDh<opaque_ke::Ristretto255, Sha512>;
    type Ksf = Argon2<'static>;
}

/// Failures intentionally avoid including a PIN, key, or protocol secret.
#[derive(Debug, Error, PartialEq, Eq)]
pub enum PairingError {
    #[error("Код сопряжения должен состоять ровно из восьми цифр.")]
    InvalidPin,
    #[error("Срок действия сеанса сопряжения истёк или сеанс уже использован.")]
    SessionExpired,
    #[error("Превышено допустимое число попыток сопряжения.")]
    AttemptsExhausted,
    #[error("Сообщение сопряжения некорректно или слишком велико.")]
    InvalidMessage,
    #[error("Не удалось проверить код сопряжения.")]
    AuthenticationFailed,
    #[error("Нет активного обмена данными для сопряжения.")]
    NoLoginInProgress,
    #[error("Не удалось сформировать ключи защищённого сеанса.")]
    KeyDerivationFailed,
    #[error("Не удалось проверить подтверждение защищённого сеанса.")]
    ConfirmationFailed,
    #[error("Не удалось получить безопасные случайные данные из системы.")]
    EntropyUnavailable,
}

/// A generated phone PIN. Its debug representation is always redacted.
pub struct PairingPin(Zeroizing<String>);

impl PairingPin {
    /// Exposes the code for immediate display on the phone UI. Callers must not
    /// persist or log this string and should clear their UI copy after use.
    pub fn expose_for_display(&self) -> &str {
        self.0.as_str()
    }
}

impl fmt::Debug for PairingPin {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("PairingPin([REDACTED])")
    }
}

/// Phone-side ephemeral OPAQUE server state for one pairing session.
pub struct PairingServer {
    session_id: [u8; 16],
    context: Vec<u8>,
    pin: PairingPin,
    server_setup: Option<ServerSetup<RemotePhoneCipherSuite>>,
    password_file: Zeroizing<Vec<u8>>,
    started_at: Instant,
    attempts: u8,
    pending_login: Option<ServerLogin<RemotePhoneCipherSuite>>,
    consumed: bool,
    locked_out: bool,
}

impl PairingServer {
    /// Creates one temporary eight-digit PIN and its in-memory OPAQUE record.
    pub fn create() -> Result<Self, PairingError> {
        let mut rng = OsRng;
        Self::create_with_rng(&mut rng, Instant::now())
    }

    fn create_with_rng<R>(rng: &mut R, started_at: Instant) -> Result<Self, PairingError>
    where
        R: RngCore + CryptoRng,
    {
        let mut session_id = [0_u8; 16];
        rng.try_fill_bytes(&mut session_id)
            .map_err(|_| PairingError::EntropyUnavailable)?;

        let pin = generate_pin(rng)?;
        let server_setup = ServerSetup::<RemotePhoneCipherSuite>::new(rng);
        let user_id = user_identifier(&session_id);
        let password_file = register_locally(
            &server_setup,
            &user_id,
            pin.expose_for_display().as_bytes(),
            rng,
        )?;

        let context = login_context(&session_id);
        Ok(Self {
            session_id,
            context,
            pin,
            server_setup: Some(server_setup),
            password_file: Zeroizing::new(password_file),
            started_at,
            attempts: 0,
            pending_login: None,
            consumed: false,
            locked_out: false,
        })
    }

    /// Returns the random session identifier for the unauthenticated HELLO frame.
    pub fn session_id(&self) -> &[u8; 16] {
        &self.session_id
    }

    /// Returns the one-time PIN for immediate presentation on the phone.
    pub fn pin(&self) -> &PairingPin {
        &self.pin
    }

    /// Number of well-formed OPAQUE login requests charged to this session.
    pub fn attempts_used(&self) -> u8 {
        self.attempts
    }

    /// Remaining advertised lifetime, rounded up to whole seconds.
    pub(crate) fn remaining_lifetime_seconds(
        &mut self,
        now: Instant,
    ) -> Result<u16, PairingError> {
        self.ensure_active(now)?;
        let remaining = PIN_LIFETIME.saturating_sub(now.saturating_duration_since(self.started_at));
        let seconds = remaining
            .as_secs()
            .saturating_add((remaining.subsec_nanos() > 0) as u64)
            .min(crate::MAX_SESSION_LIFETIME_SECONDS as u64)
            .max(1);
        Ok(seconds as u16)
    }

    /// Processes a bounded LOGIN1 payload and returns a serialized LOGIN2.
    /// The request is parsed before it is charged; the attempt is then charged
    /// before the OPAQUE server performs cryptographic processing.
    pub fn begin_login(&mut self, payload: &[u8]) -> Result<Vec<u8>, PairingError> {
        self.begin_login_at(payload, Instant::now())
    }

    fn begin_login_at(
        &mut self,
        payload: &[u8],
        now: Instant,
    ) -> Result<Vec<u8>, PairingError> {
        self.ensure_active(now)?;
        if payload.is_empty() || payload.len() > MAX_OPAQUE_PAYLOAD_BYTES {
            return Err(PairingError::InvalidMessage);
        }
        let request = CredentialRequest::<RemotePhoneCipherSuite>::deserialize(payload)
            .map_err(|_| PairingError::InvalidMessage)?;
        if self.attempts >= MAX_LOGIN_ATTEMPTS {
            self.locked_out = true;
            self.clear_secrets();
            return Err(PairingError::AttemptsExhausted);
        }
        self.attempts += 1;

        let record = ServerRegistration::<RemotePhoneCipherSuite>::deserialize(
            self.password_file.as_slice(),
        )
        .map_err(|_| PairingError::AuthenticationFailed)?;
        let setup = self
            .server_setup
            .as_ref()
            .ok_or(PairingError::SessionExpired)?;
        let params = self.server_login_parameters();
        let mut rng = OsRng;
        let result = ServerLogin::start(
            &mut rng,
            setup,
            Some(record),
            request,
            &user_identifier(&self.session_id),
            params,
        )
        .map_err(|_| PairingError::InvalidMessage)?;

        let response = result.message.serialize().to_vec();
        if response.len() > MAX_OPAQUE_PAYLOAD_BYTES {
            return Err(PairingError::InvalidMessage);
        }
        // A new LOGIN1 drops any previous pending state. Every well-formed
        // request above has already consumed its own attempt.
        self.pending_login = Some(result.state);
        Ok(response)
    }

    /// Completes OPAQUE after LOGIN3 and returns keys held only in native Rust.
    pub fn finish_login(&mut self, payload: &[u8]) -> Result<SessionKeys, PairingError> {
        self.finish_login_at(payload, Instant::now())
    }

    fn finish_login_at(
        &mut self,
        payload: &[u8],
        now: Instant,
    ) -> Result<SessionKeys, PairingError> {
        self.ensure_active(now)?;
        if payload.is_empty() || payload.len() > MAX_OPAQUE_PAYLOAD_BYTES {
            self.pending_login = None;
            return Err(PairingError::InvalidMessage);
        }
        let state = self
            .pending_login
            .take()
            .ok_or(PairingError::NoLoginInProgress)?;
        let finalization = CredentialFinalization::<RemotePhoneCipherSuite>::deserialize(payload)
            .map_err(|_| PairingError::AuthenticationFailed)?;
        let result = state
            .finish(finalization, self.server_login_parameters())
            .map_err(|_| PairingError::AuthenticationFailed)?;
        let keys = SessionKeys::from_opaque_key(
            result.session_key.as_slice(),
            &self.session_id,
            EndpointRole::Phone,
        )?;

        self.consumed = true;
        self.clear_secrets();
        Ok(keys)
    }

    /// Cancels this pairing session and clears transient credentials.
    pub fn cancel(&mut self) {
        self.consumed = true;
        self.clear_secrets();
    }

    /// Drops a partially completed OPAQUE exchange when its transport closes.
    pub(crate) fn abandon_pending_login(&mut self) {
        self.pending_login = None;
    }

    fn ensure_active(&mut self, now: Instant) -> Result<(), PairingError> {
        if self.consumed {
            return Err(PairingError::SessionExpired);
        }
        if self.locked_out {
            return Err(PairingError::AttemptsExhausted);
        }
        if now.saturating_duration_since(self.started_at) >= PIN_LIFETIME {
            self.consumed = true;
            self.clear_secrets();
            return Err(PairingError::SessionExpired);
        }
        Ok(())
    }

    fn clear_secrets(&mut self) {
        self.pending_login = None;
        self.pin.0.zeroize();
        self.password_file.zeroize();
        self.server_setup.take();
    }

    fn server_login_parameters(&self) -> ServerLoginParameters<'_, '_> {
        ServerLoginParameters {
            context: Some(&self.context),
            identifiers: identifiers(),
        }
    }
}

/// Desktop-side OPAQUE client state. PIN is cleared best-effort after LOGIN2.
pub struct PairingClient {
    session_id: [u8; 16],
    context: Vec<u8>,
    pin: Zeroizing<String>,
    state: Option<ClientLogin<RemotePhoneCipherSuite>>,
}

impl PairingClient {
    /// Starts a desktop login and returns the serialized LOGIN1 request.
    pub fn start(pin: &str, session_id: [u8; 16]) -> Result<(Self, Vec<u8>), PairingError> {
        let mut rng = OsRng;
        Self::start_with_rng(pin, session_id, &mut rng)
    }

    fn start_with_rng<R>(
        pin: &str,
        session_id: [u8; 16],
        rng: &mut R,
    ) -> Result<(Self, Vec<u8>), PairingError>
    where
        R: RngCore + CryptoRng,
    {
        validate_pin(pin)?;
        let context = login_context(&session_id);
        let pin = Zeroizing::new(pin.to_owned());
        let result = ClientLogin::<RemotePhoneCipherSuite>::start(rng, pin.as_bytes())
            .map_err(|_| PairingError::InvalidPin)?;
        let payload = result.message.serialize().to_vec();

        Ok((
            Self {
                session_id,
                context,
                pin,
                state: Some(result.state),
            },
            payload,
        ))
    }

    /// Finishes the client login, returning LOGIN3 and the native-only keys.
    pub fn finish(
        &mut self,
        response_payload: &[u8],
    ) -> Result<(Vec<u8>, SessionKeys), PairingError> {
        let mut rng = OsRng;
        self.finish_with_rng(response_payload, &mut rng)
    }

    fn finish_with_rng<R>(
        &mut self,
        response_payload: &[u8],
        rng: &mut R,
    ) -> Result<(Vec<u8>, SessionKeys), PairingError>
    where
        R: RngCore + CryptoRng,
    {
        if response_payload.is_empty() || response_payload.len() > MAX_OPAQUE_PAYLOAD_BYTES {
            self.state = None;
            self.pin.zeroize();
            return Err(PairingError::InvalidMessage);
        }
        let Some(state) = self.state.take() else {
            self.pin.zeroize();
            return Err(PairingError::NoLoginInProgress);
        };

        let result = (|| {
            let response = CredentialResponse::<RemotePhoneCipherSuite>::deserialize(response_payload)
                .map_err(|_| PairingError::InvalidMessage)?;
            let ksf = argon2id_ksf();
            let params = ClientLoginFinishParameters {
                context: Some(&self.context),
                identifiers: identifiers(),
                ksf: Some(&ksf),
            };
            let result = state
                .finish(rng, self.pin.as_bytes(), response, params)
                .map_err(|_| PairingError::AuthenticationFailed)?;
            let payload = result.message.serialize().to_vec();
            let keys = SessionKeys::from_opaque_key(
                result.session_key.as_slice(),
                &self.session_id,
                EndpointRole::Pc,
            )?;
            Ok((payload, keys))
        })();
        self.pin.zeroize();
        result
    }
}

/// Direction-specific keys and OPAQUE confirmation key. Secret bytes are not
/// exposed through the public API and are zeroized on drop.
pub struct SessionKeys {
    confirm: Zeroizing<[u8; 64]>,
    pc_to_phone: Zeroizing<[u8; 32]>,
    phone_to_pc: Zeroizing<[u8; 32]>,
    endpoint: EndpointRole,
}

impl SessionKeys {
    fn from_opaque_key(
        opaque_key: &[u8],
        session_id: &[u8; 16],
        endpoint: EndpointRole,
    ) -> Result<Self, PairingError> {
        let hkdf = Hkdf::<Sha512>::new(Some(session_id), opaque_key);
        let mut confirm = Zeroizing::new([0_u8; 64]);
        let mut pc_to_phone = Zeroizing::new([0_u8; 32]);
        let mut phone_to_pc = Zeroizing::new([0_u8; 32]);
        hkdf.expand(CONFIRM_INFO, confirm.as_mut())
            .map_err(|_| PairingError::KeyDerivationFailed)?;
        hkdf.expand(PC_TO_PHONE_INFO, pc_to_phone.as_mut())
            .map_err(|_| PairingError::KeyDerivationFailed)?;
        hkdf.expand(PHONE_TO_PC_INFO, phone_to_pc.as_mut())
            .map_err(|_| PairingError::KeyDerivationFailed)?;
        Ok(Self {
            confirm,
            pc_to_phone,
            phone_to_pc,
            endpoint,
        })
    }

    /// Produces AUTH_OK for the exact handshake transcript hash.
    pub fn server_confirmation(
        &self,
        session_id: &[u8; 16],
        transcript_hash: &[u8; 64],
    ) -> [u8; 64] {
        hmac_sha512(
            self.confirm.as_ref(),
            &[SERVER_FINISHED_LABEL, session_id, transcript_hash],
        )
    }

    /// Verifies AUTH_OK in constant time.
    pub fn verify_server_confirmation(
        &self,
        session_id: &[u8; 16],
        transcript_hash: &[u8; 64],
        tag: &[u8],
    ) -> Result<(), PairingError> {
        let expected = self.server_confirmation(session_id, transcript_hash);
        let valid: bool = expected.as_slice().ct_eq(tag).into();
        if valid {
            Ok(())
        } else {
            Err(PairingError::ConfirmationFailed)
        }
    }

    /// Produces AUTH_ACK, bound to the previously verified AUTH_OK tag.
    pub fn client_confirmation(
        &self,
        session_id: &[u8; 16],
        transcript_hash: &[u8; 64],
        server_tag: &[u8; 64],
    ) -> [u8; 64] {
        hmac_sha512(
            self.confirm.as_ref(),
            &[
                CLIENT_FINISHED_LABEL,
                session_id,
                transcript_hash,
                server_tag,
            ],
        )
    }

    /// Verifies AUTH_ACK in constant time.
    pub fn verify_client_confirmation(
        &self,
        session_id: &[u8; 16],
        transcript_hash: &[u8; 64],
        server_tag: &[u8; 64],
        client_tag: &[u8],
    ) -> Result<(), PairingError> {
        let expected = self.client_confirmation(session_id, transcript_hash, server_tag);
        let valid: bool = expected.as_slice().ct_eq(client_tag).into();
        if valid {
            Ok(())
        } else {
            Err(PairingError::ConfirmationFailed)
        }
    }

    pub(crate) fn signal_key_pc_to_phone(&self) -> &[u8; 32] {
        &*self.pc_to_phone
    }

    pub(crate) fn signal_key_phone_to_pc(&self) -> &[u8; 32] {
        &*self.phone_to_pc
    }
}

impl fmt::Debug for SessionKeys {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("SessionKeys([REDACTED])")
    }
}

fn identifiers() -> Identifiers<'static> {
    Identifiers {
        client: Some(CLIENT_ID),
        server: Some(SERVER_ID),
    }
}

fn login_context(session_id: &[u8; 16]) -> Vec<u8> {
    let mut context = Vec::with_capacity(PIN_PREFIX.len() + session_id.len());
    context.extend_from_slice(PIN_PREFIX);
    context.extend_from_slice(session_id);
    context
}

fn user_identifier(session_id: &[u8; 16]) -> Vec<u8> {
    let mut identifier = Vec::with_capacity(USER_ID_PREFIX.len() + session_id.len());
    identifier.extend_from_slice(USER_ID_PREFIX);
    identifier.extend_from_slice(session_id);
    identifier
}

fn argon2id_ksf() -> Argon2<'static> {
    let params = Params::new(
        ARGON2_MEMORY_COST_KIB,
        ARGON2_TIME_COST,
        ARGON2_LANES,
        None,
    )
    .expect("the fixed Argon2id profile must always be valid");
    Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
}

fn register_locally<R>(
    server_setup: &ServerSetup<RemotePhoneCipherSuite>,
    user_id: &[u8],
    pin: &[u8],
    rng: &mut R,
) -> Result<Vec<u8>, PairingError>
where
    R: RngCore + CryptoRng,
{
    let client_start = ClientRegistration::<RemotePhoneCipherSuite>::start(rng, pin)
        .map_err(|_| PairingError::AuthenticationFailed)?;
    let server_start = ServerRegistration::<RemotePhoneCipherSuite>::start(
        server_setup,
        client_start.message,
        user_id,
    )
    .map_err(|_| PairingError::AuthenticationFailed)?;
    let ksf = argon2id_ksf();
    let client_finish = client_start
        .state
        .finish(
            rng,
            pin,
            server_start.message,
            ClientRegistrationFinishParameters {
                identifiers: identifiers(),
                ksf: Some(&ksf),
            },
        )
        .map_err(|_| PairingError::AuthenticationFailed)?;
    let record = ServerRegistration::<RemotePhoneCipherSuite>::finish(client_finish.message);
    Ok(record.serialize().to_vec())
}

fn generate_pin<R>(rng: &mut R) -> Result<PairingPin, PairingError>
where
    R: RngCore + CryptoRng,
{
    // Rejection sampling avoids modulo bias over the 100,000,000 valid codes.
    let limit = (1_u64 << 32) / PIN_SPACE * PIN_SPACE;
    let value = loop {
        let mut sample_bytes = [0_u8; 4];
        rng.try_fill_bytes(&mut sample_bytes)
            .map_err(|_| PairingError::EntropyUnavailable)?;
        let sample = u32::from_le_bytes(sample_bytes) as u64;
        if sample < limit {
            break sample % PIN_SPACE;
        }
    };
    Ok(PairingPin(Zeroizing::new(format!("{value:08}"))))
}

fn validate_pin(pin: &str) -> Result<(), PairingError> {
    if pin.len() == PIN_LENGTH && pin.bytes().all(|byte| byte.is_ascii_digit()) {
        Ok(())
    } else {
        Err(PairingError::InvalidPin)
    }
}

fn hmac_sha512(key: &[u8], parts: &[&[u8]]) -> [u8; 64] {
    let mut mac = <Hmac<Sha512> as Mac>::new_from_slice(key)
        .expect("HMAC accepts keys of any length");
    for part in parts {
        mac.update(part);
    }
    let result = mac.finalize().into_bytes();
    let mut tag = [0_u8; 64];
    tag.copy_from_slice(&result);
    tag
}

#[cfg(test)]
mod tests {
    use super::*;
    use rand::SeedableRng;
    use rand_chacha::ChaCha20Rng;

    fn seeded(seed: u8) -> ChaCha20Rng {
        ChaCha20Rng::from_seed([seed; 32])
    }

    fn server_and_pin() -> (PairingServer, String) {
        let now = Instant::now();
        let mut rng = seeded(7);
        let server = PairingServer::create_with_rng(&mut rng, now).unwrap();
        let pin = server.pin().expose_for_display().to_owned();
        (server, pin)
    }

    #[test]
    fn valid_pin_completes_opaque_and_protects_the_pin() {
        let (mut server, pin) = server_and_pin();
        assert_eq!(pin.len(), PIN_LENGTH);
        assert!(pin.bytes().all(|digit| digit.is_ascii_digit()));
        assert_eq!(format!("{:?}", server.pin()), "PairingPin([REDACTED])");
        assert!(!format!("{:?}", server.pin()).contains(&pin));

        let session_id = *server.session_id();
        let mut client_rng = seeded(23);
        let (mut client, login1) =
            PairingClient::start_with_rng(&pin, session_id, &mut client_rng).unwrap();
        let now = server.started_at;
        let login2 = server.begin_login_at(&login1, now).unwrap();
        let (login3, client_keys) = client.finish_with_rng(&login2, &mut client_rng).unwrap();
        let server_keys = server.finish_login_at(&login3, now).unwrap();

        let pin_bytes = pin.as_bytes();
        for payload in [&login1, &login2, &login3] {
            assert!(!payload
                .windows(pin_bytes.len())
                .any(|window| window == pin_bytes));
        }

        let transcript_hash = [0x5a; 64];
        let server_tag = server_keys.server_confirmation(&session_id, &transcript_hash);
        client_keys
            .verify_server_confirmation(&session_id, &transcript_hash, &server_tag)
            .unwrap();
        let client_tag = client_keys.client_confirmation(&session_id, &transcript_hash, &server_tag);
        server_keys
            .verify_client_confirmation(
                &session_id,
                &transcript_hash,
                &server_tag,
                &client_tag,
            )
            .unwrap();
        assert_eq!(
            client_keys.signal_key_pc_to_phone(),
            server_keys.signal_key_pc_to_phone()
        );
        assert_eq!(
            client_keys.signal_key_phone_to_pc(),
            server_keys.signal_key_phone_to_pc()
        );
        assert!(server.finish_login_at(&login3, now).is_err());
    }

    #[test]
    fn wrong_pin_fails_and_expiry_is_enforced() {
        let (mut server, pin) = server_and_pin();
        let session_id = *server.session_id();
        let mut wrong_pin = pin.into_bytes();
        let last = wrong_pin.len() - 1;
        wrong_pin[last] = if wrong_pin[last] == b'9' {
            b'0'
        } else {
            wrong_pin[last] + 1
        };
        let wrong_pin = String::from_utf8(wrong_pin).unwrap();
        let mut client_rng = seeded(41);
        let (mut client, login1) =
            PairingClient::start_with_rng(&wrong_pin, session_id, &mut client_rng).unwrap();
        let now = server.started_at;
        let login2 = server.begin_login_at(&login1, now).unwrap();
        assert!(matches!(
            client.finish_with_rng(&login2, &mut client_rng),
            Err(PairingError::AuthenticationFailed)
        ));
        assert_eq!(server.attempts_used(), 1);
        assert!(!server.consumed);

        assert_eq!(
            server.begin_login_at(&login1, now + PIN_LIFETIME),
            Err(PairingError::SessionExpired)
        );
        assert_eq!(server.attempts_used(), 1);
        assert!(server.pin().expose_for_display().is_empty());
        assert!(server.password_file.is_empty());
    }

    #[test]
    fn malformed_requests_do_not_spend_attempts_and_sixth_login_is_blocked() {
        let (mut server, pin) = server_and_pin();
        let session_id = *server.session_id();
        let mut client_rng = seeded(53);
        let (_client, login1) =
            PairingClient::start_with_rng(&pin, session_id, &mut client_rng).unwrap();
        let now = server.started_at;

        assert_eq!(
            server.begin_login_at(&[0], now),
            Err(PairingError::InvalidMessage)
        );
        assert_eq!(server.attempts_used(), 0);
        for _ in 0..MAX_LOGIN_ATTEMPTS {
            server.begin_login_at(&login1, now).unwrap();
        }
        assert_eq!(server.attempts_used(), MAX_LOGIN_ATTEMPTS);
        assert_eq!(
            server.begin_login_at(&login1, now),
            Err(PairingError::AttemptsExhausted)
        );
        assert_eq!(
            server.begin_login_at(&login1, now),
            Err(PairingError::AttemptsExhausted)
        );
        assert!(server.pin().expose_for_display().is_empty());
        assert!(server.password_file.is_empty());
    }

    #[test]
    fn oversized_messages_are_rejected_before_opaque_processing() {
        let (mut server, pin) = server_and_pin();
        let now = server.started_at;
        let oversized_login = vec![0; MAX_OPAQUE_PAYLOAD_BYTES + 1];
        assert_eq!(
            server.begin_login_at(&oversized_login, now),
            Err(PairingError::InvalidMessage)
        );
        assert_eq!(server.attempts_used(), 0);

        let session_id = *server.session_id();
        let mut client_rng = seeded(67);
        let (mut client, _) =
            PairingClient::start_with_rng(&pin, session_id, &mut client_rng).unwrap();
        let oversized_response = vec![0; MAX_OPAQUE_PAYLOAD_BYTES + 1];
        assert!(matches!(
            client.finish_with_rng(&oversized_response, &mut client_rng),
            Err(PairingError::InvalidMessage)
        ));
        assert!(client.state.is_none());
        assert!(client.pin.is_empty());
    }

    #[test]
    fn production_argon2id_profile_cost_is_measured_for_ci() {
        let profile = argon2id_ksf();
        let input = [0_u8; OPAQUE_HASH_LEN];
        let salt = [0_u8; ARGON2_SALT_LEN];
        let mut output = [0_u8; OPAQUE_HASH_LEN];
        let mut samples = Vec::with_capacity(3);

        for _ in 0..3 {
            let started = std::time::Instant::now();
            profile
                .hash_password_into(&input, &salt, &mut output)
                .expect("the production Argon2id profile must process a valid input");
            samples.push(started.elapsed());
        }
        assert!(output.iter().any(|byte| *byte != 0));
        samples.sort_unstable();

        eprintln!(
            "Измерение профиля Argon2id v0x13, 64 МиБ, t={}, p={}, соль {} байт, вход/выход {} байт: минимум {:.2} мс, медиана {:.2} мс, максимум {:.2} мс",
            ARGON2_TIME_COST,
            ARGON2_LANES,
            ARGON2_SALT_LEN,
            OPAQUE_HASH_LEN,
            samples[0].as_secs_f64() * 1_000.0,
            samples[1].as_secs_f64() * 1_000.0,
            samples[2].as_secs_f64() * 1_000.0,
        );
    }

    #[test]
    fn pin_validation_accepts_only_eight_ascii_digits() {
        assert!(validate_pin("01234567").is_ok());
        for invalid in ["1234567", "123456789", "１２３４５６７８", "12 45678", "abcdefgh"] {
            assert_eq!(validate_pin(invalid), Err(PairingError::InvalidPin));
        }
    }
}
