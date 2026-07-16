#!/usr/bin/env bash
#
# watch-job.sh — live-refresh a single job's status.
#
# Usage:
#   ./watch-job.sh <job_id> [interval_seconds]
#
# Wraps `agentos job status <job_id>` in `watch`, with CLICOLOR_FORCE set
# so colored output survives (watch never allocates a pty to the wrapped
# command, so isatty() alone would otherwise disable color entirely).
#
# `watch` clears the screen based on how many lines the *previous* render
# used — if this render is shorter (e.g. a step finished and its detail
# lines went away), the leftover rows from the longer previous render
# don't get erased and stay visible underneath the new, shorter output.
# This is a known watch quirk, more common in -c/color mode. Fix: force
# an explicit `tput clear` inside the wrapped command itself instead of
# relying on watch's own line-counted redraw.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $(basename "$0") <job_id> [interval_seconds]" >&2
    exit 1
fi

job_id="$1"
interval="${2:-2}"

# -c: let watch pass through the ANSI color codes agentos emits instead of
#     stripping/mangling them.
# -n: refresh interval in seconds.
exec env CLICOLOR_FORCE=1 watch -c -n "$interval" \
     "tput clear; CLICOLOR_FORCE=1 ./build/agentos job status '${job_id}'"
