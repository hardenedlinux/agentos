#!/usr/bin/env bash
# scripts/test.sh
# ─────────────────────────────────────────────────────────────────────────────
# Build and run unit tests for AgentOS.
#
# Usage:
#   ./scripts/test.sh                  # Release build + tests
#   ./scripts/test.sh --debug          # Debug build
#   ./scripts/test.sh --asan           # Debug + AddressSanitizer
#   ./scripts/test.sh --musl           # Fully static (requires musl-tools)
#   ./scripts/test.sh --clean          # Clean build dir first
#   ./scripts/test.sh --coverage       # Enable coverage report
#   ./scripts/test.sh --privileged     # setcap cap_sys_admin+ep on sandbox
#                                       # test binaries after build, so
#                                       # apply_worker_sandbox exercises the
#                                       # privileged (native overlayfs +
#                                       # pivot_root) path instead of
#                                       # --unsafe. Requires sudo. setcap is
#                                       # file-bound, so this must run after
#                                       # every rebuild.
#   ./scripts/test.sh --filter <expr>  # Run only matching tests (gtest filter)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

BUILD_TYPE="Release"
USE_MUSL=OFF
CLEAN=false
USE_ASAN=false
USE_PRIVILEGED=false
AGENTOS_COVERAGE=OFF
GTEST_FILTER=""
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

while [ $# -gt 0 ]; do
  case "$1" in
    --debug)    BUILD_TYPE="Debug" ;;
    --asan)     USE_ASAN=true; BUILD_TYPE="Debug" ;;
    --musl)     USE_MUSL=ON ;;
    --clean)    CLEAN=true ;;
    --coverage) AGENTOS_COVERAGE=ON ;;
    --privileged) USE_PRIVILEGED=true ;;
    --filter)   shift; GTEST_FILTER="$1" ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
  shift
done

if $USE_ASAN; then
  BUILD_DIR="$ROOT_DIR/build-asan"
  echo "→ ASAN mode: build dir is $BUILD_DIR, static linking disabled"
fi

if $CLEAN; then
  echo "→ Cleaning $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

mkdir -p /tmp
mkdir -p "$BUILD_DIR"
export TMPDIR="${TMPDIR:-/tmp}"

# ─────────────────────────────────────────────────────────────────────────────
# Configure
# ─────────────────────────────────────────────────────────────────────────────
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  if $USE_ASAN; then
    echo "→ Configuring ASAN build (Debug, tests=ON)"
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug          \
      -DAGENTOS_MUSL=OFF                \
      -DAGENTOS_BUILD_TESTS=ON          \
      -DAGENTOS_STATIC=OFF              \
      -DAGENTOS_STRIP=OFF               \
      -DAGENTOS_COVERAGE=OFF            \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
      -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"   \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
  else
    echo "→ Configuring superbuild ($BUILD_TYPE, musl=$USE_MUSL, tests=ON)"
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE"  \
      -DAGENTOS_MUSL="$USE_MUSL"        \
      -DAGENTOS_BUILD_TESTS=ON          \
      -DAGENTOS_STATIC=ON               \
      -DAGENTOS_STRIP=ON                \
      -DAGENTOS_COVERAGE="$AGENTOS_COVERAGE"
  fi
else
  echo "→ Build tree intact, skipping configure"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Build
# ─────────────────────────────────────────────────────────────────────────────
echo "→ Building all ($JOBS jobs)"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# ─────────────────────────────────────────────────────────────────────────────
# Privileged mode: grant CAP_SYS_ADMIN to the sandbox test binaries so
# apply_worker_sandbox's has_cap_sys_admin() check exercises the privileged
# (native overlayfs + pivot_root) path instead of --unsafe (ADR-016).
# setcap is bound to the file's inode and is lost on every relink, so this
# must run after every build — hence it lives here, not in CMake.
# ─────────────────────────────────────────────────────────────────────────────
if $USE_PRIVILEGED; then
  echo ""
  echo "→ Granting CAP_SYS_ADMIN to sandbox test binaries (sudo required)"
  PRIVILEGED_BINS=(
    "$BUILD_DIR/tests/test_orchestrator"
    "$BUILD_DIR/tests/test_dispatcher"
    "$BUILD_DIR/tests/test_worker_run"
  )
  for bin in "${PRIVILEGED_BINS[@]}"; do
    if [ -f "$bin" ]; then
      sudo setcap cap_sys_admin+ep "$bin"
    else
      echo "  ! skipping $bin (not found)"
    fi
  done
fi

# ─────────────────────────────────────────────────────────────────────────────
# Run tests
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "→ Running tests:"
cd "$BUILD_DIR"

if $USE_ASAN; then
  # With ASAN: run ctest with ASAN options; halt_on_error=0 so all tests run
  ASAN_OPTIONS=halt_on_error=0:detect_leaks=1 \
    ctest --output-on-failure -j "$JOBS" \
    ${GTEST_FILTER:+--tests-regex "$GTEST_FILTER"}
else
  ctest --output-on-failure -j "$JOBS" \
    ${GTEST_FILTER:+--tests-regex "$GTEST_FILTER"}
fi

cd "$ROOT_DIR"

# ─────────────────────────────────────────────────────────────────────────────
# Coverage report (non-ASAN only)
# ─────────────────────────────────────────────────────────────────────────────
if [ "$AGENTOS_COVERAGE" = "ON" ] && ! $USE_ASAN; then
  echo ""
  echo "→ Generating coverage report..."
  mkdir -p "$BUILD_DIR/coverage"
  if command -v gcovr &>/dev/null; then
    gcovr --root "$ROOT_DIR" \
          --filter "$ROOT_DIR/src/" \
          --exclude "$ROOT_DIR/src/core/deps/" \
          --html-details "$BUILD_DIR/coverage/index.html" \
          --xml "$BUILD_DIR/coverage/coverage.xml" \
          --print-summary
    echo "Coverage report: $BUILD_DIR/coverage/index.html"
  else
      echo "gcovr not found. Install with: pip install gcovr"
  fi
fi

echo ""
if $USE_ASAN; then
    echo "✓ ASAN tests complete."
elif $USE_PRIVILEGED; then
    echo "✓ Privileged tests complete (native overlayfs + pivot_root path)."
else
    echo "✓ Tests complete."
fi
