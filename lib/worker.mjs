/**
 * Web Worker side of @revanta/pulchrasm.
 *
 * Load with  new Worker(new URL('@revanta/pulchrasm/worker', import.meta.url), { type: 'module' })
 * or, more conveniently, through `createPulchraWorker()` from the package's
 * browser entry, which wraps the message protocol in the same API as `Pulchra`.
 *
 * Protocol: the main thread posts `{ id, type, ...payload }` and receives
 * `{ id, ok: true, result }` or `{ id, ok: false, error }`.
 *   init     { wasmModule? | wasmBinary? | wasmUrl? }  -> true
 *   run      { pdb, options }                         -> run() result
 *   exec     { argv, files }                          -> exec() result
 *   version  {}                                       -> number
 *   heapUsed {}                                       -> number
 *   dispose  {}                                       -> true  (drops the wasm instance; engine stays usable)
 */
import {
  createPulchraFromModule,
  createPulchraFromBinary,
  compileFromUrl,
  serializeError,
  PulchraError,
} from './core.mjs';

let enginePromise = null;

async function init(msg) {
  if (msg.wasmModule) return createPulchraFromModule(msg.wasmModule);
  if (msg.wasmBinary) return createPulchraFromBinary(msg.wasmBinary);
  const url = msg.wasmUrl || new URL('../dist/pulchra.wasm', import.meta.url);
  return createPulchraFromModule(await compileFromUrl(url));
}

function engine() {
  if (!enginePromise) enginePromise = init({});
  return enginePromise;
}

self.onmessage = async (event) => {
  const msg = event.data || {};
  const { id, type } = msg;
  try {
    let result;
    switch (type) {
      case 'init':
        enginePromise = init(msg);
        await enginePromise;
        result = true;
        break;
      case 'run':
        result = await (await engine()).run(msg.pdb, msg.options || {});
        break;
      case 'exec':
        result = await (await engine()).exec(msg.argv, msg.files || {});
        break;
      case 'version':
        result = await (await engine()).version();
        break;
      case 'heapUsed':
        result = await (await engine()).heapUsed();
        break;
      case 'dispose':
        (await engine()).dispose();
        result = true;
        break;
      default:
        throw new PulchraError(`unknown worker message type "${type}"`);
    }
    self.postMessage({ id, ok: true, result });
  } catch (e) {
    self.postMessage({ id, ok: false, error: serializeError(e) });
  }
};
