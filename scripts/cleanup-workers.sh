#!/usr/bin/env bash
# cleanup-workers.sh — remove all Forge-generated workers from DB and filesystem
# Usage: ./scripts/cleanup-workers.sh [--dry-run]

set -euo pipefail

AGENTOS_HOME="${AGENTOS_HOME:-$HOME/.agentos}"
DB="$AGENTOS_HOME/agentos.db"
WORKERS_DIR="$AGENTOS_HOME/workers"
FORGE_DIR="$AGENTOS_HOME/forge"
DRY_RUN=false

if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=true
    echo "[dry-run] no changes will be made"
fi

if [[ ! -f "$DB" ]]; then
    echo "error: database not found at $DB" >&2
    exit 1
fi

# Collect worker IDs from DB
mapfile -t WORKER_IDS < <(sqlite3 "$DB" "SELECT id FROM agents WHERE role='worker';")

if [[ ${#WORKER_IDS[@]} -eq 0 ]]; then
    echo "no workers found."
    exit 0
fi

echo "found ${#WORKER_IDS[@]} worker(s):"
for id in "${WORKER_IDS[@]}"; do
    echo "  $id"
done

if [[ "$DRY_RUN" == true ]]; then
    echo "[dry-run] would delete ${#WORKER_IDS[@]} workers from DB and filesystem"
    exit 0
fi

# Remove from DB
sqlite3 "$DB" "DELETE FROM capabilities WHERE agent_id IN (SELECT id FROM agents WHERE role='worker');"
sqlite3 "$DB" "DELETE FROM agents WHERE role='worker';"
echo "removed ${#WORKER_IDS[@]} worker(s) from DB"

# Remove worker directories
removed_dirs=0
for id in "${WORKER_IDS[@]}"; do
    dir="$WORKERS_DIR/$id"
    if [[ -d "$dir" ]]; then
        rm -rf "$dir"
        ((removed_dirs++))
    fi
    # Also clean up forge artifacts
    forge_dir="$FORGE_DIR/$id"
    if [[ -d "$forge_dir" ]]; then
        rm -rf "$forge_dir"
    fi
done
echo "removed $removed_dirs worker director(ies) from filesystem"

echo "done. restart daemon to sync in-memory registry."
