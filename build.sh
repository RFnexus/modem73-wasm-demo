set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODEM73_DIR="${MODEM73_DIR:-$HERE/../modem73}"

if ! command -v emcc >/dev/null 2>&1; then
    echo "error: emcc not found on PATH. Run 'source <emsdk>/emsdk_env.sh' first." >&2
    exit 1
fi

INCLUDES=(
    -I"$MODEM73_DIR"
    -I"$MODEM73_DIR/deps/aicodix/dsp"
    -I"$MODEM73_DIR/deps/aicodix/code"
    -I"$MODEM73_DIR/deps/aicodix/modem"
)

mkdir -p "$HERE/web"

echo "Building web/modem73.js + web/modem73.wasm ..."
emcc -std=c++17 -O3 \
    "${INCLUDES[@]}" \
    "$HERE/src/wasm_modem.cc" \
    -o "$HERE/web/modem73.js" \
    -lembind \
    -s MODULARIZE=1 \
    -s EXPORT_NAME=createModem73 \
    -s EXPORT_ES6=0 \
    -s ENVIRONMENT=web \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=33554432 \
    -s STACK_SIZE=1048576

echo "Done:"
ls -la "$HERE/web/modem73.js" "$HERE/web/modem73.wasm"
