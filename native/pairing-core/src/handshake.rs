//! Stateful, adapter-friendly OPAQUE handshake orchestration.
//!
//! Network adapters should pass complete binary frames to this module and send
//! its returned frames verbatim. PINs and session keys remain inside Rust.

use thiserror::Error;

use crate::{
    AuthenticatedSession, Frame, FrameSender, HandshakeTranscript, Hello, MessageType,
    PairingClient, PairingError, PairingServer, SessionKeys, SignalCipher, TransportError,
    MAX_SESSION_LIFETIME_SECONDS,
};

#[derive(Debug, Error, PartialEq, Eq)]
pub enum HandshakeError {
    #[error(transparent)]
    Pairing(#[from] PairingError),
    #[error(transparent)]
    Transport(#[from] TransportError),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Step {
    AwaitLogin1,
    AwaitLogin3,
    AwaitAuthAck,
    Authenticated,
    Failed,
}

/// One phone-side WebSocket handshake. A new value is created for each
/// connection while the owning [`PairingServer`] retains the session-wide PIN
/// attempt counter. Dropping an incomplete connection clears its pending
/// OPAQUE login state.
pub struct PhoneHandshake<'a> {
    server: &'a mut PairingServer,
    session_id: [u8; 16],
    transcript: HandshakeTranscript,
    step: Step,
    keys: Option<SessionKeys>,
    transcript_hash: Option<[u8; 64]>,
    signal_cipher: Option<SignalCipher>,
}

impl<'a> PhoneHandshake<'a> {
    /// Starts a connection and returns the binary HELLO frame to send first.
    pub fn start(server: &'a mut PairingServer) -> Result<(Self, Vec<u8>), HandshakeError> {
        let session_id = *server.session_id();
        let hello = Hello::new(session_id, MAX_SESSION_LIFETIME_SECONDS)?.to_frame()?;
        let mut transcript = HandshakeTranscript::new(session_id);
        transcript.accept(FrameSender::Phone, &hello)?;
        let encoded_hello = hello.encode()?;
        Ok((
            Self {
                server,
                session_id,
                transcript,
                step: Step::AwaitLogin1,
                keys: None,
                transcript_hash: None,
                signal_cipher: None,
            },
            encoded_hello,
        ))
    }

    /// Processes exactly one expected inbound handshake frame. `Some` contains
    /// the next frame to send; `None` means the peer's AUTH_ACK was verified.
    /// Any protocol error poisons this connection; callers must close it.
    pub fn handle_handshake_frame(
        &mut self,
        encoded_frame: &[u8],
    ) -> Result<Option<Vec<u8>>, HandshakeError> {
        if self.step == Step::Failed || self.step == Step::Authenticated {
            self.step = Step::Failed;
            self.keys = None;
            self.transcript_hash = None;
            self.signal_cipher = None;
            return Err(TransportError::SessionClosed.into());
        }

        let result = self.handle_handshake_frame_inner(encoded_frame);
        if result.is_err() {
            self.step = Step::Failed;
            self.keys = None;
            self.transcript_hash = None;
            self.signal_cipher = None;
        }
        result
    }

    fn handle_handshake_frame_inner(
        &mut self,
        encoded_frame: &[u8],
    ) -> Result<Option<Vec<u8>>, HandshakeError> {
        match self.step {
            Step::AwaitLogin1 => {
                let login1 = Frame::decode(encoded_frame)?;
                self.transcript.accept(FrameSender::Pc, &login1)?;
                if login1.message_type() != MessageType::OpaqueLogin1 {
                    return Err(TransportError::UnexpectedSequence.into());
                }

                let payload = self.server.begin_login(login1.payload())?;
                let login2 = Frame::new(
                    MessageType::OpaqueLogin2,
                    self.session_id,
                    1,
                    payload,
                )?;
                self.transcript.accept(FrameSender::Phone, &login2)?;
                self.step = Step::AwaitLogin3;
                Ok(Some(login2.encode()?))
            }
            Step::AwaitLogin3 => {
                let login3 = Frame::decode(encoded_frame)?;
                self.transcript.accept(FrameSender::Pc, &login3)?;
                if login3.message_type() != MessageType::OpaqueLogin3 {
                    return Err(TransportError::UnexpectedSequence.into());
                }

                let keys = self.server.finish_login(login3.payload())?;
                let transcript_hash = self.transcript.transcript_hash()?;
                let auth_ok = keys.create_server_auth_ok(self.session_id, &transcript_hash)?;
                self.keys = Some(keys);
                self.transcript_hash = Some(transcript_hash);
                self.step = Step::AwaitAuthAck;
                Ok(Some(auth_ok.encode()?))
            }
            Step::AwaitAuthAck => {
                let auth_ack = Frame::decode(encoded_frame)?;
                let keys = self
                    .keys
                    .take()
                    .ok_or(TransportError::HandshakeIncomplete)?;
                let transcript_hash = self
                    .transcript_hash
                    .take()
                    .ok_or(TransportError::HandshakeIncomplete)?;
                let session: AuthenticatedSession =
                    keys.accept_client_auth_ack(&auth_ack, self.session_id, &transcript_hash)?;
                self.signal_cipher = Some(session.into_signal_cipher());
                self.step = Step::Authenticated;
                Ok(None)
            }
            Step::Authenticated | Step::Failed => {
                Err(TransportError::SessionClosed.into())
            }
        }
    }

    pub fn is_authenticated(&self) -> bool {
        self.step == Step::Authenticated
    }

    /// Encrypts authenticated signaling without exporting any session key.
    pub fn encrypt_signal(&mut self, plaintext: &[u8]) -> Result<Vec<u8>, TransportError> {
        if self.step != Step::Authenticated {
            return Err(TransportError::HandshakeIncomplete);
        }
        let result = self
            .signal_cipher
            .as_mut()
            .ok_or(TransportError::SessionClosed)?
            .encrypt(plaintext);
        if result.is_err() {
            self.step = Step::Failed;
            self.signal_cipher = None;
        }
        result
    }

    /// Authenticates and decrypts one signaling frame. A failure closes this
    /// handshake object, so a caller cannot resume after replay or tampering.
    pub fn decrypt_signal(&mut self, frame: &[u8]) -> Result<Vec<u8>, TransportError> {
        if self.step != Step::Authenticated {
            return Err(TransportError::HandshakeIncomplete);
        }
        let result = self
            .signal_cipher
            .as_mut()
            .ok_or(TransportError::SessionClosed)?
            .decrypt(frame);
        if result.is_err() {
            self.step = Step::Failed;
            self.signal_cipher = None;
        }
        result
    }
}

impl Drop for PhoneHandshake<'_> {
    fn drop(&mut self) {
        if self.step != Step::Authenticated {
            self.server.abandon_pending_login();
        }
    }
}

/// One desktop-side handshake. The PIN is consumed by the OPAQUE client and
/// never appears in an encoded frame or in this type's debug representation.
pub struct PcHandshake {
    session_id: [u8; 16],
    transcript: HandshakeTranscript,
    step: Step,
    client: Option<PairingClient>,
    keys: Option<SessionKeys>,
    transcript_hash: Option<[u8; 64]>,
    signal_cipher: Option<SignalCipher>,
}

impl PcHandshake {
    /// Validates the phone's HELLO frame and returns a LOGIN1 frame.
    pub fn start(pin: &str, encoded_hello: &[u8]) -> Result<(Self, Vec<u8>), HandshakeError> {
        let hello_frame = Frame::decode(encoded_hello)?;
        let hello = Hello::from_frame(&hello_frame)?;
        let mut transcript = HandshakeTranscript::new(hello.session_id);
        transcript.accept(FrameSender::Phone, &hello_frame)?;

        let (client, login1_payload) = PairingClient::start(pin, hello.session_id)?;
        let login1 = Frame::new(
            MessageType::OpaqueLogin1,
            hello.session_id,
            0,
            login1_payload,
        )?;
        transcript.accept(FrameSender::Pc, &login1)?;
        let encoded_login1 = login1.encode()?;
        Ok((
            Self {
                session_id: hello.session_id,
                transcript,
                step: Step::AwaitLogin3,
                client: Some(client),
                keys: None,
                transcript_hash: None,
                signal_cipher: None,
            },
            encoded_login1,
        ))
    }

    /// Processes one expected phone frame. `Some` is the next frame to send;
    /// after AUTH_OK this is AUTH_ACK, and the signaling cipher becomes ready.
    pub fn handle_handshake_frame(
        &mut self,
        encoded_frame: &[u8],
    ) -> Result<Option<Vec<u8>>, HandshakeError> {
        if self.step == Step::Failed || self.step == Step::Authenticated {
            self.step = Step::Failed;
            self.client = None;
            self.keys = None;
            self.transcript_hash = None;
            self.signal_cipher = None;
            return Err(TransportError::SessionClosed.into());
        }

        let result = self.handle_handshake_frame_inner(encoded_frame);
        if result.is_err() {
            self.step = Step::Failed;
            self.client = None;
            self.keys = None;
            self.transcript_hash = None;
            self.signal_cipher = None;
        }
        result
    }

    fn handle_handshake_frame_inner(
        &mut self,
        encoded_frame: &[u8],
    ) -> Result<Option<Vec<u8>>, HandshakeError> {
        match self.step {
            Step::AwaitLogin3 => {
                let login2 = Frame::decode(encoded_frame)?;
                self.transcript.accept(FrameSender::Phone, &login2)?;
                if login2.message_type() != MessageType::OpaqueLogin2 {
                    return Err(TransportError::UnexpectedSequence.into());
                }

                let client = self
                    .client
                    .as_mut()
                    .ok_or(TransportError::HandshakeIncomplete)?;
                let (login3_payload, keys) = client.finish(login2.payload())?;
                self.client = None;
                let login3 = Frame::new(
                    MessageType::OpaqueLogin3,
                    self.session_id,
                    1,
                    login3_payload,
                )?;
                self.transcript.accept(FrameSender::Pc, &login3)?;
                self.transcript_hash = Some(self.transcript.transcript_hash()?);
                self.keys = Some(keys);
                self.step = Step::AwaitAuthAck;
                Ok(Some(login3.encode()?))
            }
            Step::AwaitAuthAck => {
                let auth_ok = Frame::decode(encoded_frame)?;
                let keys = self
                    .keys
                    .take()
                    .ok_or(TransportError::HandshakeIncomplete)?;
                let transcript_hash = self
                    .transcript_hash
                    .take()
                    .ok_or(TransportError::HandshakeIncomplete)?;
                let (auth_ack, session) =
                    keys.accept_server_auth_ok(&auth_ok, self.session_id, &transcript_hash)?;
                self.signal_cipher = Some(session.into_signal_cipher());
                self.step = Step::Authenticated;
                Ok(Some(auth_ack.encode()?))
            }
            Step::AwaitLogin1 => Err(TransportError::UnexpectedSequence.into()),
            Step::Authenticated | Step::Failed => {
                Err(TransportError::SessionClosed.into())
            }
        }
    }

    pub fn is_authenticated(&self) -> bool {
        self.step == Step::Authenticated
    }

    pub fn encrypt_signal(&mut self, plaintext: &[u8]) -> Result<Vec<u8>, TransportError> {
        if self.step != Step::Authenticated {
            return Err(TransportError::HandshakeIncomplete);
        }
        let result = self
            .signal_cipher
            .as_mut()
            .ok_or(TransportError::SessionClosed)?
            .encrypt(plaintext);
        if result.is_err() {
            self.step = Step::Failed;
            self.signal_cipher = None;
        }
        result
    }

    pub fn decrypt_signal(&mut self, frame: &[u8]) -> Result<Vec<u8>, TransportError> {
        if self.step != Step::Authenticated {
            return Err(TransportError::HandshakeIncomplete);
        }
        let result = self
            .signal_cipher
            .as_mut()
            .ok_or(TransportError::SessionClosed)?
            .decrypt(frame);
        if result.is_err() {
            self.step = Step::Failed;
            self.signal_cipher = None;
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rand::SeedableRng;
    use rand_chacha::ChaCha20Rng;
    use std::time::Instant;

    fn server_and_pin() -> (PairingServer, String) {
        let mut rng = ChaCha20Rng::from_seed([0x64; 32]);
        let server = PairingServer::create_with_rng(&mut rng, Instant::now()).unwrap();
        let pin = server.pin().expose_for_display().to_owned();
        (server, pin)
    }

    fn complete_handshake<'a>(
        server: &'a mut PairingServer,
        pin: &str,
    ) -> (PhoneHandshake<'a>, PcHandshake) {
        let (mut phone, hello) = PhoneHandshake::start(server).unwrap();
        let (mut pc, login1) = PcHandshake::start(pin, &hello).unwrap();
        let login2 = phone.handle_handshake_frame(&login1).unwrap().unwrap();
        let login3 = pc.handle_handshake_frame(&login2).unwrap().unwrap();
        let auth_ok = phone.handle_handshake_frame(&login3).unwrap().unwrap();
        let auth_ack = pc.handle_handshake_frame(&auth_ok).unwrap().unwrap();
        assert!(pc.is_authenticated());
        assert_eq!(phone.handle_handshake_frame(&auth_ack).unwrap(), None);
        assert!(phone.is_authenticated());
        (phone, pc)
    }

    #[test]
    fn adapters_complete_opaque_before_exposing_bidirectional_signal_cipher() {
        let (mut server, pin) = server_and_pin();
        let (mut pending_phone, _) = PhoneHandshake::start(&mut server).unwrap();
        assert_eq!(
            pending_phone.encrypt_signal(b"early signal"),
            Err(TransportError::HandshakeIncomplete)
        );
        drop(pending_phone);

        let (mut phone, mut pc) = complete_handshake(&mut server, &pin);
        let pc_signal = pc.encrypt_signal(b"authenticated offer").unwrap();
        assert_eq!(
            phone.decrypt_signal(&pc_signal).unwrap(),
            b"authenticated offer"
        );
        let phone_signal = phone.encrypt_signal(b"authenticated answer").unwrap();
        assert_eq!(
            pc.decrypt_signal(&phone_signal).unwrap(),
            b"authenticated answer"
        );
        assert!(!pc_signal
            .windows(pin.len())
            .any(|window| window == pin.as_bytes()));
    }

    #[test]
    fn failed_pin_connection_can_retry_without_resetting_session_attempt_counter() {
        let (mut server, pin) = server_and_pin();
        let mut invalid_pin = pin.as_bytes().to_vec();
        let last = invalid_pin.len() - 1;
        invalid_pin[last] = if invalid_pin[last] == b'9' {
            b'0'
        } else {
            invalid_pin[last] + 1
        };
        let invalid_pin = String::from_utf8(invalid_pin).unwrap();

        let (mut phone, hello) = PhoneHandshake::start(&mut server).unwrap();
        let (mut pc, login1) = PcHandshake::start(&invalid_pin, &hello).unwrap();
        let login2 = phone.handle_handshake_frame(&login1).unwrap().unwrap();
        assert_eq!(
            pc.handle_handshake_frame(&login2),
            Err(HandshakeError::Pairing(PairingError::AuthenticationFailed))
        );
        drop(pc);
        drop(phone);
        assert_eq!(server.attempts_used(), 1);

        let (mut phone, hello) = PhoneHandshake::start(&mut server).unwrap();
        let (mut pc, login1) = PcHandshake::start(&pin, &hello).unwrap();
        let login2 = phone.handle_handshake_frame(&login1).unwrap().unwrap();
        let login3 = pc.handle_handshake_frame(&login2).unwrap().unwrap();
        let auth_ok = phone.handle_handshake_frame(&login3).unwrap().unwrap();
        let auth_ack = pc.handle_handshake_frame(&auth_ok).unwrap().unwrap();
        assert!(pc.is_authenticated());
        assert_eq!(phone.handle_handshake_frame(&auth_ack).unwrap(), None);
        assert!(phone.is_authenticated());
        drop(pc);
        drop(phone);
        assert_eq!(server.attempts_used(), 2);
    }
}
