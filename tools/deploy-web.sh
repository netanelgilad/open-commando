#!/bin/sh
# Packages the public static site into dist/: the page, the engine
# (release build) and the cutscene decoder. No game data: players import
# their own install in the browser.
#
#   tools/deploy-web.sh            # build dist/
#   tools/deploy-web.sh --publish  # also deploy to Cloudflare Workers
#                                  # (static assets, wrangler.jsonc;
#                                  # needs CLOUDFLARE_API_TOKEN)
#
# Workers static assets read dist/_headers: the engine needs cross-origin
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
# The build's version: a hash of what's deployed. The page asks for every
# script and game file with ?v=<version>, so a browser never mixes this
# build's files with cached ones from the last (index.html isn't cached).
version=$(cat dist/*.js dist/*.wasm | shasum | cut -c1-12)
python3 - "$commit" "$dirty" "$version" <<'PY'
import sys, re
commit, dirty, version = sys.argv[1], sys.argv[2], sys.argv[3]
p = 'dist/index.html'
s = open(p).read()
s = s.replace('<meta charset="utf-8">', '<meta charset="utf-8">\n<meta name="rebel-gamedata" content="none">\n<meta name="rebel-version" content="' + version + '">', 1)
s = re.sub(r'<script src="([\w.-]+\.js)">', lambda m: '<script src="' + m.group(1) + '?v=' + version + '">', s)
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
echo "dist/ ready ($(du -sh dist | cut -f1)), build $commit$dirty, version $version"
if [ "${1:-}" = "--publish" ]; then
  wrangler deploy --config wrangler.jsonc
fi
