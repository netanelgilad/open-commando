#!/bin/sh
# Converts the retail Bink cutscenes (Data/Movies/*.BIK) from the user's own
# install to WebM (VP9 + Opus) for the browser build's <video> overlay.
# Output names are lowercase without extension: r_intro.webm, ...
set -eu
SRC="${1:-$HOME/development/renegade/game-data/renegade/Data/Movies}"
DST="${2:-$HOME/development/renegade/web-movies}"
mkdir -p "$DST"
for f in "$SRC"/*.[bB][iI][kK]; do
  name=$(basename "$f" | sed 's/\.[bB][iI][kK]$//' | tr 'A-Z' 'a-z')
  out="$DST/$name.webm"
  [ -s "$out" ] && { echo "skip $name"; continue; }
  echo "encode $name"
  nice ffmpeg -hide_banner -loglevel error -y -i "$f" \
    -c:v libvpx-vp9 -crf 30 -b:v 0 -row-mt 1 -deadline good -cpu-used 2 \
    -c:a libopus -b:a 112k "$out.tmp.webm"
  mv "$out.tmp.webm" "$out"
done
ls -la "$DST"
