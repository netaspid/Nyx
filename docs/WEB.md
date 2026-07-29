# Nyx hosted web client

## Trust model (hosted-only)

`nyx-webd` runs on a VDS or localhost and holds account keys, Noise sessions, and
UDP media. The browser is a thin UI over HTTPS/WSS.

The host process can read plaintext of active sessions. Protect the host with TLS,
firewall, and `--admin-token`.

## Multi-account instances

Each browser session creates an isolated worker process under:

```text
<data-root>/instances/<sessionId>/
```

Workers do not share unlocked account state (process globals). Open several browser
profiles/tabs to run different accounts in parallel.

## Run locally

```bash
cmake -B build -DNYX_BUILD_WEB=ON
cmake --build build -j --target nyx-webd
# optional SPA
cd apps/nyx-web && npm install && npm run build && cd ../..
./build/nyx-webd --listen 127.0.0.1:8787 --static-dir apps/nyx-web/dist
```

Open `http://127.0.0.1:8787/`.

## VDS

```bash
./build/nyx-webd --listen 127.0.0.1:8787 \
  --data-root /var/lib/nyx-web \
  --static-dir /var/www/nyx-web \
  --admin-token "$(openssl rand -hex 16)"
```

Terminate TLS with Caddy/nginx to `127.0.0.1:8787`. Send
`Authorization: Bearer <admin-token>` when creating sessions.

## Protocol

1. `POST /api/v1/session` → `{ sessionId, token, workerPort }`
2. `Authorization: Bearer <token>` on `POST /api/v1/rpc` with JSON
   `{ "id","op","args" }`
3. `WS /api/v1/ws?token=<token>` for events + RPC
4. `GET /api/v1/files/blob?token=<token>&hash=<hex>` — stream a local file object
5. `POST /api/v1/files/upload?token=<token>&dest=<path>` — write body to host path,
   then `sendFile`

## Calls / media bridge

Call signalling uses NodeService ops (`startCall`, `acceptCall`, …). Media frames
are bridged as base64 Opus/AV1 payloads over WS events `callMedia` and op
`sendCallMedia` (browser getUserMedia → PCM/Opus over WS → host CallMesh).
Browser also keeps an `RTCPeerConnection` stub for same-host ICE (optional TURN
for remote browsers behind NAT).
