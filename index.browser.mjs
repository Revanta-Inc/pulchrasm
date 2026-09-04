// Browser / bundler (ESM) entry point for @revanta/pulchrasm.
//
// Two ways to use it:
//
//   1. On the main thread (blocks the UI while a structure is rebuilt):
//        const engine = await createPulchra();
//        const { pdb } = await engine.run(caTrace);
//
//   2. In a Web Worker (recommended for anything interactive):
//        const engine = await createPulchraWorker();
//        const { pdb } = await engine.run(caTrace);   // same API, off the main thread
//
// The .wasm file (and the worker script) are located relative to this module
// via import.meta.url, which Vite, webpack 5, Rollup, esbuild and native
// browsers all understand.  You can also pass `wasmUrl`, `wasmBinary` or a
// pre-compiled `wasmModule` (a WebAssembly.Module is structured-cloneable, so
// one compiled module can be shared by several workers).
import {
  Pulchra,
  PulchraError,
  UPSTREAM_DEFAULTS,
  createPulchraFromModule,
  createPulchraFromBinary,
  compileFromUrl,
  reviveError,
} from './lib/core.mjs';

export { Pulchra, PulchraError, UPSTREAM_DEFAULTS };

export const WASM_URL = new URL('./dist/pulchra.wasm', import.meta.url);

// ---------------------------------------------------------------------------
// Main-thread engine
// ---------------------------------------------------------------------------

/**
 * Create an independent PULCHRA engine on the current thread.
 * @param {{wasmUrl?: string|URL, wasmBinary?: Uint8Array|ArrayBuffer, wasmModule?: WebAssembly.Module}} [opts]
 */
export async function createPulchra(opts = {}) {
  if (opts.wasmModule) return createPulchraFromModule(opts.wasmModule);
  if (opts.wasmBinary) return createPulchraFromBinary(opts.wasmBinary);
  return createPulchraFromModule(await compileFromUrl(opts.wasmUrl || WASM_URL));
}

let enginePromise = null;

/** Lazily-created shared engine (downloaded + compiled once per page). */
export function getPulchra() {
  if (!enginePromise) enginePromise = createPulchra();
  return enginePromise;
}

/** One-shot convenience: rebuild a PDB string with the shared engine. */
export async function pulchra(pdb, options) {
  const engine = await getPulchra();
  return engine.run(pdb, options);
}

// ---------------------------------------------------------------------------
// Web Worker engine
// ---------------------------------------------------------------------------

/**
 * A PULCHRA engine living in a dedicated Web Worker.  Same asynchronous API
 * as {@link Pulchra} (`run`, `exec`, `version`, `heapUsed`, `dispose`), plus
 * `terminate()` to kill the worker.  Calls are serialised inside the worker
 * exactly as on a `Pulchra` engine; create several `PulchraWorker`s (sharing
 * one compiled `wasmModule`) to use several cores.
 */
export class PulchraWorker {
  /**
   * @param {{wasmUrl?: string|URL, wasmBinary?: Uint8Array|ArrayBuffer, wasmModule?: WebAssembly.Module, workerUrl?: string|URL}} [opts]
   */
  constructor(opts = {}) {
    this._pending = new Map();
    this._seq = 0;
    this._worker = opts.workerUrl
      ? new Worker(opts.workerUrl, { type: 'module', name: 'pulchra' })
      : new Worker(new URL('./lib/worker.mjs', import.meta.url), { type: 'module', name: 'pulchra' });
    this._worker.onmessage = (event) => {
      const { id, ok, result, error } = event.data || {};
      const p = this._pending.get(id);
      if (!p) return;
      this._pending.delete(id);
      if (ok) p.resolve(result); else p.reject(reviveError(error));
    };
    this._worker.onerror = (event) => {
      const err = new PulchraError(`PULCHRA worker failed: ${event && event.message ? event.message : event}`);
      this._failAll(err);
    };
    this._worker.onmessageerror = () => this._failAll(new PulchraError('PULCHRA worker: message could not be deserialised'));
    this._terminated = false;
    this.ready = this._call('init', {
      wasmModule: opts.wasmModule,
      wasmBinary: opts.wasmBinary,
      wasmUrl: opts.wasmUrl ? String(opts.wasmUrl) : undefined,
    }).then(() => this);
  }

  _failAll(err) {
    for (const p of this._pending.values()) p.reject(err);
    this._pending.clear();
  }

  _call(type, payload = {}) {
    if (this._terminated) return Promise.reject(new PulchraError('PULCHRA worker has been terminated'));
    return new Promise((resolve, reject) => {
      const id = ++this._seq;
      this._pending.set(id, { resolve, reject });
      this._worker.postMessage({ id, type, ...payload });
    });
  }

  /** Same as {@link Pulchra#run}, executed in the worker. */
  run(pdb, options = {}) {
    if (typeof pdb !== 'string' && !(pdb instanceof Uint8Array)) {
      return Promise.reject(new TypeError('pdb must be a string or Uint8Array'));
    }
    return this._call('run', { pdb, options });
  }

  /** Same as {@link Pulchra#exec}, executed in the worker. */
  exec(argv, files = {}) {
    return this._call('exec', { argv, files });
  }

  version() { return this._call('version'); }
  heapUsed() { return this._call('heapUsed'); }

  /** Drop the wasm instance inside the worker (it is re-created on demand). */
  dispose() { return this._call('dispose'); }

  /** Kill the worker.  Pending calls are rejected; the object is unusable afterwards. */
  terminate() {
    this._terminated = true;
    this._worker.terminate();
    this._failAll(new PulchraError('PULCHRA worker has been terminated'));
  }
}

/**
 * Create a PULCHRA engine in a Web Worker and wait until it has loaded the
 * wasm module.  By default the worker fetches and compiles
 * `dist/pulchra.wasm` itself, keeping the main thread free.
 */
export function createPulchraWorker(opts = {}) {
  return new PulchraWorker(opts).ready;
}

export default pulchra;
