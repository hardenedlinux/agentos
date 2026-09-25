#!/usr/bin/env bash
# Remove jobs older than N days (default 7).
# Usage: ./scripts/cleanup_jobs.sh [days]
# Example: ./scripts/cleanup_jobs.sh 0   # remove all jobs
DAYS=${1:-7}
DB=~/.agentos/agentos.db
CUTOFF=$(date -d "-${DAYS} days" +%s)
BEFORE=$(sqlite3 "$DB" "SELECT count(*) FROM jobs;")
sqlite3 "$DB" "DELETE FROM jobs WHERE updated_at < ${CUTOFF};"
AFTER=$(sqlite3 "$DB" "SELECT count(*) FROM jobs;")
echo "Removed $((BEFORE - AFTER)) jobs older than ${DAYS} day(s). ${AFTER} job(s) remaining."
