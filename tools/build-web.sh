#!/bin/sh
# Builds the browser version with Emscripten.
#
#   tools/build-web.sh [release|debug] [target]
#
# release (default): build/wasm-release, -O3, no WWDEBUG logs or asserts.
# debug:             build/wasm, -O0, ASSERTIONS and WWDEBUG logs.
# target defaults to Renegade (the game: bin/renegade.{js,wasm}).
#
# Needs an activated emsdk (EMSDK set, or emsdk_env.sh at $EMSDK_ENV) and
# llvm-rc (LLVM >= 15) for the dialog resources (REBEL_LLVM_RC).
set -eu
cd "$(dirname "$0")/.."
kind=${1:-release}
target=${2:-Renegade}
if [ -n "${EMSDK_ENV:-}" ]; then . "$EMSDK_ENV" >/dev/null; fi
command -v emcmake >/dev/null || { echo "emcmake not found: activate emsdk (or set EMSDK_ENV)" >&2; exit 1; }
rc=${REBEL_LLVM_RC:-$(command -v llvm-rc || echo /opt/homebrew/opt/llvm/bin/llvm-rc)}
case "$kind" in
  release) dir=build/wasm-release; type=Release; debug_messages=OFF ;;
  debug) dir=build/wasm; type=Debug; debug_messages=ON ;;
  *) echo "usage: $0 [release|debug] [target]" >&2; exit 2 ;;
esac
if [ ! -f "$dir/CMakeCache.txt" ]; then
  emcmake cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=$type -DREBEL_ENABLE_DEBUG_MESSAGES=$debug_messages \
    -DBUILD_TESTING=OFF -DREBEL_LLVM_RC="$rc"
fi
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
log="$dir/build-$target.log"
if make -C "$dir" -k -j"$jobs" "$target" >"$log" 2>&1; then
  echo "built $target ($kind): $dir/bin"
else
  grep -E 'error:' "$log" | sed -E 's/^.*error: //' | sort | uniq -c | sort -rn | head -40
  echo "build failed: $(grep -c 'error:' "$log") errors, see $log" >&2
  exit 1
fi
