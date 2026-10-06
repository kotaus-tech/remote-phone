//! Проверяет OPAQUE-3DH по официальным векторам RFC 9807, Appendix C.
//! Векторы используют Identity KSF, как предписано RFC; production KSF Argon2id
//! отдельно проверяется и измеряется в модульном тесте pairing-core.

use opaque_ke::ciphersuite::CipherSuite;
use opaque_ke::rand::{CryptoRng, Error, RngCore};
use opaque_ke::{
    ClientLogin, ClientLoginFinishParameters, ClientRegistration,
    ClientRegistrationFinishParameters, CredentialFinalization, CredentialRequest,
    CredentialResponse, Identifiers, Ristretto255, ServerLogin, ServerLoginParameters,
    ServerRegistration, ServerSetup, TripleDh,
};
use rand::rngs::OsRng;
use sha2::Sha512;

struct Rfc9807CipherSuite;

impl CipherSuite for Rfc9807CipherSuite {
    type OprfCs = Ristretto255;
    type KeyExchange = TripleDh<Ristretto255, Sha512>;
    type Ksf = opaque_ke::ksf::Identity;
}

/// Deterministic cyclic byte stream, used only to replay RFC-specified randomness.
struct VectorRng {
    bytes: Vec<u8>,
}

impl VectorRng {
    fn new(bytes: Vec<u8>) -> Self {
        assert!(!bytes.is_empty());
        Self { bytes }
    }
}

impl RngCore for VectorRng {
    fn next_u32(&mut self) -> u32 {
        let mut bytes = [0_u8; 4];
        self.fill_bytes(&mut bytes);
        u32::from_le_bytes(bytes)
    }

    fn next_u64(&mut self) -> u64 {
        let mut bytes = [0_u8; 8];
        self.fill_bytes(&mut bytes);
        u64::from_le_bytes(bytes)
    }

    fn fill_bytes(&mut self, destination: &mut [u8]) {
        // Mirror opaque-ke's RFC-vector CycleRng: consume only the bytes present
        // in the next vector chunk, leaving any unrequested tail unchanged.
        let length = self.bytes.len().min(destination.len());
        destination[..length].copy_from_slice(&self.bytes[..length]);
        self.bytes.rotate_left(length);
    }

    fn try_fill_bytes(&mut self, destination: &mut [u8]) -> Result<(), Error> {
        self.fill_bytes(destination);
        Ok(())
    }
}

impl CryptoRng for VectorRng {}

fn decode_hex(hex: &str) -> Vec<u8> {
    let digits: Vec<u8> = hex
        .bytes()
        .filter(|byte| !byte.is_ascii_whitespace())
        .collect();
    assert_eq!(digits.len() % 2, 0, "RFC vector has an odd number of hex digits");

    fn nibble(byte: u8) -> u8 {
        match byte {
            b'0'..=b'9' => byte - b'0',
            b'a'..=b'f' => byte - b'a' + 10,
            b'A'..=b'F' => byte - b'A' + 10,
            _ => panic!("RFC vector contains a non-hex character"),
        }
    }

    digits
        .chunks_exact(2)
        .map(|pair| (nibble(pair[0]) << 4) | nibble(pair[1]))
        .collect()
}

fn concat_hex(parts: &[&str]) -> Vec<u8> {
    let mut bytes = Vec::new();
    for part in parts {
        bytes.extend(decode_hex(part));
    }
    bytes
}

fn encode_hex(bytes: &[u8]) -> String {
    const HEX: &[u8] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        let byte = *byte;
        encoded.push(HEX[(byte >> 4) as usize] as char);
        encoded.push(HEX[(byte & 0x0f) as usize] as char);
    }
    encoded
}

#[track_caller]
fn assert_hex_eq(expected_hex: &str, actual: &[u8]) {
    let expected = decode_hex(expected_hex);
    assert_eq!(
        actual,
        expected.as_slice(),
        "RFC 9807 vector mismatch: expected {}, got {}",
        encode_hex(&expected),
        encode_hex(actual),
    );
}

fn run_real_vector(
    client_identity: Option<&[u8]>,
    server_identity: Option<&[u8]>,
    expected_registration_upload: &str,
    expected_ke2: &str,
    expected_ke3: &str,
    expected_session_key: &str,
) {
    let context = decode_hex(RFC_CONTEXT);
    let password = decode_hex(RFC_PASSWORD);
    let credential_identifier = decode_hex(RFC_CREDENTIAL_IDENTIFIER);

    let mut registration_rng = VectorRng::new(decode_hex(RFC_BLIND_REGISTRATION));
    let client_registration =
        ClientRegistration::<Rfc9807CipherSuite>::start(&mut registration_rng, &password).unwrap();
    let registration_request = client_registration.message.serialize();
    assert_hex_eq(RFC_REAL1_REGISTRATION_REQUEST, &registration_request);

    let setup_bytes = concat_hex(&[
        RFC_OPRF_SEED,
        RFC_SERVER_PRIVATE_KEY,
        RFC_SERVER_PUBLIC_KEY,
    ]);
    let server_setup = ServerSetup::<Rfc9807CipherSuite>::deserialize(&setup_bytes).unwrap();
    let server_public_key = server_setup.keypair().public().serialize();
    assert_hex_eq(RFC_SERVER_PUBLIC_KEY, &server_public_key);

    let server_registration = ServerRegistration::<Rfc9807CipherSuite>::start(
        &server_setup,
        client_registration.message,
        &credential_identifier,
    )
    .unwrap();
    let registration_response = server_registration.message.serialize();
    assert_hex_eq(RFC_REAL1_REGISTRATION_RESPONSE, &registration_response);

    let mut envelope_rng = VectorRng::new(decode_hex(RFC_ENVELOPE_NONCE));
    let registration_finish = client_registration
        .state
        .finish(
            &mut envelope_rng,
            &password,
            server_registration.message,
            ClientRegistrationFinishParameters::new(
                Identifiers {
                    client: client_identity,
                    server: server_identity,
                },
                None,
            ),
        )
        .unwrap();
    let registration_upload = registration_finish.message.serialize();
    assert_hex_eq(expected_registration_upload, &registration_upload);
    assert_hex_eq(RFC_REAL1_EXPORT_KEY, &registration_finish.export_key);
    let server_record = ServerRegistration::<Rfc9807CipherSuite>::finish(registration_finish.message);

    let mut client_rng = VectorRng::new(concat_hex(&[
        RFC_BLIND_LOGIN,
        RFC_CLIENT_KEYSHARE_SEED,
        RFC_CLIENT_NONCE,
    ]));
    let client_login = ClientLogin::<Rfc9807CipherSuite>::start(&mut client_rng, &password).unwrap();
    let login1 = client_login.message.serialize().to_vec();
    assert_hex_eq(RFC_REAL1_KE1, &login1);

    // opaque-ke requests a dummy masking key even for a registered record.
    // This 64-byte prefix is unused for the real record but keeps the remaining
    // RFC nonces and key-share seed aligned with the test-vector RNG stream.
    let mut server_randomness = vec![0_u8; 64];
    server_randomness.extend(concat_hex(&[
        RFC_MASKING_NONCE,
        RFC_SERVER_KEYSHARE_SEED,
        RFC_SERVER_NONCE,
    ]));
    let mut server_rng = VectorRng::new(server_randomness);
    let server_login = ServerLogin::<Rfc9807CipherSuite>::start(
        &mut server_rng,
        &server_setup,
        Some(server_record),
        CredentialRequest::<Rfc9807CipherSuite>::deserialize(&login1).unwrap(),
        &credential_identifier,
        ServerLoginParameters {
            context: Some(&context),
            identifiers: Identifiers {
                client: client_identity,
                server: server_identity,
            },
        },
    )
    .unwrap();
    let ke2 = server_login.message.serialize().to_vec();
    assert_hex_eq(expected_ke2, &ke2);

    let client_finish = client_login
        .state
        .finish(
            &mut OsRng,
            &password,
            CredentialResponse::<Rfc9807CipherSuite>::deserialize(&ke2).unwrap(),
            ClientLoginFinishParameters::new(
                Some(&context),
                Identifiers {
                    client: client_identity,
                    server: server_identity,
                },
                None,
            ),
        )
        .unwrap();
    let ke3 = client_finish.message.serialize().to_vec();
    assert_hex_eq(expected_ke3, &ke3);
    assert_hex_eq(expected_session_key, client_finish.session_key.as_slice());
    assert_hex_eq(RFC_REAL1_EXPORT_KEY, &client_finish.export_key);

    let server_finish = server_login
        .state
        .finish(
            CredentialFinalization::<Rfc9807CipherSuite>::deserialize(&ke3).unwrap(),
            ServerLoginParameters {
                context: Some(&context),
                identifiers: Identifiers {
                    client: client_identity,
                    server: server_identity,
                },
            },
        )
        .unwrap();
    assert_hex_eq(expected_session_key, server_finish.session_key.as_slice());
    assert_eq!(client_finish.session_key, server_finish.session_key);
}

#[test]
fn rfc9807_c11_real_vector_matches_registration_and_login() {
    run_real_vector(
        None,
        None,
        RFC_REAL1_REGISTRATION_UPLOAD,
        RFC_REAL1_KE2,
        RFC_REAL1_KE3,
        RFC_REAL1_SESSION_KEY,
    );
}

#[test]
fn rfc9807_c12_identity_bound_real_vector_matches_login() {
    let client_identity = decode_hex(RFC_REAL2_CLIENT_IDENTITY);
    let server_identity = decode_hex(RFC_REAL2_SERVER_IDENTITY);
    run_real_vector(
        Some(client_identity.as_slice()),
        Some(server_identity.as_slice()),
        RFC_REAL2_REGISTRATION_UPLOAD,
        RFC_REAL2_KE2,
        RFC_REAL2_KE3,
        RFC_REAL2_SESSION_KEY,
    );
}

#[test]
fn rfc9807_c21_fake_record_vector_matches_server_response() {
    let context = decode_hex(RFC_CONTEXT);
    let credential_identifier = decode_hex(RFC_FAKE1_CREDENTIAL_IDENTIFIER);
    let client_identity = decode_hex(RFC_FAKE1_CLIENT_IDENTITY);
    let server_identity = decode_hex(RFC_FAKE1_SERVER_IDENTITY);
    let setup_bytes = concat_hex(&[
        RFC_FAKE1_OPRF_SEED,
        RFC_FAKE1_SERVER_PRIVATE_KEY,
        RFC_FAKE1_CLIENT_PUBLIC_KEY,
    ]);
    let server_setup = ServerSetup::<Rfc9807CipherSuite>::deserialize(&setup_bytes).unwrap();
    let server_public_key = server_setup.keypair().public().serialize();
    assert_hex_eq(RFC_FAKE1_SERVER_PUBLIC_KEY, &server_public_key);

    let mut server_rng = VectorRng::new(concat_hex(&[
        RFC_FAKE1_MASKING_KEY,
        RFC_FAKE1_MASKING_NONCE,
        RFC_FAKE1_SERVER_KEYSHARE_SEED,
        RFC_FAKE1_SERVER_NONCE,
    ]));
    let login1 = decode_hex(RFC_FAKE1_KE1);
    let server_login = ServerLogin::<Rfc9807CipherSuite>::start(
        &mut server_rng,
        &server_setup,
        None,
        CredentialRequest::<Rfc9807CipherSuite>::deserialize(&login1).unwrap(),
        &credential_identifier,
        ServerLoginParameters {
            context: Some(&context),
            identifiers: Identifiers {
                client: Some(client_identity.as_slice()),
                server: Some(server_identity.as_slice()),
            },
        },
    )
    .unwrap();
    let ke2 = server_login.message.serialize();
    assert_hex_eq(RFC_FAKE1_KE2, &ke2);
}


// Official values from RFC 9807 Appendix C.1.1, C.1.2, and C.2.1.
const RFC_CONTEXT: &str = r#"
4f50415155452d504f43
"#;

const RFC_OPRF_SEED: &str = r#"
f433d0227b0b9dd54f7c4422b600e764e47fb503f1f9a0f0a47c6606b054a7fd
c65347f1a08f277e22358bbabe26f823fca82c7848e9a75661f4ec5d5c1989ef
"#;

const RFC_CREDENTIAL_IDENTIFIER: &str = r#"
31323334
"#;

const RFC_PASSWORD: &str = r#"
436f7272656374486f72736542617474657279537461706c65
"#;

const RFC_ENVELOPE_NONCE: &str = r#"
ac13171b2f17bc2c74997f0fce1e1f35bec6b91fe2e12dbd323d23ba7a38dfec
"#;

const RFC_MASKING_NONCE: &str = r#"
38fe59af0df2c79f57b8780278f5ae47355fe1f817119041951c80f612fdfc6d
"#;

const RFC_SERVER_PRIVATE_KEY: &str = r#"
47451a85372f8b3537e249d7b54188091fb18edde78094b43e2ba42b5eb89f0d
"#;

const RFC_SERVER_PUBLIC_KEY: &str = r#"
b2fe7af9f48cc502d016729d2fe25cdd433f2c4bc904660b2a382c9b79df1a78
"#;

const RFC_SERVER_NONCE: &str = r#"
71cd9960ecef2fe0d0f7494986fa3d8b2bb01963537e60efb13981e138e3d4a1
"#;

const RFC_CLIENT_NONCE: &str = r#"
da7e07376d6d6f034cfa9bb537d11b8c6b4238c334333d1f0aebb380cae6a6cc
"#;

const RFC_CLIENT_KEYSHARE_SEED: &str = r#"
82850a697b42a505f5b68fcdafce8c31f0af2b581f063cf1091933541936304b
"#;

const RFC_SERVER_KEYSHARE_SEED: &str = r#"
05a4f54206eef1ba2f615bc0aa285cb22f26d1153b5b40a1e85ff80da12f982f
"#;

const RFC_BLIND_REGISTRATION: &str = r#"
76cfbfe758db884bebb33582331ba9f159720ca8784a2a070a265d9c2d6abe01
"#;

const RFC_BLIND_LOGIN: &str = r#"
6ecc102d2e7a7cf49617aad7bbe188556792d4acd60a1a8a8d2b65d4b0790308
"#;

const RFC_REAL1_REGISTRATION_REQUEST: &str = r#"
5059ff249eb1551b7ce4991f3336205bde44a105a032e747d21bf382e75f7a71
"#;

const RFC_REAL1_REGISTRATION_RESPONSE: &str = r#"
7408a268083e03abc7097fc05b587834539065e86fb0c7b6342fcf5e01e5b019
b2fe7af9f48cc502d016729d2fe25cdd433f2c4bc904660b2a382c9b79df1a78
"#;

const RFC_REAL1_REGISTRATION_UPLOAD: &str = r#"
76a845464c68a5d2f7e442436bb1424953b17d3e2e289ccbaccafb57ac5c3675
1ac5844383c7708077dea41cbefe2fa15724f449e535dd7dd562e66f5ecfb958
64eadddec9db5874959905117dad40a4524111849799281fefe3c51fa82785c5
ac13171b2f17bc2c74997f0fce1e1f35bec6b91fe2e12dbd323d23ba7a38dfec
634b0f5b96109c198a8027da51854c35bee90d1e1c781806d07d49b76de6a28b
8d9e9b6c93b9f8b64d16dddd9c5bfb5fea48ee8fd2f75012a8b308605cdd8ba5
"#;

const RFC_REAL1_KE1: &str = r#"
c4dedb0ba6ed5d965d6f250fbe554cd45cba5dfcce3ce836e4aee778aa3cd44d
da7e07376d6d6f034cfa9bb537d11b8c6b4238c334333d1f0aebb380cae6a6cc
6e29bee50701498605b2c085d7b241ca15ba5c32027dd21ba420b94ce60da326
"#;

const RFC_REAL1_KE2: &str = r#"
7e308140890bcde30cbcea28b01ea1ecfbd077cff62c4def8efa075aabcbb471
38fe59af0df2c79f57b8780278f5ae47355fe1f817119041951c80f612fdfc6d
d6ec60bcdb26dc455ddf3e718f1020490c192d70dfc7e403981179d8073d1146
a4f9aa1ced4e4cd984c657eb3b54ced3848326f70331953d91b02535af44d9fe
dc80188ca46743c52786e0382f95ad85c08f6afcd1ccfbff95e2bdeb015b166c
6b20b92f832cc6df01e0b86a7efd92c1c804ff865781fa93f2f20b446c8371b6
71cd9960ecef2fe0d0f7494986fa3d8b2bb01963537e60efb13981e138e3d4a1
c4f62198a9d6fa9170c42c3c71f1971b29eb1d5d0bd733e40816c91f7912cc4a
660c48dae03e57aaa38f3d0cffcfc21852ebc8b405d15bd6744945ba1a93438a
162b6111699d98a16bb55b7bdddfe0fc5608b23da246e7bd73b47369169c5c90
"#;

const RFC_REAL1_KE3: &str = r#"
4455df4f810ac31a6748835888564b536e6da5d9944dfea9e34defb9575fe5e2
661ef61d2ae3929bcf57e53d464113d364365eb7d1a57b629707ca48da18e442
"#;

const RFC_REAL1_EXPORT_KEY: &str = r#"
1ef15b4fa99e8a852412450ab78713aad30d21fa6966c9b8c9fb3262a970dc62
950d4dd4ed62598229b1b72794fc0335199d9f7fcc6eaedde92cc04870e63f16
"#;

const RFC_REAL1_SESSION_KEY: &str = r#"
42afde6f5aca0cfa5c163763fbad55e73a41db6b41bc87b8e7b62214a8eedc67
31fa3cb857d657ab9b3764b89a84e91ebcb4785166fbb02cedfcbdfda215b96f
"#;

const RFC_REAL2_CLIENT_IDENTITY: &str = r#"
616c696365
"#;

const RFC_REAL2_SERVER_IDENTITY: &str = r#"
626f62
"#;

const RFC_REAL2_REGISTRATION_UPLOAD: &str = r#"
76a845464c68a5d2f7e442436bb1424953b17d3e2e289ccbaccafb57ac5c3675
1ac5844383c7708077dea41cbefe2fa15724f449e535dd7dd562e66f5ecfb958
64eadddec9db5874959905117dad40a4524111849799281fefe3c51fa82785c5
ac13171b2f17bc2c74997f0fce1e1f35bec6b91fe2e12dbd323d23ba7a38dfec
1ac902dc5589e9a5f0de56ad685ea8486210ef41449cd4d8712828913c5d2b68
0b2b3af4a26c765cff329bfb66d38ecf1d6cfa9e7a73c222c6efe0d9520f7d7c
"#;

const RFC_REAL2_KE2: &str = r#"
7e308140890bcde30cbcea28b01ea1ecfbd077cff62c4def8efa075aabcbb471
38fe59af0df2c79f57b8780278f5ae47355fe1f817119041951c80f612fdfc6d
d6ec60bcdb26dc455ddf3e718f1020490c192d70dfc7e403981179d8073d1146
a4f9aa1ced4e4cd984c657eb3b54ced3848326f70331953d91b02535af44d9fe
a502150b67fe36795dd8914f164e49f81c7688a38928372134b7dccd50e09f8f
ed9518b7b2f94835b3c4fe4c8475e7513f20eb97ff0568a39caee3fd6251876f
71cd9960ecef2fe0d0f7494986fa3d8b2bb01963537e60efb13981e138e3d4a1
c4f62198a9d6fa9170c42c3c71f1971b29eb1d5d0bd733e40816c91f7912cc4a
292371e7809a9031743e943fb3b56f51de903552fc91fba4e7419029951c3970
b2e2f0a9dea218d22e9e4e0000855bb6421aa3610d6fc0f4033a6517030d4341
"#;

const RFC_REAL2_KE3: &str = r#"
7a026de1d6126905736c3f6d92463a08d209833eb793e46d0f7f15b3e0f62c76
43763c02bbc6b8d3d15b63250cae98171e9260f1ffa789750f534ac11a0176d5
"#;

const RFC_REAL2_SESSION_KEY: &str = r#"
ae7951123ab5befc27e62e63f52cf472d6236cb386c968cc47b7e34f866aa4bc
7638356a73cfce92becf39d6a7d32a1861f12130e824241fe6cab34fbd471a57
"#;

const RFC_FAKE1_CREDENTIAL_IDENTIFIER: &str = r#"
31323334
"#;

const RFC_FAKE1_OPRF_SEED: &str = r#"
743fc168d1f826ad43738933e5adb23da6fb95f95a1b069f0daa0522d0a78b61
7f701fc6aa46d3e7981e70de7765dfcd6b1e13e3369a582eb8dc456b10aa53b0
"#;

const RFC_FAKE1_MASKING_NONCE: &str = r#"
9c035896a043e70f897d87180c543e7a063b83c1bb728fbd189c619e27b6e5a6
"#;

const RFC_FAKE1_SERVER_PRIVATE_KEY: &str = r#"
c788585ae8b5ba2942b693b849be0c0426384e41977c18d2e81fbe30fd7c9f06
"#;

const RFC_FAKE1_SERVER_PUBLIC_KEY: &str = r#"
825f832667480f08b0c9069da5083ac4d0e9ee31b49c4e0310031fea04d52966
"#;

const RFC_FAKE1_SERVER_NONCE: &str = r#"
1e10f6eeab2a7a420bf09da9b27a4639645622c46358de9cf7ae813055ae2d12
"#;

const RFC_FAKE1_SERVER_KEYSHARE_SEED: &str = r#"
360b0937f47d45f6123a4d8f0d0c0814b6120d840ebb8bc5b4f6b62df07f78c2
"#;

const RFC_FAKE1_CLIENT_IDENTITY: &str = r#"
616c696365
"#;

const RFC_FAKE1_SERVER_IDENTITY: &str = r#"
626f62
"#;

const RFC_FAKE1_CLIENT_PUBLIC_KEY: &str = r#"
84f43f9492e19c22d8bdaa4447cc3d4db1cdb5427a9f852c4707921212c36251
"#;

const RFC_FAKE1_MASKING_KEY: &str = r#"
39ebd51f0e39a07a1c2d2431995b0399bca9996c5d10014d6ebab4453dc10ce5
cef38ed3df6e56bfff40c2d8dd4671c2b4cf63c3d54860f31fe40220d690bb71
"#;

const RFC_FAKE1_KE1: &str = r#"
b0a26dcaca2230b8f5e4b1bcab9c84b586140221bb8b2848486874b0be448905
42d4e61ed3f8d64cdd3b9d153343eca15b9b0d5e388232793c6376bd2d9cfd0a
b641d7f20a245a09f1d4dbb6e301661af7f352beb0791d055e48d3645232f77f
"#;

const RFC_FAKE1_KE2: &str = r#"
928f79ad8df21963e91411b9f55165ba833dea918f441db967cdc09521d22925
9c035896a043e70f897d87180c543e7a063b83c1bb728fbd189c619e27b6e5a6
32b5ab1bff96636144faa4f9f9afaac75dd88ea99cf5175902ae3f3b2195693f
165f11929ba510a5978e64dcdabecbd7ee1e4380ce270e58fea58e6462d92964
a1aaef72698bca1c673baeb04cc2bf7de5f3c2f5553464552d3a0f7698a9ca7f
9c5e70c6cb1f706b2f175ab9d04bbd13926e816b6811a50b4aafa9799d5ed797
1e10f6eeab2a7a420bf09da9b27a4639645622c46358de9cf7ae813055ae2d12
98251c5ba55f6b0b2d58d9ff0c88fe4176484be62a96db6e2a8c4d431bd1bf27
fe6c1d0537603835217d42ebf7b2581982732e74892fd28211b31ed33863f0be
af75ba6f59474c0aaf9d78a60a9b2f4cd24d7ab54131b3c8efa192df6b72db4c
"#;
