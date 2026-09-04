#!/bin/sh
# Reproduce 02-tables/models/scn-test/pulchra.log from the CGBack paper
# (https://github.com/RikenSugitaLab/cgback-paper) with both the native C
# PULCHRA and the wasm port, then score them with the paper's own script.
#
# Requirements: gcc, node >= 18, the cgback-paper repo, and a Python source
# directory containing cgback 1.0.0 plus numpy, scipy and tqdm (see README.md).
set -e
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
PKG=$(CDPATH= cd "$SCRIPT_DIR/../.." && pwd)
RUN_FROM=$(pwd)
PAPER=${PAPER:-"$PKG/../cgback-paper-main"}       # path to the cloned paper repo
CGBACK_SRC=${CGBACK_SRC:-"$PKG/../cgsrc"}         # dir containing cgback/ (1.0.0)
case "$PAPER" in /*) ;; *) PAPER="$RUN_FROM/$PAPER" ;; esac
case "$CGBACK_SRC" in /*) ;; *) CGBACK_SRC="$RUN_FROM/$CGBACK_SRC" ;; esac
REF="$PAPER/02-tables/reference-pdbs/scn-test"
STORED="$PAPER/02-tables/models/scn-test/pulchra"

set -- "$REF"/*.pdb
if [ ! -f "$1" ]; then
  printf '%s\n' \
    "benchmark setup error: no reference PDBs found in $REF" \
    "Clone https://github.com/RikenSugitaLab/cgback-paper there, or set PAPER=/path/to/cgback-paper." >&2
  exit 2
fi
set -- "$STORED"/*.pdb
if [ ! -f "$1" ]; then
  printf '%s\n' "benchmark setup error: no stored PULCHRA models found in $STORED" >&2
  exit 2
fi
if [ ! -f "$CGBACK_SRC/cgback/parser.py" ]; then
  printf '%s\n' \
    "benchmark setup error: cgback 1.0.0 sources not found in $CGBACK_SRC" \
    "Install them there, or set CGBACK_SRC=/path/containing/cgback." >&2
  exit 2
fi
for f in calculate_metrics_v1.py parser.py penetration.py system.py; do
  [ -f "$PAPER/02-tables/$f" ] || {
    printf '%s\n' "benchmark setup error: missing $PAPER/02-tables/$f" >&2
    exit 2
  }
done

mkdir -p "$SCRIPT_DIR/work" && cd "$SCRIPT_DIR/work"
mkdir -p ca native wasm models/native-pulchra models/wasm-pulchra
rm -f ca/*.pdb native/*.pdb wasm/*.pdb \
  models/native-pulchra/*.pdb models/native-pulchra/*.log \
  models/wasm-pulchra/*.pdb models/wasm-pulchra/*.log
# 1. native PULCHRA from the same sources the wasm was built from
gcc -O2 -ffp-contract=off -include time.h -o pulchra_native \
  "$PKG/native/pulchra.c" "$PKG/native/pulchra_data.c" -lm 2>/dev/null

# 2. inputs: C-alpha trace of every reference structure (this is what the paper fed PULCHRA)
for f in "$REF"/*.pdb; do n=$(basename "$f" .pdb); awk '$1=="ATOM" && substr($0,13,4)==" CA "' "$f" > "ca/$n.pdb"; done

# 3. run native (default options) and compare with the paper's stored PDBs
ok=0; for f in ca/*.pdb; do n=$(basename "$f" .pdb); cp "$f" "native/$n.pdb"; ./pulchra_native "native/$n.pdb" >/dev/null
  cmp -s "native/$n.rebuilt.pdb" "$STORED/${n}_1.pdb" && ok=$((ok+1)); done; echo "native identical to stored: $ok/39"

# 4. run wasm and compare (also benchmarks; writes wasm/*.rebuilt.pdb)
STORED="$STORED" node ../bench_wasm2.js

# 5. cgback Iteration-0 checks (ring / chirality) + paper's metrics script
for src in native wasm; do d="models/$src-pulchra"; mkdir -p "$d"
  for p in $src/*.rebuilt.pdb; do n=$(basename "$p" .rebuilt.pdb); cp "$p" "$d/${n}_1.pdb"; done
  CGBACK_SRC="$CGBACK_SRC" PYTHONPATH="$CGBACK_SRC${PYTHONPATH:+:$PYTHONPATH}" \
    python3 ../cgback_check.py "$d"/*.pdb >/dev/null; done
for f in calculate_metrics_v1.py parser.py penetration.py system.py; do cp "$PAPER/02-tables/$f" .; done
for m in native-pulchra wasm-pulchra; do echo "== $m"; rm -f "models/$m.pkl"; PYTHONPATH="$CGBACK_SRC${PYTHONPATH:+:$PYTHONPATH}" python3 calculate_metrics_v1.py "$REF" "models/$m" 2>/dev/null; done
echo "== paper"; cat "$PAPER/02-tables/models/scn-test/pulchra.log"
