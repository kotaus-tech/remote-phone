import { useCallback, useEffect, useRef, useState } from 'react';
import type {
  CameraHostStatus,
  GpuTextureProbeStatus,
  PairingDevice,
  PairingStatus,
  PhoneTelemetryMessage,
  QualityProfile,
  StreamMode,
} from './remotePhone';
import { RtcCameraSession } from './rtcCameraSession';
import type { CameraTransportStatus } from './rtcCameraSession';

type PageKey = 'devices' | 'screen' | 'camera' | 'settings' | 'diagnostics';

type NavigationItem = {
  id: PageKey;
  title: string;
  glyph: string;
};

const navigation: NavigationItem[] = [
  { id: 'devices', title: 'Подключение', glyph: '▯' },
  { id: 'screen', title: 'Экран', glyph: '▣' },
  { id: 'camera', title: 'Веб-камера', glyph: '◉' },
  { id: 'settings', title: 'Настройки', glyph: '⚙' },
  { id: 'diagnostics', title: 'Диагностика', glyph: '⌁' },
];

const headings: Record<PageKey, { eyebrow: string; title: string; description: string }> = {
  devices: {
    eyebrow: 'Подключение',
    title: 'Телефон рядом',
    description: 'Найдите телефон в локальной сети и подключитесь по временному PIN-коду.',
  },
  screen: {
    eyebrow: 'Трансляция экрана',
    title: 'Экран телефона',
    description: 'Предпросмотр и управление появятся после настройки защищённого соединения.',
  },
  camera: {
    eyebrow: 'Виртуальная камера Windows',
    title: 'Веб-камера',
    description: 'После PIN-сопряжения приложение пытается установить WebRTC-видеоканал. До подтверждённого поступления кадров остаётся синтетический резервный поток.',
  },
  settings: {
    eyebrow: 'Настройки',
    title: 'Под ваш сценарий',
    description: 'Параметры пока неактивны и появятся вместе с соответствующими функциями.',
  },
  diagnostics: {
    eyebrow: 'Диагностика',
    title: 'Состояние устройств',
    description: 'Текущий статус сопряжения, WebRTC-потока и виртуальной камеры Windows.',
  },
};

function App() {
  const [page, setPage] = useState<PageKey>('devices');
  const [devices, setDevices] = useState<PairingDevice[]>([]);
  const [pairingStatus, setPairingStatus] = useState<PairingStatus>({
    phase: 'discovering',
    message: 'Ищем телефоны в локальной сети…',
  });
  const [cameraStatus, setCameraStatus] = useState<CameraHostStatus>({
    phase: 'starting',
    message: 'Тестовая виртуальная камера запускается вместе с приложением…',
  });
  const [cameraTransportStatus, setCameraTransportStatus] = useState<CameraTransportStatus>({
    phase: 'idle',
    message: 'Подключите телефон по PIN, чтобы начать передачу живого видео.',
  });
  const [gpuTextureProbeStatus, setGpuTextureProbeStatus] = useState<GpuTextureProbeStatus>({
    phase: 'idle',
    message: 'Основной маршрут живой камеры — VideoFrame → RGBA → NV12 → shared memory; GPU остаётся необязательным экспериментом.',
  });
  const [phoneTelemetry, setPhoneTelemetry] = useState<PhoneTelemetryMessage | null>(null);
  const [streamMode, setStreamMode] = useState<StreamMode | null>(null);
  const [alwaysOnTop, setAlwaysOnTop] = useState(false);
  const sessionRef = useRef<RtcCameraSession | null>(null);
  const heading = headings[page];

  // Stable identity: the preview pages re-attach in an effect keyed on this
  // callback, so a fresh closure per render would reset srcObject (and blink
  // the video black) on every telemetry update.
  const handleVideoReady = useCallback((element: HTMLVideoElement | null) => {
    sessionRef.current?.attachExternalVideo(element);
  }, []);

  useEffect(() => {
    const api = window.remotePhone;
    if (!api) {
      setPairingStatus({ phase: 'unavailable', message: 'Сетевой адаптер доступен в приложении Windows.' });
      setCameraStatus({ phase: 'unavailable', message: 'Тестовая камера доступна в установленном приложении Windows.' });
      setGpuTextureProbeStatus({ phase: 'error', message: 'GPU shared-texture проверка доступна только в установленном приложении Windows.' });
      return;
    }
    let active = true;
    const removeCameraStatusListener = api.onCameraStatus((nextStatus) => {
      if (active) setCameraStatus(nextStatus);
    });
    const removeGpuProbeListener = api.onGpuTextureProbeStatus((nextStatus) => {
      if (active) setGpuTextureProbeStatus(nextStatus);
    });
    api.getCameraStatus().then((nextStatus) => {
      if (active) setCameraStatus(nextStatus);
    }).catch(() => {
      if (active) setCameraStatus({ phase: 'error', message: 'Не удалось получить состояние тестовой камеры.' });
    });
    api.getGpuTextureProbeStatus().then((nextStatus) => {
      if (active) setGpuTextureProbeStatus(nextStatus);
    }).catch(() => {
      if (active) setGpuTextureProbeStatus({ phase: 'error', message: 'Не удалось получить состояние GPU shared-texture проверки.' });
    });
    const removeDevicesListener = api.onDevices((nextDevices) => {
      if (active) setDevices(nextDevices);
    });
    const removeStatusListener = api.onStatus((nextStatus) => {
      if (active) setPairingStatus(nextStatus);
    });
    api.getDevices().then((nextDevices) => {
      if (active) setDevices(nextDevices);
    }).catch(() => {
      if (active) setPairingStatus({ phase: 'discovery-error', message: 'Автоматический поиск недоступен. Укажите адрес телефона вручную.' });
    });
    return () => {
      active = false;
      removeCameraStatusListener();
      removeGpuProbeListener();
      removeDevicesListener();
      removeStatusListener();
    };
  }, []);

  useEffect(() => {
    const api = window.remotePhone;
    if (!api || pairingStatus.phase !== 'authenticated') {
      setCameraTransportStatus({
        phase: 'idle',
        message: 'Подключите телефон по PIN, чтобы начать передачу живого видео.',
      });
      return;
    }

    let active = true;
    const session = new RtcCameraSession(api, (nextStatus) => {
      if (active) setCameraTransportStatus(nextStatus);
    }, {
      onSessionInfo: (info) => {
        if (active) setStreamMode(info.mode);
      },
      onTelemetry: (telemetry) => {
        if (active) setPhoneTelemetry(telemetry);
      },
      onStreamMode: (mode) => {
        if (active) setStreamMode(mode);
      },
    });
    sessionRef.current = session;
    const removeSignalListener = api.onRtcSignal((signal) => {
      void session.handleSignal(signal);
    });
    void session.start().catch(() => {
      // The session publishes a localized error state itself.
    });

    return () => {
      active = false;
      sessionRef.current = null;
      removeSignalListener();
      session.stop();
    };
  }, [pairingStatus.phase]);

  useEffect(() => {
    if (pairingStatus.phase !== 'authenticated') {
      setStreamMode(null);
      setPhoneTelemetry(null);
    }
  }, [pairingStatus.phase]);

  async function runGpuTextureProbe() {
    const api = window.remotePhone;
    if (!api) {
      setGpuTextureProbeStatus({ phase: 'error', message: 'GPU shared-texture проверка доступна только в приложении Windows.' });
      return;
    }
    setGpuTextureProbeStatus({ phase: 'running', message: 'Снимаем диагностические метрики необязательного GPU-кандидата…' });
    try {
      const nextStatus = await api.runGpuTextureProbe();
      setGpuTextureProbeStatus(nextStatus);
    } catch {
      setGpuTextureProbeStatus({ phase: 'error', message: 'Не удалось выполнить GPU shared-texture проверку.' });
    }
  }

  const connected = pairingStatus.phase === 'authenticated';
  const connectionLabel = connected
    ? 'Защищённое сопряжение подтверждено'
    : pairingStatus.phase === 'connecting'
      ? 'Проверка PIN'
      : 'Соединение не установлено';

  return (
    <div className="app-frame">
      <header className="titlebar">
        <div className="titlebar-brand">
          <span className="brand-mark" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none"><path d="M4 7.5h4l1.4-2h5.2l1.4 2h4v11H4v-11Z" stroke="currentColor" strokeWidth="1.6" strokeLinejoin="round" /><circle cx="12" cy="13" r="3.5" stroke="currentColor" strokeWidth="1.6" /></svg>
          </span>
          <span>Видоискатель</span>
        </div>
        <div className="titlebar-state"><span className={`state-dot ${connected ? '' : 'muted'}`} />{connectionLabel}</div>
        <div className="window-buttons" aria-hidden="true"><span>—</span><span>□</span><span>×</span></div>
      </header>

      <div className="app-layout">
        <aside className="sidebar">
          <div className="sidebar-brand">
            <span className="brand-mark large" aria-hidden="true">
              <svg viewBox="0 0 24 24" fill="none"><path d="M4 7.5h4l1.4-2h5.2l1.4 2h4v11H4v-11Z" stroke="currentColor" strokeWidth="1.6" strokeLinejoin="round" /><circle cx="12" cy="13" r="3.5" stroke="currentColor" strokeWidth="1.6" /></svg>
            </span>
            <span><strong>Видоискатель</strong><small>Передача с телефона</small></span>
          </div>
          <div className="nav-caption">РАБОЧЕЕ ПРОСТРАНСТВО</div>
          <nav className="navigation" aria-label="Разделы приложения">
            {navigation.map((item) => (
              <button
                key={item.id}
                type="button"
                className={`nav-item ${page === item.id ? 'active' : ''}`}
                aria-current={page === item.id ? 'page' : undefined}
                onClick={() => setPage(item.id)}
              >
                <span className="nav-glyph" aria-hidden="true">{item.glyph}</span>
                <span>{item.title}</span>
              </button>
            ))}
          </nav>
          <div className="sidebar-spacer" />
          <div className="sidebar-status">
            <div className="sidebar-status-top"><span className={`state-dot ${connected ? '' : 'muted'}`} />{connected ? 'Сеанс защищён' : 'Поиск в локальной сети'}</div>
            <strong>{connected ? 'Телефон подключён' : 'Телефон не подключён'}</strong>
            <small>Сеансы и устройства не сохраняются</small>
          </div>
          <div className="sidebar-foot">Только локальная сеть · без облака</div>
        </aside>

        <main className="main-content">
          <div className="page-heading">
            <div>
              <div className="eyebrow">{heading.eyebrow}</div>
              <h1>{heading.title}</h1>
              <p>{heading.description}</p>
            </div>
          </div>
          {page === 'devices' && <DevicesPage devices={devices} status={pairingStatus} onStatusChange={setPairingStatus} />}
          {page === 'screen' && (
            <ScreenPage
              connected={connected}
              pairingMessage={pairingStatus.message}
              transport={cameraTransportStatus}
              telemetry={phoneTelemetry}
              streamMode={streamMode}
              alwaysOnTop={alwaysOnTop}
              onAlwaysOnTopChange={(flag) => {
                setAlwaysOnTop(flag);
                void window.remotePhone?.setAlwaysOnTop(flag).then((applied) => {
                  if (applied !== flag) setAlwaysOnTop(applied);
                });
              }}
              onVideoReady={handleVideoReady}
              onStopStream={() => sessionRef.current?.sendStopStream()}
              onQuality={(value) => sessionRef.current?.sendQuality(value)}
              onGoToCamera={() => setPage('camera')}
            />
          )}
          {page === 'camera' && (
            <CameraPage
              status={cameraStatus}
              transport={cameraTransportStatus}
              gpuTextureProbe={gpuTextureProbeStatus}
              onRunGpuTextureProbe={() => void runGpuTextureProbe()}
              connected={connected}
              streamMode={streamMode}
              telemetry={phoneTelemetry}
              onVideoReady={handleVideoReady}
              onStopStream={() => sessionRef.current?.sendStopStream()}
              onQuality={(value) => sessionRef.current?.sendQuality(value)}
            />
          )}
          {page === 'settings' && <SettingsPage />}
          {page === 'diagnostics' && (
            <DiagnosticsPage
              pairing={pairingStatus}
              camera={cameraStatus}
              transport={cameraTransportStatus}
              gpuTextureProbe={gpuTextureProbeStatus}
            />
          )}
        </main>
      </div>
    </div>
  );
}

type DevicesPageProps = {
  devices: PairingDevice[];
  status: PairingStatus;
  onStatusChange: (status: PairingStatus) => void;
};

function DevicesPage({ devices, status, onStatusChange }: DevicesPageProps) {
  const [selectedDevice, setSelectedDevice] = useState<string | null>(null);
  const [address, setAddress] = useState('');
  const [port, setPort] = useState('');
  const [pin, setPin] = useState('');
  const [localMessage, setLocalMessage] = useState('');
  const connected = status.phase === 'authenticated';
  const busy = status.phase === 'connecting';

  function selectDevice(device: PairingDevice) {
    setSelectedDevice(device.id);
    setAddress(device.address);
    setPort(String(device.port));
    setPin('');
    setLocalMessage('');
  }

  async function connect() {
    const api = window.remotePhone;
    if (!api) {
      setLocalMessage('Сопряжение доступно только в приложении Windows.');
      return;
    }
    if (!address.trim() || !port || pin.length !== 8) {
      setLocalMessage('Укажите локальный IP-адрес, порт и восьмизначный PIN с телефона.');
      return;
    }
    const oneTimePin = pin;
    setPin('');
    setLocalMessage('');
    onStatusChange({ phase: 'connecting', message: 'Открываем локальное соединение и проверяем PIN…' });
    try {
      const result = await api.connect({
        address: address.trim(),
        port: Number(port),
        pin: oneTimePin,
        deviceName: devices.find((device) => device.id === selectedDevice)?.name ?? null,
      });
      if (!result.ok) {
        setLocalMessage(result.message || 'Не удалось завершить сопряжение. Проверьте PIN и сеть.');
        onStatusChange({ phase: 'error', message: result.message || 'Сопряжение не завершено.' });
      }
    } catch {
      setLocalMessage('Не удалось завершить сопряжение. Проверьте PIN и локальную сеть.');
      onStatusChange({ phase: 'error', message: 'Не удалось завершить сопряжение.' });
    }
  }

  async function refreshDevices() {
    const api = window.remotePhone;
    if (!api) {
      setLocalMessage('Автоматический поиск доступен в приложении Windows.');
      return;
    }
    try {
      await api.refreshDevices();
      setLocalMessage('Запросили обновлённый список телефонов в локальной сети.');
    } catch {
      setLocalMessage('Не удалось обновить список. Укажите адрес телефона вручную.');
    }
  }

  async function disconnect() {
    await window.remotePhone?.disconnect();
    setPin('');
  }

  return (
    <section className="device-grid">
      <article className="panel discovery-panel pairing-panel">
        <div className="pairing-section-heading">
          <div className="empty-icon" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none"><path d="M5 9.5a10.2 10.2 0 0 1 14 0M8 12.5a5.8 5.8 0 0 1 8 0m-5.1 3.2a1.6 1.6 0 0 1 2.2 0M12 19h.01" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" /></svg>
          </div>
          <div>
            <h2>Телефоны рядом</h2>
            <p>Ищем временный сеанс в вашей локальной сети. PIN остаётся только на экране телефона.</p>
          </div>
          <button className="secondary-button refresh-button" type="button" onClick={() => void refreshDevices()}>Обновить поиск</button>
        </div>

        <div className="discovered-devices" aria-live="polite">
          {devices.length === 0 ? (
            <div className="no-devices">Телефон пока не найден. Убедитесь, что оба устройства подключены к одной сети Wi‑Fi.</div>
          ) : devices.map((device) => (
            <button
              className={`device-option ${selectedDevice === device.id ? 'selected' : ''}`}
              key={device.id}
              type="button"
              onClick={() => selectDevice(device)}
              aria-pressed={selectedDevice === device.id}
            >
              <span className="device-option-icon" aria-hidden="true">▯</span>
              <span className="device-option-copy"><strong>{device.name}</strong><small>{device.address}:{device.port}</small></span>
              <span className="device-option-action">Выбрать</span>
            </button>
          ))}
        </div>

        <div className="divider" />
        <form className="pairing-form" onSubmit={(event) => { event.preventDefault(); void connect(); }}>
          <div className="pairing-form-heading">
            <div><strong>{connected ? 'Сопряжение завершено' : 'Подключение вручную'}</strong><span>Можно использовать IP-адрес телефона, если автоматический поиск не сработал.</span></div>
          </div>
          <div className="pairing-fields">
            <label className="input-field address-field">
              <span>Локальный IP-адрес</span>
              <input
                value={address}
                onChange={(event) => { setAddress(event.target.value); setSelectedDevice(null); }}
                placeholder="Например, 192.168.1.24"
                inputMode="decimal"
                autoComplete="off"
                spellCheck={false}
                disabled={connected || busy}
              />
            </label>
            <label className="input-field port-field">
              <span>Порт</span>
              <input
                type="number"
                min="1"
                max="65535"
                value={port}
                onChange={(event) => setPort(event.target.value)}
                placeholder="Порт с телефона"
                disabled={connected || busy}
              />
            </label>
          </div>
          {!connected && (
            <div className="pairing-submit-row">
              <label className="input-field pin-field">
                <span>Временный PIN с телефона</span>
                <input
                  type="password"
                  inputMode="numeric"
                  autoComplete="one-time-code"
                  maxLength={8}
                  pattern="[0-9]{8}"
                  value={pin}
                  onChange={(event) => setPin(event.target.value.replace(/\D/g, '').slice(0, 8))}
                  placeholder="8 цифр"
                  disabled={busy}
                />
              </label>
              <button className="primary-button connect-button" type="submit" disabled={busy || pin.length !== 8}>
                {busy ? 'Проверяем PIN…' : 'Подключиться'}
              </button>
            </div>
          )}
          {connected && <button className="secondary-button disconnect-button" type="button" onClick={() => void disconnect()}>Завершить защищённый сеанс</button>}
          {(localMessage || status.message) && <p className="pairing-feedback" role="status">{localMessage || status.message}</p>}
        </form>
      </article>

      <aside className="panel note-panel">
        <div className="note-mark" aria-hidden="true">✓</div>
        <h2>Личное подключение</h2>
        <p>Сопряжение работает напрямую в локальной сети. PIN не передаётся, а устройства и сеансы не сохраняются.</p>
        <div className="divider" />
        <div className="privacy-note"><span className="privacy-dot" />Только локальная сеть</div>
        <div className="pairing-security-note">После проверки кода телефон и компьютер подтверждают друг друга. Передача изображения здесь пока не запускается.</div>
      </aside>
    </section>
  );
}

function ScreenPage({
  connected,
  pairingMessage,
  transport,
  telemetry,
  streamMode,
  alwaysOnTop,
  onAlwaysOnTopChange,
  onVideoReady,
  onStopStream,
  onQuality,
  onGoToCamera,
}: {
  connected: boolean;
  pairingMessage: string;
  transport: CameraTransportStatus;
  telemetry: PhoneTelemetryMessage | null;
  streamMode: StreamMode | null;
  alwaysOnTop: boolean;
  onAlwaysOnTopChange: (flag: boolean) => void;
  onVideoReady: (element: HTMLVideoElement | null) => void;
  onStopStream: () => void;
  onQuality: (value: QualityProfile) => void;
  onGoToCamera: () => void;
}) {
  const stageRef = useRef<HTMLDivElement | null>(null);
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const live = connected && streamMode === 'screen';

  // `live` is a dependency: the <video> element mounts when the screen mode
  // starts, and the session must attach the track at that moment.
  useEffect(() => {
    onVideoReady(videoRef.current);
    return () => onVideoReady(null);
  }, [onVideoReady, live]);

  useEffect(() => {
    const element = stageRef.current;
    if (!element) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.target instanceof HTMLInputElement || event.target instanceof HTMLTextAreaElement) return;
      if (event.key === 'f' || event.key === 'F' || event.key === 'F11' || event.key === 'Escape') {
        event.preventDefault();
        if (document.fullscreenElement) void document.exitFullscreen();
        else void element.requestFullscreen().catch(() => undefined);
      }
    };
    element.addEventListener('keydown', onKeyDown);
    return () => element.removeEventListener('keydown', onKeyDown);
  }, []);

  const cameraActive = connected && streamMode === 'camera';
  const aspect = telemetry && telemetry.width > 0
    ? `${telemetry.width} / ${telemetry.height}`
    : '9 / 19.5';

  return (
    <section className="screen-layout">
      <article className="panel preview-panel screen-panel">
        <div className="preview-toolbar">
          <span>ТРАНСЛЯЦИЯ ЭКРАНА ТЕЛЕФОНА</span>
          <span className={`camera-state ${live ? 'running' : 'idle'}`} role="status">
            {live ? 'Экран передаётся' : cameraActive ? 'Телефон в режиме камеры' : connected ? 'Ожидание потока' : 'Нет подключения'}
          </span>
        </div>
        <div
          className="screen-stage"
          ref={stageRef}
          tabIndex={0}
          onDoubleClick={() => {
            if (document.fullscreenElement) void document.exitFullscreen();
            else void stageRef.current?.requestFullscreen().catch(() => undefined);
          }}
        >
          {live && (
            <video
              ref={videoRef}
              className="screen-video"
              style={{ aspectRatio: aspect }}
              autoPlay
              muted
              playsInline
            />
          )}
          {!live && (
            <div className="screen-empty">
              <span className="phone-placeholder-icon">▣</span>
              <strong>{cameraActive ? 'Телефон передаёт веб-камеру' : connected ? 'Ожидаем видеопоток…' : 'Нет сигнала'}</strong>
              <small>
                {cameraActive
                  ? 'Изображение камеры уходит в виртуальную камеру Windows; предпросмотр — на странице «Веб-камера».'
                  : !connected
                    ? 'Подключите телефон по PIN, чтобы начать трансляцию экрана.'
                    : pairingStatusMessage(pairingMessage, transport, streamMode)}
              </small>
              {cameraActive && (
                <button type="button" className="secondary-button" onClick={onGoToCamera}>
                  Перейти в «Веб-камеру»
                </button>
              )}
            </div>
          )}
        </div>
        <div className="screen-hints">
          <span>F / F11 / Esc или двойной клик — во весь экран</span>
          <label className="screen-aot">
            <input
              type="checkbox"
              checked={alwaysOnTop}
              onChange={(event) => onAlwaysOnTopChange(event.target.checked)}
            />
            Поверх всех окон
          </label>
        </div>
      </article>

      <aside className="panel screen-side">
        <h2>Телеметрия телефона</h2>
        {telemetry ? (
          <div className="telemetry-grid">
            <TelemetryCell label="Разрешение" value={`${telemetry.width}×${telemetry.height}`} />
            <TelemetryCell label="FPS" value={telemetry.fps > 0 ? telemetry.fps.toFixed(1) : '—'} />
            <TelemetryCell label="Битрейт" value={telemetry.bitrateKbps > 0 ? `${Math.round(telemetry.bitrateKbps)} кбит/с` : '—'} />
            <TelemetryCell label="Батарея" value={telemetry.batteryPercent >= 0 ? `${telemetry.batteryPercent}%` : '—'} />
            <TelemetryCell label="Питание" value={telemetry.charging ? 'Заряжается' : 'Батарея'} />
            <TelemetryCell label="Нагрев" value={telemetry.thermal} />
          </div>
        ) : (
          <p className="screen-side-note">
            Телеметрия появится вместе с видеопотоком: частота кадров, битрейт, батарея и нагрев телефона.
          </p>
        )}
        <div className="divider" />
        <h2>Качество</h2>
        <div className="quality-row">
          {(['auto', 'max', 'economy'] as QualityProfile[]).map((profile) => (
            <button
              key={profile}
              type="button"
              className={`quality-chip ${telemetry?.quality === profile ? 'active' : ''}`}
              disabled={!connected}
              onClick={() => onQuality(profile)}
            >
              {qualityTitle(profile)}
            </button>
          ))}
        </div>
        <div className="divider" />
        <button
          type="button"
          className="secondary-button danger"
          disabled={!live}
          onClick={onStopStream}
        >
          Остановить трансляцию
        </button>
        <p className="screen-side-note">
          Сеанс при этом остаётся защищённым: возобновите трансляцию с телефона или переподключитесь без нового PIN после сбоя Wi-Fi.
        </p>
      </aside>
    </section>
  );
}

function CameraPage({
  status,
  transport,
  gpuTextureProbe,
  onRunGpuTextureProbe,
  connected,
  streamMode,
  telemetry,
  onVideoReady,
  onStopStream,
  onQuality,
}: {
  status: CameraHostStatus;
  transport: CameraTransportStatus;
  gpuTextureProbe: GpuTextureProbeStatus;
  onRunGpuTextureProbe: () => void;
  connected: boolean;
  streamMode: StreamMode | null;
  telemetry: PhoneTelemetryMessage | null;
  onVideoReady: (element: HTMLVideoElement | null) => void;
  onStopStream: () => void;
  onQuality: (value: QualityProfile) => void;
}) {
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const [mirror, setMirror] = useState(false);

  const cameraLive = connected && streamMode === 'camera';
  // `cameraLive` is a dependency: the preview element mounts when the webcam
  // mode starts, and the session must attach the track at that moment.
  useEffect(() => {
    onVideoReady(videoRef.current);
    return () => onVideoReady(null);
  }, [onVideoReady, cameraLive]);

  const isRunning = status.phase === 'running';
  const statusLabel = isRunning
    ? 'Камера запущена'
    : status.phase === 'starting'
      ? 'Запускается…'
      : status.phase === 'stopped'
        ? 'Камера остановлена'
        : status.phase === 'unavailable'
          ? 'Недоступна в этом режиме'
          : 'Не удалось запустить';
  return (
    <section className="camera-layout">
      <article className="panel camera-panel">
        <div className="preview-toolbar">
          <span>{cameraLive ? 'ЖИВАЯ КАМЕРА ТЕЛЕФОНА' : 'CPU-ПОТОК · NV12'}</span>
          <span className={`camera-state ${cameraLive ? 'running' : status.phase}`} role="status">
            {cameraLive ? 'Камера передаётся' : statusLabel}
          </span>
        </div>
        <div className={`camera-stage ${cameraLive ? 'camera-stage-live' : isRunning ? 'camera-stage-ready' : ''}`} aria-live="polite">
          {cameraLive ? (
            <video
              ref={videoRef}
              className={`camera-preview ${mirror ? 'mirrored' : ''}`}
              autoPlay
              muted
              playsInline
            />
          ) : (
            <>
              <div className="camera-status-icon" aria-hidden="true">◉</div>
              <strong>{transport.phase === 'receiving' ? 'Отправка NV12-кадров в host-процесс' : isRunning ? 'Виртуальная камера запущена' : statusLabel}</strong>
              <span>{transport.message}</span>
            </>
          )}
        </div>
        {cameraLive && (
          <div className="camera-live-controls">
            <label className="screen-aot">
              <input
                type="checkbox"
                checked={mirror}
                onChange={(event) => setMirror(event.target.checked)}
              />
              Зеркально
            </label>
            <div className="quality-row">
              {(['auto', 'max', 'economy'] as QualityProfile[]).map((profile) => (
                <button
                  key={profile}
                  type="button"
                  className={`quality-chip ${telemetry?.quality === profile ? 'active' : ''}`}
                  onClick={() => onQuality(profile)}
                >
                  {qualityTitle(profile)}
                </button>
              ))}
            </div>
            <button type="button" className="secondary-button danger" onClick={onStopStream}>
              Остановить камеру
            </button>
          </div>
        )}
        <div className="camera-actions-note">
          {cameraLive
            ? `Телефон передаёт живое видео${transport.width && transport.height ? ` (${transport.width}×${transport.height})` : ''}; оно же уходит в виртуальную камеру Windows. Выберите «Видоискатель — тестовая камера» в OBS, Discord или Zoom.`
            : 'Выберите устройство «Видоискатель — тестовая камера» во внешнем приложении Windows. Пока телефон не подключён, виртуальная камера выдаёт резервный синтетический кадр.'}
          {transport.frames !== undefined && transport.frames > 0 && (
            <span className="camera-frame-note"> Кадров передано: {transport.frames}{transport.dropped ? `, пропущено: ${transport.dropped}` : ''}.</span>
          )}
        </div>
        <section className={`gpu-probe-panel ${gpuTextureProbe.phase}`} aria-live="polite">
          <div className="gpu-probe-heading">
            <div>
              <strong>Необязательный GPU-эксперимент</strong>
              <span>{gpuTextureProbe.message}</span>
            </div>
            <button
              className="secondary-button gpu-probe-button"
              type="button"
              onClick={onRunGpuTextureProbe}
              disabled={gpuTextureProbe.phase === 'running'}
            >
              {gpuTextureProbe.phase === 'running' ? 'Проверяем…' : 'Запустить проверку'}
            </button>
          </div>
          <p>Этот эксперимент измеряет только GPU shared-texture Chromium → Direct3D 11 (D3D11), не сквозную задержку телефона. Основной путь VideoFrame.copyTo(RGBA) → CPU-конвертация в NV12 → shared memory уже подключается; преимуществ GPU пока не доказано. Переключаться можно только после сравнения обоих трактов на тех же разрешении и частоте.</p>
          {(gpuTextureProbe.frameCount ?? 0) > 0 && (
            <div className="gpu-probe-metrics">
              <span>Размер <strong>{gpuTextureProbe.width}×{gpuTextureProbe.height}</strong></span>
              <span>Кадров <strong>{gpuTextureProbe.frameCount}</strong></span>
              <span>Разных <strong>{gpuTextureProbe.uniqueFrames}</strong></span>
              <span>Темп <strong>{gpuTextureProbe.observedFps} fps</strong></span>
              <span>Средний readback <strong>{gpuTextureProbe.averageReadbackMs} мс</strong></span>
              <span>Максимум <strong>{gpuTextureProbe.maxReadbackMs} мс</strong></span>
              <span>Пропущено <strong>{gpuTextureProbe.droppedFrames}</strong></span>
            </div>
          )}
        </section>
      </article>
      <aside className="panel camera-note">
        <div className="note-mark blue" aria-hidden="true">i</div>
        <h2>Проверка в приложениях Windows</h2>
        <p>При подключённом телефоне внешние приложения показывают живую картинку камеры. Пока поток не идёт, камера выдаёт резервный кадр с полосами и счётчиком.</p>
        <div className="divider" />
        {telemetry && cameraLive ? (
          <div className="telemetry-grid">
            <TelemetryCell label="Разрешение" value={`${telemetry.width}×${telemetry.height}`} />
            <TelemetryCell label="FPS" value={telemetry.fps > 0 ? telemetry.fps.toFixed(1) : '—'} />
            <TelemetryCell label="Битрейт" value={telemetry.bitrateKbps > 0 ? `${Math.round(telemetry.bitrateKbps)} кбит/с` : '—'} />
            <TelemetryCell label="Батарея" value={telemetry.batteryPercent >= 0 ? `${telemetry.batteryPercent}%` : '—'} />
            <TelemetryCell label="Нагрев" value={telemetry.thermal} />
            <TelemetryCell label="Качество" value={qualityTitle(telemetry.quality)} />
          </div>
        ) : (
          <>
            <div className="camera-note-row"><span>Режимы</span><strong>720p / 1080p / 4K · 30 / 60 fps</strong></div>
            <div className="camera-note-row"><span>Форматы</span><strong>NV12 · RGB32</strong></div>
            <div className="camera-note-row"><span>Завершение</span><strong>Закрыть «Видоискатель»</strong></div>
          </>
        )}
        <div className="camera-test-instructions">
          <strong>Оставьте приложение открытым</strong>
          <span>Проверьте webcamtests.com, OBS и Discord по очереди. Закройте их перед выходом из приложения — камера действует только пока открыт «Видоискатель».</span>
        </div>
      </aside>
    </section>
  );
}

function SettingsPage() {
  return (
    <section className="settings-grid">
      <article className="panel settings-panel">
        <SettingRow title="Запускать вместе с Windows" detail="Появится после настройки приложения." />
        <SettingRow title="Качество по умолчанию" detail="Автоматический выбор появится позже." />
        <SettingRow title="Чувствительность прокрутки" detail="Настройки управления появятся в режиме «Экран»." />
      </article>
      <aside className="panel note-panel">
        <div className="note-mark" aria-hidden="true">i</div>
        <h2>Только важные настройки</h2>
        <p>Профессиональные параметры будут открываться отдельно, чтобы не перегружать главный экран.</p>
      </aside>
    </section>
  );
}

function SettingRow({ title, detail }: { title: string; detail: string }) {
  return (
    <div className="setting-row">
      <div><strong>{title}</strong><span>{detail}</span></div>
      <span className="disabled-control" aria-hidden="true" />
    </div>
  );
}

function DiagnosticsPage({
  pairing,
  camera,
  transport,
  gpuTextureProbe,
}: {
  pairing: PairingStatus;
  camera: CameraHostStatus;
  transport: CameraTransportStatus;
  gpuTextureProbe: GpuTextureProbeStatus;
}) {
  const sourceSize = transport.width && transport.height
    ? `${transport.width}×${transport.height} NV12`
    : 'Ещё нет видеокадров';
  const gpuFacts = gpuTextureProbe.frameCount === undefined
    ? []
    : [
        { label: 'Размер', value: `${gpuTextureProbe.width ?? 0}×${gpuTextureProbe.height ?? 0}` },
        { label: 'Кадров', value: String(gpuTextureProbe.frameCount) },
        { label: 'Разных кадров', value: String(gpuTextureProbe.uniqueFrames ?? 0) },
        { label: 'Темп', value: `${gpuTextureProbe.observedFps ?? 0} fps` },
        { label: 'Readback в среднем', value: `${gpuTextureProbe.averageReadbackMs ?? 0} мс` },
        { label: 'Пропущено', value: String(gpuTextureProbe.droppedFrames ?? 0) },
      ];

  return (
    <section className="diagnostics-grid" aria-label="Текущая диагностика">
      <DiagnosticCard
        title="Сопряжение телефона"
        phase={pairing.phase}
        message={pairing.message}
      />
      <DiagnosticCard
        title="Виртуальная камера Windows"
        phase={camera.phase}
        message={camera.message}
      />
      <DiagnosticCard
        title="Видеоканал WebRTC"
        phase={transport.phase}
        message={transport.message}
        facts={[
          { label: 'Источник', value: sourceSize },
          { label: 'Передано кадров', value: transport.frames === undefined ? '—' : String(transport.frames) },
          { label: 'Пропущено кадров', value: transport.dropped === undefined ? '—' : String(transport.dropped) },
        ]}
      />
      <DiagnosticCard
        title="GPU shared-texture · эксперимент"
        phase={gpuTextureProbe.phase}
        message={gpuTextureProbe.message}
        facts={gpuFacts}
      />
    </section>
  );
}

function DiagnosticCard({
  title,
  phase,
  message,
  facts = [],
}: {
  title: string;
  phase: string;
  message: string;
  facts?: { label: string; value: string }[];
}) {
  return (
    <article className="panel diagnostics-card">
      <div className="diagnostics-card-heading">
        <h2>{title}</h2>
        <span className={`diagnostics-state ${phase}`}>{diagnosticPhaseLabel(phase)}</span>
      </div>
      <p className="diagnostics-message" role="status">{message}</p>
      {facts.length > 0 && (
        <dl className="diagnostics-facts">
          {facts.map((fact) => (
            <div key={fact.label}>
              <dt>{fact.label}</dt>
              <dd>{fact.value}</dd>
            </div>
          ))}
        </dl>
      )}
    </article>
  );
}

function diagnosticPhaseLabel(phase: string): string {
  const labels: Record<string, string> = {
    authenticated: 'PIN подтверждён',
    connecting: 'Подключаемся',
    discovering: 'Ищем телефон',
    'discovery-error': 'Поиск недоступен',
    error: 'Ошибка',
    unavailable: 'Недоступно',
    starting: 'Запускается',
    running: 'Работает',
    stopped: 'Остановлена',
    idle: 'Ожидание',
    negotiating: 'Согласование WebRTC',
    connected: 'Подключено',
    receiving: 'Передаёт видео',
    passed: 'Проверка пройдена',
  };
  return labels[phase] ?? phase;
}

function pairingStatusMessage(
  pairingMessage: string,
  transport: CameraTransportStatus,
  streamMode: StreamMode | null,
): string {
  if (streamMode === 'camera') {
    return 'Телефон сейчас передаёт режим «Веб-камера» — изображение идёт в виртуальную камеру Windows.';
  }
  if (transport.phase === 'error') return transport.message;
  return pairingMessage;
}

function qualityTitle(profile: QualityProfile): string {
  return profile === 'auto' ? 'Авто' : profile === 'max' ? 'Максимум' : 'Экономия';
}

function TelemetryCell({ label, value }: { label: string; value: string }) {
  return (
    <div className="telemetry-cell">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

export default App;
