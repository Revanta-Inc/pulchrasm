#!/usr/bin/env node
// Command-line front end with the behaviour of the original PULCHRA binary:
//
//   pulchrasm [options] <pdb_file>
//
// The argument vector is handed to the PULCHRA C code unchanged, so options,
// their combinations (-vc), the usage text, "Unknown option" handling, the
// output file name (<pdb_file minus ".pdb">.rebuilt.pdb, next to the input),
// the trajectory file (<pdb_file>.tra with -t) and the exit status (255 on
// error, like the C program's `return -1`) are all PULCHRA's own.  This
// script only reads the input file(s) into memory beforehand and writes the
// produced file(s) back to disk afterwards.
import fs from 'node:fs';
import path from 'node:path';
import { createPulchra } from '../index.node.mjs';

const args = process.argv.slice(2);

// Find the file names PULCHRA will open, using the same scan as its main():
// the first non-option argument is the input; "-i <file>" names the initial
// C-alpha coordinates; "-u <value>" consumes the next argument; the rest of
// an option group after 'i' or 'u' is ignored.
const inputs = new Set();
let name = null;
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a[0] === '-') {
    for (let j = 1; j < a.length; j++) {
      if (a[j] === 'i') { if (i + 1 < args.length) inputs.add(args[++i]); break; }
      if (a[j] === 'u') { i++; break; }
    }
  } else if (name === null) {
    name = a;
  }
}
if (name !== null) inputs.add(name);

const files = {};
for (const f of inputs) {
  try { files[f] = fs.readFileSync(f); } catch (_) { /* PULCHRA reports the missing file itself */ }
}

let result;
try {
  const engine = await createPulchra();
  result = await engine.exec(['pulchrasm', ...args], files);
} catch (e) {
  process.stderr.write(`pulchrasm: ${e && e.message ? e.message : e}\n`);
  process.exit(255);
}

if (result.stdout) process.stdout.write(result.stdout + '\n');
if (result.stderr) process.stderr.write(result.stderr + '\n');
for (const [file, text] of Object.entries(result.files)) {
  const dir = path.dirname(file);
  if (dir && dir !== '.') fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(file, text);
}
process.exitCode = result.exitCode & 0xff;
