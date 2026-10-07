const MAX_SIGNAL_BYTES = 192 * 1024;
const MAX_SDP_BYTES = 160 * 1024;
const MAX_CANDIDATE_BYTES = 8192;
const MAX_MID_BYTES = 256;

function validateRtcSignal(signal) {
  if (!signal || typeof signal !== 'object' || Array.isArray(signal) || signal.v !== 1) {
    throw new Error('Некорректная версия сигнализации.');
  }

  if (signal.type === 'offer' || signal.type === 'answer') {
    if (typeof signal.sdp !== 'string' || Buffer.byteLength(signal.sdp, 'utf8') === 0
      || Buffer.byteLength(signal.sdp, 'utf8') > MAX_SDP_BYTES) {
      throw new Error('Некорректное SDP-описание.');
    }
    return signal;
  }

  if (signal.type === 'ice') {
    if (typeof signal.candidate !== 'string'
      || Buffer.byteLength(signal.candidate, 'utf8') === 0
      || Buffer.byteLength(signal.candidate, 'utf8') > MAX_CANDIDATE_BYTES) {
      throw new Error('Некорректный ICE-кандидат.');
    }
    if (signal.sdpMid !== null && signal.sdpMid !== undefined
      && (typeof signal.sdpMid !== 'string' || Buffer.byteLength(signal.sdpMid, 'utf8') > MAX_MID_BYTES)) {
      throw new Error('Некорректный идентификатор SDP media.');
    }
    if (signal.sdpMLineIndex !== null && signal.sdpMLineIndex !== undefined
      && (!Number.isInteger(signal.sdpMLineIndex) || signal.sdpMLineIndex < 0 || signal.sdpMLineIndex > 255)) {
      throw new Error('Некорректный индекс SDP media.');
    }
    return signal;
  }

  if (signal.type === 'bye') return signal;

  if (signal.type === 'error') {
    if (typeof signal.code !== 'string' || !/^[A-Z0-9_]{1,48}$/.test(signal.code)) {
      throw new Error('Некорректный код сигнальной ошибки.');
    }
    return signal;
  }

  throw new Error('Неизвестный тип WebRTC-сигнализации.');
}

function encodeRtcSignal(signal) {
  validateRtcSignal(signal);
  const payload = Buffer.from(JSON.stringify(signal), 'utf8');
  if (payload.length === 0 || payload.length > MAX_SIGNAL_BYTES) {
    payload.fill(0);
    throw new Error('Сигнальное сообщение превышает лимит.');
  }
  return payload;
}

function decodeRtcSignal(payload) {
  if (!(Buffer.isBuffer(payload) || payload instanceof Uint8Array)
    || payload.byteLength === 0 || payload.byteLength > MAX_SIGNAL_BYTES) {
    throw new Error('Некорректный размер сигнального сообщения.');
  }
  const decoder = new TextDecoder('utf-8', { fatal: true });
  const signal = JSON.parse(decoder.decode(payload));
  return validateRtcSignal(signal);
}

module.exports = {
  MAX_SIGNAL_BYTES,
  MAX_SDP_BYTES,
  MAX_CANDIDATE_BYTES,
  validateRtcSignal,
  encodeRtcSignal,
  decodeRtcSignal,
};
