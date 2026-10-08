const assert = require('node:assert/strict');
const test = require('node:test');
const {
  MAX_SIGNAL_BYTES,
  encodeRtcSignal,
  decodeRtcSignal,
} = require('./rtc-signaling.cjs');

test('шифруемая сигнализация round-trips offer, answer, ICE и завершение', () => {
  const signals = [
    { v: 1, type: 'offer', sdp: 'v=0\r\n', },
    { v: 1, type: 'answer', sdp: 'v=0\r\na=sendonly\r\n' },
    { v: 1, type: 'ice', candidate: 'candidate:1 1 UDP 1 192.168.1.2 5000 typ host', sdpMid: '0', sdpMLineIndex: 0 },
    { v: 1, type: 'bye' },
    { v: 1, type: 'error', code: 'CAMERA_NOT_ENABLED' },
  ];

  for (const signal of signals) {
    const payload = encodeRtcSignal(signal);
    assert.deepEqual(decodeRtcSignal(payload), signal);
    payload.fill(0);
  }
});

test('отклоняет неизвестную версию, типы и слишком большие поля', () => {
  assert.throws(() => encodeRtcSignal({ v: 2, type: 'bye' }));
  assert.throws(() => encodeRtcSignal({ v: 1, type: 'screen-frame', data: 'no' }));
  assert.throws(() => encodeRtcSignal({ v: 1, type: 'offer', sdp: '' }));
  assert.throws(() => encodeRtcSignal({ v: 1, type: 'answer', sdp: 'x'.repeat(160 * 1024 + 1) }));
  assert.throws(() => encodeRtcSignal({ v: 1, type: 'ice', candidate: '' }));
  assert.throws(() => encodeRtcSignal({ v: 1, type: 'ice', candidate: 'x', sdpMLineIndex: -1 }));
  assert.throws(() => decodeRtcSignal(Buffer.alloc(MAX_SIGNAL_BYTES + 1)));
  assert.throws(() => decodeRtcSignal(Buffer.from([0xff, 0xfe])));
});
