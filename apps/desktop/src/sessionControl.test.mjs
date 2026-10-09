import assert from 'node:assert/strict';
import test from 'node:test';
import {
  MAX_CONTROL_MESSAGE_BYTES,
  decodeSessionMessage,
  encodeSessionControl,
  validateTelemetry,
} from './sessionControl.ts';

const validTelemetry = {
  v: 1,
  type: 'telemetry',
  mode: 'screen',
  quality: 'auto',
  fps: 29.8,
  bitrateKbps: 3900,
  width: 1080,
  height: 2400,
  batteryPercent: 84,
  charging: false,
  thermal: 'Норма',
  thermalLevel: 0,
};

test('телеметрия round-trips и нормализуется', () => {
  const decoded = decodeSessionMessage(new TextEncoder().encode(JSON.stringify(validTelemetry)));
  assert.deepEqual(decoded, validTelemetry);
});

test('session-info от телефона распознаётся', () => {
  const decoded = decodeSessionMessage(
    new TextEncoder().encode(JSON.stringify({ v: 1, type: 'session-info', mode: 'camera', quality: 'max' }))
  );
  assert.deepEqual(decoded, { v: 1, type: 'session-info', mode: 'camera', quality: 'max' });
});

test('отклоняет повреждённую или подделанную телеметрию', () => {
  assert.throws(() => decodeSessionMessage(new Uint8Array(0)));
  assert.throws(() => decodeSessionMessage(new Uint8Array(MAX_CONTROL_MESSAGE_BYTES + 1)));
  assert.throws(() => decodeSessionMessage(new TextEncoder().encode('not json')));
  assert.throws(() => validateTelemetry({ ...validTelemetry, v: 2 }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, mode: 'audio' }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, quality: 'ultra' }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, fps: -1 }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, fps: 999 }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, bitrateKbps: 10 ** 9 }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, width: 0 }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, batteryPercent: 101 }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, charging: 'yes' }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, thermal: 5 }));
  assert.throws(() => validateTelemetry({ ...validTelemetry, thermalLevel: 9 }));
  assert.throws(() => decodeSessionMessage(new TextEncoder().encode(JSON.stringify({ v: 1, type: 'keyboard', text: 'hello' }))));
});

test('команды ПК кодируются строго и без текстового ввода', () => {
  assert.deepEqual(
    JSON.parse(new TextDecoder().decode(encodeSessionControl({ v: 1, type: 'stop-stream' }))),
    { v: 1, type: 'stop-stream' }
  );
  assert.deepEqual(
    JSON.parse(new TextDecoder().decode(encodeSessionControl({ v: 1, type: 'quality', value: 'economy' }))),
    { v: 1, type: 'quality', value: 'economy' }
  );
  assert.throws(() => encodeSessionControl({ v: 1, type: 'quality', value: 'ultra' }));
  assert.throws(() => encodeSessionControl({ v: 1, type: 'keyboard', text: 'hello' }));
  assert.throws(() => encodeSessionControl({ v: 2, type: 'stop-stream' }));
});
