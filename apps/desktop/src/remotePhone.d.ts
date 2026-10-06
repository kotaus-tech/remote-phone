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

declare global {
  interface Window {
    remotePhone?: {
      getDevices: () => Promise<PairingDevice[]>;
      refreshDevices: () => Promise<PairingDevice[]>;
      connect: (request: { address: string; port: number; pin: string }) => Promise<PairingConnectResult>;
      disconnect: () => Promise<{ ok: boolean }>;
      onDevices: (callback: (devices: PairingDevice[]) => void) => () => void;
      onStatus: (callback: (status: PairingStatus) => void) => () => void;
    };
  }
}
