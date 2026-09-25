#!/usr/bin/env bash
#
# Round 2 — negative-path functional tests, following Round 1
# (functest_round1_positive.sh) going fully green. Covers:
#
#   Scenario 1: ADR-036 config fail-fast (typo'd algorithm name)
#   Scenario 2: authentication rejection (no key / malformed key / wrong key)
#   Scenario 3: decayed fact_type with no [memory_curve.*] config entry
#   Scenario 4: param validation + subject.* validation + cross-user isolation
#
# Each scenario gets its own throwaway AGENTOS_HOME and its own daemon
# lifecycle — unlike Round 1, several of these need a daemon restart or a
# second identity mid-scenario, so a single shared sandbox doesn't work.
#
# Usage:
#   AGENTOS_BIN=/path/to/agentos ./functest_round2_negative.sh
#
# Requires: jq, sqlite3
#
# All scenario directories are collected and removed on exit unless
# KEEP_HOMES=1 is set.

set -uo pipefail

AGENTOS_BIN="./build/agentos" #"${AGENTOS_BIN:-agentos}"
KEEP_HOMES="${KEEP_HOMES:-0}"

PASS=0
FAIL=0
DAEMON_PID=""
declare -a ALL_HOMES=()

# ---------------------------------------------------------------------------
# generic helpers (same conventions as Round 1)
# ---------------------------------------------------------------------------

log()  { printf '\033[1;34m[*]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[PASS]\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
info() { printf '\033[1;36m[INFO]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[ABORT]\033[0m %s\n' "$*"; cleanup; exit 1; }

stop_daemon() {
  if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    kill "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
  fi
  DAEMON_PID=""
}

cleanup() {
  stop_daemon
  if [[ "$KEEP_HOMES" != "1" ]]; then
    for h in "${ALL_HOMES[@]:-}"; do
      [[ -n "$h" ]] && rm -rf "$h"
    done
  else
    log "keeping scenario directories (KEEP_HOMES=1): ${ALL_HOMES[*]:-}"
  fi
}
trap cleanup EXIT

require_jq() {
  command -v jq >/dev/null 2>&1 || die "jq is required but not found on PATH"
}
require_sqlite3() {
  command -v sqlite3 >/dev/null 2>&1 || die "sqlite3 is required but not found on PATH"
}

new_home() {
  local h
  h="$(mktemp -d /tmp/agentos-functest2-XXXXXX)"
  ALL_HOMES+=("$h")
  printf '%s' "$h"
}

# kill -0 <pid> succeeds even for a zombie (exited but not yet reaped by
# `wait`) — it only tells you the PID still exists in the process table,
# not that the process is actually still running. A daemon that dies
# almost instantly (e.g. config-load fail-fast) can easily still be a
# zombie the first few times this loop checks, making a genuinely-dead
# process look "alive" and masking an early exit. Read /proc/<pid>/stat's
# state field instead — 'Z' (zombie) or a missing /proc entry both count
# as "not really running".
is_process_alive() {
  local pid="$1"
  [[ -d "/proc/$pid" ]] || return 1
  local state
  state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)
  [[ -n "$state" && "$state" != "Z" ]]
}

# Same fix as Round 1: extract the real JSON line by actually validating
# each line with jq, since spdlog output can land before OR after the
# payload and "[HH:MM:SS] ..." log lines false-match a naive
# "starts with { or [" filter.
json_line() {
  local line result=""
  while IFS= read -r line; do
    if printf '%s' "$line" | jq -e . >/dev/null 2>&1; then
      result="$line"
    fi
  done <<< "$1"
  printf '%s\n' "$result"
}

# Starts a daemon against $1 (AGENTOS_HOME), waits up to 5s for the socket.
# Sets DAEMON_PID and global SOCK. Return codes:
#   0 = socket bound (daemon is up and accepting connections)
#   1 = process exited before binding (check $1/daemon.log)
#   2 = still running but never bound within the timeout (possibly hung)
start_daemon() {
  local home="$1"
  # Don't rely on the daemon's own initialise_home() stale-socket cleanup
  # having already run by the time we start polling — if a previous
  # daemon against this same home left a socket file behind and this new
  # daemon dies (e.g. config fail-fast) before Gateway::start() ever
  # re-binds it, a leftover file from the OLD process could make our -S
  # check below report "bound" when nothing new actually bound anything.
  rm -f "$home/run/agentos.sock" 2>/dev/null

  AGENTOS_HOME="$home" stdbuf -oL -eL "$AGENTOS_BIN" run >"$home/daemon.log" 2>&1 &
  DAEMON_PID=$!
  SOCK="$home/run/agentos.sock"
  local i
  for i in $(seq 1 50); do
    [[ -S "$SOCK" ]] && return 0
    if ! is_process_alive "$DAEMON_PID"; then
      wait "$DAEMON_PID" 2>/dev/null
      return 1
    fi
    sleep 0.1
  done
  return 2
}

# Runs a command, expects a NONzero exit AND the combined output to
# contain $expected_substr. Prints raw output on mismatch either way.
expect_error_containing() {
  local desc="$1" expected="$2"; shift 2
  local out rc
  out=$("$@" 2>&1)
  rc=$?
  if [[ $rc -ne 0 ]] && echo "$out" | grep -qF "$expected"; then
    ok "$desc"
  else
    bad "$desc (exit=$rc; expected output to contain: '$expected')"
    echo "$out"
  fi
}

expect_success() {
  local desc="$1" __outvar="$2"; shift 2
  local out rc
  out=$("$@" 2>&1)
  rc=$?
  if [[ $rc -eq 0 ]]; then
    ok "$desc"
  else
    bad "$desc (exit=$rc)"
    echo "$out"
  fi
  printf -v "$__outvar" '%s' "$out"
  return $rc
}

require_jq
require_sqlite3

# ===========================================================================
# Scenario 1 — ADR-036 config fail-fast: a typo'd algorithm name for an
# EXISTING [memory_curve.<fact_type>] block should make the daemon refuse
# to start, per ADR-036: "fails fast at startup if any name doesn't
# resolve". This is a real open question, not an assumed-safe check — the
# handoff doc flagged that this validation might still be a stubbed TODO,
# and cmd_user_facts_record's own runtime lookup (config_.memory_curve.
# find(...)) suggests the enforcement point may not be at config load at
# all. Whichever way this goes is useful information.
# ===========================================================================

log "== Scenario 1: config fail-fast on a typo'd memory_curve algorithm name =="

HOME1=$(new_home)
log "HOME1 = $HOME1"
export AGENTOS_HOME="$HOME1"

# First boot: seeds a valid default config.toml (home_init.cpp) and
# confirms clean startup, so we know the *only* variable changed below is
# the corrupted algorithm name — not some unrelated startup failure.
if start_daemon "$HOME1"; then
  ok "(setup) daemon starts cleanly with the untouched seeded config"
  stop_daemon
else
  bad "(setup) daemon did not start cleanly even before corrupting config — aborting Scenario 1"
  cat "$HOME1/daemon.log"
fi

CFG1="$HOME1/config.toml"
if [[ -f "$CFG1" ]]; then
  sed -i.bak 's/^algorithm = "ema"$/algorithm = "no_such_algorithm"/' "$CFG1"
  # Corrupt only the FIRST occurrence (risk_preference, per file order) —
  # confirm exactly one line changed so we know precisely what we broke.
  CHANGED=$(diff "$CFG1.bak" "$CFG1" | grep -c '^>' || true)
  info "corrupted $CHANGED occurrence(s) of algorithm = \"ema\" -> \"no_such_algorithm\" in $CFG1"
fi

start_daemon "$HOME1"
RC=$?
if [[ $RC -eq 1 ]]; then
  if grep -qi "no_such_algorithm\|memory_curve\|algorithm" "$HOME1/daemon.log"; then
    ok "daemon refused to start with a bad algorithm name, and the error names the problem (ADR-036 fail-fast confirmed working)"
  else
    ok "daemon refused to start with a bad algorithm name (exit before bind), but the log doesn't clearly name the cause — check $HOME1/daemon.log manually"
    cat "$HOME1/daemon.log"
  fi
elif [[ $RC -eq 0 ]]; then
  bad "daemon STARTED SUCCESSFULLY despite a bad algorithm name — ADR-036's fail-fast validation is not actually enforced at config load (this matches the handoff doc's worry that this check might still be a stubbed TODO). Not a script bug — a real gap to confirm with Roy."
  info "daemon.log (now line-buffered, should reflect reality):"
  cat "$HOME1/daemon.log"
  stop_daemon
else
  bad "daemon neither bound the socket nor exited within 5s (RC=2) — possibly hung on the bad config. Check $HOME1/daemon.log"
  cat "$HOME1/daemon.log"
  stop_daemon
fi

# ===========================================================================
# Scenario 2 — authentication rejection. None of these need a real access
# key to exist: authenticate() rejects malformed keys (wrong length/charset)
# before ever consulting active_keys_, and a well-formed-but-unknown key
# simply won't be found in the map — so a completely key-less sandbox is
# the cleanest way to test all three without the active_keys_ staleness
# bug (documented in Round 1) being a confound.
# ===========================================================================

log "== Scenario 2: auth rejection (no key / malformed key / wrong key) =="

HOME2=$(new_home)
log "HOME2 = $HOME2"
export AGENTOS_HOME="$HOME2"

if start_daemon "$HOME2"; then
  ok "(setup) daemon started for Scenario 2"
else
  bad "(setup) daemon failed to start for Scenario 2"
  cat "$HOME2/daemon.log"
  die "cannot continue Scenario 2 without a running daemon"
fi

# 2a. zero keys exist anywhere — CLI's own local lookup should fail before
# ever attempting to connect.
expect_error_containing \
  "no key anywhere -> CLI reports 'no active access key'" \
  "no active access key" \
  "$AGENTOS_BIN" user facts get

# 2b. malformed key (wrong length/charset) — authenticate() rejects on the
# size/hex-charset check before even doing a map lookup.
expect_error_containing \
  "malformed --key (too short, not hex) -> Failed to authorize" \
  "Failed to authorize" \
  "$AGENTOS_BIN" user --key deadbeef facts get

# 2c. well-formed (64 lowercase hex chars) but nonexistent key.
FAKE_KEY=$(printf '0%.0s' $(seq 1 64))
expect_error_containing \
  "well-formed but unknown --key -> Failed to authorize" \
  "Failed to authorize" \
  "$AGENTOS_BIN" user --key "$FAKE_KEY" facts get

stop_daemon

# ===========================================================================
# Scenario 3 — a decayed fact_type with NO [memory_curve.<fact_type>] entry
# at all (not a bad name — the block is simply absent). This exercises the
# separate runtime check in cmd_user_facts_record
# (config_.memory_curve.find(fact_type_name) == end() -> -32031), which is
# a different code path from Scenario 1's config-load-time check.
# ===========================================================================

log "== Scenario 3: decayed fact_type with no memory_curve config entry =="

HOME3=$(new_home)
log "HOME3 = $HOME3"
export AGENTOS_HOME="$HOME3"

if start_daemon "$HOME3"; then
  stop_daemon
else
  bad "(setup) daemon did not start cleanly for Scenario 3 — aborting"
  cat "$HOME3/daemon.log"
fi

CFG3="$HOME3/config.toml"
if [[ -f "$CFG3" ]]; then
  # Remove the [memory_curve.risk_preference] block (3 lines: header,
  # algorithm, params) entirely, leaving category_interest/market_region
  # untouched.
  awk '
    /^\[memory_curve\.risk_preference\]$/ { skip=3; next }
    skip > 0 { skip--; next }
    { print }
  ' "$CFG3" > "$CFG3.tmp" && mv "$CFG3.tmp" "$CFG3"
  if grep -q 'memory_curve.risk_preference' "$CFG3"; then
    bad "failed to remove the [memory_curve.risk_preference] block — check the awk pattern against the actual seeded format"
    cat "$CFG3"
  else
    ok "(setup) removed [memory_curve.risk_preference] entirely from config.toml"
  fi
fi

# key must exist BEFORE the daemon starts (Round 1's active_keys_
# staleness finding still applies here).
KEYGEN3=$("$AGENTOS_BIN" key generate --role admin --description "functest2-s3" 2>&1)
RAW3=$(echo "$KEYGEN3" | grep -oE 'ak_[0-9a-f]{64}' | sed 's/^ak_//')
if [[ -z "$RAW3" ]]; then
  bad "could not extract a raw key from key generate output for Scenario 3"
  echo "$KEYGEN3"
  die "cannot continue Scenario 3 without a key"
fi

if start_daemon "$HOME3"; then
  ok "(setup) daemon started with risk_preference's memory_curve block removed (config-load itself doesn't reject a missing block, only a bad name — consistent with Scenario 1 testing a different check)"
else
  bad "(setup) daemon refused to start merely because one decayed fact_type has no config block — if this is intentional, Scenario 3's premise is wrong and needs rethinking with Roy"
  cat "$HOME3/daemon.log"
fi

if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
  expect_error_containing \
    "recording risk_preference (no config entry) -> No memory_curve config for fact_type" \
    "No memory_curve config for fact_type" \
    "$AGENTOS_BIN" user --key "$RAW3" facts record \
      --fact-type risk_preference --fact-key test --payload '{}' --signal 0.5
fi

stop_daemon

# ===========================================================================
# Scenario 4 — param validation for user.facts.*/subject.*, plus the
# highest-priority item from the original handoff: cross-user isolation.
#
# Note on scope: fields marked ->required() in our own CLI wrappers
# (--payload, --entry-value, --source-job-id, etc.) can't be omitted
# through the CLI itself — CLI11 rejects a missing required option before
# ever building the RPC request, so the server's own "Invalid params"
# HasMember() checks for those fields are NOT reachable this way. Only a
# raw JSON-RPC client (bypassing the CLI) could exercise that specific
# validation path. What IS reachable: fields the CLI accepts as free-form
# strings (--fact-type, --unit-type) or leaves optional client-side but
# required server-side (--signal for decayed types) — those are covered
# below.
# ===========================================================================

log "== Scenario 4: param validation + subject.* validation + cross-user isolation =="

HOME4=$(new_home)
log "HOME4 = $HOME4"
export AGENTOS_HOME="$HOME4"

KEYGEN_A=$("$AGENTOS_BIN" key generate --role admin --description "functest2-userA" 2>&1)
RAW_A=$(echo "$KEYGEN_A" | grep -oE 'ak_[0-9a-f]{64}' | sed 's/^ak_//')
KEYGEN_B=$("$AGENTOS_BIN" key generate --role admin --description "functest2-userB" 2>&1)
RAW_B=$(echo "$KEYGEN_B" | grep -oE 'ak_[0-9a-f]{64}' | sed 's/^ak_//')

if [[ -n "$RAW_A" && -n "$RAW_B" ]]; then
  ok "(setup) generated two distinct admin keys (A and B) before daemon startup"
else
  bad "(setup) failed to extract one or both raw keys"
  echo "KEYGEN_A: $KEYGEN_A"
  echo "KEYGEN_B: $KEYGEN_B"
  die "cannot continue Scenario 4 without two distinct keys"
fi

if start_daemon "$HOME4"; then
  ok "(setup) daemon started for Scenario 4"
else
  bad "(setup) daemon failed to start for Scenario 4"
  cat "$HOME4/daemon.log"
  die "cannot continue Scenario 4 without a running daemon"
fi

DB4="$HOME4/agentos.db"
ID_A=$(sqlite3 "$DB4" "SELECT id FROM access_keys WHERE key='$RAW_A';")
ID_B=$(sqlite3 "$DB4" "SELECT id FROM access_keys WHERE key='$RAW_B';")
info "user A id=$ID_A, user B id=$ID_B"

# --- param validation (as user A) ---

expect_error_containing \
  "unknown fact_type -> Unknown fact_type" \
  "Unknown fact_type" \
  "$AGENTOS_BIN" user --key "$RAW_A" facts record \
    --fact-type not_a_real_fact_type --fact-key x --payload '{}'

expect_error_containing \
  "decayed fact_type without --signal -> signal required" \
  "signal required for decayed fact_type" \
  "$AGENTOS_BIN" user --key "$RAW_A" facts record \
    --fact-type category_interest --fact-key electronics --payload '{}'

expect_error_containing \
  "subject register with invalid unit_type -> rejected" \
  "unit_type must be 'file' or 'line'" \
  "$AGENTOS_BIN" subject --key "$RAW_A" register \
    --subject-type codebase --unit-type chunk

RANDOM_SUBJECT_ID="00000000-0000-0000-0000-000000000000"

expect_error_containing \
  "subject units next on a nonexistent subject_id -> subject not found" \
  "subject not found" \
  "$AGENTOS_BIN" subject --key "$RAW_A" units next \
    --subject-id "$RANDOM_SUBJECT_ID"

expect_error_containing \
  "subject units progress on a nonexistent subject_id -> subject not found" \
  "subject not found" \
  "$AGENTOS_BIN" subject --key "$RAW_A" units progress \
    --subject-id "$RANDOM_SUBJECT_ID"

expect_error_containing \
  "subject memory query on a nonexistent subject_id -> subject not found" \
  "subject not found" \
  "$AGENTOS_BIN" subject --key "$RAW_A" memory query \
    --subject-id "$RANDOM_SUBJECT_ID"

# --- cross-user isolation: A creates, B is rejected on every subject.* op ---

expect_success \
  "(setup) user A registers a subject" REG_A \
  "$AGENTOS_BIN" subject --json --key "$RAW_A" register \
    --subject-type codebase --unit-type file --title "userA private repo"
SUBJECT_A=$(json_line "$REG_A" | jq -r '.subject_id // empty')
if [[ -z "$SUBJECT_A" ]]; then
  bad "could not extract subject_id for user A — aborting cross-user checks"
else
  ok "user A's subject_id = $SUBJECT_A"

  expect_error_containing \
    "user B: units populate on A's subject -> subject not found" \
    "subject not found" \
    "$AGENTOS_BIN" subject --key "$RAW_B" units populate \
      --subject-id "$SUBJECT_A" --unit x.py

  expect_error_containing \
    "user B: units next on A's subject -> subject not found" \
    "subject not found" \
    "$AGENTOS_BIN" subject --key "$RAW_B" units next \
      --subject-id "$SUBJECT_A"

  expect_error_containing \
    "user B: units complete on A's subject -> subject not found" \
    "subject not found" \
    "$AGENTOS_BIN" subject --key "$RAW_B" units complete \
      --subject-id "$SUBJECT_A" --index 0

  expect_error_containing \
    "user B: units progress on A's subject -> subject not found" \
    "subject not found" \
    "$AGENTOS_BIN" subject --key "$RAW_B" units progress \
      --subject-id "$SUBJECT_A"

  expect_error_containing \
    "user B: memory upsert on A's subject -> subject not found" \
    "subject not found" \
    "$AGENTOS_BIN" subject --key "$RAW_B" memory upsert \
      --subject-id "$SUBJECT_A" --entry-key k --entry-value '{}' \
      --source-job-id job1

  expect_error_containing \
    "user B: memory query on A's subject -> subject not found" \
    "subject not found" \
    "$AGENTOS_BIN" subject --key "$RAW_B" memory query \
      --subject-id "$SUBJECT_A"

  # sanity: B is not globally broken — B can register and use their OWN
  # subject fine. Without this check, "everything returns subject not
  # found for B" could be masking a totally broken key B rather than
  # correctly-scoped rejection.
  REG_B=""
  expect_success \
    "user B can still register their OWN subject (isolation isn't overly broad)" REG_B \
    "$AGENTOS_BIN" subject --json --key "$RAW_B" register \
      --subject-type codebase --unit-type file --title "userB own repo"
  SUBJECT_B=$(json_line "$REG_B" | jq -r '.subject_id // empty')
  if [[ -n "$SUBJECT_B" ]]; then
    POP_B_OUT=""
    expect_success \
      "user B can populate units on their OWN subject" POP_B_OUT \
      "$AGENTOS_BIN" subject --key "$RAW_B" units populate \
        --subject-id "$SUBJECT_B" --unit y.py
  fi
fi

# --- cross-user isolation: user.facts.* (implicit per-caller scoping, not
# an explicit ownership param — so the "negative" case here is data NOT
# leaking through a successful call, rather than an error response) ---

"$AGENTOS_BIN" user --key "$RAW_A" facts record \
  --fact-type card_reaction --fact-key userA_only_sku --payload '{}' >/dev/null 2>&1

GET_AS_B=$("$AGENTOS_BIN" user --json --key "$RAW_B" facts get 2>&1)
GET_AS_B_JSON=$(json_line "$GET_AS_B")
if echo "$GET_AS_B_JSON" | jq -e '.facts[] | select(.fact_key=="userA_only_sku")' >/dev/null 2>&1; then
  bad "user B's facts.get leaked user A's fact — cross-user isolation broken for user.facts.get"
  echo "$GET_AS_B_JSON"
else
  ok "user B's facts.get does not see user A's card_reaction fact (implicit per-caller scoping holds)"
fi

stop_daemon

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------

echo
log "== summary =="
echo "PASS: $PASS   FAIL: $FAIL"
if [[ "$FAIL" -eq 0 ]]; then
  log "all round-2 negative-path checks green"
  exit 0
else
  log "one or more checks failed or surfaced a real finding — see [FAIL] lines above"
  exit 1
fi
