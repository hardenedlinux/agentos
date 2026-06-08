#!/usr/bin/env bash
# scripts/build.sh
# ─────────────────────────────────────────────────────────────────────────────
# AgentOS superbuild script.
#
# Build flow:
#   1. deps/CMakeLists.txt  — downloads & installs all deps to deps-build/
#   2. src/CMakeLists.txt   — builds AgentOS core against pre-installed deps
#
# Usage:
#   ./scripts/build.sh                  # Release, static libstdc++
#   ./scripts/build.sh --debug          # Debug build
#   ./scripts/build.sh --musl           # Fully static (requires musl-tools)
#   ./scripts/build.sh --tests          # Build and run unit tests
#   ./scripts/build.sh --no-tests       # Skip unit tests (default)
#   ./scripts/build.sh --deps-only      # Only build deps (useful first run)
#   ./scripts/build.sh --clean          # Clean build dir first (deps-build preserved)
#   ./scripts/build.sh --clean-all      # Clean both build/ and deps-build/
#
# Prerequisites:
#   Ubuntu/Debian:  sudo apt install cmake ninja-build build-essential git
#   macOS:          brew install cmake ninja
#   Musl (optional): sudo apt install musl-tools
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_BUILD_DIR="$ROOT_DIR/deps-build"
BUILD_DIR="$ROOT_DIR/build"

BUILD_TYPE="Release"
USE_MUSL=OFF
BUILD_TESTS=OFF
DEPS_ONLY=false
CLEAN=false
CLEAN_ALL=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
AGENTOS_COVERAGE=OFF

while [ $# -gt 0 ]; do
  case "$1" in
    --debug)     BUILD_TYPE="Debug"  ;;
    --musl)      USE_MUSL=ON         ;;
    --tests)     BUILD_TESTS=ON      ;;
    --no-tests)  BUILD_TESTS=OFF     ;;
    --deps-only) DEPS_ONLY=true      ;;
    --clean)     CLEAN=true          ;;
    --clean-all) CLEAN_ALL=true      ;;
    --target)    shift; TARGET="$1"  ;;
    --coverage)  AGENTOS_COVERAGE=ON ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
  shift
done

if $CLEAN_ALL; then
  echo "→ Cleaning $BUILD_DIR and $DEPS_BUILD_DIR"
  rm -rf "$BUILD_DIR" "$DEPS_BUILD_DIR"
elif $CLEAN; then
  echo "→ Cleaning $BUILD_DIR (deps-build preserved)"
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

# ─────────────────────────────────────────────────────────────────────────────
# Dependency check: inspect stamp files rather than .a paths, because stamp
# files live in deps-build/ and survive rm -rf build/.  The .a check below
# is a secondary sanity guard.
# ─────────────────────────────────────────────────────────────────────────────
deps_stamp_ok=true
for dep in spdlog libzmq libseccomp libcap; do
  if [ ! -f "$DEPS_BUILD_DIR/stamps/$dep/${dep}_ext-install" ]; then
    deps_stamp_ok=false
    break
  fi
done

# Also verify the actual libraries are present (stamps can exist without libs
# if a previous install step was interrupted)
deps_libs_ok=true
for lib in spdlog libzmq libseccomp libcap; do
  if [ ! -f "$DEPS_BUILD_DIR/$lib/lib/lib$lib.a" ]; then
    deps_libs_ok=false
    break
  fi
done

if [ "$BUILD_TESTS" = "ON" ]; then
  if [ ! -f "$DEPS_BUILD_DIR/stamps/googletest/googletest_ext-install" ] || \
     [ ! -f "$DEPS_BUILD_DIR/googletest/lib/libgtest.a" ] || \
     [ ! -f "$DEPS_BUILD_DIR/googletest/lib/libgtest_main.a" ]; then
    deps_stamp_ok=false
    deps_libs_ok=false
  fi
fi

deps_built=false
if $deps_stamp_ok && $deps_libs_ok; then
  deps_built=true
fi

# ─────────────────────────────────────────────────────────────────────────────
# CMake configure step.
#
# Always re-configure if:
#   (a) deps are not yet built, OR
#   (b) build/ was deleted (CMakeCache.txt is absent)
#
# This ensures that deleting only build/ triggers a re-configure (which is
# fast — deps are already built) without re-downloading or rebuilding deps.
# ─────────────────────────────────────────────────────────────────────────────
needs_configure=false
if ! $deps_built; then
  needs_configure=true
elif [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  needs_configure=true
fi

if $needs_configure; then
  echo "→ Configuring superbuild ($BUILD_TYPE, musl=$USE_MUSL, tests=$BUILD_TESTS)"
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"            \
    -DAGENTOS_MUSL="$USE_MUSL"                  \
    -DAGENTOS_BUILD_TESTS="$BUILD_TESTS"        \
    -DAGENTOS_STATIC=ON                         \
    -DAGENTOS_STRIP=ON                          \
    -DAGENTOS_COVERAGE="$AGENTOS_COVERAGE"
else
  echo "→ Dependencies already built and build tree intact, skipping configure"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Build step
# ─────────────────────────────────────────────────────────────────────────────
if $DEPS_ONLY; then
  echo "→ Building deps only ($JOBS jobs)"
  # Build all ExternalProject targets; agentos core is excluded by convention
  # because it depends on the dep targets which cmake will now skip (already built)
  cmake --build "$BUILD_DIR" --parallel "$JOBS"
  echo "✓ Deps built. Run without --deps-only to build agentos core."
  exit 0
fi

if [ -n "${TARGET:-}" ]; then
  echo "→ Building target '$TARGET' ($JOBS jobs)"
  cmake --build "$BUILD_DIR" --target "$TARGET" --parallel "$JOBS"
elif $deps_built; then
  echo "→ Building agentos core only ($JOBS jobs)"
  cmake --build "$BUILD_DIR" --target agentos --parallel "$JOBS"
else
  echo "→ Building all ($JOBS jobs)"
  echo "  Step 1/2: deps  (downloaded + built once, cached in deps-build/ after)"
  echo "  Step 2/2: agentos core"
  cmake --build "$BUILD_DIR" --parallel "$JOBS"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Post-build report
# ─────────────────────────────────────────────────────────────────────────────
BINARY="$BUILD_DIR/agentos"
echo ""
echo "─────────────────────────────────────────────────────────────"
echo " Binary : $BINARY"
if [ -f "$BINARY" ]; then
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
else
    echo " (binary not found – skipping size/ldd/run)"
fi
echo "─────────────────────────────────────────────────────────────"

echo ""
echo "✓ Build complete. Binary at: $BINARY"
