/**
 * Strict, small session-control protocol for the DTLS-protected WebRTC data
 * channel. Phone→PC: session-info and telemetry. PC→phone: stop-stream and
 * quality. No text input ever travels this channel; gesture commands are
 * reserved for the next milestone and will be validated separately.
 */

export const MAX_CONTROL_MESSAGE_BYTES = 4096;

export type StreamMode = 'camera' | 'screen';
export type QualityProfile = 'auto' | 'max' | 'economy';

const STREAM_MODES: ReadonlySet<string> = new Set(['camera', 'screen']);
const QUALITY_PROFILES: ReadonlySet<string> = new Set(['auto', 'max', 'economy']);

export type PhoneTelemetryMessage = {
  v: 1;
  type: 'telemetry';
  mode: StreamMode;
  quality: QualityProfile;
  fps: number;
  bitrateKbps: number;
  width: number;
  height: number;
  batteryPercent: number;
  charging: boolean;
  thermal: string;
  thermalLevel: number;
};

export type SessionInfoMessage = {
  v: 1;
  type: 'session-info';
  mode: StreamMode;
  quality: QualityProfile;
};

export type SessionMessage = PhoneTelemetryMessage | SessionInfoMessage;

export type SessionControlMessage =
  | { v: 1; type: 'stop-stream' }
  | { v: 1; type: 'quality'; value: QualityProfile };

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
}

function isIntegerInRange(value: unknown, min: number, max: number): value is number {
  return Number.isInteger(value) && (value as number) >= min && (value as number) <= max;
}

export function validateTelemetry(message: unknown): PhoneTelemetryMessage {
  if (!message || typeof message !== 'object' || Array.isArray(message)) {
    throw new Error('Некорректное сообщение телеметрии.');
  }
  const raw = message as Record<string, unknown>;
  if (raw.v !== 1) throw new Error('Некорректная версия телеметрии.');
  if (raw.type !== 'telemetry') throw new Error('Ожидалось сообщение телеметрии.');
  if (typeof raw.mode !== 'string' || !STREAM_MODES.has(raw.mode)) {
    throw new Error('Некорректный режим трансляции в телеметрии.');
  }
  if (typeof raw.quality !== 'string' || !QUALITY_PROFILES.has(raw.quality)) {
    throw new Error('Некорректный профиль качества в телеметрии.');
  }
  if (!isFiniteNumber(raw.fps) || raw.fps < 0 || raw.fps > 240) {
    throw new Error('Некорректная частота кадров в телеметрии.');
  }
  if (!isFiniteNumber(raw.bitrateKbps) || raw.bitrateKbps < 0 || raw.bitrateKbps > 200_000) {
    throw new Error('Некорректный битрейт в телеметрии.');
  }
  if (!isIntegerInRange(raw.width, 1, 7680) || !isIntegerInRange(raw.height, 1, 4320)) {
    throw new Error('Некорректное разрешение в телеметрии.');
  }
  if (!isIntegerInRange(raw.batteryPercent, -1, 100)) {
    throw new Error('Некорректный заряд батареи в телеметрии.');
  }
  if (typeof raw.charging !== 'boolean') {
    throw new Error('Некорректный статус питания в телеметрии.');
  }
  if (typeof raw.thermal !== 'string' || raw.thermal.length > 64) {
    throw new Error('Некорректный тепловой статус в телеметрии.');
  }
  if (!isIntegerInRange(raw.thermalLevel, 0, 8)) {
    throw new Error('Некорректный уровень нагрева в телеметрии.');
  }
  return {
    v: 1,
    type: 'telemetry',
    mode: raw.mode as StreamMode,
    quality: raw.quality as QualityProfile,
    fps: raw.fps,
    bitrateKbps: raw.bitrateKbps,
    width: raw.width,
    height: raw.height,
    batteryPercent: raw.batteryPercent,
    charging: raw.charging,
    thermal: raw.thermal,
    thermalLevel: raw.thermalLevel,
  };
}

export function validateSessionInfo(message: unknown): SessionInfoMessage {
  if (!message || typeof message !== 'object' || Array.isArray(message)) {
    throw new Error('Некорректное описание сеанса.');
  }
  const raw = message as Record<string, unknown>;
  if (raw.v !== 1) throw new Error('Некорректная версия описания сеанса.');
  if (raw.type !== 'session-info') throw new Error('Ожидалось описание сеанса.');
  if (typeof raw.mode !== 'string' || !STREAM_MODES.has(raw.mode)) {
    throw new Error('Некорректный режим трансляции на телефоне.');
  }
  if (typeof raw.quality !== 'string' || !QUALITY_PROFILES.has(raw.quality)) {
    throw new Error('Некорректный профиль качества на телефоне.');
  }
  return { v: 1, type: 'session-info', mode: raw.mode as StreamMode, quality: raw.quality as QualityProfile };
}

export function encodeSessionControl(message: SessionControlMessage): Uint8Array {
  if (!message || typeof message !== 'object' || (message as Record<string, unknown>).v !== 1) {
    throw new Error('Некорректная версия команды сеанса.');
  }
  let payload: string;
  if (message.type === 'stop-stream') {
    payload = JSON.stringify({ v: 1, type: 'stop-stream' });
  } else if (message.type === 'quality') {
    if (!QUALITY_PROFILES.has(message.value)) {
      throw new Error('Некорректный профиль качества.');
    }
    payload = JSON.stringify({ v: 1, type: 'quality', value: message.value });
  } else {
    throw new Error('Неизвестная команда сеанса.');
  }
  const bytes = new TextEncoder().encode(payload);
  if (bytes.byteLength === 0 || bytes.byteLength > MAX_CONTROL_MESSAGE_BYTES) {
    throw new Error('Команда сеанса превышает допустимый размер.');
  }
  return bytes;
}

export function decodeSessionMessage(bytes: Uint8Array): SessionMessage {
  if (bytes.byteLength === 0 || bytes.byteLength > MAX_CONTROL_MESSAGE_BYTES) {
    throw new Error('Некорректный размер сообщения канала управления.');
  }
  const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  const parsed: unknown = JSON.parse(text);
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    throw new Error('Некорректное сообщение канала управления.');
  }
  const type = (parsed as Record<string, unknown>).type;
  if (type === 'telemetry') return validateTelemetry(parsed);
  if (type === 'session-info') return validateSessionInfo(parsed);
  throw new Error('Неизвестный тип сообщения канала управления.');
}
