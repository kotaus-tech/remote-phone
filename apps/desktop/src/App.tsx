import { useEffect, useState } from 'react';
import type { PairingDevice, PairingStatus } from './remotePhone';

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
    eyebrow: 'Камера телефона',
    title: 'Веб-камера',
    description: 'Виртуальная камера будет доступна после проверки нативного видеотракта.',
  },
  settings: {
    eyebrow: 'Настройки',
    title: 'Под ваш сценарий',
    description: 'Основные настройки приложения и способ просмотра.',
  },
  diagnostics: {
    eyebrow: 'Диагностика',
    title: 'Состояние устройств',
    description: 'Технический отчёт появится после подключения телефона.',
  },
};

function App() {
  const [page, setPage] = useState<PageKey>('devices');
  const [devices, setDevices] = useState<PairingDevice[]>([]);
  const [pairingStatus, setPairingStatus] = useState<PairingStatus>({
    phase: 'discovering',
    message: 'Ищем телефоны в локальной сети…',
  });
  const heading = headings[page];

  useEffect(() => {
    const api = window.remotePhone;
    if (!api) {
      setPairingStatus({ phase: 'unavailable', message: 'Сетевой адаптер доступен в приложении Windows.' });
      return;
    }
    let active = true;
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
      removeDevicesListener();
      removeStatusListener();
    };
  }, []);

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
          {page === 'screen' && <ScreenPage connected={connected} />}
          {page === 'camera' && <CameraPage />}
          {page === 'settings' && <SettingsPage />}
          {page === 'diagnostics' && <DiagnosticsPage />}
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
      const result = await api.connect({ address: address.trim(), port: Number(port), pin: oneTimePin });
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

function ScreenPage({ connected }: { connected: boolean }) {
  return (
    <section className="panel preview-panel">
      <div className="preview-toolbar"><span>ПРЕДПРОСМОТР ТЕЛЕФОНА</span><span>{connected ? 'Сопряжение подтверждено' : 'Нет подключения'}</span></div>
      <div className="phone-stage">
        <div className="phone-frame"><div className="phone-notch" /><span className="phone-placeholder-icon">▣</span><strong>{connected ? 'Поток ещё не запущен' : 'Нет сигнала'}</strong><small>{connected ? 'На этом этапе проверяется только защищённое сопряжение' : 'Подключите телефон, чтобы начать просмотр'}</small></div>
      </div>
      <div className="preview-actions">
        <div className="button-group"><button type="button" disabled>Назад</button><button type="button" disabled>Домой</button><button type="button" disabled>Недавние</button></div>
        <button type="button" className="secondary-button" disabled>На весь экран</button>
      </div>
    </section>
  );
}

function CameraPage() {
  return (
    <section className="camera-layout">
      <article className="panel camera-panel">
        <div className="preview-toolbar"><span>ПРЕДПРОСМОТР · 16:9</span><span>Виртуальная камера не запущена</span></div>
        <div className="camera-stage">
          <div className="camera-status-icon" aria-hidden="true">◉</div>
          <strong>Нет сигнала</strong>
          <span>Сначала нужно подключить телефон</span>
        </div>
        <div className="preview-actions"><button type="button" className="secondary-button" disabled>Профи</button><button type="button" className="secondary-button" disabled>Зеркало</button></div>
      </article>
      <aside className="panel camera-note">
        <div className="note-mark blue" aria-hidden="true">i</div>
        <h2>Сначала проверим изображение</h2>
        <p>Перед полными настройками проверим, что картинка телефона быстро и надёжно появляется в приложениях Windows.</p>
        <div className="divider" />
        <div className="camera-note-row"><span>Источник</span><strong>Телефон</strong></div>
        <div className="camera-note-row"><span>Куда передаётся</span><strong>Камера Windows</strong></div>
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

function DiagnosticsPage() {
  return (
    <section className="panel diagnostics-empty">
      <div className="empty-icon" aria-hidden="true">⌁</div>
      <h2>Отчёт появится после подключения</h2>
      <p>Здесь будут только фактические возможности телефона, состояние соединения и сведения для отладки.</p>
    </section>
  );
}

export default App;
