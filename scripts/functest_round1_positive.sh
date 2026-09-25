#!/usr/bin/env bash
#
# Round 1 — positive-path functional tests for:
#   - ADR-034 User Facts   (user.facts.record / user.facts.get)
#   - ADR-036 Memory Curve (the "ema" algorithm behind decayed fact_types)
#   - ADR-035 Subject Memory (subject.* — register/units/memory)
#
# Everything here goes through the real `agentos` CLI binary, which means
# every call is a real ZMQ DEALER -> Gateway ROUTER -> Orchestrator dispatch
# round trip against a real (temporary) daemon instance — not a unit test
# shortcut. Round 2 (negative/error-input tests) is a separate script to
# write once this one is fully green.
#
# Usage:
#   AGENTOS_BIN=/path/to/agentos ./functest_round1_positive.sh
#
# Requires: jq (for parsing --json output and asserting on fields)
#
# Uses a throwaway AGENTOS_HOME under /tmp — never touches your real
# ~/.agentos, and is deleted on exit (success or failure) unless you
# set KEEP_HOME=1.

set -uo pipefail

AGENTOS_BIN="./build/agentos" #"${AGENTOS_BIN:-agentos}"
KEEP_HOME="${KEEP_HOME:-0}"

export AGENTOS_HOME
AGENTOS_HOME="$(mktemp -d /tmp/agentos-functest-XXXXXX)"

PASS=0
FAIL=0
DAEMON_PID=""

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

log()  { printf '\033[1;34m[*]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[PASS]\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
die()  { printf '\033[1;31m[ABORT]\033[0m %s\n' "$*"; cleanup; exit 1; }

cleanup() {
  if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    log "stopping daemon (pid $DAEMON_PID)"
    kill "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
  fi
  if [[ "$KEEP_HOME" != "1" ]]; then
    rm -rf "$AGENTOS_HOME"
  else
    log "keeping AGENTOS_HOME at $AGENTOS_HOME (KEEP_HOME=1)"
  fi
}
trap cleanup EXIT

require_jq() {
  command -v jq >/dev/null 2>&1 || die "jq is required but not found on PATH"
}

require_sqlite3() {
  command -v sqlite3 >/dev/null 2>&1 || die "sqlite3 is required but not found on PATH"
}

# Runs a CLI command, expects exit 0. Prints the raw output on failure.
# Usage: run_ok <description> -- <cli args...>
run_ok() {
  local desc="$1"; shift
  [[ "$1" == "--" ]] && shift
  local out
  if out=$("$AGENTOS_BIN" "$@" 2>&1); then
    ok "$desc"
    printf '%s\n' "$out"
  else
    bad "$desc (exit $?)"
    printf '%s\n' "$out"
  fi
  printf '%s' "$out"
}

# Asserts $1 == $2 (numeric, with tolerance $3 default 1e-6), else FAIL.
assert_close() {
  local actual="$1" expected="$2" tol="${3:-0.000001}" desc="$4"
  local diff
  diff=$(awk -v a="$actual" -v b="$expected" 'BEGIN{d=a-b; if (d<0) d=-d; print d}')
  if awk -v d="$diff" -v t="$tol" 'BEGIN{exit !(d<=t)}'; then
    ok "$desc (got $actual, expected $expected)"
  else
    bad "$desc (got $actual, expected $expected, diff $diff)"
  fi
}

assert_eq() {
  local actual="$1" expected="$2" desc="$3"
  if [[ "$actual" == "$expected" ]]; then
    ok "$desc (got '$actual')"
  else
    bad "$desc (got '$actual', expected '$expected')"
  fi
}

# The CLI's stdout can carry spdlog lines (e.g. "[info] [database] opened
# ...") ahead of OR after the actual --json payload — main.cpp sets the
# default logger level to `off`, so this is coming from somewhere else in
# the CLI path (CliClient's own local key-lookup, or the local `open_db()`
# path used by `key *`/`user facts events`) rather than being suppressed
# the way top-level `agentos run` output is. Until that's tracked down,
# extract the real JSON line by actually validating each line with jq
# rather than guessing from its leading character — log lines look like
# "[HH:MM:SS] [info] ..." which ALSO starts with '[', so a naive
# "starts with { or [" filter false-matches them (this bit us once
# already: a log line landed after the JSON and got picked as "last").
json_line() {
  local line result=""
  while IFS= read -r line; do
    if printf '%s' "$line" | jq -e . >/dev/null 2>&1; then
      result="$line"
    fi
  done <<< "$1"
  printf '%s\n' "$result"
}

# ---------------------------------------------------------------------------
# 0. fresh daemon startup — exercises the ADR-036 wiring fix + the
#    home_init.cpp default config-template fix in the same shot.
# ---------------------------------------------------------------------------

require_jq
log "AGENTOS_HOME = $AGENTOS_HOME"

# ---------------------------------------------------------------------------
# 0a. generate the default admin key BEFORE the daemon starts.
#
# key generate is a local command (ADR-020: writes directly to
# agentos.db, no daemon required) — but Orchestrator::authenticate() only
# checks an in-memory snapshot (active_keys_) populated once by
# load_active_keys() at startup, with no refresh path afterward (same
# class of bug as the already-known Registry load-once-at-startup issue,
# just with no refresh hook at all yet for keys). Generating the key
# first means the daemon's very first load_active_keys() call already
# sees it — generating it after daemon startup currently requires a
# daemon restart to become usable, which is a real gap worth its own fix,
# tracked separately from this test.
# ---------------------------------------------------------------------------

KEYGEN_OUT=$("$AGENTOS_BIN" key generate --role admin --description "functest" 2>&1)
if [[ $? -eq 0 ]]; then
  ok "generated default admin access key (pre-daemon-startup)"
else
  bad "key generate failed"
  echo "$KEYGEN_OUT"
  die "cannot continue without an access key"
fi

log "starting daemon fresh (no pre-existing ~/.agentos)"

"$AGENTOS_BIN" run >"$AGENTOS_HOME/daemon.log" 2>&1 &
DAEMON_PID=$!

# wait for the socket to appear instead of a fixed sleep
SOCK="$AGENTOS_HOME/run/agentos.sock"
for _ in $(seq 1 50); do
  [[ -S "$SOCK" ]] && break
  sleep 0.1
done

if [[ -S "$SOCK" ]]; then
  ok "daemon started and bound $SOCK"
else
  bad "daemon did not bind $SOCK within 5s — check $AGENTOS_HOME/daemon.log"
  echo "----- daemon.log -----"
  cat "$AGENTOS_HOME/daemon.log"
  echo "-----------------------"
  die "cannot continue without a running daemon"
fi

if grep -qi "memory_curve\|ema" "$AGENTOS_HOME/daemon.log" 2>/dev/null; then
  log "daemon.log mentions memory_curve/ema at startup (informational, not asserted)"
fi

# sanity: the seeded config.toml must actually contain the memory_curve
# blocks (this is the home_init.cpp fix — assert it directly rather than
# trusting the daemon not to have crashed for an unrelated reason)
CFG="$AGENTOS_HOME/config.toml"
[[ -f "$CFG" ]] || die "config.toml was not generated at $CFG"
if grep -q '\[memory_curve.category_interest\]' "$CFG" \
  && grep -q '\[memory_curve.risk_preference\]' "$CFG" \
  && grep -q '\[memory_curve.market_region\]' "$CFG"; then
  ok "seeded config.toml contains all three [memory_curve.*] blocks"
else
  bad "seeded config.toml is missing one or more [memory_curve.*] blocks"
  cat "$CFG"
fi

# ---------------------------------------------------------------------------
# 1. user facts — non-decayed (card_reaction): record + get round trip
# ---------------------------------------------------------------------------

log "== user.facts.record / get — card_reaction (non-decayed) =="

"$AGENTOS_BIN" user --json facts record \
  --fact-type card_reaction --fact-key sku_123 \
  --payload '{"note":"clicked"}' >/tmp/functest_out.json 2>&1
if [[ $? -eq 0 ]]; then
  ok "user facts record card_reaction (non-decayed, no --signal needed)"
else
  bad "user facts record card_reaction failed"
  cat /tmp/functest_out.json
fi

GET_OUT=$("$AGENTOS_BIN" user --json facts get 2>&1)
GET_JSON=$(json_line "$GET_OUT")
# card_reaction is non-decayed (ADR-034): it is written ONLY to the
# append-only user_fact_events log, never to user_facts — and
# user.facts.get reads exclusively from user_facts. So the correct
# assertion is "the row is absent from get's output", not present. There
# is currently no RPC method exposing raw user_fact_events, so this is as
# far as this can be verified from outside a direct sqlite3 inspection.
if echo "$GET_JSON" | jq -e '.facts[] | select(.fact_type=="card_reaction" and .fact_key=="sku_123")' >/dev/null 2>&1; then
  bad "user facts get unexpectedly returned a card_reaction row (should be event-log-only per ADR-034, never in user_facts)"
  echo "$GET_JSON"
else
  ok "user facts get correctly omits card_reaction (non-decayed, event-log-only — not a failure)"
fi

# ---------------------------------------------------------------------------
# 2. user facts — decayed (category_interest): two writes, verify real ema
#    alpha=0.25 (per the seeded config.toml / memory-curve-algorithm.md):
#      write 1: signal=0.8, old_score=nullopt -> score = 0.8 (seeded raw)
#      write 2: signal=0.2, old_score=0.8
#        -> new_score = 0.8*(1-0.25) + 0.2*0.25 = 0.6 + 0.05 = 0.65
# ---------------------------------------------------------------------------

log "== user.facts.record — category_interest (decayed, real ema) =="

"$AGENTOS_BIN" user --json facts record \
  --fact-type category_interest --fact-key electronics \
  --payload '{}' --signal 0.8 >/dev/null 2>&1 \
  && ok "first decayed write (signal=0.8) accepted" \
  || bad "first decayed write (signal=0.8) rejected"

GET1=$("$AGENTOS_BIN" user --json facts get --fact-type category_interest 2>&1)
SCORE1=$(json_line "$GET1" | jq -r '.facts[] | select(.fact_key=="electronics") | .value.score')
assert_close "$SCORE1" "0.8" 0.0001 "score after first write == raw signal (no prior score to blend)"

"$AGENTOS_BIN" user --json facts record \
  --fact-type category_interest --fact-key electronics \
  --payload '{}' --signal 0.2 >/dev/null 2>&1 \
  && ok "second decayed write (signal=0.2) accepted" \
  || bad "second decayed write (signal=0.2) rejected"

GET2=$("$AGENTOS_BIN" user --json facts get --fact-type category_interest 2>&1)
SCORE2=$(json_line "$GET2" | jq -r '.facts[] | select(.fact_key=="electronics") | .value.score')
assert_close "$SCORE2" "0.65" 0.0001 "score after second write matches real ema(alpha=0.25) formula"

# ---------------------------------------------------------------------------
# 3. user facts events — local/debug direct-db read, no RPC involved.
#    Verifies the write path (RPC, above) and this local read path agree on
#    the same underlying rows. Needs the access key's `id` (first 8 hex
#    chars of its hash) to pass as --user-id; there is no --json output on
#    `key list` to script against, so pull it straight from agentos.db —
#    this is test bookkeeping to identify which row we're checking, not a
#    bypass of the thing under test.
# ---------------------------------------------------------------------------

log "== user.facts.events — local/debug command (bypasses RPC entirely) =="

require_sqlite3
USER_ID=$(sqlite3 "$AGENTOS_HOME/agentos.db" \
  "SELECT id FROM access_keys ORDER BY created_at ASC LIMIT 1;")
if [[ -n "$USER_ID" ]]; then
  ok "resolved user_id=$USER_ID from access_keys (test bookkeeping only)"
else
  bad "could not resolve a user_id from access_keys"
  die "cannot continue user.facts.events tests without a user_id"
fi

CR_EVENTS=$("$AGENTOS_BIN" user --json facts events \
  --user-id "$USER_ID" --fact-type card_reaction 2>&1)
CR_JSON=$(json_line "$CR_EVENTS")
CR_COUNT=$(echo "$CR_JSON" | jq -r '.events | length')
assert_eq "$CR_COUNT" "1" "facts events shows exactly 1 card_reaction event"
CR_NOTE=$(echo "$CR_JSON" | jq -r '.events[0].payload.note // empty')
assert_eq "$CR_NOTE" "clicked" "the recorded event's payload.note round-trips correctly"

CI_EVENTS=$("$AGENTOS_BIN" user --json facts events \
  --user-id "$USER_ID" --fact-type category_interest 2>&1)
CI_JSON=$(json_line "$CI_EVENTS")
CI_COUNT=$(echo "$CI_JSON" | jq -r '.events | length')
assert_eq "$CI_COUNT" "2" "facts events shows both category_interest writes (both decayed writes above)"

# NOTE (worth flagging, not a test failure): the raw `signal` value used to
# compute a decayed write's new score is NOT itself persisted anywhere in
# user_fact_events — only whatever the caller separately put in --payload
# is stored (we sent '{}' for both category_interest writes above, per the
# original test design). So the audit trail currently cannot answer "what
# signal produced this score change" unless the caller redundantly echoes
# the signal into payload themselves. Confirm with Roy whether this is
# intentional (payload is caller-defined free-form context, signal is a
# separate scoring-only input) or a gap worth closing.
CI_PAYLOAD_0=$(echo "$CI_JSON" | jq -c '.events[0].payload')
log "category_interest events' payload (both should be '{}' as sent — signal itself isn't in the audit row): $CI_PAYLOAD_0"

# ---------------------------------------------------------------------------
# 4. subject.* — full register -> populate -> next -> complete -> progress
#    -> memory upsert -> memory query cycle. No suite involved: unit_type
#    is a plain client-supplied "file"|"line" string validated server-side,
#    not read from any suite manifest.
# ---------------------------------------------------------------------------

log "== subject.* — register / units / memory cycle =="

REG_OUT=$("$AGENTOS_BIN" subject --json register \
  --subject-type codebase --unit-type file --title "functest repo" 2>&1)
SUBJECT_ID=$(json_line "$REG_OUT" | jq -r '.subject_id // empty')
if [[ -n "$SUBJECT_ID" ]]; then
  ok "subject register returned subject_id=$SUBJECT_ID"
else
  bad "subject register did not return a subject_id"
  echo "$REG_OUT"
  die "cannot continue subject.* tests without a subject_id"
fi

POP_OUT=$("$AGENTOS_BIN" subject --json units populate \
  --subject-id "$SUBJECT_ID" --unit a.py --unit b.py --unit c.py 2>&1)
INSERTED=$(json_line "$POP_OUT" | jq -r '.inserted // -1')
assert_eq "$INSERTED" "3" "subject units populate inserted 3 new unit_refs"

# idempotency: populating the same 3 refs again should report 0 inserted,
# 3 already_known (Database method is documented as idempotent on unit_ref)
POP_OUT2=$("$AGENTOS_BIN" subject --json units populate \
  --subject-id "$SUBJECT_ID" --unit a.py --unit b.py --unit c.py 2>&1)
INSERTED2=$(json_line "$POP_OUT2" | jq -r '.inserted // -1')
ALREADY2=$(json_line "$POP_OUT2" | jq -r '.already_known // -1')
assert_eq "$INSERTED2" "0" "re-populating the same unit_refs inserts 0 (idempotent)"
assert_eq "$ALREADY2" "3" "re-populating the same unit_refs reports 3 already_known"

NEXT_OUT=$("$AGENTOS_BIN" subject --json units next \
  --subject-id "$SUBJECT_ID" --limit 50 2>&1)
NEXT_COUNT=$(json_line "$NEXT_OUT" | jq -r '.units | length')
assert_eq "$NEXT_COUNT" "3" "subject units next returns all 3 pending units before any are completed"

FIRST_INDEX=$(json_line "$NEXT_OUT" | jq -r '.units[0].unit_index')

COMPLETE_OUT=$("$AGENTOS_BIN" subject --json units complete \
  --subject-id "$SUBJECT_ID" --index "$FIRST_INDEX" 2>&1)
COMPLETE_OK=$(json_line "$COMPLETE_OUT" | jq -r '.ok // false')
assert_eq "$COMPLETE_OK" "true" "subject units complete for index=$FIRST_INDEX succeeded"

PROG_OUT=$("$AGENTOS_BIN" subject --json units progress \
  --subject-id "$SUBJECT_ID" 2>&1)
PROG_TOTAL=$(json_line "$PROG_OUT" | jq -r '.total')
PROG_DONE=$(json_line "$PROG_OUT" | jq -r '.completed')
assert_eq "$PROG_TOTAL" "3" "progress.total == 3"
assert_eq "$PROG_DONE" "1" "progress.completed == 1 after completing one unit"

UPSERT_OUT=$("$AGENTOS_BIN" subject --json memory upsert \
  --subject-id "$SUBJECT_ID" --entry-key "summary" \
  --entry-value '{"text":"first pass notes"}' \
  --source-job-id "functest-manual-1" 2>&1)
REVISION1=$(json_line "$UPSERT_OUT" | jq -r '.revision // -1')
if [[ "$REVISION1" != "-1" ]]; then
  ok "subject memory upsert (insert) returned revision=$REVISION1"
else
  bad "subject memory upsert (insert) failed"
  echo "$UPSERT_OUT"
fi

# upsert again on the same entry_key — revision should advance
UPSERT_OUT2=$("$AGENTOS_BIN" subject --json memory upsert \
  --subject-id "$SUBJECT_ID" --entry-key "summary" \
  --entry-value '{"text":"revised notes"}' \
  --source-job-id "functest-manual-1" 2>&1)
REVISION2=$(json_line "$UPSERT_OUT2" | jq -r '.revision // -1')
if [[ "$REVISION2" -gt "$REVISION1" ]] 2>/dev/null; then
  ok "second upsert on the same entry_key advanced revision ($REVISION1 -> $REVISION2)"
else
  bad "second upsert on the same entry_key did not advance revision (got $REVISION1 -> $REVISION2)"
fi

QUERY_OUT=$("$AGENTOS_BIN" subject --json memory query \
  --subject-id "$SUBJECT_ID" 2>&1)
QUERY_TEXT=$(json_line "$QUERY_OUT" | jq -r '.entries[] | select(.entry_key=="summary") | .entry_value.text')
assert_eq "$QUERY_TEXT" "revised notes" "subject memory query returns the latest (revised) value, not the first"

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------

echo
log "== summary =="
echo "PASS: $PASS   FAIL: $FAIL"
if [[ "$FAIL" -eq 0 ]]; then
  log "all round-1 positive-path checks green — safe to move to round 2 (negative tests)"
  exit 0
else
  log "one or more checks failed — see [FAIL] lines above before writing round 2"
  exit 1
fi
