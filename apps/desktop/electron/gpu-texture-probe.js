'use strict';

const canvas = document.getElementById('surface');
const context = canvas.getContext('2d', { alpha: false, desynchronized: true });
let frame = 0;

function draw(time) {
  frame += 1;
  const { width, height } = canvas;
  const shift = Math.floor((time / 16) % height);
  context.fillStyle = '#000018';
  context.fillRect(0, 0, width, height);

  for (let index = -1; index < 7; index += 1) {
    const y = ((index * 128 + shift) % (height + 128)) - 128;
    context.fillStyle = index % 2 === 0 ? '#142cff' : '#061477';
    context.fillRect(0, y, width, 72);
  }

  const barX = Math.floor((time / 7) % width);
  context.fillStyle = '#00e7a5';
  context.fillRect(barX, 0, 8, height);
  context.fillStyle = '#05070a';
  context.fillRect(16, 16, 278, 96);
  context.fillStyle = '#ffffff';
  context.font = 'bold 72px monospace';
  context.textBaseline = 'top';
  context.fillText(String(frame % 1_000_000).padStart(6, '0'), 25, 25);

  requestAnimationFrame(draw);
}

requestAnimationFrame(draw);
