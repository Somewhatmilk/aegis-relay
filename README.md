# Aegis Relay

> A server–client relay bridge for cloud-to-enterprise connectivity. A
> dependency-free C client reaches a cloud backend through an explicit,
> observable bridge boundary — the junction where real deployments enforce
> authentication, quotas, and routing.

[![License: MIT](https://img.shields.io/badge/license-MIT-green)](#license)
[![Client: C (WinSock)](https://img.shields.io/badge/client-C%20(WinSock)-orange)](#)
[![Server: Python stdlib](https://img.shields.io/badge/server-Python%20stdlib-4B8BBE)](#)

---

## Overview

Aegis Relay demonstrates the connective tissue between an enterprise client
and a cloud backend. The client is a single-file C program (WinSock2, zero
third-party dependencies) that speaks HTTP. The server is a minimal Python
stdlib HTTP endpoint. Between them sits the **relay bridge** — the boundary
at which authentication, quotas, and routing would be enforced in a
production deployment.

The bridge is modeled deliberately: both sides are observable, the payloads
that cross the boundary are explicit, and the whole system starts in two
commands. It is a working reference for how the two halves of a
server–client integration fit together.

## Quick start

```bash
# 1. start the server side
python3 server/relay_server.py

# 2. in a second terminal, build and run the client
bash build.sh                       # -- or build.cmd on Windows cmd --
dist/client.exe                     # (if a C compiler was present at build time)
```

No compiler on the machine? The client ships as source
(`client/client.c`) with build lines:

```bash
gcc -O2 -o client.exe client.c -lws2_32     # MinGW
cl  client.c ws2_32.lib                      # MSVC
```

The client connects, registers, probes the endpoints, and prints its console
UI with connection status, an event log, and per-request latency. It exits
after the run.

## Architecture

```
 +----------------------------+        +---------------------------+
 |  ENTERPRISE NETWORK        |        |  CLOUD                    |
 |  (the "business" side)     |        |                           |
 |                            |        |  server/relay_server.py   |
 |  client/client.c           |  HTTP  |  GET  /health             |
 |  single-file C UI          +------->+  GET  /ping               |
 |  WinSock2, no deps         |        |  POST /events (handshake) |
 |                            |        |  JSON in, JSON out        |
 +----------------------------+        +---------------------------+
           |                                        |
           +---------- relay bridge boundary ------+
                      (where real deployments
                       enforce auth + routing)
```

## Payload example

A handshake event as sent by the client:

```json
{
  "type": "handshake",
  "client": "aegis-client",
  "client_version": "1.0",
  "event": "client.connected"
}
```

The server's acknowledgment (what the client prints in its status box):

```json
{
  "status": "accepted",
  "event_id": "evt-0001-3f9a21c4",
  "received_at_utc": "2026-10-21T00:12:00+00:00",
  "events_seen": 1,
  "echo": { "type": "handshake", "client": "aegis-client", "client_version": "1.0", "event": "client.connected" }
}
```

## Tech stack

| Side     | Tech                               | Notes                                |
|----------|------------------------------------|--------------------------------------|
| Client   | C99, WinSock2 (`ws2_32`)           | Single file, zero third-party deps   |
| Server   | Python 3.9+ stdlib (`http.server`) | No runtime dependencies              |
| Protocol | HTTP/1.1, JSON payloads            | Plain-text on localhost              |
| Package  | `build.sh` / `build.cmd` + zip     | `dist/aegis-relay.zip`               |

## File layout

```
aegis-relay/
├── README.md              this file
├── LICENSE                MIT
├── build.sh               package script (Git-Bash/MSYS)
├── build.cmd              package script (Windows cmd)
├── server/
│   └── relay_server.py    the cloud side (Python stdlib only)
├── client/
│   └── client.c           the enterprise client (C99 + WinSock2)
└── legacy/                historical content (see LICENSE)
```

## License

MIT — see [LICENSE](LICENSE).