#!/bin/sh
# Live-server stress harness.
#
#   ./tests/stress.sh              50 clients, 10s, raw speed
#   ./tests/stress.sh --valgrind   12 clients, 10s, under valgrind + fd tracking
#
# Run from the repo root: tests/local-test.conf's roots are relative to it.
# The pass conditions are the ones an evaluator actually checks — the server is
# still alive and answering afterwards, it leaked no descriptors, and it left no
# zombies. Throughput is deliberately not asserted on; it varies per machine and
# a threshold would only produce flaky failures.

set -e

PORT=9099
CONF=tests/local-test.conf
LOG=$(mktemp)
VG_LOG=$(mktemp)
MODE=$1

cleanup() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null || true
    rm -f "$LOG" "$VG_LOG"
}
trap cleanup EXIT INT TERM

if [ ! -x ./webserv ]; then
    echo "stress: ./webserv not built — run make first" >&2
    exit 1
fi

if [ "$MODE" = "--valgrind" ]; then
    command -v valgrind >/dev/null 2>&1 || {
        echo "stress: valgrind not installed" >&2; exit 1; }
    valgrind --track-fds=yes --leak-check=full --show-leak-kinds=all \
             --log-file="$VG_LOG" ./webserv "$CONF" >"$LOG" 2>&1 &
    SRV=$!
    CLIENTS=12
    SETTLE=4
else
    ./webserv "$CONF" >"$LOG" 2>&1 &
    SRV=$!
    CLIENTS=50
    SETTLE=1
fi

sleep "$SETTLE"
kill -0 "$SRV" 2>/dev/null || { echo "stress: server died at startup" >&2; cat "$LOG"; exit 1; }

FDS_BEFORE=$(ls /proc/$SRV/fd 2>/dev/null | wc -l)
python3 tests/stress.py 10 "$CLIENTS"

# --- pass conditions -------------------------------------------------------
kill -0 "$SRV" 2>/dev/null || { echo "FAIL: server crashed under load" >&2; exit 1; }

FDS_AFTER=$(ls /proc/$SRV/fd 2>/dev/null | wc -l)
echo "fds: $FDS_BEFORE before, $FDS_AFTER after"
[ "$FDS_BEFORE" -eq "$FDS_AFTER" ] || {
    echo "FAIL: leaked $((FDS_AFTER - FDS_BEFORE)) descriptor(s)" >&2; exit 1; }

ZOMBIES=$(ps -o stat= --ppid "$SRV" 2>/dev/null | grep -c Z || true)
[ "$ZOMBIES" -eq 0 ] || { echo "FAIL: $ZOMBIES zombie child(ren)" >&2; exit 1; }

CODE=$(curl -s -o /dev/null -m 10 -w '%{http_code}' "http://127.0.0.1:$PORT/")
[ "$CODE" = "200" ] || { echo "FAIL: server unresponsive after load ($CODE)" >&2; exit 1; }

# Exercise the shutdown path too — SIGINT must unwind the loop, not kill us
# mid-poll, or the valgrind numbers below are meaningless.
kill -INT "$SRV"
i=0
while kill -0 "$SRV" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
if kill -0 "$SRV" 2>/dev/null; then
    echo "FAIL: server ignored SIGINT" >&2; kill -9 "$SRV"; exit 1
fi
SRV=

if [ "$MODE" = "--valgrind" ]; then
    echo "--- valgrind"
    grep -E 'FILE DESCRIPTORS|definitely lost|indirectly lost|possibly lost|no leaks are possible|ERROR SUMMARY' "$VG_LOG"
    grep -q 'ERROR SUMMARY: 0 errors' "$VG_LOG" || { echo "FAIL: valgrind errors" >&2; exit 1; }
    grep -qE 'definitely lost: 0 bytes|no leaks are possible' "$VG_LOG" || {
        echo "FAIL: definite leak" >&2; exit 1; }
fi

echo "PASS: stress ($CLIENTS clients, no crash, no fd leak, no zombies, clean shutdown)"
