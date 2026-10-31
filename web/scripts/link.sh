#!/bin/bash
# (the repository: two folders up from this script, in web/scripts or web/build)
GTA4_ROOT=${GTA4_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
# Link GTA IV's game code, the GTA IV glue, the ReXGlue runtime and the
# third-party libraries into one WebAssembly module (memory64 + pthreads).
# Threads get 4 MiB stacks: with the 64 KiB default a runtime thread overflows
# silently (no guard page in WebAssembly) and corrupts game memory.
# wasmfs_fetch.js replaces WasmFS's fetch backend (parallel, retried reads).
# Usage: link.sh <thirdparty build dir> [extra em++ flags...]
set -u
S=$(dirname "$0")
B=$1
shift
source ${EMSDK_DIR:-$GTA4_ROOT/../emsdk}/emsdk_env.sh >/dev/null 2>&1
IMGUI=$(find "$B" -path '*imgui.dir*' -name '*.o')
LIBS=$(find "$B" -name '*.a' ! -name 'libSDL3_test.a')
mkdir -p "$S/link"
cd "$S/link" || exit 1
start=$(date +%s)
em++ -O2 -sMEMORY64 -pthread -sUSE_ZLIB=1 -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=256MB \
  -sMAXIMUM_MEMORY=16GB -sPTHREAD_POOL_SIZE=8 -sSTACK_SIZE=8MB -Wl,--error-limit=0 \
  -sDEFAULT_PTHREAD_STACK_SIZE=4MB --js-library "$S/wasmfs_fetch.js" "$@" \
  -o gta4.js "$S"/wasm/*.o "$S"/glue-obj/*.o "$S"/crypto-obj/*.o $IMGUI \
  -Wl,--start-group "$S"/librexruntime.a $LIBS -Wl,--end-group > link.log 2>&1
status=$?
echo "link exit $status after $(( $(date +%s) - start ))s"
