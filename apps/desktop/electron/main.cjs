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

function appendGpuTextureProbeLog(line) {
  const programData = process.env.ProgramData || process.env.PROGRAMDATA;
  if (!programData) return;
  const logPath = path.join(programData, 'Kotaus', 'RemotePhone', 'logs', 'VirtualCameraMediaSource.log');
  try {
    fs.appendFileSync(logPath, `${new Date().toISOString()} electron_gpu_texture_probe ${line}\r\n`, 'utf8');
  } catch {
    // The installed product pre-creates this ProgramData log with append-only ACLs.
  }
}

function logGpuTextureProbeEvent(event, details = {}) {
  const serialized = JSON.stringify(details).replace(/[\r\n]+/g, ' ');
  appendGpuTextureProbeLog(`event=${event} details=${serialized}`);
}

function writeGpuTextureProbeLog(status) {
  const safeMessage = String(status.message || '').replace(/[\r\n]+/g, ' ');
  const metrics = `phase=${status.phase} frames=${status.frameCount ?? 0} unique=${status.uniqueFrames ?? 0} dropped=${status.droppedFrames ?? 0} fps=${status.observedFps ?? 0} readback_avg_ms=${status.averageReadbackMs ?? 0} readback_max_ms=${status.maxReadbackMs ?? 0}`;
  appendGpuTextureProbeLog(`${metrics} message="${safeMessage}"`);
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
    logGpuTextureProbeEvent('finish_deferred_until_readback', { phase, message });
    return;
  }
  logGpuTextureProbeEvent('probe_finished', {
    phase,
    message,
    frames: session.frameCount,
    uniqueFrames: session.hashes.size,
    paints: session.paintCount,
    nativeCalls: session.nativeCallCount,
  });
  gpuTextureProbeSession = null;
  clearTimeout(session.timeout);
  if (session.window && !session.window.isDestroyed()) {
    logGpuTextureProbeEvent('offscreen_window_destroy_started', { phase });
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
  logGpuTextureProbeEvent('requested', {
    pid: process.pid,
    platform: process.platform,
    electron: process.versions.electron,
  });
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
  const addonPath = getGpuTextureProbeAddonPath();
  logGpuTextureProbeEvent('native_addon_load_started', { addonPath });
  try {
    addon = require(addonPath);
    if (typeof addon.inspectSharedTexture !== 'function') {
      throw new Error('В native-модуле отсутствует inspectSharedTexture.');
    }
    logGpuTextureProbeEvent('native_addon_loaded');
  } catch (error) {
    logGpuTextureProbeEvent('native_addon_load_failed', { message: error.message });
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
      lastNativeLogAt: 0,
      frameCount: 0,
      paintCount: 0,
      nativeCallCount: 0,
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
        useContentSize: true,
        show: false,
        frame: true,
        webPreferences: {
          offscreen: { useSharedTexture: true, deviceScaleFactor: 1 },
          contextIsolation: true,
          nodeIntegration: false,
          sandbox: true,
          backgroundThrottling: false,
          paintWhenInitiallyHidden: true,
        },
      });
      logGpuTextureProbeEvent('offscreen_window_created', { width, height, hidden: true });
    } catch (error) {
      logGpuTextureProbeEvent('offscreen_window_create_failed', { message: error.message });
      finishGpuTextureProbe(session, 'error', `Не удалось создать offscreen окно: ${error.message}`);
      return;
    }

    session.onPaint = (event) => {
      session.paintCount += 1;
      const texture = event.texture;
      if (session.paintCount === 1) {
        logGpuTextureProbeEvent('first_paint', { texturePresent: Boolean(texture) });
      }
      if (!texture) {
        logGpuTextureProbeEvent('paint_without_shared_texture', { paintCount: session.paintCount });
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

      let textureInfo;
      let sharedHandle;
      let size;
      try {
        textureInfo = texture.textureInfo;
        sharedHandle = textureInfo?.handle?.ntHandle;
        size = textureInfo?.codedSize;
      } catch (error) {
        logGpuTextureProbeEvent('texture_info_read_failed', { message: error.message });
        releaseOffscreenTexture(texture);
        finishGpuTextureProbe(session, 'error', `Не удалось прочитать описание GPU-текстуры: ${error.message}`);
        return;
      }
      if (session.paintCount === 1) {
        logGpuTextureProbeEvent('first_texture_info', {
          width: size?.width ?? null,
          height: size?.height ?? null,
          handleBytes: Buffer.isBuffer(sharedHandle) ? sharedHandle.length : null,
        });
      }
      if (!Buffer.isBuffer(sharedHandle) || !size || size.width !== width || size.height !== height) {
        logGpuTextureProbeEvent('invalid_texture_info', {
          width: size?.width ?? null,
          height: size?.height ?? null,
          handleBytes: Buffer.isBuffer(sharedHandle) ? sharedHandle.length : null,
        });
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
      session.nativeCallCount += 1;
      const nativeCallNumber = session.nativeCallCount;
      const nativeLogTime = Date.now();
      if (nativeCallNumber === 1 || nativeLogTime - session.lastNativeLogAt >= 1000) {
        session.lastNativeLogAt = nativeLogTime;
        logGpuTextureProbeEvent('d3d11_readback_started', {
          call: nativeCallNumber,
          width,
          height,
          handleBytes: sharedHandle.length,
        });
      }
      let readbackError = null;
      Promise.resolve()
        .then(() => addon.inspectSharedTexture(sharedHandle, width, height))
        .then((sample) => {
          session.frameCount += 1;
          session.hashes.add(sample.pixelHash);
          session.totalReadbackMs += sample.readbackMs;
          session.maxReadbackMs = Math.max(session.maxReadbackMs, sample.readbackMs);
          const completionLogTime = Date.now();
          if (nativeCallNumber === 1 || completionLogTime - session.lastNativeLogAt >= 1000) {
            session.lastNativeLogAt = completionLogTime;
            logGpuTextureProbeEvent('d3d11_readback_completed', {
              call: nativeCallNumber,
              frameCount: session.frameCount,
              uniqueFrames: session.hashes.size,
              readbackMs: Number(sample.readbackMs.toFixed(2)),
            });
          }
          const elapsedMs = completionLogTime - session.startedAt;
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
          logGpuTextureProbeEvent('d3d11_readback_failed', {
            call: nativeCallNumber,
            message: error.message,
          });
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
      logGpuTextureProbeEvent('offscreen_window_closed', { paintCount: session.paintCount });
      if (gpuTextureProbeSession === session) {
        finishGpuTextureProbe(session, 'error', 'Offscreen окно GPU-проверки неожиданно закрылось.');
      }
    });
    session.window.webContents.once('did-finish-load', () => {
      if (gpuTextureProbeSession !== session) return;
      logGpuTextureProbeEvent('probe_page_loaded');
      try {
        session.window.webContents.setFrameRate(30);
        logGpuTextureProbeEvent('frame_rate_set', { fps: 30 });
      } catch (error) {
        logGpuTextureProbeEvent('frame_rate_set_failed', { message: error.message });
        finishGpuTextureProbe(session, 'error', `Не удалось настроить частоту GPU-теста: ${error.message}`);
      }
    });
    session.window.webContents.once('did-fail-load', (_event, code, description) => {
      logGpuTextureProbeEvent('probe_page_load_failed', { code, description });
      finishGpuTextureProbe(session, 'error', `Не удалось загрузить GPU-тест: ${description} (${code}).`);
    });
    session.window.webContents.once('render-process-gone', (_event, details) => {
      logGpuTextureProbeEvent('probe_renderer_gone', details);
      finishGpuTextureProbe(session, 'error', `Renderer тестового окна завершился: ${details.reason} (${details.exitCode}).`);
    });
    session.timeout = setTimeout(() => {
      logGpuTextureProbeEvent('probe_timeout', { paintCount: session.paintCount, nativeCalls: session.nativeCallCount });
      finishGpuTextureProbe(session, 'error', 'За отведённое время не удалось получить стабильную последовательность GPU-кадров.');
    }, 12_000);
    const probePage = path.join(__dirname, 'gpu-texture-probe.html');
    logGpuTextureProbeEvent('probe_page_load_started', { page: probePage });
    session.window.loadFile(probePage).catch((error) => {
      logGpuTextureProbeEvent('probe_page_load_rejected', { message: error.message });
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

app.on('child-process-gone', (_event, details) => {
  const session = gpuTextureProbeSession;
  if (!session && details.type !== 'GPU') return;
  logGpuTextureProbeEvent('electron_child_process_gone', {
    type: details.type,
    reason: details.reason,
    exitCode: details.exitCode,
    serviceName: details.serviceName,
  });
  if (session && details.type === 'GPU') {
    finishGpuTextureProbe(
      session,
      'error',
      `GPU-процесс Electron завершился: ${details.reason} (${details.exitCode}).`
    );
  }
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
