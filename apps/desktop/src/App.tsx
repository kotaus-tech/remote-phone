import { useState } from 'react';

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
  const heading = headings[page];

  return (
    <div className="app-frame">
      <header className="titlebar">
        <div className="titlebar-brand">
          <span className="brand-mark" aria-hidden="true">
            <svg viewBox="0 0 24 24" fill="none"><path d="M4 7.5h4l1.4-2h5.2l1.4 2h4v11H4v-11Z" stroke="currentColor" strokeWidth="1.6" strokeLinejoin="round" /><circle cx="12" cy="13" r="3.5" stroke="currentColor" strokeWidth="1.6" /></svg>
          </span>
          <span>Видоискатель</span>
        </div>
        <div className="titlebar-state"><span className="state-dot" />Соединение не установлено</div>
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
            <div className="sidebar-status-top"><span className="state-dot muted" />Обнаружение не запущено</div>
            <strong>Телефон не подключён</strong>
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
          {page === 'devices' && <DevicesPage />}
          {page === 'screen' && <ScreenPage />}
          {page === 'camera' && <CameraPage />}
          {page === 'settings' && <SettingsPage />}
          {page === 'diagnostics' && <DiagnosticsPage />}
        </main>
      </div>
    </div>
  );
}

function DevicesPage() {
  return (
    <section className="device-grid">
      <article className="panel discovery-panel">
        <div className="empty-icon" aria-hidden="true">
          <svg viewBox="0 0 24 24" fill="none"><path d="M5 9.5a10.2 10.2 0 0 1 14 0M8 12.5a5.8 5.8 0 0 1 8 0m-5.1 3.2a1.6 1.6 0 0 1 2.2 0M12 19h.01" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" /></svg>
        </div>
        <h2>Телефонов пока нет</h2>
        <p>На следующем этапе появится поиск телефонов в вашей локальной сети.</p>
        <button className="primary-button" type="button" disabled>Поиск появится позже</button>
        <button className="quiet-button" type="button" disabled>Ввести адрес вручную</button>
      </article>
      <aside className="panel note-panel">
        <div className="note-mark" aria-hidden="true">✓</div>
        <h2>Личное подключение</h2>
        <p>Устройства не сохраняются. Для нового сеанса будет использоваться отдельный PIN с телефона.</p>
        <div className="divider" />
        <div className="privacy-note"><span className="privacy-dot" />Только локальная сеть</div>
      </aside>
    </section>
  );
}

function ScreenPage() {
  return (
    <section className="panel preview-panel">
      <div className="preview-toolbar"><span>ПРЕДПРОСМОТР ТЕЛЕФОНА</span><span>Нет подключения</span></div>
      <div className="phone-stage">
        <div className="phone-frame"><div className="phone-notch" /><span className="phone-placeholder-icon">▣</span><strong>Нет сигнала</strong><small>Подключите телефон, чтобы начать просмотр</small></div>
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
