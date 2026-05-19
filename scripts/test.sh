#!/usr/bin/env bash
# scripts/test.sh
# ─────────────────────────────────────────────────────────────────────────────
# Build and run unit tests for AgentOS.
#
# Usage:
#   ./scripts/test.sh                  # Release, static libstdc++
#   ./scripts/test.sh --debug          # Debug build
#   ./scripts/test.sh --musl           # Fully static (requires musl-tools)
#   ./scripts/test.sh --clean          # Clean build dir first
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DIST_DIR="$BUILD_DIR/dist"

BUILD_TYPE="Release"
USE_MUSL=OFF
CLEAN=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

for arg in "$@"; do
  case $arg in
    --debug)     BUILD_TYPE="Debug"  ;;
    --musl)      USE_MUSL=ON         ;;
    --clean)     CLEAN=true          ;;
    --coverage)  AGENTOS_COVERAGE=ON ;;
    *) echo "Unknown option: $arg"; exit 1 ;;
  esac
done

if $CLEAN; then
  echo "→ Cleaning $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "→ Configuring superbuild ($BUILD_TYPE, musl=$USE_MUSL, tests=ON)"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"   \
  -DAGENTOS_MUSL="$USE_MUSL"         \
  -DAGENTOS_BUILD_TESTS=ON           \
  -DAGENTOS_STATIC=ON                \
  -DAGENTOS_STRIP=ON                 \
  -DAGENTOS_COVERAGE="${AGENTOS_COVERAGE:-OFF}"

echo "→ Building all ($JOBS jobs)"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

echo ""
echo "→ Running tests:"
cd "$BUILD_DIR"
ctest --output-on-failure -j "$JOBS"
cd "$ROOT_DIR"

echo ""
echo "✓ Tests complete."
