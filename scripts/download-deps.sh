#!/usr/bin/env bash
#
# download-deps.sh
#
# Downloads and verifies the uv static binary for the agentos release
# packaging pipeline.
#
# The version is pinned below (UV_VERSION) and never resolved as "latest".
# The checksum is not hardcoded in this script; instead it is fetched at
# build time from the sha256.sum asset published alongside the pinned
# GitHub Release. GitHub Releases are immutable once published, so this is
# equivalent to hardcoding the hash, but avoids manually re-copying it on
# every version bump (and the transcription errors that come with that).
#
# Usage:
#   ./download-deps.sh <output_dir> [target_triple]
#
#   output_dir     required. Directory to stage the uv binary under.
#                  Result: <output_dir>/<arch>/uv
#   target_triple  optional. Auto-detected from `uname -m` if omitted.
#                  Pass explicitly to cross-package another architecture,
#                  e.g. ./download-deps.sh ./deps-out aarch64-unknown-linux-musl
#
# Requires: wget, tar, sha256sum (coreutils)

set -euo pipefail

# ---- Pinned version ------------------------------------------------------
# Bump this on its own reviewed commit when upgrading uv; do not let it
# drift silently as part of unrelated changes.
readonly UV_VERSION="0.11.28"

readonly GITHUB_REPO="astral-sh/uv"
readonly RELEASE_BASE="https://github.com/${GITHUB_REPO}/releases/download/${UV_VERSION}"

# ---- Argument parsing ------------------------------------------------------
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <output_dir> [target_triple]" >&2
    exit 1
fi

OUTPUT_DIR="$1"
TARGET_TRIPLE="${2:-}"

detect_target_triple() {
    local arch
    arch="$(uname -m)"
    case "$arch" in
        x86_64)
            echo "x86_64-unknown-linux-musl"
            ;;
        aarch64|arm64)
            echo "aarch64-unknown-linux-musl"
            ;;
        *)
            echo "ERROR: unsupported host architecture: $arch" >&2
            exit 1
            ;;
    esac
}

if [[ -z "$TARGET_TRIPLE" ]]; then
    TARGET_TRIPLE="$(detect_target_triple)"
fi

# uv release assets are always named uv-<target_triple>.tar.gz
readonly ASSET_NAME="uv-${TARGET_TRIPLE}.tar.gz"
readonly ASSET_URL="${RELEASE_BASE}/${ASSET_NAME}"
readonly CHECKSUM_URL="${RELEASE_BASE}/sha256.sum"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "==> uv version:      ${UV_VERSION}"
echo "==> target triple:   ${TARGET_TRIPLE}"
echo "==> asset:           ${ASSET_NAME}"

# ---- Download asset and official checksum manifest ------------------------
echo "==> downloading ${ASSET_URL}"
wget -c --secure-protocol=TLSv1_2 \
    -O "${WORK_DIR}/${ASSET_NAME}" \
    "${ASSET_URL}"

echo "==> downloading sha256.sum"
wget -c --secure-protocol=TLSv1_2 \
    -O "${WORK_DIR}/sha256.sum" \
    "${CHECKSUM_URL}"

# ---- Verify -----------------------------------------------------------------
EXPECTED_LINE="$(grep -F "${ASSET_NAME}" "${WORK_DIR}/sha256.sum" || true)"
if [[ -z "$EXPECTED_LINE" ]]; then
    echo "ERROR: ${ASSET_NAME} not found in sha256.sum for version ${UV_VERSION}" >&2
    echo "       Upstream asset naming may have changed; check manually:" >&2
    echo "       ${RELEASE_BASE}" >&2
    exit 1
fi

echo "==> verifying checksum"
(
    cd "${WORK_DIR}"
    echo "${EXPECTED_LINE}" | sha256sum --check --status -
) || {
    echo "ERROR: checksum verification FAILED for ${ASSET_NAME}" >&2
    echo "       Refusing to use an unverified binary. This may indicate a" >&2
    echo "       network MITM, a corrupted download, or an upstream change." >&2
    exit 1
}
echo "==> checksum OK"

# ---- Extract and stage ------------------------------------------------------
mkdir -p "${WORK_DIR}/extracted"
tar -xzf "${WORK_DIR}/${ASSET_NAME}" -C "${WORK_DIR}/extracted"

# The internal directory name inside the tarball (uv-<triple>/uv) has
# changed across releases before, so locate the binary instead of
# hardcoding the path.
UV_BIN_PATH="$(find "${WORK_DIR}/extracted" -type f -name 'uv' -perm -u+x | head -n1)"
if [[ -z "$UV_BIN_PATH" ]]; then
    echo "ERROR: could not locate 'uv' executable inside ${ASSET_NAME}" >&2
    exit 1
fi

DEST_DIR="${OUTPUT_DIR}/${TARGET_TRIPLE}"
mkdir -p "${DEST_DIR}"
cp "${UV_BIN_PATH}" "${DEST_DIR}/uv"
chmod 0755 "${DEST_DIR}/uv"

echo "==> uv binary staged at: ${DEST_DIR}/uv"
"${DEST_DIR}/uv" --version
