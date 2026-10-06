const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { isLocalAddress, loadNativeBridge } = require('./pairing.cjs');

test('разрешает только локальные IPv4-диапазоны для ручного подключения', () => {
  for (const address of ['10.1.2.3', '172.16.0.1', '172.31.255.254', '192.168.1.24', '169.254.10.20']) {
    assert.equal(isLocalAddress(address), true, address);
  }
  for (const address of ['8.8.8.8', '172.32.0.1', '192.0.2.1', '127.0.0.1', 'not-an-ip']) {
    assert.equal(isLocalAddress(address), false, address);
  }
});

test('разрешает локальные IPv6-адреса, но отклоняет глобальные', () => {
  assert.equal(isLocalAddress('fd12:3456::1'), true);
  assert.equal(isLocalAddress('fe80::1234'), true);
  assert.equal(isLocalAddress('2001:4860:4860::8888'), false);
});

const nativeBridgePath = path.join(__dirname, '..', 'native-runtime', 'remote_phone_pairing_bridge.dll');
test('загружает C ABI-мост и проверяет вызовы через Koffi', {
  skip: !fs.existsSync(nativeBridgePath),
}, () => {
  const native = loadNativeBridge();
  assert.equal(native.pcIsAuthenticated(0n), -1);
  assert.equal(native.pcDestroy(0n), -1);
  assert.equal(Number(native.pcStart(Buffer.alloc(8), 8, Buffer.alloc(0), 0)), 0);
});
