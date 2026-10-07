const { contextBridge, ipcRenderer } = require('electron');

function subscribe(channel, callback) {
  const listener = (_event, payload) => callback(payload);
  ipcRenderer.on(channel, listener);
  return () => ipcRenderer.removeListener(channel, listener);
}

contextBridge.exposeInMainWorld('remotePhone', {
  getDevices: () => ipcRenderer.invoke('pairing:get-devices'),
  refreshDevices: () => ipcRenderer.invoke('pairing:refresh-devices'),
  connect: (request) => ipcRenderer.invoke('pairing:connect', request),
  disconnect: () => ipcRenderer.invoke('pairing:disconnect'),
  sendRtcSignal: (signal) => ipcRenderer.invoke('rtc:send-signal', signal),
  onRtcSignal: (callback) => subscribe('rtc:signal', callback),
  writeNv12Frame: (frame) => ipcRenderer.invoke('camera:write-nv12-frame', frame),
  onDevices: (callback) => subscribe('pairing:devices', callback),
  onStatus: (callback) => subscribe('pairing:status', callback),
  getCameraStatus: () => ipcRenderer.invoke('camera:get-status'),
  onCameraStatus: (callback) => subscribe('camera:status', callback),
  getGpuTextureProbeStatus: () => ipcRenderer.invoke('camera:get-gpu-probe-status'),
  runGpuTextureProbe: () => ipcRenderer.invoke('camera:run-gpu-texture-probe'),
  onGpuTextureProbeStatus: (callback) => subscribe('camera:gpu-texture-probe-status', callback),
});
