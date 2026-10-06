const { app, BrowserWindow, ipcMain } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { PairingController } = require('./pairing.cjs');

let mainWindow = null;
let pairingController = null;
let cameraHostProcess = null;
let ipcReady = false;
let appIsQuitting = false;
let cameraHostError = '';
let cameraStatus = {
  phase: 'starting',
  message: 'Тестовая виртуальная камера запускается вместе с приложением…',
};

function sendToRenderer(channel, payload) {
  if (mainWindow && !mainWindow.isDestroyed() && !mainWindow.webContents.isDestroyed()) {
    mainWindow.webContents.send(channel, payload);
  }
}

function publishCameraStatus(status) {
  cameraStatus = status;
  sendToRenderer('camera:status', status);
}

function getCameraHostPath() {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'native', 'RemotePhone.VirtualCameraHost.exe');
  }
  return path.join(__dirname, '..', 'native-runtime', 'RemotePhone.VirtualCameraHost.exe');
}

function startCameraHost() {
  if (process.platform !== 'win32') {
    publishCameraStatus({ phase: 'unavailable', message: 'Тестовая камера доступна в установленном приложении Windows.' });
    return;
  }

  const hostPath = getCameraHostPath();
  if (!fs.existsSync(hostPath)) {
    publishCameraStatus({ phase: 'error', message: 'Не найден системный компонент виртуальной камеры. Переустановите приложение из Setup.exe.' });
    return;
  }

  publishCameraStatus({ phase: 'starting', message: 'Запускаем тестовую виртуальную камеру Windows…' });
  cameraHostError = '';

  let child;
  try {
    child = spawn(hostPath, ['--application-session'], {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    });
  } catch (error) {
    publishCameraStatus({
      phase: 'error',
      message: `Не удалось запустить тестовую камеру: ${error.message}`,
    });
    return;
  }

  cameraHostProcess = child;
  let stdoutBuffer = '';
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', (text) => {
    stdoutBuffer = (stdoutBuffer + text).slice(-4096);
    if (stdoutBuffer.includes('CAMERA_STATUS=RUNNING')) {
      publishCameraStatus({
        phase: 'running',
        message: 'Камера запущена в текущем сеансе Windows. Проверьте фактический кадр во внешнем приложении.',
      });
      stdoutBuffer = '';
    }
  });

  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (text) => {
    cameraHostError = (cameraHostError + text).slice(-2048).trim();
  });

  child.once('error', (error) => {
    cameraHostError = error.message;
    publishCameraStatus({
      phase: 'error',
      message: `Не удалось запустить тестовую виртуальную камеру: ${error.message}`,
    });
  });

  child.once('exit', (code, signal) => {
    if (cameraHostProcess === child) cameraHostProcess = null;
    if (appIsQuitting) return;

    if (code === 0) {
      publishCameraStatus({ phase: 'stopped', message: 'Тестовая камера остановлена.' });
      return;
    }

    const detail = cameraHostError || (signal ? `сигнал ${signal}` : `код ${code}`);
    publishCameraStatus({
      phase: 'error',
      message: `Тестовая камера не запустилась (${detail}). Проверьте журнал ProgramData.`,
    });
  });
}

function stopCameraHost() {
  if (!cameraHostProcess || !cameraHostProcess.stdin || cameraHostProcess.stdin.destroyed) return;
  try {
    // EOF tells the host to stop and remove the session-scoped camera cleanly.
    cameraHostProcess.stdin.end();
  } catch {
    // The host may already have exited during application shutdown.
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
  ipcMain.handle('camera:get-status', (event) => {
    assertTrustedWindow(event);
    return cameraStatus;
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
  startCameraHost();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('before-quit', () => {
  appIsQuitting = true;
  pairingController?.shutdown();
  stopCameraHost();
});

app.on('window-all-closed', () => {
  app.quit();
});
