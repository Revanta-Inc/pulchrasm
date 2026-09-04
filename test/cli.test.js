'use strict';
// Checks that bin/pulchrasm.mjs behaves like the original command-line tool.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const BIN = path.join(__dirname, '..', 'bin', 'pulchrasm.mjs');
const STORED = process.env.PULCHRA_TEST_FIXTURES ||
  path.join(__dirname, '..', 'pulchra-scn-test-reproduction', 'outputs', 'wasm-pulchra');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'pulchrasm-cli-'));
const run = (args, cwd = tmp) => spawnSync(process.execPath, [BIN, ...args], { cwd, encoding: 'utf8' });

const name = fs.readdirSync(STORED).filter((f) => f.endsWith('.pdb')).sort()[0];
const stored = fs.readFileSync(path.join(STORED, name), 'utf8');
const ca = stored.split('\n').filter((l) => /^ATOM.{8} CA /.test(l)).join('\n') + '\n';
fs.writeFileSync(path.join(tmp, 'model.pdb'), ca);
fs.mkdirSync(path.join(tmp, 'sub'));
fs.writeFileSync(path.join(tmp, 'sub', 'other.pdb'), ca);

// default run: output next to the input, exit 0, PULCHRA's one stdout line
let r = run(['model.pdb']);
assert.strictEqual(r.status, 0, r.stdout + r.stderr);
assert.strictEqual(r.stdout, 'Initial coordinates will be preserved.\n');
assert.strictEqual(fs.readFileSync(path.join(tmp, 'model.rebuilt.pdb'), 'utf8'), stored);
console.log('ok  pulchrasm model.pdb -> model.rebuilt.pdb, identical to stored output');

// path with a directory component
r = run(['-v', 'sub/other.pdb']);
assert.strictEqual(r.status, 0, r.stdout);
assert.ok(/residua read/.test(r.stdout) && /Writing output file sub\/other.rebuilt.pdb/.test(r.stdout));
assert.strictEqual(fs.readFileSync(path.join(tmp, 'sub', 'other.rebuilt.pdb'), 'utf8'), stored);
console.log('ok  -v and directory components');

// combined flags, -i <file> and -u <value> must be consumed as in main():
// the positional input is still model.pdb.  (With PULCHRA's default
// "preserve" the C-alpha optimisation never runs, so -i/-t have no effect;
// the CLI has no flag to turn preserve off, exactly like the original.)
fs.unlinkSync(path.join(tmp, 'model.rebuilt.pdb'));
r = run(['-vt', '-u', '2.0', '-i', 'sub/other.pdb', 'model.pdb']);
assert.strictEqual(r.status, 0, r.stdout);
assert.ok(/Initial coordinates will be preserved/.test(r.stdout));
assert.strictEqual(fs.readFileSync(path.join(tmp, 'model.rebuilt.pdb'), 'utf8'), stored);
assert.ok(!fs.existsSync(path.join(tmp, 'model.pdb.tra')));
console.log('ok  combined flags, -i, -u');

// engine-level: -i is read and the trajectory is written when preserve is off
const { createPulchra } = require('..');
(async () => {
  const eng = await createPulchra();
  const r2 = await eng.exec(['pulchra', '-vt', '-i', 'ini.pdb', 'm.pdb'], { 'm.pdb': ca, 'ini.pdb': ca });
  assert.ok(/Initial coordinates will be preserved/.test(r2.stdout));
  const r3 = await eng.run(ca, { args: ['-vt'], preserve: false, initialCoordinates: ca });
  assert.ok(/Reading initial structure initial.pdb/.test(r3.stdout) && r3.trajectory.endsWith('END\n'));
  console.log('ok  exec() reads -i and writes the trajectory once preserve is off');
})().catch((e) => { console.error(e); process.exit(1); });

// usage: no file -> PULCHRA's usage text, exit 255 (C returns -1)
r = run([]);
assert.strictEqual(r.status, 255);
assert.ok(/^PULCHRA Protein Chain Restoration Algorithm version 3.06\nUsage: pulchrasm \[options\] <pdb_file>/.test(r.stdout));
assert.ok(/-u value : maximum shift/.test(r.stdout));
console.log('ok  usage text and exit status 255');

// unknown option, missing file
r = run(['-Q', 'model.pdb']);
assert.strictEqual(r.status, 255);
assert.strictEqual(r.stdout, 'Unknown option: Q\n');
r = run(['-v', 'does-not-exist.pdb']);
assert.strictEqual(r.status, 255);
assert.ok(/Can't read the input file!/.test(r.stdout));
r = run(['does-not-exist.pdb']);
assert.strictEqual(r.status, 255);
assert.strictEqual(r.stdout, '');
console.log('ok  unknown option / missing input behave like the C program');

fs.rmSync(tmp, { recursive: true, force: true });
console.log('all CLI tests passed');
