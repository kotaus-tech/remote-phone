const assert = require('node:assert/strict');
const test = require('node:test');
const {
  HEADER_BYTES,
  MAGIC,
  MAX_PAYLOAD_BYTES,
  encodeNv12FramePacket,
  nv12ByteLength,
} = require('./frame-pipe.cjs');

test('пакет NV12 имеет 32-байтовый little-endian header и точный payload', () => {
  const data = Buffer.from(Array.from({ length: 24 }, (_, index) => index));
  const packet = encodeNv12FramePacket({ width: 4, height: 4, timestampNs: 123456789n, data });

  assert.equal(packet.length, HEADER_BYTES + data.length);
  assert.equal(packet.readUInt32LE(0), MAGIC);
  assert.equal(packet.readUInt16LE(4), 1);
  assert.equal(packet.readUInt16LE(6), HEADER_BYTES);
  assert.equal(packet.readUInt32LE(8), 4);
  assert.equal(packet.readUInt32LE(12), 4);
  assert.equal(packet.readUInt32LE(16), data.length);
  assert.equal(packet.readUInt32LE(20), 0);
  assert.equal(packet.readBigUInt64LE(24), 123456789n);
  assert.deepEqual(packet.subarray(HEADER_BYTES), data);
  packet.fill(0);
});

test('NV12 frame limits retain 4K and reject malformed dimensions and payloads', () => {
  assert.equal(nv12ByteLength(3840, 2160), MAX_PAYLOAD_BYTES);
  assert.equal(nv12ByteLength(1920, 1080), 1920 * 1080 * 3 / 2);
  for (const [width, height] of [[0, 2], [3, 4], [4, 3], [3842, 2160], [3840, 2162]]) {
    assert.throws(() => nv12ByteLength(width, height));
  }
  assert.throws(() => encodeNv12FramePacket({ width: 4, height: 4, timestampNs: 1, data: Buffer.alloc(23) }));
  assert.throws(() => encodeNv12FramePacket({ width: 4, height: 4, timestampNs: -1, data: Buffer.alloc(24) }));
  assert.throws(() => encodeNv12FramePacket({ width: 4, height: 4, timestampNs: 1.5, data: Buffer.alloc(24) }));
});
