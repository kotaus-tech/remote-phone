// Same rule as the native buffer: any orientation inside the 4K pixel budget
// (portrait phone screens are taller than 2160 scan lines).
const MAX_WIDTH = 4096;
const MAX_HEIGHT = 4096;
const MAX_FRAME_PIXELS = 3840 * 2160;
const HEADER_BYTES = 32;
const MAX_PAYLOAD_BYTES = MAX_FRAME_PIXELS * 3 / 2;
const MAGIC = 0x31465052; // ASCII RPF1 in little-endian order.

function nv12ByteLength(width, height) {
  if (!Number.isInteger(width) || !Number.isInteger(height)
    || width < 2 || height < 2 || (width & 1) !== 0 || (height & 1) !== 0
    || width > MAX_WIDTH || height > MAX_HEIGHT
    || width * height > MAX_FRAME_PIXELS) {
    throw new Error('Недопустимые размеры NV12-кадра.');
  }
  return width * height * 3 / 2;
}

function encodeNv12FramePacket({ width, height, timestampNs, data }) {
  const payloadBytes = nv12ByteLength(width, height);
  if (!(Buffer.isBuffer(data) || data instanceof Uint8Array) || data.byteLength !== payloadBytes) {
    throw new Error('Размер NV12-данных не совпадает с заголовком.');
  }

  let timestamp;
  try {
    timestamp = typeof timestampNs === 'bigint' ? timestampNs : BigInt(timestampNs);
  } catch {
    throw new Error('Некорректная метка времени NV12-кадра.');
  }
  if (timestamp < 0n || timestamp > 0xffffffffffffffffn) {
    throw new Error('Метка времени NV12-кадра вне диапазона.');
  }

  const packet = Buffer.allocUnsafe(HEADER_BYTES + payloadBytes);
  packet.writeUInt32LE(MAGIC, 0);
  packet.writeUInt16LE(1, 4);
  packet.writeUInt16LE(HEADER_BYTES, 6);
  packet.writeUInt32LE(width, 8);
  packet.writeUInt32LE(height, 12);
  packet.writeUInt32LE(payloadBytes, 16);
  packet.writeUInt32LE(0, 20);
  packet.writeBigUInt64LE(timestamp, 24);
  Buffer.from(data.buffer, data.byteOffset, data.byteLength).copy(packet, HEADER_BYTES);
  return packet;
}

module.exports = {
  HEADER_BYTES,
  MAGIC,
  MAX_WIDTH,
  MAX_HEIGHT,
  MAX_FRAME_PIXELS,
  MAX_PAYLOAD_BYTES,
  nv12ByteLength,
  encodeNv12FramePacket,
};
