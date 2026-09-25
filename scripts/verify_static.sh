#!/usr/bin/env bash
set -euo pipefail
BINARY="${1:-./build/dist/bin/agentos}"
if [ ! -f "$BINARY" ]; then
  echo "Binary not found: $BINARY — run ./scripts/build.sh first."
  exit 1
fi
echo "Verifying: $BINARY"
if [[ "$(uname)" == "Darwin" ]]; then
  otool -L "$BINARY" | sed 's/^/  /'
  echo "Note: libSystem.dylib is always dynamic on macOS."
  exit 0
fi
LDD=$(ldd "$BINARY" 2>&1)
echo "$LDD" | sed 's/^/  /'
FAIL=0
for lib in libstdc++ libgcc_s; do
  if echo "$LDD" | grep -q "$lib"; then
    echo "FAIL: $lib is dynamic (expected static)"
    FAIL=1
  else
    echo "PASS: $lib is statically linked"
  fi
done
exit $FAIL
