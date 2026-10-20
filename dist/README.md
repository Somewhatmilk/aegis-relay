# relay-bridge-demo

> A demonstrative relay bridge between a **cloud backend** (server side) and a
> **business-enterprise app written in C** (client side) — built to be shown,
> not shipped. Repo started as a fork of `hermes-dist`; the old distribution
> content now lives under [`legacy/old-hermes-dist/`](legacy/old-hermes-dist/).

[![License: MIT](https://img.shields.io/badge/license-MIT-green)](#license)
[![Status: demo](https://img.shields.io/badge/status-demo-blue)](#what-this-demonstrates)
[![Client: C (WinSock)](https://img.shields.io/badge/client-C%20(WinSock)-orange)](#)
[![Server: Python stdlib](https://img.shields.io/badge/server-Python%20stdlib-4B8BBE)](#)

---

## What this demonstrates

The story this demo tells is a **cloud → business-enterprise** connection:

1. A **business enterprise app** — represented by `fake_webui.exe`, a real
   single-file C program that speaks HTTP over WinSock — tries to reach a
   corporate backend.
2. The request crosses a **relay bridge**: an explicit, observable junction
   between the enterprise network and the cloud side. The bridge is the
   boundary where a real deployment would enforce auth, quotas and routing;
   this demo models the boundary so a viewer can see what flows across it.
3. The **cloud side** — `server/fake_server.py`, a tiny stdlib-only Python
   HTTP server — accepts the client's handshake event, stores nothing,
   and echoes a confirmation (`event_id`, server time, echo of the payload).

What a **viewer sees** when they run it: the C client opens a connection,
sends `client.connected` as its first event, probes `/health` and `/ping`,
and prints an ASCII console UI with the connection status box, an event log
with timestamps, and per-request latency. The cloud terminal shows each
request with `200 OK` lines and the payload.

It is intentionally a *fake*: no TLS, no auth, no persistence — both sides
are stand-ins. The point is the *shape* of the bridge and the two-line
startup, not production machinery.

---

## Quick start

```bash
# 1. start the cloud side
python3 server/fake_server.py

# 2. in a second terminal, run the enterprise client
bash build.sh                       # -- or build.cmd on Windows cmd --
dist/fake_webui.exe                 # (if a C compiler was present at build time)
```

No compiler on the machine running the demo? The client is still shipped as
source (`client/fake_webui.c`) plus build lines:

```bash
gcc -O2 -o fake_webui.exe fake_webui.c -lws2_32     # MinGW
cl  fake_webui.c ws2_32.lib                          # MSVC
```

The demo client runs against a real server in about 5 seconds, prints its
console UI, and exits.

---

## Architecture

```
 +----------------------------+        +---------------------------+
 |  ENTERPRISE NETWORK        |        |  CLOUD (assumed)          |
 |  (the "business" side)     |        |                           |
 |                            |        |  server/fake_server.py    |
 |  client/fake_webui.c       |  HTTP  |  GET  /health             |
 |  single-file C UI          +------->+  GET  /ping               |
 |  WinSock2, no deps         |        |  POST /events (handshake) |
 |                            |        |  JSON in, JSON out        |
 +----------------------------+        +---------------------------+
           |                                        |
           +---------- relay bridge boundary ------+
                      (where a real deployment
                       enforces auth + routing)
```

## Payload example

A handshake event as sent by the C client:

```json
{
  "type": "handshake",
  "client": "fake-webui",
  "client_version": "1.0",
  "event": "client.connected"
}
```

And the cloud side's confirmation (what the client prints in its status box):

```json
{
  "status": "accepted",
  "event_id": "evt-0001-3f9a21c4",
  "received_at_utc": "2026-10-21T00:12:00+00:00",
  "events_seen": 1,
  "echo": { "type": "handshake", "client": "fake-webui", "client_version": "1.0", "event": "client.connected" }
}
```

## Tech stack

| Side       | Tech                               | Notes                                     |
|------------|------------------------------------|-------------------------------------------|
| Client     | C99, WinSock2 (`ws2_32`)           | Single file, zero third-party deps        |
| Server     | Python 3.9+ stdlib (`http.server`) | No pip install needed                     |
| Protocol   | HTTP/1.1, JSON payloads            | Plain-text on localhost for the demo      |
| Package    | `build.sh` / `build.cmd` + zip     | `dist/relay-bridge-demo.zip`              |

## File layout

```
relay-bridge-demo/
├── README.md                  this file
├── LICENSE                    MIT (demo code)
├── build.sh                   package script (Git-Bash/MSYS)
├── build.cmd                  package script (Windows cmd)
├── server/
│   └── fake_server.py         the fake cloud side (Python stdlib only)
├── client/
│   └── fake_webui.c           the fake enterprise WebUI (C99 + WinSock2)
└── legacy/
    └── old-hermes-dist/       the original hermes-dist distribution content
```

## License

MIT — see [LICENSE](LICENSE). Demo code only; the legacy `hermes-dist`
content under `legacy/` keeps its original (internal PoC) terms.