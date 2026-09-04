# @revanta/pulchrasm

PULCHRA 3.06 compiled to WebAssembly for Node.js and the browser. It rebuilds
full-atom protein models from C-alpha traces, optionally using side-chain
centers, and exposes both a JavaScript API and a command-line interface.

The package is dependency-free at runtime. Its statistical tables are packed
into the WebAssembly binary, decoded when an engine is loaded, and reused for
every reconstruction performed by that engine.

## Installation

```sh
npm install @revanta/pulchrasm
```

Node.js 20.19 or later is required.

## Command line

`pulchrasm` accepts the same arguments as PULCHRA and preserves its output
naming, usage text, and exit status.

```sh
npx pulchrasm model.pdb        # writes model.rebuilt.pdb
npx pulchrasm -v -n model.pdb  # verbose output; center the chain
npx pulchrasm                  # print PULCHRA usage
```

The wrapper loads the input and any `-i` file into memory, passes the complete
argument vector to PULCHRA, and writes generated files such as
`*.rebuilt.pdb` and `*.pdb.tra` to disk.

## Node.js API

```js
import { createPulchra } from '@revanta/pulchrasm';

const engine = await createPulchra();
const { pdb, stdout, warnings } = await engine.run(caTracePdbText);

// Reuse the engine so the WebAssembly module and data tables are loaded once.
for (const trace of traces) {
  const result = await engine.run(trace);
  console.log(result.pdb);
}
```

The package also provides a shared, lazily created engine through `getPulchra()`
and a convenience function for one-off calls:

```js
import { pulchra } from '@revanta/pulchrasm';

const { pdb } = await pulchra(caTracePdbText);
```

CommonJS is supported:

```js
const { createPulchra } = require('@revanta/pulchrasm');
```

## Browser API

For interactive applications, run PULCHRA in a Web Worker so reconstruction
does not block the main thread:

```js
import { createPulchraWorker } from '@revanta/pulchrasm';

const engine = await createPulchraWorker();
const { pdb } = await engine.run(caTracePdbText);

engine.terminate();
```

`PulchraWorker` supports `run()`, `exec()`, `version()`, `heapUsed()`, and
`dispose()`, matching `Pulchra`, plus `terminate()` for stopping the worker.
The worker module and WebAssembly binary are resolved with `import.meta.url`
and work with native browser modules and common bundlers.

One worker processes calls sequentially. To use multiple CPU cores, create a
worker pool and share one compiled `WebAssembly.Module`:

```js
const wasmUrl = new URL('@revanta/pulchrasm/wasm', import.meta.url);
const wasmModule = await WebAssembly.compileStreaming(fetch(wasmUrl));

const workers = await Promise.all(
  Array.from({ length: 4 }, () => createPulchraWorker({ wasmModule })),
);
```

`createPulchra()` is also available in browsers when main-thread execution is
appropriate.

## API reference

### `engine.run(pdb, options?)`

Reconstructs a PDB supplied as a string or `Uint8Array` and resolves to:

```js
{
  pdb,
  stdout,
  stderr,
  exitCode,
  warnings,
  trajectory, // included when trajectory output is requested
}
```

Named options correspond to PULCHRA settings:

```js
const result = await engine.run(caTracePdbText, {
  verbose: true,
  center: true,
  optimizeCa: true,
  rebuildBackbone: true,
  rebuildSidechains: true,
  optimizeHBonds: false,
  hydrogens: false,
});
```

Supported options are `verbose`, `center`, `pdbsg`, `optimizeCa`,
`cisProline`, `rearrangeBackbone`, `rebuildBackbone`, `optimizeHBonds`,
`rebuildSidechains`, `fixExcludedVolume`, `checkChirality`, `hydrogens`,
`randomStart`, `timeSeed`, `trajectory`, `preserve`, `maxShift`,
`caIterations`, and `excludedVolumeIterations`.

Use `initialCoordinates` for the contents of PULCHRA's `-i` input and `args`
for raw command-line flags. The exported `UPSTREAM_DEFAULTS` object contains
the default value of every named option.

`run()` rejects with `PulchraError` if PULCHRA exits with a nonzero status or
does not produce a rebuilt structure. The error includes the available
`exitCode`, `stdout`, and `stderr` fields.

### `engine.exec(argv, files?)`

Runs the underlying program with command-line semantics. `argv` is the full
argument vector, including `argv[0]`, and `files` maps virtual file names to
strings or `Uint8Array` values.

```js
const result = await engine.exec(
  ['pulchra', '-v', 'model.pdb'],
  { 'model.pdb': caTracePdbText },
);

// result: { exitCode, stdout, stderr, files }
```

The returned `files` object contains every file created by PULCHRA.

### Engine lifecycle

- `load()` instantiates the engine immediately. Loading is otherwise lazy.
- `loaded` reports whether the WebAssembly instance exists.
- `version()` returns the embedded PULCHRA version.
- `heapUsed()` returns the number of bytes allocated in the WebAssembly heap.
- `dispose()` releases the instance and its memory. The next operation loads a
  new instance.

## Execution model

Each `Pulchra` engine owns one long-lived WebAssembly instance. PULCHRA's
per-structure native state is reset before and after every call, so results do
not depend on previous inputs and temporary allocations are released promptly.
If WebAssembly traps, the engine discards the affected instance and loads a
clean one on the next call.

Calls on a single engine execute sequentially, including calls started through
`Promise.all()`. Create independent engines or workers for parallel execution.

PULCHRA reads and writes through an in-memory virtual file system implemented
inside the module. The standalone WASI module needs no Emscripten JavaScript
runtime or host file-system access. Optimized grid searches and statistical
table lookups preserve the coordinates produced by the upstream algorithm.

The native integration is implemented in `native/wasm_entry.c`. The complete
source changes are recorded in `native/pulchra.upstream.patch`, with supporting
code in `native/fast_grid.h`, `native/fast_tables.h`, and
`native/pulchra_data.c`.

## Validation

The test suite runs 39 SCN-test structures through a shared engine and verifies
that the rebuilt PDB files are byte-for-byte identical to the stored PULCHRA
outputs. It also checks stable heap usage, deterministic state isolation,
overlapping calls, engine disposal, option handling, trajectories, output
naming, and command-line exit behavior.

```sh
npm test
```

The benchmark in `benchmark/scn-test/` reproduces PULCHRA's row in the CGBack
paper's SCN test table with both the native program and this WebAssembly port.
See [`REPRODUCTION.md`](REPRODUCTION.md) for results. It expects the paper
checkout and the `cgback` 1.0.0 Python sources in sibling directories by
default:

```sh
git clone https://github.com/RikenSugitaLab/cgback-paper.git ../cgback-paper-main
python3 -m pip install --target ../cgsrc --no-deps cgback==1.0.0 numpy scipy tqdm
npm run bench
```

Set `PAPER=/path/to/cgback-paper` or
`CGBACK_SRC=/path/containing/cgback` to use other locations.

## Building

Building requires Emscripten and produces the standalone WASI module at
`dist/pulchra.wasm`:

```sh
./build.sh
```

The build is tested with Emscripten 6.0.9. A containerized build can be run
without a local Emscripten installation:

```sh
podman run --rm \
  -v "$PWD":/src:Z \
  -w /src \
  docker.io/emscripten/emsdk:6.0.9 \
  ./build.sh
```

To regenerate and verify the packed statistical tables:

```sh
npm run pack-tables
```

## Attribution and citation

PULCHRA was developed by Piotr Rotkiewicz and Jeffrey Skolnick. The original C
implementation is copyright 2000-2009 Piotr Rotkiewicz. This package ports
PULCHRA 3.06 to WebAssembly and is maintained independently of the original
authors.

If you use PULCHRA in research, cite:

> Piotr Rotkiewicz and Jeffrey Skolnick. “Fast procedure for reconstruction of
> full-atom protein models from reduced representations.” *Journal of
> Computational Chemistry* 29(9), 1460-1465 (2008).
> [doi:10.1002/jcc.20906](https://doi.org/10.1002/jcc.20906)

See the [PULCHRA project page](https://www.pirx.com/pulchra/) and
[upstream source](https://github.com/euplotes/pulchra) for the original
software.

## License

The WebAssembly wrapper is released under the MIT License. PULCHRA is included
under the terms in [`native/LICENSE`](native/LICENSE).
