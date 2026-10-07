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
let gpuTextureProbeSession = null;
let gpuTextureProbeStatus = {
  phase: 'idle',
  message: 'GPU shared-texture мост ещё не проверен.',
  frameCount: 0,
  uniqueFrames: 0,
  droppedFrames: 0,
};
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

function writeGpuTextureProbeLog(status) {
  const programData = process.env.ProgramData || process.env.PROGRAMDATA;
  if (!programData) return;
  const logPath = path.join(programData, 'Kotaus', 'RemotePhone', 'logs', 'VirtualCameraMediaSource.log');
  const safeMessage = String(status.message || '').replace(/[\r\n]+/g, ' ');
  const metrics = `phase=${status.phase} frames=${status.frameCount ?? 0} unique=${status.uniqueFrames ?? 0} dropped=${status.droppedFrames ?? 0} fps=${status.observedFps ?? 0} readback_avg_ms=${status.averageReadbackMs ?? 0} readback_max_ms=${status.maxReadbackMs ?? 0}`;
  try {
    fs.appendFileSync(logPath, `${new Date().toISOString()} electron_gpu_texture_probe ${metrics} message="${safeMessage}"\r\n`, 'utf8');
  } catch {
    // The installed product pre-creates this ProgramData log with append-only ACLs.
  }
}

function publishGpuTextureProbeStatus(status) {
  const previousPhase = gpuTextureProbeStatus.phase;
  gpuTextureProbeStatus = status;
  if (status.phase !== 'running' || previousPhase !== 'running') writeGpuTextureProbeLog(status);
  sendToRenderer('camera:gpu-texture-probe-status', status);
}

function getGpuTextureProbeAddonPath() {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'native', 'gpu_texture_probe.node');
  }
  return path.join(__dirname, '..', 'native-runtime', 'gpu_texture_probe.node');
}

function releaseOffscreenTexture(texture) {
  try {
    texture?.release?.();
  } catch {
    // A renderer shutdown can release the backing frame pool first.
  }
}

function finishGpuTextureProbe(session, phase, message) {
  if (gpuTextureProbeSession !== session) return;
  if (session.pending) {
    session.finishAfterReadback = { phase, message };
    return;
  }
  gpuTextureProbeSession = null;
  clearTimeout(session.timeout);
  if (session.window && !session.window.isDestroyed()) {
    session.window.webContents.removeListener('paint', session.onPaint);
    session.window.destroy();
  }
  const elapsedSeconds = session.startedAt
    ? Math.max((Date.now() - session.startedAt) / 1000, 0.001)
    : 0;
  const status = {
    phase,
    message,
    frameCount: session.frameCount,
    uniqueFrames: session.hashes.size,
    droppedFrames: session.droppedFrames,
    observedFps: elapsedSeconds > 0 ? Number((session.frameCount / elapsedSeconds).toFixed(1)) : 0,
    averageReadbackMs: session.frameCount > 0
      ? Number((session.totalReadbackMs / session.frameCount).toFixed(2))
      : 0,
    maxReadbackMs: Number(session.maxReadbackMs.toFixed(2)),
    width: session.width,
    height: session.height,
  };
  publishGpuTextureProbeStatus(status);
  session.resolve(status);
}

function startGpuTextureProbe() {
  if (process.platform !== 'win32') {
    const status = {
      phase: 'error',
      message: 'GPU shared-texture проверяется только в установленном приложении Windows.',
      frameCount: 0,
      uniqueFrames: 0,
      droppedFrames: 0,
    };
    publishGpuTextureProbeStatus(status);
    return Promise.resolve(status);
  }
  if (gpuTextureProbeSession) return Promise.resolve(gpuTextureProbeStatus);

  let addon;
  try {
    addon = require(getGpuTextureProbeAddonPath());
    if (typeof addon.inspectSharedTexture !== 'function') {
      throw new Error('В native-модуле отсутствует inspectSharedTexture.');
    }
  } catch (error) {
    const status = {
      phase: 'error',
      message: `Не удалось загрузить D3D11-модуль GPU-проверки: ${error.message}`,
      frameCount: 0,
      uniqueFrames: 0,
      droppedFrames: 0,
    };
    publishGpuTextureProbeStatus(status);
    return Promise.resolve(status);
  }

  return new Promise((resolve) => {
    const width = 1280;
    const height = 720;
    const session = {
      window: null,
      onPaint: null,
      resolve,
      timeout: null,
      pending: false,
      startedAt: 0,
      lastPublishedAt: 0,
      frameCount: 0,
      droppedFrames: 0,
      hashes: new Set(),
      totalReadbackMs: 0,
      maxReadbackMs: 0,
      width,
      height,
    };
    gpuTextureProbeSession = session;
    publishGpuTextureProbeStatus({
      phase: 'running',
      message: 'Проверяем Electron GPU shared texture → D3D11; используется только hardware GPU, без CPU fallback.',
      frameCount: 0,
      uniqueFrames: 0,
      droppedFrames: 0,
      width,
      height,
    });

    try {
      session.window = new BrowserWindow({
        width,
        height,
        show: false,
        frame: false,
        webPreferences: {
          offscreen: { useSharedTexture: true, deviceScaleFactor: 1 },
          contextIsolation: true,
          nodeIntegration: false,
          sandbox: true,
          backgroundThrottling: false,
          paintWhenInitiallyHidden: true,
        },
      });
    } catch (error) {
      finishGpuTextureProbe(session, 'error', `Не удалось создать offscreen окно: ${error.message}`);
      return;
    }

    session.onPaint = (event) => {
      const texture = event.texture;
      if (!texture) {
        finishGpuTextureProbe(
          session,
          'error',
          'Electron не предоставил GPU shared texture (возможен CPU fallback или неподдерживаемый драйвер).'
        );
        return;
      }
      if (session.pending) {
        session.droppedFrames += 1;
        releaseOffscreenTexture(texture);
        return;
      }

      const textureInfo = texture.textureInfo;
      const sharedHandle = textureInfo?.handle?.ntHandle;
      const size = textureInfo?.codedSize;
      if (!Buffer.isBuffer(sharedHandle) || !size || size.width !== width || size.height !== height) {
        releaseOffscreenTexture(texture);
        finishGpuTextureProbe(
          session,
          'error',
          'Electron вернул неполные сведения D3D11-текстуры или неожиданный размер кадра.'
        );
        return;
      }

      if (session.startedAt === 0) session.startedAt = Date.now();
      session.pending = true;
      let readbackError = null;
      Promise.resolve()
        .then(() => addon.inspectSharedTexture(sharedHandle, width, height))
        .then((sample) => {
          session.frameCount += 1;
          session.hashes.add(sample.pixelHash);
          session.totalReadbackMs += sample.readbackMs;
          session.maxReadbackMs = Math.max(session.maxReadbackMs, sample.readbackMs);
          const elapsedMs = Date.now() - session.startedAt;
          if (elapsedMs - session.lastPublishedAt >= 500) {
            session.lastPublishedAt = elapsedMs;
            const elapsedSeconds = Math.max(elapsedMs / 1000, 0.001);
            publishGpuTextureProbeStatus({
              phase: 'running',
              message: `D3D11 открыл ${sample.width}×${sample.height}; проверено ${session.hashes.size} разных кадров.`,
              frameCount: session.frameCount,
              uniqueFrames: session.hashes.size,
              droppedFrames: session.droppedFrames,
              observedFps: Number((session.frameCount / elapsedSeconds).toFixed(1)),
              averageReadbackMs: Number((session.totalReadbackMs / session.frameCount).toFixed(2)),
              maxReadbackMs: Number(session.maxReadbackMs.toFixed(2)),
              width,
              height,
            });
          }
        })
        .catch((error) => {
          readbackError = error;
        })
        .then(() => {
          releaseOffscreenTexture(texture);
          session.pending = false;
          if (gpuTextureProbeSession !== session) return;
          if (readbackError) {
            session.finishAfterReadback = null;
            finishGpuTextureProbe(
              session,
              'error',
              `Не удалось импортировать/прочитать GPU-текстуру D3D11: ${readbackError.message}`
            );
            return;
          }
          if (session.finishAfterReadback) {
            const deferredFinish = session.finishAfterReadback;
            session.finishAfterReadback = null;
            finishGpuTextureProbe(session, deferredFinish.phase, deferredFinish.message);
            return;
          }
          const elapsedMs = Date.now() - session.startedAt;
          if (elapsedMs >= 5000) {
            const passed = session.frameCount >= 20 && session.hashes.size >= 10;
            const elapsedSeconds = Math.max(elapsedMs / 1000, 0.001);
            const observedFps = Number((session.frameCount / elapsedSeconds).toFixed(1));
            finishGpuTextureProbe(
              session,
              passed ? 'passed' : 'error',
              passed
                ? `GPU texture gate пройден: ${session.frameCount} кадров, ${session.hashes.size} уникальных, ${observedFps} кадров/с; средний D3D11 readback ${Number((session.totalReadbackMs / session.frameCount).toFixed(2))} мс.`
                : `GPU texture gate не пройден: получено ${session.frameCount} кадров и ${session.hashes.size} уникальных за ${Number(elapsedSeconds.toFixed(1))} с.`,
            );
          }
        });
    };

    session.window.webContents.on('paint', session.onPaint);
    session.window.once('closed', () => {
      if (gpuTextureProbeSession === session) {
        finishGpuTextureProbe(session, 'error', 'Offscreen окно GPU-проверки неожиданно закрылось.');
      }
    });
    session.window.webContents.once('did-finish-load', () => {
      if (gpuTextureProbeSession !== session) return;
      session.window.webContents.setFrameRate(30);
    });
    session.window.webContents.once('did-fail-load', (_event, code, description) => {
      finishGpuTextureProbe(session, 'error', `Не удалось загрузить GPU-тест: ${description} (${code}).`);
    });
    session.timeout = setTimeout(() => {
      finishGpuTextureProbe(session, 'error', 'За отведённое время не удалось получить стабильную последовательность GPU-кадров.');
    }, 12_000);
    session.window.loadFile(path.join(__dirname, 'gpu-texture-probe.html')).catch((error) => {
      finishGpuTextureProbe(session, 'error', `Не удалось открыть страницу GPU-теста: ${error.message}`);
    });
  });
}

function cancelGpuTextureProbe() {
  if (!gpuTextureProbeSession) return;
  finishGpuTextureProbe(
    gpuTextureProbeSession,
    'error',
    'GPU-проверка остановлена при закрытии приложения.'
  );
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
  ipcMain.handle('camera:get-gpu-probe-status', (event) => {
    assertTrustedWindow(event);
    return gpuTextureProbeStatus;
  });
  ipcMain.handle('camera:run-gpu-texture-probe', (event) => {
    assertTrustedWindow(event);
    return startGpuTextureProbe();
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
  cancelGpuTextureProbe();
  stopCameraHost();
});

app.on('window-all-closed', () => {
  app.quit();
});
