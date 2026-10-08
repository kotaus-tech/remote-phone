export type PairingDevice = {
  id: string;
  name: string;
  address: string;
  port: number;
};

export type PairingStatus = {
  phase: string;
  message: string;
};

export type PairingConnectResult = {
  ok: boolean;
  message?: string;
};

export type RtcSignal =
  | { v: 1; type: 'offer' | 'answer'; sdp: string }
  | { v: 1; type: 'ice'; candidate: string; sdpMid: string | null; sdpMLineIndex: number | null }
  | { v: 1; type: 'bye' }
  | { v: 1; type: 'error'; code: string };

export type Nv12FrameWriteResult = {
  ok: boolean;
  dropped?: boolean;
  message?: string;
};

export type CameraHostStatus = {
  phase: 'starting' | 'running' | 'error' | 'stopped' | 'unavailable';
  message: string;
};

export type GpuTextureProbeStatus = {
  phase: 'idle' | 'running' | 'passed' | 'error';
  message: string;
  frameCount?: number;
  uniqueFrames?: number;
  droppedFrames?: number;
  observedFps?: number;
  averageReadbackMs?: number;
  maxReadbackMs?: number;
  width?: number;
  height?: number;
};

declare global {
  interface Window {
    remotePhone?: {
      getDevices: () => Promise<PairingDevice[]>;
      refreshDevices: () => Promise<PairingDevice[]>;
      connect: (request: { address: string; port: number; pin: string }) => Promise<PairingConnectResult>;
      disconnect: () => Promise<{ ok: boolean }>;
      sendRtcSignal: (signal: RtcSignal) => Promise<PairingConnectResult>;
      onRtcSignal: (callback: (signal: RtcSignal) => void) => () => void;
      writeNv12Frame: (frame: { width: number; height: number; timestampNs: string; data: Uint8Array }) => Promise<Nv12FrameWriteResult>;
      onDevices: (callback: (devices: PairingDevice[]) => void) => () => void;
      onStatus: (callback: (status: PairingStatus) => void) => () => void;
      getCameraStatus: () => Promise<CameraHostStatus>;
      onCameraStatus: (callback: (status: CameraHostStatus) => void) => () => void;
      getGpuTextureProbeStatus: () => Promise<GpuTextureProbeStatus>;
      runGpuTextureProbe: () => Promise<GpuTextureProbeStatus>;
      onGpuTextureProbeStatus: (callback: (status: GpuTextureProbeStatus) => void) => () => void;
    };
  }
}
