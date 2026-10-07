const fs = require('node:fs');
const path = require('node:path');

const source = path.join(__dirname, '..', 'native', 'gpu-texture-probe', 'build', 'Release', 'gpu_texture_probe.node');
const destinationDirectory = path.join(__dirname, '..', 'native-runtime');
const destination = path.join(destinationDirectory, 'gpu_texture_probe.node');

if (!fs.existsSync(source)) {
  throw new Error(`Не найден собранный GPU shared-texture модуль: ${source}`);
}
fs.mkdirSync(destinationDirectory, { recursive: true });
fs.copyFileSync(source, destination);
console.log(`Подготовлен native addon GPU shared-texture: ${destination}`);
