# The match server image: Node + the game's own build (renegade.js/.wasm) run
# headless (tools/server/run-server.mjs), with the game data baked in.
# PRIVATE: the image contains game data from the owner's install; it goes
# only to this account's Cloudflare registry, never anywhere public.
# Build context: build/match-image (tools/build-server-image.sh assembles it).
FROM node:24-slim
WORKDIR /app
COPY tools/server/package.json tools/server/package-lock.json tools/server/
RUN cd tools/server && npm ci --omit=dev
COPY bin/ bin/
COPY web/net.js web/
COPY tools/server/*.mjs tools/server/
COPY srv/ /srv/
ENV REBEL_STATUS_PORT=8080
EXPOSE 8080
ENTRYPOINT ["node", "tools/server/run-server.mjs", "--work", "/srv", "--in-place", "--bin", "/app/bin"]
