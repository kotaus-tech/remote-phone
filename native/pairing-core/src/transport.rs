use chacha20poly1305::aead::{Aead, Payload};
use chacha20poly1305::{ChaCha20Poly1305, KeyInit, Nonce};
use sha2::{Digest, Sha512};
use thiserror::Error;
use zeroize::Zeroizing;

use crate::{EndpointRole, PairingError, SessionKeys};

pub const FRAME_HEADER_LEN: usize = 29;
pub const MAX_FRAME_BYTES: usize = 256 * 1024;
pub const MAX_UNAUTHENTICATED_PAYLOAD_BYTES: usize = 16 * 1024;
pub const HELLO_PAYLOAD_LEN: usize = 5;
pub const AUTH_TAG_LEN: usize = 64;
pub const AEAD_TAG_LEN: usize = 16;
pub const PROTOCOL_VERSION: u8 = 1;
pub const PROFILE_ID: u16 = 0x0001;
pub const MAX_SESSION_LIFETIME_SECONDS: u16 = 300;

const FRAME_MAGIC: &[u8; 4] = b"RVP1";
const FIRST_SIGNAL_SEQUENCE: u32 = 3;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum TransportError {
    #[error("Кадр повреждён, имеет неверный размер или неизвестный тип.")]
    MalformedFrame,
    #[error("Версия протокола или криптографический профиль не поддерживается.")]
    UnsupportedProfile,
    #[error("Время действия сеанса задано неверно.")]
    InvalidLifetime,
    #[error("Размер сообщения превышает допустимый предел.")]
    PayloadTooLarge,
    #[error("Нарушен порядок сообщений или последовательность кадров.")]
    UnexpectedSequence,
    #[error("Кадр относится к другому сеансу.")]
    SessionMismatch,
    #[error("Проверка подлинности сообщения не пройдена.")]
    AuthenticationFailed,
    #[error("Роль отправителя не соответствует этапу рукопожатия.")]
    WrongEndpointRole,
    #[error("Рукопожатие ещё не завершено.")]
    HandshakeIncomplete,
    #[error("Сеанс закрыт после ошибки защиты транспорта.")]
    SessionClosed,
    #[error("Счётчик кадров исчерпан; повторное использование запрещено.")]
    SequenceExhausted,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum MessageType {
    Hello = 0x01,
    OpaqueLogin1 = 0x10,
    OpaqueLogin2 = 0x11,
    OpaqueLogin3 = 0x12,
    AuthOk = 0x13,
    AuthAck = 0x14,
    Signal = 0x20,
}

impl TryFrom<u8> for MessageType {
    type Error = TransportError;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0x01 => Ok(Self::Hello),
            0x10 => Ok(Self::OpaqueLogin1),
            0x11 => Ok(Self::OpaqueLogin2),
            0x12 => Ok(Self::OpaqueLogin3),
            0x13 => Ok(Self::AuthOk),
            0x14 => Ok(Self::AuthAck),
            0x20 => Ok(Self::Signal),
            _ => Err(TransportError::MalformedFrame),
        }
    }
}

/// A validated protocol frame. The binary representation has a fixed 29-byte
/// header followed by exactly `payload_length` bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Frame {
    message_type: MessageType,
    session_id: [u8; 16],
    sequence: u32,
    payload: Vec<u8>,
}

impl Frame {
    pub fn new(
        message_type: MessageType,
        session_id: [u8; 16],
        sequence: u32,
        payload: Vec<u8>,
    ) -> Result<Self, TransportError> {
        let frame = Self {
            message_type,
            session_id,
            sequence,
            payload,
        };
        frame.validate()?;
        Ok(frame)
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, TransportError> {
        if bytes.len() < FRAME_HEADER_LEN || bytes.len() > MAX_FRAME_BYTES {
            return Err(TransportError::MalformedFrame);
        }
        if &bytes[..4] != FRAME_MAGIC {
            return Err(TransportError::MalformedFrame);
        }

        let message_type = MessageType::try_from(bytes[4])?;
        let mut session_id = [0_u8; 16];
        session_id.copy_from_slice(&bytes[5..21]);
        let sequence = u32::from_be_bytes(
            bytes[21..25]
                .try_into()
                .map_err(|_| TransportError::MalformedFrame)?,
        );
        let payload_length = u32::from_be_bytes(
            bytes[25..29]
                .try_into()
                .map_err(|_| TransportError::MalformedFrame)?,
        ) as usize;
        let expected_length = FRAME_HEADER_LEN
            .checked_add(payload_length)
            .ok_or(TransportError::MalformedFrame)?;
        if expected_length != bytes.len() {
            return Err(TransportError::MalformedFrame);
        }

        let frame = Self {
            message_type,
            session_id,
            sequence,
            payload: bytes[FRAME_HEADER_LEN..].to_vec(),
        };
        frame.validate()?;
        Ok(frame)
    }

    pub fn encode(&self) -> Result<Vec<u8>, TransportError> {
        self.validate()?;
        let header = encode_header(
            self.message_type,
            &self.session_id,
            self.sequence,
            self.payload.len(),
        )?;
        let mut bytes = Vec::with_capacity(FRAME_HEADER_LEN + self.payload.len());
        bytes.extend_from_slice(&header);
        bytes.extend_from_slice(&self.payload);
        Ok(bytes)
    }

    pub fn message_type(&self) -> MessageType {
        self.message_type
    }

    pub fn session_id(&self) -> &[u8; 16] {
        &self.session_id
    }

    pub fn sequence(&self) -> u32 {
        self.sequence
    }

    pub fn payload(&self) -> &[u8] {
        &self.payload
    }

    fn validate(&self) -> Result<(), TransportError> {
        let frame_length = FRAME_HEADER_LEN
            .checked_add(self.payload.len())
            .ok_or(TransportError::PayloadTooLarge)?;
        if frame_length > MAX_FRAME_BYTES || self.payload.len() > u32::MAX as usize {
            return Err(TransportError::PayloadTooLarge);
        }
        if self.message_type != MessageType::Signal
            && self.payload.len() > MAX_UNAUTHENTICATED_PAYLOAD_BYTES
        {
            return Err(TransportError::PayloadTooLarge);
        }

        let expected_sequence = match self.message_type {
            MessageType::Hello | MessageType::OpaqueLogin1 => 0,
            MessageType::OpaqueLogin2 | MessageType::OpaqueLogin3 => 1,
            MessageType::AuthOk | MessageType::AuthAck => 2,
            MessageType::Signal if self.sequence >= FIRST_SIGNAL_SEQUENCE => self.sequence,
            MessageType::Signal => return Err(TransportError::UnexpectedSequence),
        };
        if self.message_type != MessageType::Signal && self.sequence != expected_sequence {
            return Err(TransportError::UnexpectedSequence);
        }

        match self.message_type {
            MessageType::Hello => validate_hello_payload(&self.payload),
            MessageType::OpaqueLogin1 | MessageType::OpaqueLogin2 | MessageType::OpaqueLogin3 => {
                if self.payload.is_empty() {
                    Err(TransportError::MalformedFrame)
                } else {
                    Ok(())
                }
            }
            MessageType::AuthOk | MessageType::AuthAck => {
                if self.payload.len() == AUTH_TAG_LEN {
                    Ok(())
                } else {
                    Err(TransportError::MalformedFrame)
                }
            }
            MessageType::Signal => {
                if self.payload.len() >= AEAD_TAG_LEN {
                    Ok(())
                } else {
                    Err(TransportError::MalformedFrame)
                }
            }
        }
    }
}

/// The fixed HELLO payload carries a version, ciphersuite profile and lifetime.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Hello {
    pub session_id: [u8; 16],
    pub lifetime_seconds: u16,
}

impl Hello {
    pub fn new(
        session_id: [u8; 16],
        lifetime_seconds: u16,
    ) -> Result<Self, TransportError> {
        if lifetime_seconds == 0 || lifetime_seconds > MAX_SESSION_LIFETIME_SECONDS {
            return Err(TransportError::InvalidLifetime);
        }
        Ok(Self {
            session_id,
            lifetime_seconds,
        })
    }

    pub fn to_frame(self) -> Result<Frame, TransportError> {
        let mut payload = Vec::with_capacity(HELLO_PAYLOAD_LEN);
        payload.push(PROTOCOL_VERSION);
        payload.extend_from_slice(&PROFILE_ID.to_be_bytes());
        payload.extend_from_slice(&self.lifetime_seconds.to_be_bytes());
        Frame::new(MessageType::Hello, self.session_id, 0, payload)
    }

    pub fn from_frame(frame: &Frame) -> Result<Self, TransportError> {
        if frame.message_type != MessageType::Hello || frame.sequence != 0 {
            return Err(TransportError::UnexpectedSequence);
        }
        validate_hello_payload(&frame.payload)?;
        let lifetime_seconds = u16::from_be_bytes([frame.payload[3], frame.payload[4]]);
        Ok(Self {
            session_id: frame.session_id,
            lifetime_seconds,
        })
    }
}

fn validate_hello_payload(payload: &[u8]) -> Result<(), TransportError> {
    if payload.len() != HELLO_PAYLOAD_LEN {
        return Err(TransportError::MalformedFrame);
    }
    if payload[0] != PROTOCOL_VERSION
        || u16::from_be_bytes([payload[1], payload[2]]) != PROFILE_ID
    {
        return Err(TransportError::UnsupportedProfile);
    }
    let lifetime = u16::from_be_bytes([payload[3], payload[4]]);
    if lifetime == 0 || lifetime > MAX_SESSION_LIFETIME_SECONDS {
        return Err(TransportError::InvalidLifetime);
    }
    Ok(())
}

fn encode_header(
    message_type: MessageType,
    session_id: &[u8; 16],
    sequence: u32,
    payload_length: usize,
) -> Result<[u8; FRAME_HEADER_LEN], TransportError> {
    if payload_length > u32::MAX as usize {
        return Err(TransportError::PayloadTooLarge);
    }
    let mut header = [0_u8; FRAME_HEADER_LEN];
    header[..4].copy_from_slice(FRAME_MAGIC);
    header[4] = message_type as u8;
    header[5..21].copy_from_slice(session_id);
    header[21..25].copy_from_slice(&sequence.to_be_bytes());
    header[25..29].copy_from_slice(&(payload_length as u32).to_be_bytes());
    Ok(header)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrameSender {
    Phone,
    Pc,
}

/// Hashes the exact four binary handshake frames, including their headers.
/// Any out-of-order frame poisons this transcript and requires a new session.
pub struct HandshakeTranscript {
    session_id: [u8; 16],
    accepted_frames: u8,
    hasher: Sha512,
    failed: bool,
}

impl HandshakeTranscript {
    pub fn new(session_id: [u8; 16]) -> Self {
        Self {
            session_id,
            accepted_frames: 0,
            hasher: Sha512::new(),
            failed: false,
        }
    }

    pub fn accept(
        &mut self,
        sender: FrameSender,
        frame: &Frame,
    ) -> Result<(), TransportError> {
        if self.failed {
            return Err(TransportError::SessionClosed);
        }
        let result = self.accept_inner(sender, frame);
        if result.is_err() {
            self.failed = true;
        }
        result
    }

    pub fn transcript_hash(&self) -> Result<[u8; 64], TransportError> {
        if self.failed {
            return Err(TransportError::SessionClosed);
        }
        if self.accepted_frames != 4 {
            return Err(TransportError::HandshakeIncomplete);
        }
        let digest = self.hasher.clone().finalize();
        let mut hash = [0_u8; 64];
        hash.copy_from_slice(&digest);
        Ok(hash)
    }

    fn accept_inner(
        &mut self,
        sender: FrameSender,
        frame: &Frame,
    ) -> Result<(), TransportError> {
        if frame.session_id != self.session_id {
            return Err(TransportError::SessionMismatch);
        }
        let (expected_sender, expected_type, expected_sequence) = match self.accepted_frames {
            0 => (FrameSender::Phone, MessageType::Hello, 0),
            1 => (FrameSender::Pc, MessageType::OpaqueLogin1, 0),
            2 => (FrameSender::Phone, MessageType::OpaqueLogin2, 1),
            3 => (FrameSender::Pc, MessageType::OpaqueLogin3, 1),
            _ => return Err(TransportError::UnexpectedSequence),
        };
        if sender != expected_sender
            || frame.message_type != expected_type
            || frame.sequence != expected_sequence
        {
            return Err(TransportError::UnexpectedSequence);
        }
        let encoded = frame.encode()?;
        self.hasher.update(encoded);
        self.accepted_frames += 1;
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum LocalEndpoint {
    Phone,
    Pc,
}

/// A session token is created only after the peer's AUTH confirmation is
/// validated. SignalCipher can be obtained only from this token.
pub struct AuthenticatedSession {
    session_id: [u8; 16],
    keys: SessionKeys,
    local_endpoint: LocalEndpoint,
}

impl AuthenticatedSession {
    pub fn into_signal_cipher(self) -> SignalCipher {
        let AuthenticatedSession {
            session_id,
            keys,
            local_endpoint,
        } = self;
        let (send_key, receive_key) = match local_endpoint {
            LocalEndpoint::Pc => (keys.pc_to_phone, keys.phone_to_pc),
            LocalEndpoint::Phone => (keys.phone_to_pc, keys.pc_to_phone),
        };
        SignalCipher {
            session_id,
            send_key,
            receive_key,
            next_send_sequence: Some(FIRST_SIGNAL_SEQUENCE),
            next_receive_sequence: Some(FIRST_SIGNAL_SEQUENCE),
            failed: false,
        }
    }
}

impl SessionKeys {
    /// Builds the phone's AUTH_OK frame for the exact completed transcript.
    pub fn create_server_auth_ok(
        &self,
        session_id: [u8; 16],
        transcript_hash: &[u8; 64],
    ) -> Result<Frame, TransportError> {
        if self.endpoint != EndpointRole::Phone {
            return Err(TransportError::WrongEndpointRole);
        }
        Frame::new(
            MessageType::AuthOk,
            session_id,
            2,
            self.server_confirmation(&session_id, transcript_hash).to_vec(),
        )
    }

    /// The PC verifies AUTH_OK, produces AUTH_ACK and receives an authenticated
    /// session token. Callers must send the ACK before starting signal traffic.
    pub fn accept_server_auth_ok(
        self,
        frame: &Frame,
        session_id: [u8; 16],
        transcript_hash: &[u8; 64],
    ) -> Result<(Frame, AuthenticatedSession), TransportError> {
        if self.endpoint != EndpointRole::Pc {
            return Err(TransportError::WrongEndpointRole);
        }
        validate_auth_frame(frame, MessageType::AuthOk, &session_id)?;
        self.verify_server_confirmation(&session_id, transcript_hash, &frame.payload)
            .map_err(|_| TransportError::AuthenticationFailed)?;
        let server_tag: [u8; 64] = frame
            .payload
            .as_slice()
            .try_into()
            .map_err(|_| TransportError::AuthenticationFailed)?;
        let ack = Frame::new(
            MessageType::AuthAck,
            session_id,
            2,
            self.client_confirmation(&session_id, transcript_hash, &server_tag)
                .to_vec(),
        )?;
        Ok((
            ack,
            AuthenticatedSession {
                session_id,
                keys: self,
                local_endpoint: LocalEndpoint::Pc,
            },
        ))
    }

    /// The phone accepts only an AUTH_ACK bound to its AUTH_OK and completed
    /// transcript, then yields the authenticated session token.
    pub fn accept_client_auth_ack(
        self,
        frame: &Frame,
        session_id: [u8; 16],
        transcript_hash: &[u8; 64],
    ) -> Result<AuthenticatedSession, TransportError> {
        if self.endpoint != EndpointRole::Phone {
            return Err(TransportError::WrongEndpointRole);
        }
        validate_auth_frame(frame, MessageType::AuthAck, &session_id)?;
        let server_tag = self.server_confirmation(&session_id, transcript_hash);
        self.verify_client_confirmation(
            &session_id,
            transcript_hash,
            &server_tag,
            &frame.payload,
        )
        .map_err(|_| TransportError::AuthenticationFailed)?;
        Ok(AuthenticatedSession {
            session_id,
            keys: self,
            local_endpoint: LocalEndpoint::Phone,
        })
    }
}

fn validate_auth_frame(
    frame: &Frame,
    expected_type: MessageType,
    expected_session_id: &[u8; 16],
) -> Result<(), TransportError> {
    if frame.session_id != *expected_session_id {
        return Err(TransportError::SessionMismatch);
    }
    if frame.message_type != expected_type || frame.sequence != 2 {
        return Err(TransportError::UnexpectedSequence);
    }
    frame.encode().map(|_| ())
}

/// Stateful ChaCha20-Poly1305 transport for authenticated signaling frames.
/// Any malformed, replayed, reordered or unauthenticated inbound frame closes
/// the instance so a caller cannot accidentally continue on a poisoned stream.
pub struct SignalCipher {
    session_id: [u8; 16],
    send_key: Zeroizing<[u8; 32]>,
    receive_key: Zeroizing<[u8; 32]>,
    next_send_sequence: Option<u32>,
    next_receive_sequence: Option<u32>,
    failed: bool,
}

impl SignalCipher {
    pub fn session_id(&self) -> &[u8; 16] {
        &self.session_id
    }

    pub fn encrypt(&mut self, plaintext: &[u8]) -> Result<Vec<u8>, TransportError> {
        if self.failed {
            return Err(TransportError::SessionClosed);
        }
        let sequence = match self.next_send_sequence {
            Some(sequence) => sequence,
            None => {
                self.failed = true;
                return Err(TransportError::SequenceExhausted);
            }
        };
        let payload_length = plaintext
            .len()
            .checked_add(AEAD_TAG_LEN)
            .ok_or(TransportError::PayloadTooLarge)?;
        if FRAME_HEADER_LEN + payload_length > MAX_FRAME_BYTES
            || payload_length > u32::MAX as usize
        {
            return Err(TransportError::PayloadTooLarge);
        }

        let header = encode_header(
            MessageType::Signal,
            &self.session_id,
            sequence,
            payload_length,
        )?;
        let nonce_bytes = signal_nonce(sequence);
        let cipher = ChaCha20Poly1305::new_from_slice(self.send_key.as_ref())
            .map_err(|_| TransportError::AuthenticationFailed)?;
        let ciphertext = cipher
            .encrypt(
                Nonce::from_slice(&nonce_bytes),
                Payload {
                    msg: plaintext,
                    aad: &header,
                },
            )
            .map_err(|_| TransportError::AuthenticationFailed)?;
        if ciphertext.len() != payload_length {
            self.failed = true;
            return Err(TransportError::AuthenticationFailed);
        }

        let mut frame = Vec::with_capacity(FRAME_HEADER_LEN + ciphertext.len());
        frame.extend_from_slice(&header);
        frame.extend_from_slice(&ciphertext);
        self.next_send_sequence = sequence.checked_add(1);
        Ok(frame)
    }

    pub fn decrypt(&mut self, bytes: &[u8]) -> Result<Vec<u8>, TransportError> {
        if self.failed {
            return Err(TransportError::SessionClosed);
        }
        let result = self.decrypt_inner(bytes);
        if result.is_err() {
            self.failed = true;
        }
        result
    }

    fn decrypt_inner(&mut self, bytes: &[u8]) -> Result<Vec<u8>, TransportError> {
        let frame = Frame::decode(bytes)?;
        if frame.message_type != MessageType::Signal {
            return Err(TransportError::UnexpectedSequence);
        }
        if frame.session_id != self.session_id {
            return Err(TransportError::SessionMismatch);
        }
        let expected_sequence = self
            .next_receive_sequence
            .ok_or(TransportError::SequenceExhausted)?;
        if frame.sequence != expected_sequence {
            return Err(TransportError::UnexpectedSequence);
        }

        let nonce_bytes = signal_nonce(frame.sequence);
        let cipher = ChaCha20Poly1305::new_from_slice(self.receive_key.as_ref())
            .map_err(|_| TransportError::AuthenticationFailed)?;
        let plaintext = cipher
            .decrypt(
                Nonce::from_slice(&nonce_bytes),
                Payload {
                    msg: frame.payload(),
                    aad: &bytes[..FRAME_HEADER_LEN],
                },
            )
            .map_err(|_| TransportError::AuthenticationFailed)?;
        self.next_receive_sequence = expected_sequence.checked_add(1);
        Ok(plaintext)
    }
}

fn signal_nonce(sequence: u32) -> [u8; 12] {
    let mut nonce = [0_u8; 12];
    nonce[4..].copy_from_slice(&u64::from(sequence).to_be_bytes());
    nonce
}

#[cfg(test)]
mod tests {
    use super::*;

    fn session_keys(session_id: &[u8; 16], secret: u8, endpoint: EndpointRole) -> SessionKeys {
        SessionKeys::from_opaque_key(&[secret; 64], session_id, endpoint).unwrap()
    }

    fn authenticated_pair(
        session_id: [u8; 16],
    ) -> (AuthenticatedSession, AuthenticatedSession) {
        let transcript_hash = [0x5a; 64];
        let phone_keys = session_keys(&session_id, 0x41, EndpointRole::Phone);
        let pc_keys = session_keys(&session_id, 0x41, EndpointRole::Pc);
        let auth_ok = phone_keys
            .create_server_auth_ok(session_id, &transcript_hash)
            .unwrap();
        let (auth_ack, pc_session) = pc_keys
            .accept_server_auth_ok(&auth_ok, session_id, &transcript_hash)
            .unwrap();
        let phone_session = phone_keys
            .accept_client_auth_ack(&auth_ack, session_id, &transcript_hash)
            .unwrap();
        (pc_session, phone_session)
    }

    #[test]
    fn frame_roundtrip_uses_fixed_header_and_exact_hello_payload() {
        let session_id = [0x27; 16];
        let hello = Hello::new(session_id, 300).unwrap();
        let frame = hello.to_frame().unwrap();
        let encoded = frame.encode().unwrap();

        assert_eq!(encoded.len(), FRAME_HEADER_LEN + HELLO_PAYLOAD_LEN);
        assert_eq!(&encoded[..4], b"RVP1");
        assert_eq!(encoded[4], MessageType::Hello as u8);
        assert_eq!(&encoded[5..21], &session_id);
        assert_eq!(&encoded[21..25], &0_u32.to_be_bytes());
        assert_eq!(
            &encoded[25..29],
            &(HELLO_PAYLOAD_LEN as u32).to_be_bytes()
        );
        assert_eq!(&encoded[29..], &[1, 0, 1, 1, 44]);
        let decoded = Frame::decode(&encoded).unwrap();
        assert_eq!(decoded, frame);
        assert_eq!(Hello::from_frame(&decoded).unwrap(), hello);
    }

    #[test]
    fn frame_decoder_rejects_trailing_truncated_unknown_and_oversized_data() {
        let hello = Hello::new([3; 16], 300).unwrap().to_frame().unwrap();
        let encoded = hello.encode().unwrap();
        assert_eq!(
            Frame::decode(&encoded[..FRAME_HEADER_LEN - 1]),
            Err(TransportError::MalformedFrame)
        );
        let mut trailing = encoded.clone();
        trailing.push(0);
        assert_eq!(
            Frame::decode(&trailing),
            Err(TransportError::MalformedFrame)
        );
        let mut unknown = encoded.clone();
        unknown[4] = 0xff;
        assert_eq!(
            Frame::decode(&unknown),
            Err(TransportError::MalformedFrame)
        );
        let mut wrong_magic = encoded;
        wrong_magic[0] ^= 1;
        assert_eq!(
            Frame::decode(&wrong_magic),
            Err(TransportError::MalformedFrame)
        );

        assert_eq!(
            Frame::new(
                MessageType::OpaqueLogin1,
                [0; 16],
                0,
                vec![1; MAX_UNAUTHENTICATED_PAYLOAD_BYTES + 1],
            ),
            Err(TransportError::PayloadTooLarge)
        );
    }

    #[test]
    fn frame_decoder_is_panic_free_for_arbitrary_truncated_and_extreme_inputs() {
        use rand::{RngCore, SeedableRng};
        use rand_chacha::ChaCha20Rng;

        let mut rng = ChaCha20Rng::from_seed([0xa4; 32]);
        let valid_hello = Hello::new([0x39; 16], 300).unwrap().to_frame().unwrap();
        let encoded_hello = valid_hello.encode().unwrap();

        // Every strict prefix of a valid frame is rejected; the complete frame
        // decodes and re-encodes byte-for-byte.
        for length in 0..encoded_hello.len() {
            assert_eq!(
                Frame::decode(&encoded_hello[..length]),
                Err(TransportError::MalformedFrame)
            );
        }
        assert_eq!(
            Frame::decode(&encoded_hello).unwrap().encode().unwrap(),
            encoded_hello
        );

        // Unstructured random inputs and random headers with valid magic/types
        // exercise parser branches without invoking OPAQUE or Argon2.
        for _ in 0..2_048 {
            let length = rng.next_u32() as usize % 513;
            let mut bytes = vec![0; length];
            rng.fill_bytes(&mut bytes);
            if let Ok(frame) = Frame::decode(&bytes) {
                assert_eq!(frame.encode().unwrap(), bytes);
            }
        }

        let known_types = [
            MessageType::Hello as u8,
            MessageType::OpaqueLogin1 as u8,
            MessageType::OpaqueLogin2 as u8,
            MessageType::OpaqueLogin3 as u8,
            MessageType::AuthOk as u8,
            MessageType::AuthAck as u8,
            MessageType::Signal as u8,
            0xff,
        ];
        for index in 0..2_048 {
            let length = FRAME_HEADER_LEN + rng.next_u32() as usize % 257;
            let mut bytes = vec![0; length];
            rng.fill_bytes(&mut bytes);
            bytes[..4].copy_from_slice(b"RVP1");
            bytes[4] = known_types[index % known_types.len()];

            let sequence = match bytes[4] {
                0x01 | 0x10 => 0_u32,
                0x11 | 0x12 => 1,
                0x13 | 0x14 => 2,
                0x20 => 3,
                _ => rng.next_u32(),
            };
            bytes[21..25].copy_from_slice(&sequence.to_be_bytes());
            let declared_length = match index % 4 {
                0 => (length - FRAME_HEADER_LEN) as u32,
                1 => u32::MAX,
                2 => (length - FRAME_HEADER_LEN + 1) as u32,
                _ => rng.next_u32(),
            };
            bytes[25..29].copy_from_slice(&declared_length.to_be_bytes());

            if let Ok(frame) = Frame::decode(&bytes) {
                assert_eq!(frame.encode().unwrap(), bytes);
            }
        }

        // Oversized buffers are rejected before payload copying; a huge length
        // claim in a short header is rejected without attempting allocation.
        let mut oversized = vec![0; MAX_FRAME_BYTES + 1];
        oversized[..4].copy_from_slice(b"RVP1");
        assert_eq!(
            Frame::decode(&oversized),
            Err(TransportError::MalformedFrame)
        );
        let mut huge_length_claim = vec![0; FRAME_HEADER_LEN];
        huge_length_claim[..4].copy_from_slice(b"RVP1");
        huge_length_claim[4] = MessageType::OpaqueLogin1 as u8;
        huge_length_claim[21..25].copy_from_slice(&0_u32.to_be_bytes());
        huge_length_claim[25..29].copy_from_slice(&u32::MAX.to_be_bytes());
        assert_eq!(
            Frame::decode(&huge_length_claim),
            Err(TransportError::MalformedFrame)
        );

        let max_sized_signal = Frame::new(
            MessageType::Signal,
            [0x61; 16],
            FIRST_SIGNAL_SEQUENCE,
            vec![0; MAX_FRAME_BYTES - FRAME_HEADER_LEN],
        )
        .unwrap()
        .encode()
        .unwrap();
        assert_eq!(max_sized_signal.len(), MAX_FRAME_BYTES);
        assert_eq!(
            Frame::decode(&max_sized_signal).unwrap().encode().unwrap(),
            max_sized_signal
        );
    }

    #[test]
    fn hello_rejects_unknown_version_profile_and_invalid_lifetime() {
        assert_eq!(
            Hello::new([0; 16], 0),
            Err(TransportError::InvalidLifetime)
        );
        assert_eq!(
            Hello::new([0; 16], MAX_SESSION_LIFETIME_SECONDS + 1),
            Err(TransportError::InvalidLifetime)
        );

        let mut frame = Hello::new([0; 16], 300).unwrap().to_frame().unwrap();
        frame.payload[0] = PROTOCOL_VERSION + 1;
        assert_eq!(frame.encode(), Err(TransportError::UnsupportedProfile));
        frame.payload[0] = PROTOCOL_VERSION;
        frame.payload[2] = 2;
        assert_eq!(frame.encode(), Err(TransportError::UnsupportedProfile));
    }

    #[test]
    fn transcript_accepts_only_exact_order_and_hashes_encoded_frames() {
        let session_id = [8; 16];
        let hello = Hello::new(session_id, 300).unwrap().to_frame().unwrap();
        let login1 = Frame::new(MessageType::OpaqueLogin1, session_id, 0, vec![1, 2]).unwrap();
        let login2 = Frame::new(MessageType::OpaqueLogin2, session_id, 1, vec![3, 4]).unwrap();
        let login3 = Frame::new(MessageType::OpaqueLogin3, session_id, 1, vec![5, 6]).unwrap();
        let frames = [&hello, &login1, &login2, &login3];

        let mut transcript = HandshakeTranscript::new(session_id);
        assert_eq!(
            transcript.transcript_hash(),
            Err(TransportError::HandshakeIncomplete)
        );
        for (sender, frame) in [
            (FrameSender::Phone, &hello),
            (FrameSender::Pc, &login1),
            (FrameSender::Phone, &login2),
            (FrameSender::Pc, &login3),
        ] {
            transcript.accept(sender, frame).unwrap();
        }
        let expected = Sha512::digest(
            frames
                .iter()
                .flat_map(|frame| frame.encode().unwrap())
                .collect::<Vec<_>>(),
        );
        assert_eq!(transcript.transcript_hash().unwrap().as_slice(), expected.as_slice());

        let mut out_of_order = HandshakeTranscript::new(session_id);
        assert_eq!(
            out_of_order.accept(FrameSender::Pc, &hello),
            Err(TransportError::UnexpectedSequence)
        );
        assert_eq!(
            out_of_order.accept(FrameSender::Phone, &hello),
            Err(TransportError::SessionClosed)
        );

        let mut wrong_session = HandshakeTranscript::new(session_id);
        assert_eq!(
            wrong_session.accept(
                FrameSender::Phone,
                &Hello::new([9; 16], 300).unwrap().to_frame().unwrap()
            ),
            Err(TransportError::SessionMismatch)
        );
    }

    #[test]
    fn mutual_authentication_gates_signal_cipher_and_binds_ack_to_transcript() {
        let session_id = [4; 16];
        let transcript_hash = [0x22; 64];
        let phone_keys = session_keys(&session_id, 0x71, EndpointRole::Phone);
        let pc_keys = session_keys(&session_id, 0x71, EndpointRole::Pc);
        let auth_ok = phone_keys
            .create_server_auth_ok(session_id, &transcript_hash)
            .unwrap();
        let (auth_ack, pc_session) = pc_keys
            .accept_server_auth_ok(&auth_ok, session_id, &transcript_hash)
            .unwrap();
        assert_eq!(auth_ack.message_type(), MessageType::AuthAck);
        assert_eq!(auth_ack.sequence(), 2);
        let phone_session = phone_keys
            .accept_client_auth_ack(&auth_ack, session_id, &transcript_hash)
            .unwrap();
        let mut pc = pc_session.into_signal_cipher();
        let mut phone = phone_session.into_signal_cipher();

        let from_pc = pc.encrypt(b"offer and candidates").unwrap();
        assert_eq!(&from_pc[21..25], &3_u32.to_be_bytes());
        assert_eq!(phone.decrypt(&from_pc).unwrap(), b"offer and candidates");
        let from_phone = phone.encrypt(b"answer and candidates").unwrap();
        assert_eq!(&from_phone[21..25], &3_u32.to_be_bytes());
        assert_eq!(pc.decrypt(&from_phone).unwrap(), b"answer and candidates");

        let mut changed_transcript = [0x22; 64];
        changed_transcript[0] ^= 1;
        let phone_keys = session_keys(&session_id, 0x71, EndpointRole::Phone);
        let forged_auth_ok = phone_keys
            .create_server_auth_ok(session_id, &changed_transcript)
            .unwrap();
        let pc_keys = session_keys(&session_id, 0x71, EndpointRole::Pc);
        assert!(matches!(
            pc_keys.accept_server_auth_ok(&forged_auth_ok, session_id, &transcript_hash),
            Err(TransportError::AuthenticationFailed)
        ));

        let wrong_role_keys = session_keys(&session_id, 0x71, EndpointRole::Pc);
        assert_eq!(
            wrong_role_keys.create_server_auth_ok(session_id, &transcript_hash),
            Err(TransportError::WrongEndpointRole)
        );
    }

    #[test]
    fn complete_opaque_handshake_runs_over_the_encoded_protocol_frames() {
        use crate::{PairingClient, PairingServer};
        use rand::SeedableRng;
        use rand_chacha::ChaCha20Rng;
        use std::time::Instant;

        let mut phone_rng = ChaCha20Rng::from_seed([0x19; 32]);
        let mut pc_rng = ChaCha20Rng::from_seed([0x71; 32]);
        let mut phone =
            PairingServer::create_with_rng(&mut phone_rng, Instant::now()).unwrap();
        let pin = phone.pin().expose_for_display().to_owned();
        let session_id = *phone.session_id();
        let mut phone_transcript = HandshakeTranscript::new(session_id);
        let mut pc_transcript = HandshakeTranscript::new(session_id);

        let hello = Hello::new(session_id, MAX_SESSION_LIFETIME_SECONDS)
            .unwrap()
            .to_frame()
            .unwrap();
        phone_transcript.accept(FrameSender::Phone, &hello).unwrap();
        pc_transcript.accept(FrameSender::Phone, &Frame::decode(&hello.encode().unwrap()).unwrap()).unwrap();

        let (mut pc, login1_payload) =
            PairingClient::start_with_rng(&pin, session_id, &mut pc_rng).unwrap();
        let login1 = Frame::new(MessageType::OpaqueLogin1, session_id, 0, login1_payload).unwrap();
        phone_transcript.accept(FrameSender::Pc, &login1).unwrap();
        pc_transcript.accept(FrameSender::Pc, &Frame::decode(&login1.encode().unwrap()).unwrap()).unwrap();

        let login2_payload = phone.begin_login(login1.payload()).unwrap();
        let login2 = Frame::new(MessageType::OpaqueLogin2, session_id, 1, login2_payload).unwrap();
        phone_transcript.accept(FrameSender::Phone, &login2).unwrap();
        pc_transcript.accept(FrameSender::Phone, &Frame::decode(&login2.encode().unwrap()).unwrap()).unwrap();

        let (login3_payload, pc_keys) = pc.finish(login2.payload()).unwrap();
        let login3 = Frame::new(MessageType::OpaqueLogin3, session_id, 1, login3_payload).unwrap();
        phone_transcript.accept(FrameSender::Pc, &login3).unwrap();
        pc_transcript.accept(FrameSender::Pc, &Frame::decode(&login3.encode().unwrap()).unwrap()).unwrap();
        let phone_keys = phone.finish_login(login3.payload()).unwrap();
        let phone_hash = phone_transcript.transcript_hash().unwrap();
        let pc_hash = pc_transcript.transcript_hash().unwrap();
        assert_eq!(phone_hash, pc_hash);

        let auth_ok = phone_keys
            .create_server_auth_ok(session_id, &phone_hash)
            .unwrap();
        let (auth_ack, pc_session) = pc_keys
            .accept_server_auth_ok(&Frame::decode(&auth_ok.encode().unwrap()).unwrap(), session_id, &pc_hash)
            .unwrap();
        let phone_session = phone_keys
            .accept_client_auth_ack(&Frame::decode(&auth_ack.encode().unwrap()).unwrap(), session_id, &phone_hash)
            .unwrap();
        let mut pc_signal = pc_session.into_signal_cipher();
        let mut phone_signal = phone_session.into_signal_cipher();
        let signal = pc_signal.encrypt(b"offer from authenticated OPAQUE session").unwrap();
        assert_eq!(
            phone_signal.decrypt(&signal).unwrap(),
            b"offer from authenticated OPAQUE session"
        );
        assert!(!signal.windows(pin.len()).any(|window| window == pin.as_bytes()));
    }

    #[test]
    fn signal_cipher_rejects_tampering_replay_reordering_and_wrong_session() {
        let session_id = [5; 16];
        let (pc_session, phone_session) = authenticated_pair(session_id);
        let mut pc = pc_session.into_signal_cipher();
        let mut phone = phone_session.into_signal_cipher();
        let mut tampered = pc.encrypt(b"sdp offer").unwrap();
        let last = tampered.len() - 1;
        tampered[last] ^= 0x40;
        assert_eq!(
            phone.decrypt(&tampered),
            Err(TransportError::AuthenticationFailed)
        );
        assert_eq!(phone.decrypt(&tampered), Err(TransportError::SessionClosed));

        let (pc_session, phone_session) = authenticated_pair(session_id);
        let mut pc = pc_session.into_signal_cipher();
        let mut phone = phone_session.into_signal_cipher();
        let first = pc.encrypt(b"first").unwrap();
        let second = pc.encrypt(b"second").unwrap();
        assert_eq!(
            phone.decrypt(&second),
            Err(TransportError::UnexpectedSequence)
        );
        assert_eq!(phone.decrypt(&first), Err(TransportError::SessionClosed));

        let (pc_session, phone_session) = authenticated_pair(session_id);
        let mut pc = pc_session.into_signal_cipher();
        let mut phone = phone_session.into_signal_cipher();
        let first = pc.encrypt(b"first").unwrap();
        assert_eq!(phone.decrypt(&first).unwrap(), b"first");
        assert_eq!(
            phone.decrypt(&first),
            Err(TransportError::UnexpectedSequence)
        );

        let (pc_session, phone_session) = authenticated_pair(session_id);
        let mut pc = pc_session.into_signal_cipher();
        let mut phone = phone_session.into_signal_cipher();
        let mut wrong_session = pc.encrypt(b"offer").unwrap();
        wrong_session[5] ^= 1;
        assert_eq!(
            phone.decrypt(&wrong_session),
            Err(TransportError::SessionMismatch)
        );
    }

    #[test]
    fn signal_cipher_enforces_size_limit_and_never_reuses_exhausted_nonce() {
        let session_id = [6; 16];
        let (pc_session, phone_session) = authenticated_pair(session_id);
        let mut pc = pc_session.into_signal_cipher();
        let mut phone = phone_session.into_signal_cipher();
        let too_large = vec![0; MAX_FRAME_BYTES - FRAME_HEADER_LEN - AEAD_TAG_LEN + 1];
        assert_eq!(pc.encrypt(&too_large), Err(TransportError::PayloadTooLarge));
        let normal = pc.encrypt(b"still usable").unwrap();
        assert_eq!(phone.decrypt(&normal).unwrap(), b"still usable");

        let mut sender = SignalCipher {
            session_id,
            send_key: Zeroizing::new([0x31; 32]),
            receive_key: Zeroizing::new([0x42; 32]),
            next_send_sequence: Some(u32::MAX),
            next_receive_sequence: Some(FIRST_SIGNAL_SEQUENCE),
            failed: false,
        };
        let mut receiver = SignalCipher {
            session_id,
            send_key: Zeroizing::new([0x42; 32]),
            receive_key: Zeroizing::new([0x31; 32]),
            next_send_sequence: Some(FIRST_SIGNAL_SEQUENCE),
            next_receive_sequence: Some(u32::MAX),
            failed: false,
        };
        let final_frame = sender.encrypt(b"last allowed sequence").unwrap();
        assert_eq!(receiver.decrypt(&final_frame).unwrap(), b"last allowed sequence");
        assert_eq!(
            sender.encrypt(b"must not wrap"),
            Err(TransportError::SequenceExhausted)
        );
        assert_eq!(
            sender.encrypt(b"must remain closed"),
            Err(TransportError::SessionClosed)
        );
    }
}
