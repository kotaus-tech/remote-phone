const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repositoryRoot = path.resolve(__dirname, '..', '..', '..');
const addonPath = path.join(
  repositoryRoot,
  'apps',
  'desktop',
  'native',
  'gpu-texture-probe-test',
  'build',
  'Release',
  'gpu_texture_probe_lifecycle_test.node',
);

function findManualSelfDeletes(root) {
  const findings = [];
  const sourceExtensions = new Set(['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx']);
  const excludedDirectories = new Set(['.git', 'build', 'node_modules', 'packages', 'release', 'target']);

  function visit(directory) {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      if (entry.isDirectory()) {
        if (!excludedDirectories.has(entry.name)) visit(path.join(directory, entry.name));
        continue;
      }
      if (!sourceExtensions.has(path.extname(entry.name).toLowerCase())) continue;

      const filePath = path.join(directory, entry.name);
      const lines = fs.readFileSync(filePath, 'utf8').split(/\r?\n/);
      lines.forEach((line, index) => {
        if (/\bdelete\s+this\s*;/.test(line)) {
          findings.push(`${path.relative(repositoryRoot, filePath)}:${index + 1}`);
        }
      });
    }
  }

  visit(root);
  return findings;
}

async function main() {
  console.log('Checking native sources for manual delete this...');
  const manualDeletes = [
    ...findManualSelfDeletes(path.join(repositoryRoot, 'apps', 'desktop', 'native')),
    ...findManualSelfDeletes(path.join(repositoryRoot, 'native')),
  ];
  assert.deepEqual(manualDeletes, [], `Manual delete this found in native code: ${manualDeletes.join(', ')}`);

  assert.ok(fs.existsSync(addonPath), `Test addon was not built: ${addonPath}`);
  const addon = require(addonPath);
  assert.equal(typeof addon.inspectSharedTexture, 'function');
  assert.equal(typeof addon.testReadbackWorkerLifecycle, 'function');

  console.log('Running 16 repeated 4K inspectSharedTexture rejection cases...');
  // The 4K boundary must reach the asynchronous worker; a null NT handle then
  // deliberately rejects through the same OnError path used by production.
  for (let index = 0; index < 16; index += 1) {
    let readback;
    assert.doesNotThrow(() => {
      readback = addon.inspectSharedTexture(Buffer.alloc(8), 3840, 2160);
    });
    assert.equal(typeof readback?.then, 'function');
    await assert.rejects(readback);
  }

  assert.throws(
    () => addon.inspectSharedTexture(Buffer.alloc(8), 3841, 2160),
    /320x180..3840x2160/,
  );
  assert.throws(
    () => addon.inspectSharedTexture(Buffer.alloc(8), 3840, 2161),
    /320x180..3840x2160/,
  );
  assert.throws(
    () => addon.inspectSharedTexture(Buffer.alloc(8), 319, 180),
    /320x180..3840x2160/,
  );

  // Exercise both callbacks repeatedly using the production ReadbackWorker
  // class, without requiring a physical GPU for its success-path fixture.
  console.log('Running 128 sequential success/error worker pairs...');
  for (let index = 0; index < 128; index += 1) {
    const result = await addon.testReadbackWorkerLifecycle(false);
    assert.equal(result.width, 640);
    assert.equal(result.height, 480);
    assert.equal(result.pixelHash, '00000000A17A5EED');
    await assert.rejects(
      addon.testReadbackWorkerLifecycle(true),
      /Synthetic GPU probe AsyncWorker lifecycle test failure/,
    );
  }

  console.log('Running 64 concurrent workers...');
  const concurrent = await Promise.allSettled(
    Array.from({ length: 64 }, (_, index) => addon.testReadbackWorkerLifecycle(index % 2 === 1)),
  );
  assert.equal(concurrent.filter((result) => result.status === 'fulfilled').length, 32);
  assert.equal(concurrent.filter((result) => result.status === 'rejected').length, 32);

  console.log('GPU ReadbackWorker lifecycle stress test passed (16 inspect errors, 128 sequential success/error pairs, 64 concurrent workers).');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
