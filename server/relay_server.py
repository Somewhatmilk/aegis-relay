#!/usr/bin/env python3
"""
Aegis Relay -- CLOUD SIDE (server half of the relay bridge)

A minimal, stdlib-only mock backend that stands in for the "cloud" side
of the cloud-to-business-enterprise story. A real relay bridge would
terminate TLS, authenticate clients and fan events into durable storage;
this demo keeps exactly the three endpoints a viewer can poke:

    GET  /health   -> liveness + uptime + event counter
    GET  /ping     -> round-trip probe, echoes server UTC time
    POST /events   -> accepts a client event, assigns an id, echoes status

Run:  python3 server/relay_server.py  (default host 0.0.0.0, port 8765)
No third-party dependencies: Python 3.9+ standard library only.
"""

import argparse
import json
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

START = time.time()
EVENT_COUNTER = {"n": 0}


class FakeCloud(BaseHTTPRequestHandler):
    """The pretend enterprise-cloud edge endpoint."""

    server_version = "RelayBridgeDemo-FakeCloud/1.0"

    # -- helpers ---------------------------------------------------------
    def _reply(self, code: int, obj: dict) -> None:
        body = json.dumps(obj, indent=2).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Relay-Bridge", "aegis/1.0")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # quieter console: remote -- line
        print("  [server] %s -- %s" % (self.address_string(), fmt % args))

    # -- routes ----------------------------------------------------------
    def do_GET(self):  # /health, /ping
        if self.path.split("?")[0] == "/health":
            self._reply(200, {
                "service": "aegis-relay",
                "role": "cloud side",
                "status": "ok",
                "uptime_s": round(time.time() - START, 1),
                "events_seen": EVENT_COUNTER["n"],
                "server_time_utc": datetime.now(timezone.utc).isoformat(),
            })
        elif self.path.split("?")[0] == "/ping":
            self._reply(200, {
                "pong": True,
                "server_time_utc": datetime.now(timezone.utc).isoformat(),
                "latency_hint": "measured client-side",
            })
        else:
            self._reply(404, {"error": "not found", "known": ["/health", "/ping", "/events"]})

    def do_POST(self):  # /events
        if self.path.split("?")[0] != "/events":
            self._reply(404, {"error": "not found", "known": ["/health", "/ping", "/events"]})
            return
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            payload = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            self._reply(400, {"status": "rejected", "reason": "invalid JSON", "detail": str(exc)})
            return
        EVENT_COUNTER["n"] += 1
        event_id = "evt-%04d-%s" % (EVENT_COUNTER["n"], uuid.uuid4().hex[:8])
        self._reply(200, {
            "status": "accepted",
            "event_id": event_id,
            "received_at_utc": datetime.now(timezone.utc).isoformat(),
            "events_seen": EVENT_COUNTER["n"],
            "echo": payload,  # demo: echo proves the bridge carried it intact
        })


def main() -> None:
    ap = argparse.ArgumentParser(description="Aegis Relay cloud side (stdlib only)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    httpd = ThreadingHTTPServer((args.host, args.port), FakeCloud)
    print("* Fake cloud server on http://%s:%d" % (args.host, args.port))
    print("* Endpoints: GET /health, GET /ping, POST /events (JSON)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n* shutting down")
        httpd.server_close()


if __name__ == "__main__":
    main()