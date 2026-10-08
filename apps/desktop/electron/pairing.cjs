const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const koffi = require('koffi');
const WebSocket = require('ws');
const { Bonjour } = require('bonjour-service');
const { encodeRtcSignal, decodeRtcSignal } = require('./rtc-signaling.cjs');

const MAX_FRAME_BYTES = 256 * 1024;
const PIN_LENGTH = 8;
const PAIRING_SERVICE_TYPE = 'remotephone';
const AUTHENTICATION_TIMEOUT_MS = 45_000;

function isLocalAddress(address) {
  const family = net.isIP(address);
  if (family === 4) {
    const octets = address.split('.').map(Number);
    const [first, second] = octets;
    return first === 10
      || (first === 172 && second >= 16 && second <= 31)
      || (first === 192 && second === 168)
      || (first === 169 && second === 254);
  }
  if (family === 6) {
    const normalized = address.toLowerCase().split('%')[0];
    return normalized === '::1'
      || normalized.startsWith('fc')
      || normalized.startsWith('fd')
      || normalized.startsWith('fe8')
      || normalized.startsWith('fe9')
      || normalized.startsWith('fea')
      || normalized.startsWith('feb');
  }
  return false;
}

function toWebSocketHost(address) {
  return net.isIPv6(address) ? `[${address}]` : address;
}

function loadNativeBridge() {
  const candidates = [];
  if (process.resourcesPath) {
    candidates.push(path.join(process.resourcesPath, 'native', 'remote_phone_pairing_bridge.dll'));
  }
  candidates.push(path.join(__dirname, '..', 'native-runtime', 'remote_phone_pairing_bridge.dll'));
  const libraryPath = candidates.find((candidate) => fs.existsSync(candidate));
  if (!libraryPath) {
    throw new Error('Не найдена нативная библиотека сопряжения.');
  }

  const library = koffi.load(libraryPath);
  return {
    pcStart: library.func('uint64_t rp_pc_start(const uint8_t *pin, size_t pin_length, const uint8_t *hello_frame, size_t hello_length)'),
    pcTakeInitialFrame: library.func('int64_t rp_pc_take_initial_frame(uint64_t handle, uint8_t *frame_out, size_t frame_capacity)'),
    pcHandleFrame: library.func('int64_t rp_pc_handle_frame(uint64_t handle, const uint8_t *frame, size_t frame_length, uint8_t *reply_out, size_t reply_capacity)'),
    pcIsAuthenticated: library.func('int32_t rp_pc_is_authenticated(uint64_t handle)'),
    pcDestroy: library.func('int32_t rp_pc_destroy(uint64_t handle)'),
    pcEncryptSignal: library.func('int64_t rp_pc_encrypt_signal(uint64_t handle, const uint8_t *plaintext, size_t plaintext_length, uint8_t *frame_out, size_t frame_capacity)'),
    pcDecryptSignal: library.func('int64_t rp_pc_decrypt_signal(uint64_t handle, const uint8_t *frame, size_t frame_length, uint8_t *plaintext_out, size_t plaintext_capacity)'),
  };
}

class PairingController {
  constructor(sendToRenderer) {
    this.sendToRenderer = sendToRenderer;
    this.bonjour = null;
    this.browser = null;
    this.devices = new Map();
    this.native = null;
    this.activeConnection = null;
    this.discoveryError = false;
  }

  startDiscovery() {
    if (this.browser) return;
    try {
      this.bonjour = new Bonjour();
      this.browser = this.bonjour.find({ type: PAIRING_SERVICE_TYPE, protocol: 'tcp' });
      this.browser.on('up', (service) => this.addService(service));
      this.browser.on('down', (service) => this.removeService(service));
      this.browser.on('error', () => this.setDiscoveryError());
      this.sendStatus('discovering', 'Ищем телефоны в локальной сети…');
      this.sendDevices();
    } catch (_error) {
      this.setDiscoveryError();
    }
  }

  getDevices() {
    return [...this.devices.values()];
  }

  isAuthenticated() {
    const connection = this.activeConnection;
    return Boolean(connection?.authenticated && connection.socket.readyState === WebSocket.OPEN);
  }

  refreshDiscovery() {
    if (!this.browser) {
      this.startDiscovery();
      return;
    }
    try {
      this.browser.update();
    } catch (_error) {
      this.setDiscoveryError();
    }
  }

  async connect(request) {
    if (this.activeConnection) {
      return { ok: false, message: 'Сначала завершите текущее подключение.' };
    }
    const address = typeof request?.address === 'string' ? request.address.trim() : '';
    const port = Number(request?.port);
    const pin = typeof request?.pin === 'string' ? request.pin : '';
    if (!isLocalAddress(address)) {
      return { ok: false, message: 'Укажите IP-адрес телефона в локальной сети.' };
    }
    if (!Number.isInteger(port) || port < 1 || port > 65535) {
      return { ok: false, message: 'Проверьте порт телефона.' };
    }
    if (!/^\d{8}$/.test(pin)) {
      return { ok: false, message: 'PIN должен состоять ровно из восьми цифр.' };
    }

    let native;
    try {
      native = this.native || (this.native = loadNativeBridge());
    } catch (_error) {
      return { ok: false, message: 'Не удалось загрузить общий модуль защищённого сопряжения.' };
    }

    const pinBytes = Buffer.from(pin, 'ascii');
    this.sendStatus('connecting', 'Открываем локальное соединение и проверяем PIN…');
    return new Promise((resolve) => {
      const host = toWebSocketHost(address);
      const socket = new WebSocket(`ws://${host}:${port}`, {
        maxPayload: MAX_FRAME_BYTES,
        perMessageDeflate: false,
        handshakeTimeout: 8000,
      });
      const connection = {
        socket,
        handle: 0n,
        stage: 'hello',
        pinBytes,
        resolved: false,
        timer: null,
        authenticated: false,
        intentionalClose: false,
      };
      this.activeConnection = connection;

      const resolveOnce = (result) => {
        if (connection.resolved) return;
        connection.resolved = true;
        resolve(result);
      };

      connection.timer = setTimeout(() => {
        this.fail(connection, 'Не удалось завершить сопряжение за отведённое время. Проверьте сеть и PIN.');
      }, AUTHENTICATION_TIMEOUT_MS);

      socket.on('open', () => {
        this.sendStatus('connecting', 'Соединение установлено. Проверяем защищённое рукопожатие…');
      });
      socket.on('message', (data, isBinary) => {
        try {
          if (!isBinary) throw new Error('Ожидалось двоичное сообщение протокола.');
          const frame = Buffer.from(data);
          try {
            this.handleFrame(connection, frame, resolveOnce);
          } finally {
            frame.fill(0);
          }
        } catch (_error) {
          this.fail(connection, 'Не удалось подтвердить PIN. Проверьте код на телефоне и попробуйте снова.');
        }
      });
      socket.on('error', () => {
        this.fail(connection, 'Не удалось подключиться к телефону. Проверьте адрес и локальную сеть.');
      });
      socket.on('close', () => {
        clearTimeout(connection.timer);
        if (this.activeConnection !== connection) return;
        connection.pinBytes.fill(0);
        this.destroyNativeSession(connection);
        this.activeConnection = null;
        if (connection.authenticated) {
          this.sendStatus('closed', 'Защищённое соединение закрыто. Создайте новый PIN на телефоне для следующего сеанса.');
        } else if (!connection.intentionalClose) {
          this.sendStatus('error', 'Соединение закрыто до завершения сопряжения. Проверьте PIN и попробуйте снова.');
          resolveOnce({ ok: false, message: 'Соединение закрыто до завершения сопряжения.' });
        }
      });

      connection.resolveOnce = resolveOnce;
    });
  }

  handleFrame(connection, frame, resolveOnce) {
    if (frame.length === 0 || frame.length > MAX_FRAME_BYTES) {
      throw new Error('Размер сообщения не соответствует протоколу.');
    }
    const native = this.native;
    if (!native) throw new Error('Общий модуль сопряжения недоступен.');

    if (connection.stage === 'hello') {
      const handle = native.pcStart(connection.pinBytes, connection.pinBytes.length, frame, frame.length);
      connection.pinBytes.fill(0);
      if (handle === 0n || handle === 0) throw new Error('Не удалось начать обмен PIN.');
      connection.handle = handle;
      connection.stage = 'login2';
      const output = Buffer.alloc(MAX_FRAME_BYTES);
      const length = Number(native.pcTakeInitialFrame(connection.handle, output, output.length));
      this.sendNativeFrame(connection, output, length);
      return;
    }

    if (connection.handle === 0n) throw new Error('Сеанс сопряжения не создан.');
    if (connection.authenticated) {
      const plaintext = Buffer.alloc(MAX_FRAME_BYTES);
      try {
        const length = Number(native.pcDecryptSignal(
          connection.handle,
          frame,
          frame.length,
          plaintext,
          plaintext.length,
        ));
        if (length <= 0 || length > plaintext.length) {
          throw new Error('Не удалось проверить защищённую сигнализацию.');
        }
        const signal = decodeRtcSignal(plaintext.subarray(0, length));
        this.sendToRenderer('rtc:signal', signal);
      } finally {
        plaintext.fill(0);
      }
      return;
    }

    const output = Buffer.alloc(MAX_FRAME_BYTES);
    const length = Number(native.pcHandleFrame(
      connection.handle,
      frame,
      frame.length,
      output,
      output.length,
    ));
    if (!Number.isInteger(length) || length < 0 || length > output.length) {
      throw new Error('Обмен PIN отклонён.');
    }
    if (length > 0) this.sendNativeFrame(connection, output, length);
    if (native.pcIsAuthenticated(connection.handle) === 1) {
      connection.authenticated = true;
      connection.stage = 'authenticated';
      clearTimeout(connection.timer);
      this.sendStatus('authenticated', 'PIN-сопряжение подтверждено. Настраиваем WebRTC-видеоканал; изображение ещё не подтверждено.');
      resolveOnce({ ok: true });
    }
  }

  sendRtcSignal(signal) {
    const connection = this.activeConnection;
    if (!connection || !connection.authenticated || connection.handle === 0n
      || connection.socket.readyState !== WebSocket.OPEN || !this.native) {
      return { ok: false, message: 'Нет активного защищённого WebRTC-сеанса.' };
    }

    let plaintext;
    const output = Buffer.alloc(MAX_FRAME_BYTES);
    try {
      plaintext = encodeRtcSignal(signal);
      const length = Number(this.native.pcEncryptSignal(
        connection.handle,
        plaintext,
        plaintext.length,
        output,
        output.length,
      ));
      if (!Number.isInteger(length) || length <= 0 || length > output.length) {
        throw new Error('Не удалось зашифровать WebRTC-сигнализацию.');
      }
      this.sendNativeFrame(connection, output, length);
      return { ok: true };
    } catch (_error) {
      this.fail(connection, 'Не удалось отправить защищённую WebRTC-сигнализацию.');
      return { ok: false, message: 'Не удалось отправить защищённую WebRTC-сигнализацию.' };
    } finally {
      plaintext?.fill(0);
      output.fill(0);
    }
  }

  sendNativeFrame(connection, output, length) {
    if (!Number.isInteger(length) || length <= 0 || length > output.length) {
      throw new Error('Не удалось сформировать сообщение сопряжения.');
    }
    const bytes = Buffer.from(output.subarray(0, length));
    connection.socket.send(bytes, { binary: true }, (error) => {
      bytes.fill(0);
      if (error) this.fail(connection, 'Не удалось отправить сообщение сопряжения.');
    });
    output.fill(0);
  }

  fail(connection, message) {
    if (this.activeConnection !== connection) return;
    clearTimeout(connection.timer);
    connection.intentionalClose = true;
    connection.pinBytes.fill(0);
    this.destroyNativeSession(connection);
    this.activeConnection = null;
    this.sendStatus('error', message);
    if (connection.socket.readyState === WebSocket.CONNECTING) {
      connection.socket.terminate();
    } else if (connection.socket.readyState === WebSocket.OPEN) {
      connection.socket.close(1008, 'Сопряжение отклонено');
    }
    if (connection.resolveOnce) connection.resolveOnce({ ok: false, message });
  }

  disconnect() {
    const connection = this.activeConnection;
    if (!connection) return;
    connection.intentionalClose = true;
    clearTimeout(connection.timer);
    connection.pinBytes.fill(0);
    this.destroyNativeSession(connection);
    this.activeConnection = null;
    this.sendStatus('closed', 'Подключение завершено. PIN и временное состояние удалены.');
    if (connection.socket.readyState === WebSocket.CONNECTING) {
      connection.socket.terminate();
    } else if (connection.socket.readyState === WebSocket.OPEN) {
      connection.socket.close(1000, 'Подключение завершено');
    }
    if (connection.resolveOnce) connection.resolveOnce({ ok: false, message: 'Подключение завершено.' });
  }

  destroyNativeSession(connection) {
    if (connection.handle !== 0n && connection.handle !== 0 && this.native) {
      try {
        this.native.pcDestroy(connection.handle);
      } catch (_error) {
        // Native state is already being discarded; do not expose low-level errors to the UI.
      }
      connection.handle = 0n;
    }
  }

  addService(service) {
    const txt = service.txt || {};
    if (String(txt.version) !== '1' || String(txt.profile) !== '0001') return;
    const addresses = Array.isArray(service.addresses) ? service.addresses : [];
    const address = addresses.find((candidate) => net.isIP(candidate) === 4 && isLocalAddress(candidate))
      || addresses.find((candidate) => isLocalAddress(candidate));
    if (!address || !Number.isInteger(service.port) || service.port < 1 || service.port > 65535) return;
    const id = `${service.fqdn || service.name}|${address}|${service.port}`;
    this.devices.set(id, {
      id,
      name: String(service.name || 'Телефон'),
      address,
      port: service.port,
    });
    this.sendDevices();
  }

  removeService(service) {
    const serviceName = String(service.fqdn || service.name || '');
    for (const [id, device] of this.devices.entries()) {
      if (id.startsWith(`${serviceName}|`) || device.name === service.name) this.devices.delete(id);
    }
    this.sendDevices();
  }

  setDiscoveryError() {
    this.discoveryError = true;
    this.sendStatus('discovery-error', 'Автоматический поиск недоступен. Можно указать IP-адрес и порт вручную.');
    this.sendDevices();
  }

  sendDevices() {
    this.sendToRenderer('pairing:devices', this.getDevices());
  }

  sendStatus(phase, message) {
    this.sendToRenderer('pairing:status', { phase, message });
  }

  shutdown() {
    this.disconnect();
    if (this.browser) {
      try {
        this.browser.stop();
      } catch (_error) {
        // Continue shutting down local discovery.
      }
      this.browser = null;
    }
    if (this.bonjour) {
      try {
        this.bonjour.destroy();
      } catch (_error) {
        // The process is already exiting.
      }
      this.bonjour = null;
    }
    this.devices.clear();
  }
}

module.exports = { PairingController, isLocalAddress, loadNativeBridge };
