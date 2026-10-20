#!/usr/bin/env bash
# build.sh -- package relay-bridge-demo into dist/ and zip it.
# Usage:  bash build.sh     (on Windows: run under Git-Bash/MSYS; or use build.cmd)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST="$ROOT/dist"
ZIP="$DIST/relay-bridge-demo.zip"

echo "== relay-bridge-demo : build/package =="
rm -rf "$DIST"
mkdir -p "$DIST/client" "$DIST/server"

# 1) server sources
cp "$ROOT/server/fake_server.py" "$DIST/server/"
echo "  [1/4] server/fake_server.py  -> dist/server/"

# 2) client sources (always ship source + build notes)
cp "$ROOT/client/fake_webui.c" "$DIST/client/"
cp "$ROOT/build.cmd" "$DIST/"
cp "$ROOT/build.sh" "$DIST/"

# 3) client binary -- only if a C compiler is present
BIN=""
if command -v gcc >/dev/null 2>&1; then
  echo "  [2/4] gcc found: compiling client..."
  if gcc -O2 -o "$DIST/client/fake_webui.exe" "$ROOT/client/fake_webui.c" -lws2_32 2>"$DIST/client/build.log"; then
    BIN=client/fake_webui.exe
    echo "         compiled: dist/$BIN"
  else
    echo "         gcc failed (see dist/client/build.log) -- shipping source only"
  fi
elif command -v cl >/dev/null 2>&1; then
  echo "  [2/4] MSVC cl found: compiling client..."
  if cl /nologo /O2 "$ROOT/client/fake_webui.c" ws2_32.lib /Fe:"$DIST/client/fake_webui.exe" >"$DIST/client/build.log" 2>&1; then
    BIN=client/fake_webui.exe
    echo "         compiled: dist/$BIN"
  else
    echo "         cl failed (see dist/client/build.log) -- shipping source only"
  fi
else
  echo "  [2/4] no C compiler found on this host -- shipping source + build instructions"
  echo "        build with:  gcc -O2 -o fake_webui.exe fake_webui.c -lws2_32"
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
python - "$PY_ZIP" "$PY_DIST" <<'PY'
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
echo "  [4/4] dist/relay-bridge-demo.zip created"
echo "== done =="
ls -la "$DIST"