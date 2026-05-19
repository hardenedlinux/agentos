#!/usr/bin/env bash
# scripts/build.sh
# ─────────────────────────────────────────────────────────────────────────────
# AgentOS superbuild script.
#
# Build flow:
#   1. deps/CMakeLists.txt  — downloads & installs all deps to build/deps/install/
#   2. src/CMakeLists.txt   — builds AgentOS core against pre-installed deps
#
# Usage:
#   ./scripts/build.sh                  # Release, static libstdc++
#   ./scripts/build.sh --debug          # Debug build
#   ./scripts/build.sh --musl           # Fully static (requires musl-tools)
#   ./scripts/build.sh --no-tests       # Skip unit tests
#   ./scripts/build.sh --deps-only      # Only build deps (useful first run)
#   ./scripts/build.sh --clean          # Clean build dir first
#
# Prerequisites:
#   Ubuntu/Debian:  sudo apt install cmake ninja-build build-essential git
#   macOS:          brew install cmake ninja
#   Musl (optional): sudo apt install musl-tools
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DIST_DIR="$BUILD_DIR/dist"

BUILD_TYPE="Release"
USE_MUSL=OFF
BUILD_TESTS=OFF
DEPS_ONLY=false
CLEAN=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

while [ $# -gt 0 ]; do
  case "$1" in
    --debug)     BUILD_TYPE="Debug"  ;;
    --musl)      USE_MUSL=ON         ;;
    --no-tests)  BUILD_TESTS=OFF     ;;
    --deps-only) DEPS_ONLY=true      ;;
    --clean)     CLEAN=true          ;;
    --target)    shift; TARGET="$1"  ;;
    --coverage)  AGENTOS_COVERAGE=ON ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
  shift
done

if $CLEAN; then
  echo "→ Cleaning $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "→ Configuring superbuild ($BUILD_TYPE, musl=$USE_MUSL, tests=$BUILD_TESTS)"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"   \
  -DAGENTOS_MUSL="$USE_MUSL"         \
  -DAGENTOS_BUILD_TESTS="$BUILD_TESTS" \
  -DAGENTOS_STATIC=ON                \
  -DAGENTOS_STRIP=ON                 \
  -DAGENTOS_COVERAGE="${AGENTOS_COVERAGE:-OFF}"

if $DEPS_ONLY; then
  echo "→ Building deps only"
  cmake --build "$BUILD_DIR" --target deps --parallel "$JOBS"
  echo "✓ Deps installed to $BUILD_DIR/deps/install"
  exit 0
fi

if [ -n "${TARGET:-}" ]; then
  echo "→ Building target '$TARGET' ($JOBS jobs)"
  cmake --build "$BUILD_DIR" --target "$TARGET" --parallel "$JOBS"
else
  echo "→ Building all ($JOBS jobs)"
  echo "  Step 1/2: deps  (downloaded + built once, cached after)"
  echo "  Step 2/2: agentos core"
  cmake --build "$BUILD_DIR" --parallel "$JOBS"
fi

BINARY="$DIST_DIR/bin/agentos"
echo ""
echo "─────────────────────────────────────────────────────────────"
echo " Binary : $BINARY"
echo " Size   : $(ls -lh "$BINARY" | awk '{print $5}')"
echo "─────────────────────────────────────────────────────────────"

if command -v ldd &>/dev/null; then
  echo " ldd output:"
  ldd "$BINARY" 2>&1 | sed 's/^/   /'
fi
echo "─────────────────────────────────────────────────────────────"

echo ""
echo "→ Running binary:"
"$BINARY"

echo ""
echo "✓ Build complete. Binary at: $BINARY"
