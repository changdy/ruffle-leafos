const fs = require('node:fs');
const assert = require('node:assert/strict');
(async () => {
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(process.argv[2]), {});
  const failedLine = instance.exports.probe_tests();
  assert.equal(failedLine, 0, `C input-state test failed at source line ${failedLine}`);
  console.log('PASS: compiled C input tests: launch-key suppression, fresh keys, repeats, D-pad, controller, mouse, drift, lifecycle');
})().catch(error => { console.error(error); process.exitCode = 1; });
