// Node.js (ESM) entry point for @revanta/pulchrasm.
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import {
  Pulchra,
  PulchraError,
  UPSTREAM_DEFAULTS,
  OPTION_TABLE,
  createPulchraFromModule,
  createPulchraFromBinary,
} from './lib/core.mjs';

export { Pulchra, PulchraError, UPSTREAM_DEFAULTS, OPTION_TABLE };

export const WASM_PATH = fileURLToPath(new URL('./dist/pulchra.wasm', import.meta.url));

/**
 * Create an independent PULCHRA engine.
 * @param {{wasmBinary?: Uint8Array|ArrayBuffer, wasmModule?: WebAssembly.Module, wasmPath?: string}} [opts]
 */
export async function createPulchra(opts = {}) {
  if (opts.wasmModule) return createPulchraFromModule(opts.wasmModule);
  const bytes = opts.wasmBinary || fs.readFileSync(opts.wasmPath || WASM_PATH);
  return createPulchraFromBinary(bytes);
}

let enginePromise = null;

/** Lazily-created shared engine (compiled once per process). */
export function getPulchra() {
  if (!enginePromise) enginePromise = createPulchra();
  return enginePromise;
}

/** One-shot convenience: rebuild a PDB string with the shared engine. */
export async function pulchra(pdb, options) {
  const engine = await getPulchra();
  return engine.run(pdb, options);
}

export default pulchra;
