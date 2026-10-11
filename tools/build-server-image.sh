#!/bin/sh
# Assembles the match server image's build context in build/match-image
# (services/match/Dockerfile): the release build, the server scripts and the
# game data from the owner's install (without movies). PRIVATE: the image
# holds game data; push it only to your own account's registry.
#
#   tools/build-server-image.sh [game dir]
set -eu
cd "$(dirname "$0")/.."
game=${1:-${REBEL_GAME_DIR:-../game-data/renegade}}
out=build/match-image
rm -rf "$out"
mkdir -p "$out/bin" "$out/web" "$out/tools/server" "$out/srv/renegade/Data"
cp build/wasm-release/bin/renegade.js build/wasm-release/bin/renegade.wasm "$out/bin/"
cp web/net.js "$out/web/"
cp tools/server/run-server.mjs tools/server/package.json tools/server/package-lock.json "$out/tools/server/"
cp services/match/Dockerfile "$out/"
# The game: the files the browser imports (root config + Data), minus movies
# and the single-player missions (M01-M13.mix): a match server needs neither.
for f in "$game"/*; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in *.exe|*.dll|*.asi|*.m3d|*.doc|*.ico|*.bmp|server.ini) continue ;; esac
  cp "$f" "$out/srv/renegade/"
done
data=$(ls -d "$game"/[Dd]ata)
(cd "$data" && find . -type f ! -ipath '*/movies/*' ! -iname 'M[0-9][0-9].mix') | while read -r f; do
  mkdir -p "$out/srv/renegade/Data/$(dirname "$f")"
  ln "$data/$f" "$out/srv/renegade/Data/$f" 2>/dev/null || cp "$data/$f" "$out/srv/renegade/Data/$f"
done
du -sh "$out" | sed 's/^/image context: /'
