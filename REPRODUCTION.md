# Reproducing the CGBack paper's PULCHRA results — native C vs. WebAssembly

Target: `02-tables/models/scn-test/pulchra.log` in
https://github.com/RikenSugitaLab/cgback-paper

```
Bond score:       98.598 ± 0.000
Clash score:       0.526 ± 0.000
Chirality score:   0.000 ± 0.000
Ring score:        0.254 ± 0.000
Diversity score:     nan
```

## Result

| Metric | Paper (committed log) | Native C (gcc -O2) | wasm port (Node 22) |
|---|---:|---:|---:|
| Bond score | 98.598 ± 0.000 | **98.598 ± 0.000** | **98.598 ± 0.000** |
| Clash score | 0.526 ± 0.000 | **0.526 ± 0.000** | **0.526 ± 0.000** |
| Chirality score | 0.000 ± 0.000 | **0.000 ± 0.000** | **0.000 ± 0.000** |
| Ring score | 0.254 ± 0.000 | **0.254 ± 0.000** | **0.254 ± 0.000** |
| Diversity score | nan | nan | nan |
| Rebuilt PDBs byte-identical to the paper's 39 stored files | — | **39 / 39** | **39 / 39** |

Both implementations reproduce every one of the 39 rebuilt structures stored in the
paper repository byte-for-byte, and the paper's own scoring script therefore
produces the identical five-line log. (Diversity is `nan` in the paper too: it needs
more than one sample per protein and PULCHRA is deterministic.)

## How the paper ran PULCHRA (reverse-engineered)

The repository does not contain the PULCHRA command line, so it was recovered by
trial: the stored files carry `REMARK 999 REBUILT BY PULCHRA V.3.06` (the exact
version at github.com/euplotes/pulchra), and feeding the **C-alpha trace of each
reference PDB** to `pulchra <file>` with **default options** reproduces them exactly.
Feeding the full-atom reference or enabling C-alpha optimisation does not.

## Pipeline

1. `reference-pdbs/scn-test/*.pdb` → keep `ATOM` records whose atom name is `CA` (39 CASP12 targets, 10 273 residues).
2. Run PULCHRA (native: `./pulchra X.pdb`; wasm: `pulchra(pdbText)` from the npm package), default options.
3. Compare to `models/scn-test/pulchra/<name>_1.pdb` with `cmp`.
4. Run the check that produces the `[INFO] Number of residues with rings / Number of chiral centers / Iteration: 0 …` log lines that `calculate_metrics_v1.py` parses for the chirality and ring scores. The paper obtained these from `cgback -v --skip-sampling --skip-add-hydrogen --fix-structure-max-iterations 0`. cgback needs PyTorch only to load its diffusion model, which is irrelevant when no fixing iterations run, so `benchmark/scn-test/cgback_check.py` calls cgback 1.0.0's own `penetration.py` / `hydrogen.py` / `clash.py` functions directly. Validated against the 39 stored cgback logs: all counts identical.
5. Run the paper's unmodified `02-tables/calculate_metrics_v1.py` on `(reference dir, model dir)`.

Everything is scripted in `benchmark/scn-test/reproduce.sh` of the package.

## Benchmark

Environment: Intel Xeon @ 2.10 GHz (sandbox VM), Node v22.22.2, gcc 13 `-O2` (native),
Emscripten 3.1.6 / LLVM 15 `-O3` (wasm). Each protein run 10× ; median wall time.
Native time is one process invocation (`subprocess.run`), i.e. includes ~0.9 ms of process spawn.
wasm time is one `engine.run()` call, i.e. includes creating a fresh wasm instance
(3.3 ms: zero 16 MB heap, copy the 3.4 MB embedded data tables, set up the virtual FS)
plus writing/reading the PDB through the in-memory FS. One-off wasm compile: 6 ms.

| | Native C | wasm | ratio |
|---|---:|---:|---:|
| Total, 39 proteins / 10273 residues | 2017 ms | 2575 ms | 1.28× |
| Throughput | 5092 res/s | 3989 res/s | |
| Proteins < 150 residues (13) | 164 ms | 291 ms | 1.78× |
| Proteins ≥ 400 residues (8) | 850 ms | 998 ms | 1.18× |
| Fixed cost per call | 0.9 ms (spawn) | 3.3 ms (instantiate) | |

The wasm port runs at about **78 % of native speed overall** and within **5–15 % of
native on the largest structures**; the gap on small proteins is the per-call
instantiation cost, which is deliberate (PULCHRA keeps all state in C globals and
never frees memory, so a fresh instance per call is what makes the port
re-entrant, leak-free and safe to run concurrently).

### Per protein

| Protein | Residues | Native (ms) | wasm (ms) | wasm/native |
|---|---:|---:|---:|---:|
| TBM#T0865 | 62 | 3.5 | 14.1 | 4.02× |
| TBM#T0922 | 74 | 8.0 | 15.8 | 1.98× |
| TBM#T0872 | 88 | 10.2 | 14.8 | 1.45× |
| FM#T0862 | 93 | 9.3 | 15.0 | 1.61× |
| FM#T0900 | 102 | 12.7 | 27.0 | 2.14× |
| FM#T0869 | 104 | 14.0 | 21.9 | 1.57× |
| TBM#T0891 | 112 | 14.4 | 22.7 | 1.57× |
| FM#T0859 | 113 | 10.5 | 25.2 | 2.39× |
| FM#T0866 | 115 | 13.4 | 24.7 | 1.84× |
| TBM-hard#T0868 | 116 | 14.3 | 22.8 | 1.60× |
| FM#T0870 | 123 | 17.8 | 25.3 | 1.42× |
| TBM#T0860 | 136 | 20.3 | 29.4 | 1.45× |
| TBM#T0921 | 138 | 15.5 | 32.6 | 2.10× |
| TBM-hard#T0898 | 161 | 34.2 | 38.7 | 1.13× |
| TBM#T0947 | 175 | 40.7 | 51.8 | 1.27× |
| TBM-hard#T0892 | 193 | 48.2 | 75.9 | 1.58× |
| TBM#T0879 | 220 | 38.0 | 52.3 | 1.38× |
| FM#T0886 | 229 | 22.6 | 29.7 | 1.32× |
| TBM#T0889 | 239 | 34.4 | 50.0 | 1.45× |
| TBM#T0893 | 242 | 52.2 | 71.0 | 1.36× |
| FM#T0864 | 246 | 36.9 | 49.9 | 1.35× |
| FM#T0897 | 262 | 33.6 | 43.4 | 1.29× |
| TBM#T0902 | 300 | 32.2 | 53.9 | 1.67× |
| FM#T0904 | 311 | 61.6 | 78.4 | 1.27× |
| TBM#T0861 | 312 | 43.9 | 53.7 | 1.22× |
| TBM#T0871 | 319 | 83.4 | 90.6 | 1.09× |
| FM#T0941 | 341 | 99.0 | 105.3 | 1.06× |
| TBM#T0928 | 341 | 49.4 | 80.9 | 1.64× |
| TBM#T0903 | 348 | 45.1 | 61.2 | 1.36× |
| FM#T0918 | 349 | 87.6 | 101.2 | 1.16× |
| TBM-hard#T0945 | 375 | 99.5 | 110.4 | 1.11× |
| TBM#T0942 | 387 | 61.6 | 87.0 | 1.41× |
| TBM#T0911 | 408 | 65.7 | 86.3 | 1.31× |
| TBM-hard#T0896 | 447 | 83.5 | 111.7 | 1.34× |
| TBM#T0873 | 462 | 78.2 | 89.4 | 1.14× |
| TBM-hard#T0943 | 509 | 139.8 | 145.2 | 1.04× |
| TBM#T0920 | 540 | 146.5 | 201.3 | 1.37× |
| FM#T0863 | 582 | 125.9 | 145.8 | 1.16× |
| TBM-hard#T0912 | 599 | 209.9 | 218.8 | 1.04× |

## Files

- `benchmark/scn-test/reproduce.sh` — full pipeline (inputs → both implementations → cgback checks → paper's metrics script).
- `benchmark/scn-test/bench_wasm2.js` — wasm run + timing + byte comparison.
- `benchmark/scn-test/cgback_check.py` — torch-free driver for cgback 1.0.0's Iteration-0 ring/chirality check, emitting the log layout `calculate_metrics_v1.py` expects.
- `outputs/native-pulchra/`, `outputs/wasm-pulchra/` — the 39 rebuilt PDBs and cgback-style logs from each implementation; `*.metrics.log` — the scoring script's output.

## Addendum: one wasm instance for all structures

The benchmark above instantiated a fresh wasm instance for every call, because
PULCHRA keeps its state in C globals. `pulchra_reset()` (see README.md,
"Changes to the upstream source") now returns that state to pristine, so an
engine keeps one instance and only loads the 3.4 MB of tables once.

Correctness: the 39 structures run forward and then backward through one
instance are byte-identical to the stored files (78/78), the heap holds 0 bytes
between runs, and verbose stdout / trajectories for non-default option sets
match the fresh-instance engine exactly. The reset path was also run under
AddressSanitizer natively (80 reconstructions in one process, no findings).

The module was then rebuilt as a standalone WASI reactor (PULCHRA's file I/O
redirected to in-memory buffers inside the wasm; no Emscripten JavaScript
runtime). Same checks, same results: 78/78 identical, heap flat, option runs
identical to the fresh-instance engine.

Timing on an Apple-silicon laptop (Node 24, median of 10 runs per protein;
the Xeon VM used above was not available):

| | fresh Emscripten instance per run (old) | one standalone instance (new) | |
|---|---:|---:|---:|
| all 39 proteins (sum of medians) | 1748 ms | 1670 ms | 1.05× |
| proteins < 150 residues (13) | 153 ms | 132 ms | 1.16× |
| proteins ≥ 400 residues (7) | 739 ms | 719 ms | 1.03× |
| smallest protein (62 residues) | 3.98 ms | 2.81 ms | 1.42× |
| one-time load (instantiate + table copy) | per call | 1.1 ms | |
| JavaScript shipped | 64 KB glue + wrapper | wrapper only | |

On this machine instantiation was cheap (about 1 ms per call), so the speed-up
is modest and confined to small proteins; on the slower Xeon VM the per-call
overhead was 3.3 ms, i.e. a larger share of the small-protein time. The main
gains are structural: no 16 MB heap zeroing and 3.4 MB table copy per call,
constant memory across any number of structures, and the C code no longer
depends on a fresh process for correctness.

## Addendum: fast grid / tables and SIMD

`native/fast_grid.patch` (with `fast_grid.h` and `fast_tables.h`) was applied
and the wasm rebuilt with `-msimd128`. The patch changes the data layout of the
excluded-volume conflict search and the scan range of the statistical-table
lookups, not the arithmetic; the native `PULCHRA_GRID_SELFCHECK` build, which
runs the upstream and the fast conflict search on every call and aborts on any
disagreement, ran the 39 structures twice plus option runs under
AddressSanitizer without a finding. All four wasm builds below are 39/39
byte-identical to the stored PULCHRA output.

Same laptop and method as the previous addendum (median of 10 runs per protein):

| | old: Emscripten, fresh instance per run | standalone, one instance | + fast grid / tables | + SIMD (shipped) |
|---|---:|---:|---:|---:|
| all 39 proteins (sum of medians) | 1732 ms | 1676 ms | 330 ms (5.25×) | 328 ms (5.27×) |
| proteins < 150 residues (13) | 154 ms | 134 ms | 35 ms (4.39×) | 36 ms (4.32×) |
| proteins ≥ 400 residues (7) | 731 ms | 720 ms | 125 ms (5.84×) | 125 ms (5.86×) |
| largest protein (599 residues) | 184 ms | 182 ms | 26 ms (7.10×) | 26 ms (7.20×) |
| smallest protein (62 residues) | 4.46 ms | 4.93 ms | 1.16 ms (3.85×) | 1.15 ms (3.86×) |
| whole set, median of 5 sequential passes | 1722 ms | 1672 ms | 332 ms | 332 ms |

The entire gain comes from the fast grid and table lookups. WebAssembly SIMD
makes no measurable difference for this code (double-precision pointer-chasing
loops that LLVM cannot vectorise); the flag is kept since it costs nothing and
every current engine supports it. The whole SCN test set now rebuilds in about
a third of a second, roughly five times faster than the native `gcc -O2`
binary measured at the top of this document on the Xeon VM.

## Addendum: packed statistical tables

The wasm binary was 97% data. The tables are now shipped as int16
milli-Ångström coordinates and int8 indices (`native/packed_tables.h`,
generated by `native/tools/pack_tables.c`) and decoded at start-up into the
original float/int arrays. `native/tools/verify_tables.c` confirms every
decoded value is bit-identical to the original headers, and all suites
(39/39 stored outputs, options, CLI, native gcc build) pass unchanged.

| | before | after |
|---|---:|---:|
| `dist/pulchra.wasm` | 3351 KB | 1665 KB |
| gzip -9 | 1881 KB | 1243 KB |
| brotli 11 | 1543 KB | 1081 KB |
| instantiate + decode | 1.1 ms | 1.1 ms |
| whole 39-protein set | 332 ms | 332 ms |
