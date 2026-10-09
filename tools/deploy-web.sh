#!/bin/sh
# Packages the public static site into dist/: the page, the engine
# (release build) and the cutscene decoder. No game data: players import
# their own install in the browser.
#
#   tools/deploy-web.sh            # build dist/
#   tools/deploy-web.sh --publish  # also upload to Cloudflare Pages
#                                  # (wrangler, project $PAGES_PROJECT,
#                                  # default open-commando)
#
# Cloudflare Pages reads dist/_headers: the engine needs cross-origin
# isolation (COOP/COEP) for SharedArrayBuffer and threads.
set -eu
cd "$(dirname "$0")/.."
bin=build/wasm-release/bin
for f in "$bin/renegade.js" "$bin/renegade.wasm" build/bink/bink.js build/bink/bink.wasm; do
  [ -f "$f" ] || { echo "missing $f: run tools/build-web.sh release and tools/build-bink.sh" >&2; exit 1; }
done
commit=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain --untracked-files=no | grep -q . && echo "+local changes" || true)
rm -rf dist
mkdir -p dist
cp web/*.html web/*.js dist/
cp "$bin/renegade.js" "$bin/renegade.wasm" build/bink/bink.js build/bink/bink.wasm dist/
# Public build: no server-side game data; the source commit for the GPL
# source offer, linked from the page.
python3 - "$commit" "$dirty" <<'PY'
import sys
commit, dirty = sys.argv[1], sys.argv[2]
p = 'dist/index.html'
s = open(p).read()
s = s.replace('<meta charset="utf-8">', '<meta charset="utf-8">\n<meta name="rebel-gamedata" content="none">', 1)
link = f', build <a href="https://github.com/netanelgilad/open-commando/tree/{commit}">{commit}</a>{(" " + dirty) if dirty else ""}'
s = s.replace('<span id="build"></span>', '<span id="build">' + link + '</span>', 1)
open(p, 'w').write(s)
PY
cat > dist/_headers <<'H'
/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  Cross-Origin-Resource-Policy: same-origin
  X-Content-Type-Options: nosniff

/*.wasm
  Content-Type: application/wasm
  Cache-Control: public, max-age=3600

/*.js
  Cache-Control: public, max-age=300

/index.html
  Cache-Control: no-cache
H
echo "dist/ ready ($(du -sh dist | cut -f1)), build $commit$dirty"
if [ "${1:-}" = "--publish" ]; then
  npx --yes wrangler pages deploy dist --project-name "${PAGES_PROJECT:-open-commando}" --branch main --commit-hash "$commit"
fi
