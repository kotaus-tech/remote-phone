#![deny(unsafe_code)]

//! Minimal C ABI around the shared OPAQUE pairing state machine. All unsafe
//! pointer handling is isolated in `ffi`; protocol and cryptography stay in the
//! safe Rust core.

#[allow(unsafe_code)]
mod ffi;

pub use ffi::*;
