#!/usr/bin/env python3
"""
Aegis Relay -- CLOUD SIDE (server half of the relay bridge)

A stdlib-only mock backend that stands in for the "cloud" side of the
cloud-to-enterprise story. Authenticated endpoints (GET /ping, POST /events)
require HMAC-SHA256 request signing:

    X-Aegis-Timestamp : Unix seconds when the request was signed
    X-Aegis-Client    : client id used for quota accounting
    X-Aegis-Signature : hex HMAC-SHA256(key, "METHOD|path|timestamp|body")

The signing string is byte-exact: METHOD and timestamp are ASCII, path is
the raw request target, body is the raw request body (empty for GET), so
the C client and this server agree without any normalization.

Checks enforced at the relay boundary (in order):
    401  missing / invalid signature, missing client header, |clock skew| > 30s
    403  replay of the same (signature, timestamp) within the replay window
    429  > 10 requests/min per client (fixed window, Retry-After returned)

GET /health is deliberately open (liveness probe for load balancers).

Run:  AEGIS_SHARED_KEY=<key> python3 server/relay_server.py
Without AEGIS_SHARED_KEY the server refuses to start, unless
AEGIS_ALLOW_INSECURE=1 is set explicitly, which enables the dev default
key "aegis-demo-key" with a loud warning. No third-party dependencies:
Python 3.9+ standard library only.
"""

import argparse
import hashlib
import hmac
import json
import os
import sys
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

START = time.time()
EVENT_COUNTER = {"n": 0}

SHARED_KEY = None             # set in main(); fail-loud if absent (see _configure_key)
SKEW_LIMIT_S = 30             # max |now - X-Aegis-Timestamp| before 401
REPLAY_WINDOW_S = 60          # how long a (sig, ts) pair is remembered -> 403
REPLAYED = {}                 # "sig|ts" -> first-seen epoch (module-level, survives requests)
QUOTA_LIMIT = 10              # requests per client per fixed window
QUOTA_WINDOW_S = 60
QUOTA = {}                    # client id -> (window_start_epoch, count)

_INSECURE_DEFAULT_KEY = "aegis-demo-key"


def _configure_key() -> str:
    """AEGIS_SHARED_KEY required; dev default only under AEGIS_ALLOW_INSECURE=1."""
    key = os.environ.get("AEGIS_SHARED_KEY")
    if key:
        return key
    if os.environ.get("AEGIS_ALLOW_INSECURE") == "1":
        print("!! WARNING: AEGIS_SHARED_KEY unset and AEGIS_ALLOW_INSECURE=1 set --", file=sys.stderr)
        print("!! using dev default key '%s'. NEVER use in production." % _INSECURE_DEFAULT_KEY,
              file=sys.stderr)
        return _INSECURE_DEFAULT_KEY
    print("ERROR: AEGIS_SHARED_KEY is not set.", file=sys.stderr)
    print("       Refusing to start an unauthenticated relay.", file=sys.stderr)
    print("       Set AEGIS_SHARED_KEY=<key> to start, or AEGIS_ALLOW_INSECURE=1 for local dev only.",
          file=sys.stderr)
    sys.exit(1)


def _signing_string(method: str, path: str, ts: str, body: bytes) -> bytes:
    # Canonical boundary string: f"{method}|{path}|{ts}|{body}"
    return b"%s|%s|%s|%s" % (
        method.encode("ascii"), path.encode("utf-8"), ts.encode("ascii"), body)


def _valid_signature(key: str, sig: str, method: str, path: str, ts: str, body: bytes) -> bool:
    if not sig:
        return False
    try:
        expected = hmac.new(key.encode("utf-8"),
                            _signing_string(method, path, ts, body),
                            hashlib.sha256).hexdigest()
    except (ValueError, TypeError):
        return False
    # constant-time compare; accept lowercase hex only
    return hmac.compare_digest(expected, sig.strip().lower())


def _check_replay(sig: str, ts: str) -> bool:
    """True if this exact (sig, ts) was already seen within the replay window."""
    now = time.time()
    marker = "%s|%s" % (sig, ts)
    for stale in [k for k, v in REPLAYED.items() if now - v > REPLAY_WINDOW_S]:
        del REPLAYED[stale]
    if marker in REPLAYED:
        return True
    REPLAYED[marker] = now
    return False


def _check_quota(client_id: str) -> int:
    """Fixed-window quota per client id. Returns 0 (allowed) or seconds until reset."""
    now = time.time()
    entry = QUOTA.get(client_id)
    if entry is None or now - entry[0] >= QUOTA_WINDOW_S:
        QUOTA[client_id] = (now, 1)
        return 0
    start, count = entry
    if count >= QUOTA_LIMIT:
        return int(QUOTA_WINDOW_S - (now - start)) + 1
    QUOTA[client_id] = (start, count + 1)
    return 0


class FakeCloud(BaseHTTPRequestHandler):
    """The relay boundary: authenticate, replay-check, quota, then serve."""

    server_version = "AegisRelay/1.0"

    # -- helpers ---------------------------------------------------------
    def _reply(self, code: int, obj: dict, extra_headers=None) -> None:
        body = json.dumps(obj, indent=2).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Relay-Bridge", "aegis/1.0")
        for name, value in (extra_headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # quieter console: remote -- line
        print("  [server] %s -- %s" % (self.address_string(), fmt % args))

    def _authenticate(self, body: bytes):
        """Returns None if allowed, else (code, json_obj). /health is open."""
        if self.path.split("?")[0] == "/health":
            return None
        sig = self.headers.get("X-Aegis-Signature", "")
        ts = self.headers.get("X-Aegis-Timestamp", "")
        client = self.headers.get("X-Aegis-Client", "")
        if not (sig and ts and client):
            return (401, {"error": "unauthorized",
                          "reason": "missing X-Aegis-Signature / X-Aegis-Timestamp / X-Aegis-Client"})
        try:
            ts_i = int(ts)
        except ValueError:
            return (401, {"error": "unauthorized", "reason": "X-Aegis-Timestamp not an integer"})
        if abs(time.time() - ts_i) > SKEW_LIMIT_S:
            return (401, {"error": "unauthorized",
                          "reason": "clock skew exceeds %ds limit" % SKEW_LIMIT_S})
        if _check_replay(sig, ts):
            return (403, {"error": "forbidden",
                          "reason": "replay detected: signature+timestamp pair reused"})
        if not _valid_signature(SHARED_KEY, sig, self.command, self.path, ts, body):
            return (401, {"error": "unauthorized", "reason": "invalid signature"})
        wait = _check_quota(client)
        if wait:
            return (429, {"error": "rate limited",
                          "reason": "quota exceeded: >%d requests/min per client" % QUOTA_LIMIT,
                          "retry_after_s": wait})
        return None

    def _read_body(self) -> bytes:
        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            return b""
        if length < 0:
            return b""
        return self.rfile.read(length) if length else b""

    # -- routes ----------------------------------------------------------
    def do_GET(self):  # /health (open), /ping (signed)
        path = self.path.split("?")[0]
        if path == "/health":
            self._reply(200, {
                "service": "aegis-relay",
                "role": "cloud side",
                "status": "ok",
                "uptime_s": round(time.time() - START, 1),
                "events_seen": EVENT_COUNTER["n"],
                "server_time_utc": datetime.now(timezone.utc).isoformat(),
            })
            return
        denied = self._authenticate(b"")
        if denied:
            code, obj = denied
            extra = {"Retry-After": str(obj["retry_after_s"])} if code == 429 else None
            self._reply(code, obj, extra_headers=extra)
            return
        if path == "/ping":
            self._reply(200, {
                "pong": True,
                "server_time_utc": datetime.now(timezone.utc).isoformat(),
                "latency_hint": "measured client-side",
            })
            return
        self._reply(404, {"error": "not found", "known": ["/health", "/ping", "/events"]})

    def do_POST(self):  # /events (signed)
        path = self.path.split("?")[0]
        raw = self._read_body()
        if path != "/events":
            self._reply(404, {"error": "not found", "known": ["/health", "/ping", "/events"]})
            return
        denied = self._authenticate(raw)
        if denied:
            code, obj = denied
            extra = {"Retry-After": str(obj["retry_after_s"])} if code == 429 else None
            self._reply(code, obj, extra_headers=extra)
            return
        try:
            payload = json.loads(raw.decode("utf-8")) if raw else {}
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
    global SHARED_KEY
    SHARED_KEY = _configure_key()  # fail-loud before binding any port

    ap = argparse.ArgumentParser(description="Aegis Relay cloud side (stdlib only)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    httpd = ThreadingHTTPServer((args.host, args.port), FakeCloud)
    print("* Aegis Relay cloud server on http://%s:%d" % (args.host, args.port))
    print("* Endpoints: GET /health (open), GET /ping + POST /events (HMAC-SHA256 signed)")
    print("* Auth: X-Aegis-Signature / X-Aegis-Timestamp / X-Aegis-Client; skew <= %ds; quota %d/min/client"
          % (SKEW_LIMIT_S, QUOTA_LIMIT))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n* shutting down")
        httpd.server_close()


if __name__ == "__main__":
    main()