/**
 * Environment-agnostic core of @revanta/pulchrasm (plain ES module, no
 * dependencies; runs in Node, browsers, web workers, Deno, Bun).
 *
 * dist/pulchra.wasm is a standalone WASI reactor: it has no JavaScript glue
 * of its own.  This file supplies the few host functions it imports (stdout /
 * stderr, a clock) and marshals strings in and out of its memory.  PULCHRA's
 * file I/O is redirected inside the module to in-memory "virtual files", so
 * no file system is involved anywhere.
 *
 * The caller supplies a compiled `WebAssembly.Module` (or the raw bytes) once.
 * A `Pulchra` engine then instantiates it ONCE, lazily, and runs every
 * structure through that same instance.  Instantiation is where the 3.4 MB of
 * statistical tables are copied into linear memory, so this is the "load the
 * data once" step; each subsequent `run()` only pays for the reconstruction
 * itself.
 *
 * PULCHRA was written as a one-shot command-line program and keeps its
 * working state in C globals.  native/wasm_entry.c adds `pulchra_reset()`,
 * which frees that state and restores every option to its upstream default;
 * it is called before and after each run, so results are identical to a fresh
 * process and memory does not grow between runs.
 *
 * Concurrency model: one instance is single-threaded, so runs on one engine
 * execute strictly one after another.  `run()` is safe to call concurrently
 * (e.g. `Promise.all`) - the work is simply serialised.  For true parallelism
 * create several engines (each `createPulchra*()` call is independent) or one
 * per worker thread.
 */

const INPUT_NAME = 'input.pdb';
const INITIAL_NAME = 'initial.pdb';
const OUTPUT_NAME = 'input.rebuilt.pdb';
const TRAJECTORY_NAME = 'input.pdb.tra';

/**
 * Map of JS option name -> [C global name, CLI flag, kind]
 *   kind 'on'  : flag turns the switch ON  (value written = boolean)
 *   kind 'off' : flag turns the switch OFF (value written = !boolean)
 *   kind 'num' : numeric value
 */
export const OPTION_TABLE = {
  verbose:            ['VERBOSE',       '-v', 'on'],
  center:             ['CENTER_CHAIN',  '-n', 'on'],
  pdbsg:              ['PDB_SG',        '-g', 'on'],
  optimizeCa:         ['CA_OPTIMIZE',   '-c', 'off'],
  cisProline:         ['CISPRO',        '-p', 'on'],
  rearrangeBackbone:  ['BB_REARRANGE',  '-e', 'on'],
  rebuildBackbone:    ['REBUILD_BB',    '-b', 'off'],
  optimizeHBonds:     ['BB_OPTIMIZE',   '-q', 'on'],
  rebuildSidechains:  ['REBUILD_SC',    '-s', 'off'],
  fixExcludedVolume:  ['XVOLUME',       '-o', 'off'],
  checkChirality:     ['CHIRAL',        '-z', 'off'],
  hydrogens:          ['REBUILD_H',     '-h', 'on'],
  randomStart:        ['CA_RANDOM',     '-r', 'on'],
  timeSeed:           ['TIME_SEED',     '-x', 'on'],
  trajectory:         ['CA_TRAJECTORY', '-t', 'on'],
  preserve:           ['PRESERVE',      '-f', 'on'],
  maxShift:           ['CA_START_DIST', '-u', 'num'],
  caIterations:       ['CA_ITER',       null, 'num'],
  excludedVolumeIterations: ['XVOL_ITER', null, 'num'],
};

/** Upstream defaults of the compiled C globals (pulchra.c lines 59-77). */
export const UPSTREAM_DEFAULTS = Object.freeze({
  verbose: false,
  center: false,
  pdbsg: false,
  optimizeCa: true,
  cisProline: false,
  rearrangeBackbone: true,
  rebuildBackbone: true,
  optimizeHBonds: false,
  rebuildSidechains: true,
  fixExcludedVolume: true,
  checkChirality: true,
  hydrogens: false,
  randomStart: false,
  timeSeed: false,
  trajectory: false,
  preserve: true,
  maxShift: 3.0,
  caIterations: 100,
  excludedVolumeIterations: 3,
});

export class PulchraError extends Error {
  constructor(message, details) {
    super(message);
    this.name = 'PulchraError';
    Object.assign(this, details);
  }
}

function toUint8(bytes) {
  if (bytes instanceof Uint8Array) return bytes;
  if (bytes instanceof ArrayBuffer) return new Uint8Array(bytes);
  if (ArrayBuffer.isView(bytes)) return new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  throw new TypeError('wasmBinary must be an ArrayBuffer or Uint8Array');
}

/** Convert JS options into (a) C option overrides and (b) extra argv entries. */
export function compileOptions(options) {
  const overrides = [];
  const argv = [];
  for (const [key, value] of Object.entries(options)) {
    if (value === undefined || value === null) continue;
    const spec = OPTION_TABLE[key];
    if (!spec) continue; // non-C options handled elsewhere
    const [cname, , kind] = spec;
    if (kind === 'num') {
      if (typeof value !== 'number' || !Number.isFinite(value)) {
        throw new TypeError(`option "${key}" must be a finite number`);
      }
      overrides.push([cname, value]);
    } else {
      overrides.push([cname, value ? 1 : 0]);
    }
  }
  if (options.args) {
    if (!Array.isArray(options.args)) throw new TypeError('options.args must be an array of strings');
    for (const a of options.args) argv.push(String(a));
  }
  return { overrides, argv };
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/**
 * One live wasm instance plus the WASI host functions it needs.
 * @private
 */
class Instance {
  constructor() {
    this.exports = null;
    this.sink = null;            // { out: string[], err: string[] } of the run in progress
    this._partial = ['', ''];    // unterminated stdout / stderr line
  }

  async init(wasmModule) {
    const self = this;
    // The buffer is replaced when memory grows, so always re-derive views.
    const u8 = () => new Uint8Array(self.exports.memory.buffer);
    const dv = () => new DataView(self.exports.memory.buffer);

    const write = (fd, text) => {
      const idx = fd === 2 ? 1 : 0;
      const lines = (this._partial[idx] + text).split('\n');
      this._partial[idx] = lines.pop();
      if (this.sink) (idx ? this.sink.err : this.sink.out).push(...lines);
    };

    const wasi = {
      fd_write(fd, iovs, iovsLen, nwrittenPtr) {
        const d = dv(); const m = u8();
        let total = 0; let text = '';
        for (let i = 0; i < iovsLen; i++) {
          const ptr = d.getUint32(iovs + 8 * i, true);
          const len = d.getUint32(iovs + 8 * i + 4, true);
          text += decoder.decode(m.subarray(ptr, ptr + len));
          total += len;
        }
        if (fd === 1 || fd === 2) write(fd, text);
        d.setUint32(nwrittenPtr, total, true);
        return 0;
      },
      fd_read(fd, iovs, iovsLen, nreadPtr) { dv().setUint32(nreadPtr, 0, true); return 0; }, // EOF
      fd_seek() { return 8; },   // EBADF: no real files exist
      fd_close() { return 0; },
      clock_time_get(id, precision, outPtr) {
        dv().setBigUint64(outPtr, BigInt(Date.now()) * 1000000n, true);
        return 0;
      },
      proc_exit(code) { throw new PulchraError(`PULCHRA called exit(${code})`, { exitCode: code }); },
    };
    const env = { emscripten_notify_memory_growth() {} };

    // Provide exactly what the module asks for; fail loudly on anything new.
    const imports = {};
    for (const { module, name } of WebAssembly.Module.imports(wasmModule)) {
      const table = module === 'wasi_snapshot_preview1' ? wasi : module === 'env' ? env : null;
      if (!table || !table[name]) throw new PulchraError(`pulchra.wasm needs an unsupported import ${module}.${name}`);
      (imports[module] = imports[module] || {})[name] = table[name];
    }

    const { exports } = await WebAssembly.instantiate(wasmModule, imports);
    this.exports = exports;
    exports._initialize(); // run C static constructors (WASI reactor)
    return this;
  }

  /** Push any unterminated output line into the sink (end of run). */
  flush() {
    for (let idx = 0; idx < 2; idx++) {
      if (this._partial[idx] && this.sink) (idx ? this.sink.err : this.sink.out).push(this._partial[idx]);
      this._partial[idx] = '';
    }
  }

  /** Copy a JS string into wasm memory as a NUL-terminated C string. */
  cstr(s, allocated) {
    const bytes = encoder.encode(s);
    const p = this.exports.malloc(bytes.length + 1);
    const m = new Uint8Array(this.exports.memory.buffer);
    m.set(bytes, p);
    m[p + bytes.length] = 0;
    allocated.push(p);
    return p;
  }

  /** Read a NUL-terminated C string from wasm memory. */
  readCString(ptr) {
    const m = new Uint8Array(this.exports.memory.buffer);
    let end = ptr;
    while (m[end] !== 0) end++;
    return decoder.decode(m.subarray(ptr, end));
  }

  /** Store bytes as a virtual file (ownership of the buffer passes to C). */
  setFile(name, data, allocated) {
    const bytes = typeof data === 'string' ? encoder.encode(data) : toUint8(data);
    const p = this.exports.malloc(Math.max(1, bytes.length));
    new Uint8Array(this.exports.memory.buffer).set(bytes, p);
    if (this.exports.pulchra_vfs_set(this.cstr(name, allocated), p, bytes.length) !== 0) {
      throw new PulchraError('virtual file table full');
    }
  }

  /** Read a virtual file as text, or null if it does not exist. */
  getFile(name, allocated) {
    const n = this.cstr(name, allocated);
    const len = this.exports.pulchra_vfs_size(n);
    if (len < 0) return null;
    const ptr = this.exports.pulchra_vfs_data(n);
    return decoder.decode(new Uint8Array(this.exports.memory.buffer, ptr, len));
  }

  /** Names of all virtual files currently present. */
  listFiles() {
    const n = this.exports.pulchra_vfs_count();
    const names = [];
    for (let i = 0; i < n; i++) names.push(this.readCString(this.exports.pulchra_vfs_name(i)));
    return names;
  }
}

/**
 * A ready-to-use PULCHRA engine bound to one compiled wasm module and one
 * (lazily created, long-lived) wasm instance.
 */
export class Pulchra {
  /** @private use {@link createPulchraFromModule} / {@link createPulchraFromBinary} */
  constructor(wasmModule) {
    if (!(wasmModule instanceof WebAssembly.Module)) {
      throw new TypeError('Pulchra expects a compiled WebAssembly.Module');
    }
    this._module = wasmModule;
    this._instance = null;   // Promise<Instance> | null
  }

  /** Version number of the embedded PULCHRA C code (e.g. 3.06). */
  async version() {
    const I = await this._acquire();
    return I.exports.pulchra_version();
  }

  /**
   * Bytes currently allocated inside the wasm heap.  Between runs this is
   * constant: PULCHRA's per-structure memory is released by `pulchra_reset()`.
   */
  async heapUsed() {
    const I = await this._acquire();
    return I.exports.pulchra_heap_used();
  }

  /**
   * Whether the wasm instance currently exists.  It is created on the first
   * call that needs it and dropped by {@link dispose} or after a crash.
   */
  get loaded() {
    return this._instance !== null;
  }

  /**
   * Instantiate eagerly (copy the data tables into memory now rather than on
   * the first `run()`).  Optional; `run()` does this on demand.
   */
  async load() {
    await this._acquire();
    return this;
  }

  /**
   * Drop the wasm instance and its memory.  The engine stays usable: the next
   * call re-instantiates from the compiled module.
   */
  dispose() {
    this._instance = null;
  }

  /** @private get (creating if necessary) the shared instance */
  _acquire() {
    if (!this._instance) this._instance = new Instance().init(this._module);
    return this._instance;
  }

  /**
   * Run the PULCHRA program exactly as the command-line tool would.
   *
   * `argv` is the complete argument vector including `argv[0]`
   * (e.g. `['pulchra', '-v', 'model.pdb']`); `files` maps the file names the
   * program will open (the input, and the `-i` file if any) to their
   * contents.  Every option, the usage text, output naming and the exit code
   * are PULCHRA's own.  Files the program wrote come back in `files`
   * (e.g. `model.rebuilt.pdb`, `model.pdb.tra`).
   *
   * @param {string[]} argv
   * @param {Record<string, string|Uint8Array>} [files]
   * @returns {Promise<{exitCode:number, stdout:string, stderr:string, files:Record<string,string>}>}
   */
  exec(argv, files = {}) {
    if (!Array.isArray(argv) || !argv.every((a) => typeof a === 'string')) {
      throw new TypeError('argv must be an array of strings');
    }
    return this._exec(argv, files, []);
  }

  /** @private */
  async _exec(argv, files, overrides) {
    const instancePromise = this._acquire();
    const I = await instancePromise;
    const X = I.exports;

    // ------------------------------------------------------------------
    // Everything below is synchronous.  JavaScript cannot interleave two
    // runs inside this block, so the shared instance (its C globals, stdout
    // sink and virtual files) is never touched by two runs at once even when
    // callers overlap promises.
    // ------------------------------------------------------------------
    const allocated = [];
    const sink = { out: [], err: [] };
    I.sink = sink;
    let exitCode;
    const outputs = {};
    let crashed = false;
    try {
      X.pulchra_vfs_clear();
      for (const [name, data] of Object.entries(files)) I.setFile(name, data, allocated);

      const argvPtr = X.malloc(4 * (argv.length + 1));
      allocated.push(argvPtr);
      const argvStrs = argv.map((s) => I.cstr(s, allocated));
      const namesPtr = X.malloc(4 * Math.max(1, overrides.length));
      const valuesPtr = X.malloc(8 * Math.max(1, overrides.length));
      allocated.push(namesPtr, valuesPtr);
      const nameStrs = overrides.map(([name]) => I.cstr(name, allocated));

      const dv = new DataView(X.memory.buffer); // no allocation between here and the call
      argvStrs.forEach((p, i) => dv.setUint32(argvPtr + 4 * i, p, true));
      dv.setUint32(argvPtr + 4 * argv.length, 0, true);
      overrides.forEach(([, value], i) => {
        dv.setUint32(namesPtr + 4 * i, nameStrs[i], true);
        dv.setFloat64(valuesPtr + 8 * i, value, true);
      });

      try {
        exitCode = X.pulchra_run_with_options(
          argv.length, argvPtr, overrides.length, namesPtr, valuesPtr,
        );
      } catch (e) {
        // A trap/abort leaves the instance unusable; drop it so the next
        // run starts from a clean one.
        crashed = true;
        I.flush();
        throw new PulchraError(`PULCHRA crashed: ${e && e.message ? e.message : e}`, {
          stdout: sink.out.join('\n'),
          stderr: sink.err.join('\n'),
          cause: e,
        });
      }
      I.flush();

      for (const name of I.listFiles()) {
        if (!(name in files)) outputs[name] = I.getFile(name, allocated);
      }
    } finally {
      I.sink = null;
      if (crashed) {
        if (this._instance === instancePromise) this._instance = null;
      } else {
        X.pulchra_vfs_clear();
        for (const p of allocated) X.free(p);
      }
    }

    return {
      exitCode,
      stdout: sink.out.join('\n'),
      stderr: sink.err.join('\n'),
      files: outputs,
    };
  }

  /**
   * Reconstruct a full-atom model from a reduced (C-alpha, optionally with
   * side-chain centres) PDB string.
   *
   * @param {string|Uint8Array} pdb  Input PDB text.
   * @param {object} [options]       See README.
   * @returns {Promise<{pdb:string, stdout:string, stderr:string, exitCode:number, trajectory?:string, warnings:string[]}>}
   */
  async run(pdb, options = {}) {
    if (typeof pdb !== 'string' && !(pdb instanceof Uint8Array)) {
      throw new TypeError('pdb must be a string or Uint8Array');
    }
    const { overrides, argv: extraArgv } = compileOptions(options);

    const files = { [INPUT_NAME]: pdb };
    const argv = ['pulchra'];
    if (options.initialCoordinates !== undefined && options.initialCoordinates !== null) {
      files[INITIAL_NAME] = options.initialCoordinates;
      argv.push('-i', INITIAL_NAME);
    }
    argv.push(...extraArgv, INPUT_NAME);

    const r = await this._exec(argv, files, overrides);
    const rebuilt = OUTPUT_NAME in r.files ? r.files[OUTPUT_NAME] : null;
    const warnings = r.stdout.split('\n').filter((l) => /^WARNING/i.test(l));

    if (r.exitCode !== 0 || rebuilt === null) {
      throw new PulchraError(
        r.exitCode !== 0
          ? `PULCHRA exited with code ${r.exitCode}${r.stdout ? `: ${r.stdout.trim()}` : ''}`
          : 'PULCHRA produced no output file',
        { exitCode: r.exitCode, stdout: r.stdout, stderr: r.stderr },
      );
    }

    const result = { pdb: rebuilt, stdout: r.stdout, stderr: r.stderr, exitCode: r.exitCode, warnings };
    const wantsTrajectory = options.trajectory ||
      (options.args && options.args.some((a) => /^-[a-z]*t/.test(a)));
    if (wantsTrajectory) result.trajectory = r.files[TRAJECTORY_NAME] || '';
    return result;
  }
}

/** Build an engine from an already-compiled WebAssembly.Module. */
export function createPulchraFromModule(wasmModule) {
  return new Pulchra(wasmModule);
}

/** Build an engine from raw .wasm bytes (compiled once here). */
export async function createPulchraFromBinary(wasmBinary) {
  const mod = await WebAssembly.compile(toUint8(wasmBinary));
  return new Pulchra(mod);
}

/**
 * Fetch and compile the module from a URL (browsers, Deno, Bun, Node >= 18).
 * Uses streaming compilation when the server sends `application/wasm`.
 */
export async function compileFromUrl(url) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new PulchraError(`Failed to fetch ${url}: ${response.status} ${response.statusText}`);
  }
  if (typeof WebAssembly.compileStreaming === 'function' &&
      /application\/wasm/.test(response.headers.get('content-type') || '')) {
    return WebAssembly.compileStreaming(response);
  }
  return WebAssembly.compile(await response.arrayBuffer());
}

/** Turn a serialised error (e.g. from a worker) back into a PulchraError. */
export function reviveError(e) {
  if (!e || typeof e !== 'object') return new PulchraError(String(e));
  const { name, message, stack, ...details } = e;
  const err = name === 'TypeError' ? new TypeError(message) : new PulchraError(message, details);
  return err;
}

/** Serialise an error for postMessage (structured clone drops prototypes). */
export function serializeError(e) {
  if (!(e instanceof Error)) return { name: 'Error', message: String(e) };
  const out = { name: e.name, message: e.message };
  for (const k of ['exitCode', 'stdout', 'stderr']) if (k in e) out[k] = e[k];
  return out;
}
