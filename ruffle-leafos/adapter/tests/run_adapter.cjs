const fs = require('node:fs');
const assert = require('node:assert/strict');
(async () => {
  const bytes = fs.readFileSync(process.argv[2]);
  for (const mode of [0, 1]) {
    const {instance} = await WebAssembly.instantiate(bytes, {});
    const line = instance.exports.adapter_tests(mode);
    assert.equal(line, 0, `adapter mode=${mode} failed at C line ${line}`);
    assert.equal(instance.exports.metric_tests(), 0, 'frame histogram tests');
  }
  console.log('PASS: actual SDL adapter compiled to WASM: native events byte-preserved, NULL poll, probe quit, context/swap forwarding, timing histogram');
})().catch(e => { console.error(e); process.exitCode = 1; });
