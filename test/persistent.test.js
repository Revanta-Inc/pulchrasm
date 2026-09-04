'use strict';
// Runs the 39 SCN-test structures through ONE engine (one wasm instance) and
// checks that every result is byte-identical to the stored PULCHRA output,
// that the heap does not grow, that overlapping run() calls are fine and that
// nothing leaks from one run into the next.
//   node test/persistent.test.js
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { createPulchra, PulchraError } = require('..');

const STORED = process.env.PULCHRA_TEST_FIXTURES ||
  path.join(__dirname, '..', 'pulchra-scn-test-reproduction', 'outputs', 'wasm-pulchra');

(async () => {
  const names = fs.readdirSync(STORED).filter((f) => f.endsWith('.pdb')).sort();
  const stored = names.map((f) => fs.readFileSync(path.join(STORED, f), 'utf8'));
  // PULCHRA preserves C-alpha positions, so the CA trace of a rebuilt file is
  // exactly the input that produced it.
  const inputs = stored.map((t) => t.split('\n').filter((l) => /^ATOM.{8} CA /.test(l)).join('\n') + '\n');

  const eng = await createPulchra();
  assert.strictEqual(eng.loaded, false, 'instance is created lazily');

  let identical = 0;
  for (const order of [names.map((_, i) => i), names.map((_, i) => names.length - 1 - i)]) {
    for (const i of order) identical += (await eng.run(inputs[i])).pdb === stored[i];
  }
  assert.strictEqual(identical, 2 * names.length);
  console.log(`ok  ${2 * names.length} runs on one instance, all byte-identical to stored output`);

  const h0 = await eng.heapUsed();
  for (let k = 0; k < 50; k++) await eng.run(inputs[k % names.length]);
  const h1 = await eng.heapUsed();
  assert.strictEqual(h0, h1, `heap grew from ${h0} to ${h1} bytes`);
  console.log(`ok  heap flat across runs (${h1} bytes allocated between runs)`);

  const rs = await Promise.all(inputs.slice(0, 10).map((s) => eng.run(s)));
  rs.forEach((r, i) => assert.strictEqual(r.pdb, stored[i]));
  console.log('ok  overlapping run() calls');

  await assert.rejects(eng.run(inputs[0], { args: ['-Q'] }), PulchraError);
  const v = await eng.run(inputs[0], { verbose: true, center: true, optimizeHBonds: true });
  assert.ok(/Optimizing backbone/.test(v.stdout) && v.pdb !== stored[0]);
  const r = await eng.run(inputs[0]);
  assert.strictEqual(r.pdb, stored[0], 'options leaked into the next run');
  assert.strictEqual(r.stdout.trim(), 'Initial coordinates will be preserved.', 'stdout leaked into the next run');
  console.log('ok  failed / non-default runs leave no trace');

  eng.dispose();
  assert.strictEqual((await eng.run(inputs[1])).pdb, stored[1]);
  console.log('ok  dispose() and re-instantiate');
  console.log('all tests passed');
})().catch((e) => { console.error(e); process.exit(1); });
