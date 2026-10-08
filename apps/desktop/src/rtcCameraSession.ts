import type { RtcSignal } from './remotePhone';

type CameraTransportPhase = 'idle' | 'negotiating' | 'connected' | 'receiving' | 'error';

export type CameraTransportStatus = {
  phase: CameraTransportPhase;
  message: string;
  width?: number;
  height?: number;
  frames?: number;
  dropped?: number;
};

type VideoPlaneLayout = { offset: number; stride: number };

type WebCodecsVideoFrame = {
  codedWidth: number;
  codedHeight: number;
  timestamp: number;
  copyTo: (destination: Uint8Array, options: {
    format: 'RGBA';
    layout: VideoPlaneLayout[];
  }) => Promise<VideoPlaneLayout[]>;
  close: () => void;
};

type VideoFrameConstructor = new (
  source: HTMLVideoElement,
  init: { timestamp: number },
) => WebCodecsVideoFrame;

type VideoFrameMetadata = { mediaTime: number };
type VideoFrameCallback = (now: number, metadata: VideoFrameMetadata) => void;
type VideoElementWithFrameCallbacks = HTMLVideoElement & {
  requestVideoFrameCallback?: (callback: VideoFrameCallback) => number;
  cancelVideoFrameCallback?: (id: number) => void;
};

type ScheduledVideoFrame = {
  kind: 'video' | 'animation';
  id: number;
  element: VideoElementWithFrameCallbacks;
};

type VideoFrameScratchBuffers = {
  rgba: Uint8Array | null;
  nv12: Uint8Array | null;
};

type CameraTransportApi = {
  sendRtcSignal: (signal: RtcSignal) => Promise<{ ok: boolean; message?: string }>;
  writeNv12Frame: (frame: {
    width: number;
    height: number;
    timestampNs: string;
    data: Uint8Array;
  }) => Promise<{ ok: boolean; dropped?: boolean; message?: string }>;
};

const MAX_WIDTH = 3840;
const MAX_HEIGHT = 2160;
const MAX_NV12_BYTES = MAX_WIDTH * MAX_HEIGHT * 3 / 2;
const MAX_RGBA_BYTES = MAX_WIDTH * MAX_HEIGHT * 4;
const MAX_RTC_CANDIDATES = 128;
const CONNECTION_TIMEOUT_MS = 30_000;
const FIRST_FRAME_TIMEOUT_MS = 15_000;

export class RtcCameraSession {
  private readonly api: CameraTransportApi;
  private readonly onStatus: (status: CameraTransportStatus) => void;
  private peer: RTCPeerConnection | null = null;
  private controlChannel: RTCDataChannel | null = null;
  private video: HTMLVideoElement | null = null;
  private videoTrack: MediaStreamTrack | null = null;
  private scheduledVideoFrame: ScheduledVideoFrame | null = null;
  private stopped = false;
  private starting = false;
  private remoteDescriptionReady = false;
  private offerSent = false;
  private pendingRemoteCandidates: RTCIceCandidateInit[] = [];
  private pendingLocalCandidates: RtcSignal[] = [];
  private localCandidateCount = 0;
  private remoteCandidateCount = 0;
  private framesWritten = 0;
  private framesDropped = 0;
  private reportedWriteFailure = false;
  private readonly frameScratch: VideoFrameScratchBuffers = { rgba: null, nv12: null };
  private connectionTimer: number | null = null;
  private firstFrameTimer: number | null = null;

  constructor(api: CameraTransportApi, onStatus: (status: CameraTransportStatus) => void) {
    this.api = api;
    this.onStatus = onStatus;
  }

  async start(): Promise<void> {
    if (this.starting || this.peer || this.stopped) return;
    this.starting = true;
    this.onStatus({ phase: 'negotiating', message: 'Согласуем защищённый видеоканал…' });

    try {
      const peer = new RTCPeerConnection({ iceServers: [] });
      this.peer = peer;
      peer.addTransceiver('video', { direction: 'recvonly' });
      this.controlChannel = peer.createDataChannel('remote-phone-control', { ordered: true });
      this.controlChannel.onmessage = () => {
        // Отдельный канал зарезервирован для согласованного протокола; ввод и текстовые команды не принимаются.
        this.controlChannel?.close();
        this.controlChannel = null;
      };
      peer.ondatachannel = (event) => {
        if (event.channel.label !== 'remote-phone-control') {
          event.channel.close();
          return;
        }
        event.channel.onmessage = () => event.channel.close();
      };
      peer.ontrack = (event) => {
        if (event.track.kind !== 'video' || this.stopped) {
          event.track.stop();
          return;
        }
        void this.attachRemoteTrack(event.track).catch((error: unknown) => {
          this.fail(error instanceof Error ? error.message : 'Не удалось подготовить входящее видео.');
        });
      };
      peer.onicecandidate = (event) => {
        const candidate = event.candidate;
        if (!candidate || !candidate.candidate) return;
        this.localCandidateCount += 1;
        if (this.localCandidateCount > MAX_RTC_CANDIDATES) {
          this.fail('Слишком много ICE-кандидатов в локальном WebRTC-сеансе.');
          return;
        }
        const signal: RtcSignal = {
          v: 1,
          type: 'ice',
          candidate: candidate.candidate,
          sdpMid: candidate.sdpMid,
          sdpMLineIndex: candidate.sdpMLineIndex,
        };
        if (!this.offerSent) {
          this.pendingLocalCandidates.push(signal);
          return;
        }
        void this.sendSignal(signal).catch((error: unknown) => {
          this.fail(error instanceof Error ? error.message : 'Не удалось передать ICE-кандидат.');
        });
      };
      peer.onconnectionstatechange = () => {
        if (this.stopped || this.peer !== peer) return;
        if (peer.connectionState === 'connected') {
          this.clearConnectionTimer();
          this.clearFirstFrameTimer();
          this.firstFrameTimer = window.setTimeout(() => {
            if (!this.stopped && this.framesWritten === 0) {
              this.fail('WebRTC подключён, но видеокадр от телефона не поступил.');
            }
          }, FIRST_FRAME_TIMEOUT_MS);
          this.onStatus({ phase: 'connected', message: 'WebRTC подключён; ожидаем первый кадр камеры телефона.' });
        } else if (peer.connectionState === 'failed' || peer.connectionState === 'closed') {
          this.fail('Не удалось установить локальный видеоканал WebRTC.');
        } else if (peer.connectionState === 'disconnected') {
          this.onStatus({ phase: 'connected', message: 'Восстанавливаем локальный видеоканал…' });
        }
      };

      const offer = await peer.createOffer();
      await peer.setLocalDescription(offer);
      this.connectionTimer = window.setTimeout(() => {
        if (!this.stopped && peer.connectionState !== 'connected') {
          this.fail('Не удалось установить WebRTC-соединение за отведённое время.');
        }
      }, CONNECTION_TIMEOUT_MS);
      const result = await this.api.sendRtcSignal({
        v: 1,
        type: 'offer',
        sdp: peer.localDescription?.sdp || offer.sdp || '',
      });
      if (!result.ok) throw new Error(result.message || 'Не удалось отправить защищённое предложение WebRTC.');
      this.offerSent = true;
      for (const candidate of this.pendingLocalCandidates.splice(0)) {
        await this.sendSignal(candidate);
      }
    } catch (error) {
      this.fail(error instanceof Error ? error.message : 'Не удалось запустить видеосеанс.');
      throw error;
    } finally {
      this.starting = false;
    }
  }

  async handleSignal(signal: RtcSignal): Promise<void> {
    if (this.stopped) return;
    if (signal.type === 'error') {
      this.fail(signal.code === 'CAMERA_NOT_ENABLED'
        ? 'На телефоне не выбран режим «Веб-камера» или не разрешён доступ к камере.'
        : `Телефон отклонил настройку видеоканала (${signal.code}).`);
      return;
    }
    if (signal.type === 'bye') {
      this.stop();
      this.onStatus({ phase: 'idle', message: 'Телефон завершил видеосеанс.' });
      return;
    }

    const peer = this.peer;
    if (!peer) return;
    try {
      if (signal.type === 'answer') {
        if (peer.signalingState !== 'have-local-offer') {
          throw new Error('Получен ответ WebRTC вне ожидаемой стадии.');
        }
        await peer.setRemoteDescription({ type: 'answer', sdp: signal.sdp });
        this.remoteDescriptionReady = true;
        for (const candidate of this.pendingRemoteCandidates.splice(0)) {
          await peer.addIceCandidate(candidate);
        }
        return;
      }

      if (signal.type === 'ice') {
        this.remoteCandidateCount += 1;
        if (this.remoteCandidateCount > MAX_RTC_CANDIDATES) {
          throw new Error('Телефон прислал слишком много ICE-кандидатов.');
        }
        const candidate: RTCIceCandidateInit = {
          candidate: signal.candidate,
          sdpMid: signal.sdpMid,
          sdpMLineIndex: signal.sdpMLineIndex,
        };
        if (!this.remoteDescriptionReady) {
          this.pendingRemoteCandidates.push(candidate);
        } else {
          await peer.addIceCandidate(candidate);
        }
        return;
      }

      throw new Error('Телефон прислал неожиданный тип WebRTC-сигнализации.');
    } catch (error) {
      this.fail(error instanceof Error ? error.message : 'Некорректная WebRTC-сигнализация.');
    }
  }

  stop(): void {
    if (this.stopped) return;
    this.stopped = true;
    this.clearConnectionTimer();
    this.clearFirstFrameTimer();
    this.cancelScheduledVideoFrame();

    if (this.controlChannel) {
      this.controlChannel.onmessage = null;
      this.controlChannel.close();
      this.controlChannel = null;
    }

    const video = this.video;
    this.video = null;
    if (video) {
      video.pause();
      video.srcObject = null;
      video.remove();
    }
    const videoTrack = this.videoTrack;
    this.videoTrack = null;
    if (videoTrack) {
      videoTrack.onended = null;
      videoTrack.stop();
    }

    if (this.peer) {
      this.peer.ontrack = null;
      this.peer.onicecandidate = null;
      this.peer.ondatachannel = null;
      this.peer.onconnectionstatechange = null;
      this.peer.getReceivers().forEach((receiver) => receiver.track?.stop());
      this.peer.close();
      this.peer = null;
    }
    this.pendingLocalCandidates = [];
    this.pendingRemoteCandidates = [];
  }

  private async sendSignal(signal: RtcSignal): Promise<void> {
    const result = await this.api.sendRtcSignal(signal);
    if (!result.ok) throw new Error(result.message || 'Не удалось передать сигнал WebRTC.');
  }

  private async attachRemoteTrack(track: MediaStreamTrack): Promise<void> {
    if (this.stopped) return;
    if (this.videoTrack && this.videoTrack !== track) {
      track.stop();
      return;
    }
    this.videoTrack = track;
    track.onended = () => {
      if (!this.stopped) this.fail('Телефон завершил передачу видеотрека.');
    };

    const video = document.createElement('video');
    video.muted = true;
    video.autoplay = true;
    video.playsInline = true;
    video.setAttribute('aria-hidden', 'true');
    video.tabIndex = -1;
    video.style.cssText = 'position:fixed;left:-4px;top:-4px;width:2px;height:2px;opacity:0;pointer-events:none;z-index:-1;';
    video.srcObject = new MediaStream([track]);
    document.body.appendChild(video);
    this.video = video;

    await video.play();
    if (this.stopped || this.video !== video) return;
    this.scheduleVideoFrame(video, track);
  }

  private scheduleVideoFrame(video: HTMLVideoElement, track: MediaStreamTrack): void {
    if (this.stopped || this.video !== video || track.readyState !== 'live') return;
    const element = video as VideoElementWithFrameCallbacks;
    if (typeof element.requestVideoFrameCallback === 'function') {
      const id = element.requestVideoFrameCallback((now, metadata) => {
        this.scheduledVideoFrame = null;
        void this.copyAndDeliverFrame(video, track, now, metadata.mediaTime);
      });
      this.scheduledVideoFrame = { kind: 'video', id, element };
      return;
    }

    const id = window.requestAnimationFrame((now) => {
      this.scheduledVideoFrame = null;
      void this.copyAndDeliverFrame(video, track, now, video.currentTime);
    });
    this.scheduledVideoFrame = { kind: 'animation', id, element };
  }

  private async copyAndDeliverFrame(
    video: HTMLVideoElement,
    track: MediaStreamTrack,
    _now: number,
    mediaTimeSeconds: number,
  ): Promise<void> {
    if (this.stopped || this.video !== video || track.readyState !== 'live') return;
    const VideoFrameType = (globalThis as typeof globalThis & {
      VideoFrame?: VideoFrameConstructor;
    }).VideoFrame;
    if (!VideoFrameType) {
      this.fail('В этой версии Chromium недоступен WebCodecs VideoFrame.copyTo().');
      return;
    }

    let frame: WebCodecsVideoFrame | null = null;
    let packed: Uint8Array | null = null;
    try {
      if (video.videoWidth < 2 || video.videoHeight < 2) {
        return;
      }
      const timestampUs = Math.max(0, Math.trunc(mediaTimeSeconds * 1_000_000));
      frame = new VideoFrameType(video, { timestamp: timestampUs });
      packed = await copyFrameToPackedNv12(frame, this.frameScratch);
      if (this.stopped || this.video !== video || track.readyState !== 'live') return;
      const timestampNs = (BigInt(Math.max(0, Math.trunc(frame.timestamp))) * 1000n).toString();
      const result = await this.api.writeNv12Frame({
        width: frame.codedWidth,
        height: frame.codedHeight,
        timestampNs,
        data: packed,
      });
      if (!result.ok) {
        if (!this.reportedWriteFailure) {
          this.reportedWriteFailure = true;
          this.fail(result.message || 'Не удалось передать NV12-кадр процессу виртуальной камеры.');
        }
        return;
      }
      this.clearFirstFrameTimer();
      if (result.dropped) this.framesDropped += 1;
      else this.framesWritten += 1;
      const totalFrames = this.framesWritten + this.framesDropped;
      if (this.framesWritten === 1 || totalFrames % 30 === 0) {
        this.onStatus({
          phase: 'receiving',
          message: `Отправляем NV12-кадры в host-процесс: ${frame.codedWidth}×${frame.codedHeight}.`,
          width: frame.codedWidth,
          height: frame.codedHeight,
          frames: this.framesWritten,
          dropped: this.framesDropped,
        });
      }
    } catch (error) {
      if (!this.stopped) this.fail(error instanceof Error ? error.message : 'Не удалось прочитать видеокадр.');
    } finally {
      this.frameScratch.nv12?.fill(0);
      frame?.close();
      if (!this.stopped && this.video === video && track.readyState === 'live') {
        this.scheduleVideoFrame(video, track);
      }
    }
  }

  private cancelScheduledVideoFrame(): void {
    const scheduled = this.scheduledVideoFrame;
    this.scheduledVideoFrame = null;
    if (!scheduled) return;
    if (scheduled.kind === 'video') {
      scheduled.element.cancelVideoFrameCallback?.(scheduled.id);
    } else {
      window.cancelAnimationFrame(scheduled.id);
    }
  }

  private clearConnectionTimer(): void {
    if (this.connectionTimer === null) return;
    window.clearTimeout(this.connectionTimer);
    this.connectionTimer = null;
  }

  private clearFirstFrameTimer(): void {
    if (this.firstFrameTimer === null) return;
    window.clearTimeout(this.firstFrameTimer);
    this.firstFrameTimer = null;
  }

  private fail(message: string): void {
    if (this.stopped) return;
    this.onStatus({ phase: 'error', message });
    void this.api.sendRtcSignal({ v: 1, type: 'bye' }).catch(() => undefined);
    this.stop();
  }
}

async function copyFrameToPackedNv12(
  frame: WebCodecsVideoFrame,
  scratch: VideoFrameScratchBuffers,
): Promise<Uint8Array> {
  const width = frame.codedWidth;
  const height = frame.codedHeight;
  if (!Number.isInteger(width) || !Number.isInteger(height)
    || width < 2 || height < 2 || (width & 1) !== 0 || (height & 1) !== 0
    || width > MAX_WIDTH || height > MAX_HEIGHT) {
    throw new Error(`Недопустимый размер видеокадра ${width}×${height}.`);
  }
  const pixelCount = width * height;
  const packedBytes = pixelCount * 3 / 2;
  const rgbaBytes = pixelCount * 4;
  if (packedBytes > MAX_NV12_BYTES || rgbaBytes > MAX_RGBA_BYTES) {
    throw new Error('Видеокадр превышает предел 4K.');
  }

  if (!scratch.rgba || scratch.rgba.byteLength !== rgbaBytes) scratch.rgba = new Uint8Array(rgbaBytes);
  if (!scratch.nv12 || scratch.nv12.byteLength !== packedBytes) scratch.nv12 = new Uint8Array(packedBytes);
  const rgbaStorage = scratch.rgba;
  const packedStorage = scratch.nv12;
  const rgba = rgbaStorage;
  const packed = packedStorage;
  const requestedLayout = [{ offset: 0, stride: width * 4 }];
  try {
    let layouts: VideoPlaneLayout[];
    try {
      layouts = await frame.copyTo(rgba, { format: 'RGBA', layout: requestedLayout });
    } catch (error) {
      const reason = error instanceof Error ? error.message : 'неизвестная ошибка';
      throw new Error(`Не удалось скопировать кадр WebRTC в RGBA: ${reason}`);
    }
    if (!Array.isArray(layouts) || layouts.length !== 1
      || layouts[0].offset !== requestedLayout[0].offset
      || layouts[0].stride !== requestedLayout[0].stride) {
      throw new Error('Chromium не применил плотную одноплоскостную разметку RGBA.');
    }

    convertRgbaToPackedNv12(rgba, packed, width, height);
    return packed;
  } catch (error) {
    packedStorage.fill(0);
    throw error;
  } finally {
    rgbaStorage.fill(0);
  }
}

// Convert each 2×2 RGBA block to limited-range BT.709 NV12; WebCodecs does not reliably copy directly to NV12.
function convertRgbaToPackedNv12(
  rgba: Uint8Array,
  packed: Uint8Array,
  width: number,
  height: number,
): void {
  const lumaBytes = width * height;
  const chromaOffset = lumaBytes;
  for (let y = 0; y < height; y += 2) {
    const topRow = y * width;
    const bottomRow = topRow + width;
    for (let x = 0; x < width; x += 2) {
      const topLeft = (topRow + x) * 4;
      const topRight = topLeft + 4;
      const bottomLeft = (bottomRow + x) * 4;
      const bottomRight = bottomLeft + 4;

      const redTopLeft = rgba[topLeft];
      const greenTopLeft = rgba[topLeft + 1];
      const blueTopLeft = rgba[topLeft + 2];
      const redTopRight = rgba[topRight];
      const greenTopRight = rgba[topRight + 1];
      const blueTopRight = rgba[topRight + 2];
      const redBottomLeft = rgba[bottomLeft];
      const greenBottomLeft = rgba[bottomLeft + 1];
      const blueBottomLeft = rgba[bottomLeft + 2];
      const redBottomRight = rgba[bottomRight];
      const greenBottomRight = rgba[bottomRight + 1];
      const blueBottomRight = rgba[bottomRight + 2];

      // Q16 coefficients convert nonlinear sRGB/BT.709 RGB into limited-range BT.709 luma.
      packed[topRow + x] = 16 + ((redTopLeft * 11966 + greenTopLeft * 40254 + blueTopLeft * 4064 + 32768) >> 16);
      packed[topRow + x + 1] = 16 + ((redTopRight * 11966 + greenTopRight * 40254 + blueTopRight * 4064 + 32768) >> 16);
      packed[bottomRow + x] = 16 + ((redBottomLeft * 11966 + greenBottomLeft * 40254 + blueBottomLeft * 4064 + 32768) >> 16);
      packed[bottomRow + x + 1] = 16 + ((redBottomRight * 11966 + greenBottomRight * 40254 + blueBottomRight * 4064 + 32768) >> 16);

      const red = (redTopLeft + redTopRight + redBottomLeft + redBottomRight + 2) >> 2;
      const green = (greenTopLeft + greenTopRight + greenBottomLeft + greenBottomRight + 2) >> 2;
      const blue = (blueTopLeft + blueTopRight + blueBottomLeft + blueBottomRight + 2) >> 2;
      const chromaIndex = chromaOffset + (y / 2) * width + x;
      packed[chromaIndex] = 128 + ((-red * 6596 - green * 22189 + blue * 28784 + 32768) >> 16);
      packed[chromaIndex + 1] = 128 + ((red * 28784 - green * 26145 - blue * 2639 + 32768) >> 16);
    }
  }
}
