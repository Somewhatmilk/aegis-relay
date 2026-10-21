#!/usr/bin/env bash
# build.sh -- package aegis-relay into dist/ and zip it.
# Usage:  bash build.sh     (on Windows: run under Git-Bash/MSYS; or use build.cmd)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST="$ROOT/dist"
ZIP="$DIST/aegis-relay.zip"

echo "== aegis-relay : build/package =="
rm -rf "$DIST"
mkdir -p "$DIST/client" "$DIST/server"

# 1) server source
cp "$ROOT/server/relay_server.py" "$DIST/server/"
echo "  [1/4] server/relay_server.py  -> dist/server/"

# 2) client source (always ship source + build notes)
cp "$ROOT/client/client.c" "$DIST/client/"
cp "$ROOT/build.cmd" "$DIST/"
cp "$ROOT/build.sh" "$DIST/"

# 3) client binary -- only if a C compiler is present
BIN=""
LOG="${TMPDIR:-/tmp}/aegis-client-build.log"
if command -v gcc >/dev/null 2>&1; then
  echo "  [2/4] gcc found: compiling client..."
  if gcc -O2 -o "$DIST/client/client.exe" "$ROOT/client/client.c" -lws2_32 2>"$LOG"; then
    BIN=client/client.exe
    echo "         compiled: dist/$BIN"
  else
    echo "         gcc failed (see $LOG) -- shipping source only"
  fi
elif command -v cl >/dev/null 2>&1; then
  echo "  [2/4] MSVC cl found: compiling client..."
  if cl /nologo /O2 "$ROOT/client/client.c" ws2_32.lib /Fe:"$DIST/client/client.exe" >"$LOG" 2>&1; then
    BIN=client/client.exe
    echo "         compiled: dist/$BIN"
  else
    echo "         cl failed (see $LOG) -- shipping source only"
  fi
else
  echo "  [2/4] no C compiler found on this host -- shipping source + build instructions"
  echo "        build with:  gcc -O2 -o client.exe client.c -lws2_32"
fi

# 4) README + zip
cp "$ROOT/README.md" "$DIST/"
echo "  [3/4] README.md  -> dist/"
# native Python needs Windows-style paths; convert if cygpath exists
if command -v cygpath >/dev/null 2>&1; then
  PY_ZIP="$(cygpath -w "$ZIP")"; PY_DIST="$(cygpath -w "$DIST")"
else
  PY_ZIP="$ZIP"; PY_DIST="$DIST"
fi
# resolve a usable interpreter (bare `python` may be absent in subprocess PATH)
PY=python
command -v "$PY" >/dev/null 2>&1 || PY=python3
command -v "$PY" >/dev/null 2>&1 || PY=py
command -v "$PY" >/dev/null 2>&1 || { echo "error: no python/python3/py on PATH" >&2; exit 1; }
"$PY" - "$PY_ZIP" "$PY_DIST" <<'PY'
import sys, zipfile, os
zip_path, dist = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(dist):
        for f in files:
            full = os.path.join(root, f)
            rel = os.path.relpath(full, dist)
            if os.path.abspath(full) == os.path.abspath(zip_path):
                continue  # never embed the zip inside itself
            z.write(full, rel)
            print("   zip: %s" % rel)
    print("   wrote %s" % zip_path)
PY
echo "  [4/4] dist/aegis-relay.zip created"
echo "== done =="
ls -la "$DIST"