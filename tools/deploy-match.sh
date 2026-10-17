#!/bin/sh
# Deploys the match service (services/match) with a fresh server image:
# assembles the image context, runs wrangler deploy (once more if it fails:
# wrangler's token refresh sometimes fails mid-deploy), waits for every
# region's container rollout to finish, then deletes the
# local copies of the server images (each build leaves ~700 MB per region in
# Docker; the registry keeps what's deployed).
#
#   CLOUDFLARE_ACCOUNT_ID=... tools/deploy-match.sh [--containers-rollout=none]
set -eu
cd "$(dirname "$0")/.."
tools/build-server-image.sh
cd services/match
npx wrangler deploy --secrets-file .secrets.json "$@" || npx wrangler deploy --secrets-file .secrets.json "$@"
# Wait for every region's rollout: until then, running match servers get
# replaced (stopped) under their players.
for app in $(npx wrangler containers list 2>/dev/null | grep -o '[0-9a-f-]\{36\} *│ *open-commando-match-matchserver[a-z]*' | cut -c1-36); do
  n=0
  while npx wrangler containers info "$app" 2>/dev/null | grep -q '"active_rollout_id": "'; do
    n=$((n + 1)); [ $n -gt 60 ] && echo "rollout of $app still running after 20 min" && break
    sleep 20
  done
done
echo "rollouts done"
docker images --format '{{.Repository}}:{{.Tag}}' | grep -E '/open-commando-match-matchserver' | xargs docker rmi -f >/dev/null 2>&1 || true
docker builder prune -af >/dev/null 2>&1 || true
