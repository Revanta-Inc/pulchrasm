const { createPulchra } = require('../../index.cjs');
const fs = require('fs'), path = require('path');
const { performance } = require('perf_hooks');
const median = a => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
(async () => {
  const t0 = performance.now(); const eng = await createPulchra(); const compileMs = performance.now() - t0;
  // one-time instantiation (tables copied into memory); happens once per engine, not per run
  const inst = []; for (let i = 0; i < 20; i++) { eng.dispose(); const t = performance.now(); await eng.load(); inst.push(performance.now() - t); }
  const files = fs.readdirSync('ca').filter(f => f.endsWith('.pdb')).sort();
  const STORED = process.env.STORED; let identical = 0;
  const res = {};
  for (const f of files) {
    const n = f.slice(0, -4);
    const input = fs.readFileSync(path.join('ca', f), 'utf8');
    const ts = []; let out;
    for (let i = 0; i < 10; i++) { const t = performance.now(); out = await eng.run(input); ts.push(performance.now() - t); }
    fs.writeFileSync(`wasm/${n}.rebuilt.pdb`, out.pdb);
    if (STORED) identical += out.pdb === fs.readFileSync(`${STORED}/${n}_1.pdb`, 'utf8');
    res[n] = { nres: input.trim().split('\n').length, median_ms: median(ts), min_ms: Math.min(...ts) };
  }
  if (STORED) console.log(`wasm identical to stored: ${identical}/${files.length}`);
  fs.writeFileSync('wasm_bench.json', JSON.stringify({ compileMs, instantiate_median_ms: median(inst), res }, null, 1));
  console.log(`wasm: compile ${compileMs.toFixed(1)} ms (once), instantiate ${median(inst).toFixed(2)} ms (once per engine)`);
})();
