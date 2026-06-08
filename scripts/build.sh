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
#   Ubuntu/Debian:  sudo apt install cmake ninja-build build-essential git perl
#   (OpenSSL build requires perl; no system libssl-dev needed)
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
# Dependency check.
#
# Strategy:
#   1. Check stamp files (in deps-build/stamps/, survive rm -rf build/)
#   2. Check library files as secondary sanity guard
#
# Deps managed by ExternalProject (compiled, produce .a):
#   spdlog, libzmq, libseccomp, libcap, openssl
#
# Deps managed by ExternalProject (header-only, no .a):
#   httplib, rapidjson, cppzmq, sqlite3_amalgamation
#
# Deps managed by file(DOWNLOAD) (single file, no stamp):
#   tomlplusplus → deps-src/downloads/toml.hpp
# ─────────────────────────────────────────────────────────────────────────────

# Stamp check for ExternalProject deps
deps_stamp_ok=true

# Compiled deps
for dep in spdlog libzmq libseccomp libcap openssl; do
  if [ ! -f "$DEPS_BUILD_DIR/stamps/$dep/${dep}_ext-install" ]; then
    deps_stamp_ok=false
    break
  fi
done

# Header-only deps (no install step — check download stamp)
if $deps_stamp_ok; then
  for dep in httplib rapidjson cppzmq; do
    if [ ! -f "$DEPS_BUILD_DIR/stamps/$dep/${dep}_ext-download" ]; then
      deps_stamp_ok=false
      break
    fi
  done
fi

# SQLite amalgamation (header-only, ExternalProject)
if $deps_stamp_ok; then
  if [ ! -f "$DEPS_BUILD_DIR/stamps/sqlite3/sqlite3_amalgamation_ext-download" ]; then
    deps_stamp_ok=false
  fi
fi

# toml++ single-file download (no stamp — check file presence)
if $deps_stamp_ok; then
  if [ ! -f "$ROOT_DIR/deps-src/downloads/toml.hpp" ]; then
    deps_stamp_ok=false
  fi
fi

# Library file check (compiled deps only)
deps_libs_ok=true
for lib in spdlog libzmq libseccomp libcap; do
  if [ ! -f "$DEPS_BUILD_DIR/$lib/lib/lib$lib.a" ]; then
    deps_libs_ok=false
    break
  fi
done
# OpenSSL produces two libraries
if $deps_libs_ok; then
  if [ ! -f "$DEPS_BUILD_DIR/openssl/lib/libssl.a" ] || \
     [ ! -f "$DEPS_BUILD_DIR/openssl/lib/libcrypto.a" ]; then
    deps_libs_ok=false
  fi
fi

# GoogleTest (only when tests enabled)
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
