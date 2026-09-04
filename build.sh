#!/bin/sh
# Builds dist/pulchra.wasm from the C sources as a standalone WASI reactor
# with WebAssembly SIMD (-msimd128; supported by every current engine).
# There is no JavaScript glue: lib/core.cjs provides the handful of WASI
# imports the module needs (stdout, a clock).  Requires emcc (Emscripten,
# tested with 6.0.9); without a local install:
#   podman run --rm -v "$PWD":/src:Z -w /src docker.io/emscripten/emsdk:6.0.9 ./build.sh
set -e
cd "$(dirname "$0")"
mkdir -p dist
rm -f dist/pulchra.js
emcc -O3 -msimd128 \
  native/wasm_entry.c native/pulchra_data.c \
  -o dist/pulchra.wasm \
  -lm \
  -s STANDALONE_WASM=1 \
  --no-entry \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=16777216 \
  -s EXPORTED_FUNCTIONS='["_pulchra_run","_pulchra_run_with_options","_pulchra_reset","_pulchra_heap_used","_pulchra_set_option","_pulchra_version","_pulchra_vfs_clear","_pulchra_vfs_set","_pulchra_vfs_data","_pulchra_vfs_size","_pulchra_vfs_count","_pulchra_vfs_name","_malloc","_free"]'
ls -la dist
