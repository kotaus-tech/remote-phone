const { spawnSync } = require('node:child_process');
const path = require('node:path');

const electronExecutable = require('electron');
const testScript = path.join(__dirname, 'test-gpu-probe-worker.cjs');
const result = spawnSync(electronExecutable, [testScript], {
  cwd: path.resolve(__dirname, '..'),
  env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' },
  stdio: 'inherit',
  windowsHide: true,
});

if (result.error) {
  console.error('Failed to launch Electron as Node.js:', result.error);
  process.exitCode = 1;
} else if (result.signal) {
  console.error(`Electron test process was terminated by signal ${result.signal}.`);
  process.exitCode = 1;
} else {
  process.exitCode = result.status ?? 1;
}
