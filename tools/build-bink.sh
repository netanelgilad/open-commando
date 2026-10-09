#!/bin/sh
# Builds the page's Bink cutscene decoder (web: bink.js + bink.wasm) from a
# minimal FFmpeg (LGPL 2.1+): the bink demuxer and the bink / binkaudio
# decoders only. Output: build/bink/bink.{js,wasm} (served by tools/serve.mjs
# and copied by tools/deploy-web.sh).
#
# Needs an activated emsdk (or EMSDK_ENV pointing at emsdk_env.sh).
set -eu
cd "$(dirname "$0")/.."
root=$PWD
if [ -n "${EMSDK_ENV:-}" ]; then . "$EMSDK_ENV" >/dev/null; fi
command -v emcc >/dev/null || { echo "emcc not found: activate emsdk (or set EMSDK_ENV)" >&2; exit 1; }
version=7.1
work=build/ffmpeg
src=$work/ffmpeg-$version
mkdir -p "$work" build/bink
if [ ! -d "$src" ]; then
  curl -fsSL -o "$work/ffmpeg-$version.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
  tar -C "$work" -xf "$work/ffmpeg-$version.tar.xz"
fi
if [ ! -f "$work/install/lib/libavcodec.a" ]; then
  (cd "$src" && emconfigure ./configure --prefix="$root/$work/install" --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib \
    --nm=emnm --target-os=none --arch=x86_32 --enable-cross-compile --disable-everything --disable-programs \
    --disable-doc --disable-asm --disable-inline-asm --disable-x86asm --disable-pthreads --disable-w32threads \
    --disable-os2threads --disable-network --disable-autodetect --disable-debug --disable-runtime-cpudetect \
    --disable-swscale --disable-swresample --disable-avdevice --disable-avfilter --disable-postproc --enable-small \
    --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-demuxer=bink --extra-cflags=-O3 >../configure.log &&
    emmake make -j"$(getconf _NPROCESSORS_ONLN)" >../make.log && emmake make install >../install.log)
fi
emcc -O3 Code/Platform/media/bink_player_web.c -I"$work/install/include" -L"$work/install/lib" -lavformat -lavcodec -lavutil \
  -sMODULARIZE=1 -sEXPORT_NAME=createBinkModule -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 \
  -sENVIRONMENT=web -sEXPORTED_FUNCTIONS=_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32 \
  -o build/bink/bink.js
echo "built build/bink/bink.{js,wasm} ($(wc -c < build/bink/bink.wasm) bytes wasm)"
