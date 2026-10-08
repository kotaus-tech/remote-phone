import assert from 'node:assert/strict';
import test from 'node:test';
import { RtcCameraSession } from './rtcCameraSession.ts';

function installBrowserMocks({ videoFrameLayout, videoFrameError } = {}) {
  const previous = new Map();
  const globals = ['window', 'document', 'MediaStream', 'VideoFrame', 'RTCPeerConnection'];
  for (const name of globals) previous.set(name, Object.getOwnPropertyDescriptor(globalThis, name));

  let nextId = 1;
  const timers = new Map();
  const fakeWindow = {
    setTimeout(callback, delay) {
      const id = nextId++;
      timers.set(id, { callback, delay });
      return id;
    },
    clearTimeout(id) {
      timers.delete(id);
    },
    requestAnimationFrame(callback) {
      const id = nextId++;
      timers.set(id, { callback, animationFrame: true });
      return id;
    },
    cancelAnimationFrame(id) {
      timers.delete(id);
    },
  };

  class FakeDataChannel {
    constructor(label) {
      this.label = label;
      this.closed = false;
      this.onmessage = null;
    }
    close() { this.closed = true; }
  }

  class FakePeerConnection {
    constructor(configuration) {
      this.configuration = configuration;
      this.connectionState = 'new';
      this.signalingState = 'stable';
      this.localDescription = null;
      this.remoteDescription = null;
      this.transceivers = [];
      this.addedCandidates = [];
      this.closed = false;
      this.localDataChannel = null;
      FakePeerConnection.last = this;
    }
    addTransceiver(kind, options) {
      this.transceivers.push({ kind, options });
    }
    createDataChannel(label, options) {
      this.dataChannelOptions = options;
      this.localDataChannel = new FakeDataChannel(label);
      return this.localDataChannel;
    }
    async createOffer() {
      return { type: 'offer', sdp: 'v=0\r\na=group:BUNDLE 0\r\n' };
    }
    async setLocalDescription(description) {
      this.localDescription = description;
      this.signalingState = 'have-local-offer';
      this.onicecandidate?.({ candidate: {
        candidate: 'candidate:1 1 UDP 1 192.168.1.2 5000 typ host',
        sdpMid: '0',
        sdpMLineIndex: 0,
      } });
    }
    async setRemoteDescription(description) {
      this.remoteDescription = description;
      this.signalingState = 'stable';
    }
    async addIceCandidate(candidate) {
      this.addedCandidates.push(candidate);
    }
    getReceivers() { return []; }
    close() { this.closed = true; this.connectionState = 'closed'; }
  }

  class FakeMediaStream {
    constructor(tracks) { this.tracks = tracks; }
  }

  class FakeVideoFrame {
    constructor(_source, init) {
      this.codedWidth = 4;
      this.codedHeight = 2;
      this.timestamp = init.timestamp;
      this.closed = false;
      FakeVideoFrame.last = this;
    }
    async copyTo(destination, options) {
      this.copyOptions = options;
      if (videoFrameError) throw videoFrameError;
      assert.equal(options.format, 'RGBA');
      destination.set([
        255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255,
      ]);
      return videoFrameLayout ? videoFrameLayout(options.layout) : options.layout;
    }
    close() { this.closed = true; }
  }

  const videos = [];
  globalThis.window = fakeWindow;
  globalThis.document = {
    body: { appendChild(video) { video.appended = true; } },
    createElement(tag) {
      assert.equal(tag, 'video');
      const callbacks = new Map();
      const video = {
        muted: false,
        autoplay: false,
        playsInline: false,
        tabIndex: 0,
        videoWidth: 4,
        videoHeight: 2,
        currentTime: 0,
        srcObject: null,
        style: { cssText: '' },
        removed: false,
        paused: false,
        nextCallbackId: 1,
        setAttribute() {},
        async play() {},
        pause() { this.paused = true; },
        remove() { this.removed = true; },
        requestVideoFrameCallback(callback) {
          const id = this.nextCallbackId++;
          callbacks.set(id, callback);
          this.callbacks = callbacks;
          return id;
        },
        cancelVideoFrameCallback(id) {
          callbacks.delete(id);
          this.cancelledCallback = id;
        },
        emitVideoFrame(mediaTime) {
          const entry = callbacks.entries().next().value;
          if (!entry) throw new Error('No scheduled video-frame callback');
          const [id, callback] = entry;
          callbacks.delete(id);
          callback(0, { mediaTime });
        },
      };
      videos.push(video);
      return video;
    },
  };
  globalThis.MediaStream = FakeMediaStream;
  globalThis.VideoFrame = FakeVideoFrame;
  globalThis.RTCPeerConnection = FakePeerConnection;

  return {
    timers,
    videos,
    get peer() { return FakePeerConnection.last; },
    get lastVideoFrame() { return FakeVideoFrame.last; },
    restore() {
      for (const [name, descriptor] of previous) {
        if (descriptor) Object.defineProperty(globalThis, name, descriptor);
        else delete globalThis[name];
      }
    },
  };
}

function createApi() {
  const sentSignals = [];
  const writtenFrames = [];
  return {
    sentSignals,
    writtenFrames,
    async sendRtcSignal(signal) {
      sentSignals.push(signal);
      return { ok: true };
    },
    async writeNv12Frame(frame) {
      writtenFrames.push({
        width: frame.width,
        height: frame.height,
        timestampNs: frame.timestampNs,
        data: Array.from(frame.data),
      });
      return { ok: true, dropped: false };
    },
  };
}

async function settleAsyncWork() {
  await new Promise((resolve) => setImmediate(resolve));
  await new Promise((resolve) => setImmediate(resolve));
}

test('защищённый offer уходит раньше отложенных ICE-кандидатов; удалённые ICE ждут answer', async (t) => {
  const browser = installBrowserMocks();
  t.after(browser.restore);
  const api = createApi();
  const session = new RtcCameraSession(api, () => {});

  await session.start();
  assert.deepEqual(api.sentSignals.map((signal) => signal.type), ['offer', 'ice']);
  assert.equal(api.sentSignals[0].sdp, 'v=0\r\na=group:BUNDLE 0\r\n');
  assert.deepEqual(browser.peer.configuration, { iceServers: [] });
  assert.deepEqual(browser.peer.transceivers, [{ kind: 'video', options: { direction: 'recvonly' } }]);

  await session.handleSignal({
    v: 1,
    type: 'ice',
    candidate: 'candidate:2 1 UDP 1 192.168.1.3 5001 typ host',
    sdpMid: '0',
    sdpMLineIndex: 0,
  });
  assert.equal(browser.peer.addedCandidates.length, 0);

  await session.handleSignal({ v: 1, type: 'answer', sdp: 'v=0\r\na=recvonly\r\n' });
  assert.equal(browser.peer.remoteDescription.type, 'answer');
  assert.equal(browser.peer.addedCandidates.length, 1);
  assert.equal(browser.peer.localDataChannel.label, 'remote-phone-control');
  assert.deepEqual(browser.peer.dataChannelOptions, { ordered: true });

  session.stop();
  assert.equal(browser.peer.closed, true);
  assert.equal(browser.peer.localDataChannel.closed, true);
  assert.equal(browser.timers.size, 0);
});

test('копирует входящий VideoFrame в RGBA, конвертирует в NV12 и передаёт кадр', async (t) => {
  const browser = installBrowserMocks();
  t.after(browser.restore);
  const api = createApi();
  const statuses = [];
  const session = new RtcCameraSession(api, (status) => statuses.push(status));
  const track = {
    kind: 'video',
    readyState: 'live',
    onended: null,
    stopped: false,
    stop() { this.stopped = true; },
  };

  await session.start();
  const peer = browser.peer;
  peer.connectionState = 'connected';
  peer.onconnectionstatechange();
  peer.ontrack({ track });
  await settleAsyncWork();

  assert.equal(browser.videos.length, 1);
  const video = browser.videos[0];
  assert.equal(video.appended, true);
  assert.equal(video.muted, true);
  video.emitVideoFrame(1.25);
  await settleAsyncWork();

  assert.equal(api.writtenFrames.length, 1);
  assert.deepEqual(api.writtenFrames[0], {
    width: 4,
    height: 2,
    timestampNs: '1250000000',
    data: [63, 63, 173, 173, 63, 63, 173, 173, 102, 240, 42, 26],
  });
  assert.deepEqual(browser.lastVideoFrame.copyOptions, {
    format: 'RGBA',
    layout: [{ offset: 0, stride: 16 }],
  });
  assert.equal(browser.lastVideoFrame.closed, true);
  assert.ok(statuses.some((status) => status.phase === 'receiving' && status.frames === 1));
  assert.equal(video.callbacks.size, 1, 'a subsequent frame callback should be scheduled');

  session.stop();
  assert.equal(track.stopped, true);
  assert.equal(video.paused, true);
  assert.equal(video.removed, true);
  assert.equal(video.cancelledCallback, 2);
  assert.equal(peer.closed, true);
  assert.equal(browser.timers.size, 0);
});

test('отклоняет RGBA-разметку с padding и не передаёт невалидный кадр в host', async (t) => {
  const browser = installBrowserMocks({
    videoFrameLayout: (layout) => [{ offset: layout[0].offset, stride: layout[0].stride + 4 }],
  });
  t.after(browser.restore);
  const api = createApi();
  const statuses = [];
  const session = new RtcCameraSession(api, (status) => statuses.push(status));
  const track = {
    kind: 'video',
    readyState: 'live',
    onended: null,
    stopped: false,
    stop() { this.stopped = true; },
  };

  await session.start();
  browser.peer.ontrack({ track });
  await settleAsyncWork();
  browser.videos[0].emitVideoFrame(0.5);
  await settleAsyncWork();

  assert.equal(api.writtenFrames.length, 0);
  assert.ok(statuses.some((status) => status.phase === 'error'
    && status.message.includes('плотную одноплоскостную разметку RGBA')));
  assert.equal(api.sentSignals.at(-1).type, 'bye');
  assert.equal(browser.lastVideoFrame.closed, true);
  assert.equal(track.stopped, true);
  assert.equal(browser.peer.closed, true);
  assert.equal(browser.videos[0].removed, true);
});


test('сообщает об ошибке чтения RGBA и завершает неработающий сеанс', async (t) => {
  const browser = installBrowserMocks({ videoFrameError: new Error('This pixel format conversion is not supported.') });
  t.after(browser.restore);
  const api = createApi();
  const statuses = [];
  const session = new RtcCameraSession(api, (status) => statuses.push(status));
  const track = {
    kind: 'video',
    readyState: 'live',
    onended: null,
    stopped: false,
    stop() { this.stopped = true; },
  };

  await session.start();
  browser.peer.ontrack({ track });
  await settleAsyncWork();
  browser.videos[0].emitVideoFrame(0.5);
  await settleAsyncWork();

  assert.equal(api.writtenFrames.length, 0);
  assert.ok(statuses.some((status) => status.phase === 'error'
    && status.message.includes('Не удалось скопировать кадр WebRTC в RGBA')
    && status.message.includes('This pixel format conversion is not supported.')));
  assert.equal(api.sentSignals.at(-1).type, 'bye');
  assert.equal(browser.lastVideoFrame.closed, true);
  assert.equal(track.stopped, true);
  assert.equal(browser.peer.closed, true);
});

test('сбрасывает статус и закрывает ресурсы при завершении видеосеанса телефоном', async (t) => {
  const browser = installBrowserMocks();
  t.after(browser.restore);
  const api = createApi();
  const statuses = [];
  const session = new RtcCameraSession(api, (status) => statuses.push(status));

  await session.start();
  await session.handleSignal({ v: 1, type: 'bye' });

  assert.deepEqual(statuses.at(-1), {
    phase: 'idle',
    message: 'Телефон завершил видеосеанс.',
  });
  assert.equal(browser.peer.closed, true);
  assert.equal(browser.peer.localDataChannel.closed, true);
  assert.equal(browser.timers.size, 0);
});
