const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');
const { PairingController } = require('./pairing.cjs');

let mainWindow = null;
let pairingController = null;
let ipcReady = false;

function sendToRenderer(channel, payload) {
  if (mainWindow && !mainWindow.isDestroyed() && !mainWindow.webContents.isDestroyed()) {
    mainWindow.webContents.send(channel, payload);
  }
}

function registerPairingIpc() {
  if (ipcReady) return;
  ipcReady = true;
  const assertTrustedWindow = (event) => {
    if (!mainWindow || event.sender !== mainWindow.webContents) {
      throw new Error('Недопустимый источник запроса.');
    }
  };
  ipcMain.handle('pairing:get-devices', (event) => {
    assertTrustedWindow(event);
    return pairingController ? pairingController.getDevices() : [];
  });
  ipcMain.handle('pairing:refresh-devices', (event) => {
    assertTrustedWindow(event);
    pairingController?.refreshDiscovery();
    return pairingController ? pairingController.getDevices() : [];
  });
  ipcMain.handle('pairing:connect', (event, request) => {
    assertTrustedWindow(event);
    if (!pairingController) {
      return { ok: false, message: 'Сетевой адаптер пока не запущен.' };
    }
    return pairingController.connect(request);
  });
  ipcMain.handle('pairing:disconnect', (event) => {
    assertTrustedWindow(event);
    pairingController?.disconnect();
    return { ok: true };
  });
}

function createWindow() {
  const window = new BrowserWindow({
    width: 1200,
    height: 820,
    minWidth: 860,
    minHeight: 620,
    show: false,
    backgroundColor: '#0b0e12',
    backgroundMaterial: 'mica',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      webSecurity: true,
    },
  });

  window.once('ready-to-show', () => window.show());
  window.on('closed', () => {
    if (mainWindow === window) mainWindow = null;
  });
  mainWindow = window;
  window.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
}

app.whenReady().then(() => {
  registerPairingIpc();
  pairingController = new PairingController(sendToRenderer);
  createWindow();
  pairingController.startDiscovery();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('before-quit', () => {
  pairingController?.shutdown();
});

app.on('window-all-closed', () => {
  app.quit();
});
