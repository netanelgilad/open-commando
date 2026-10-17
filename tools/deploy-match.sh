#!/bin/sh
# Deploys the match service (services/match) with a fresh server image:
# assembles the image context, runs wrangler deploy (once more if it fails:
# wrangler's token refresh sometimes fails mid-deploy), then deletes the
# local copies of the server images (each build leaves ~700 MB per region in
# Docker; the registry keeps what's deployed).
#
#   CLOUDFLARE_ACCOUNT_ID=... tools/deploy-match.sh [--containers-rollout=none]
set -eu
cd "$(dirname "$0")/.."
tools/build-server-image.sh
cd services/match
npx wrangler deploy --secrets-file .secrets.json "$@" || npx wrangler deploy --secrets-file .secrets.json "$@"
docker images --format '{{.Repository}}:{{.Tag}}' | grep -E '/open-commando-match-matchserver' | xargs docker rmi -f >/dev/null 2>&1 || true
docker builder prune -af >/dev/null 2>&1 || true
