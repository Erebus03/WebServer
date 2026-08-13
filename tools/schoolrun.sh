#!/bin/bash
# Run every tester we have and write ONE small report you can paste back.
#
#   ./tools/schoolrun.sh                       # everything it can find
#   ./tools/schoolrun.sh /path/to/web-serv-Tester
#
# Output: report.txt in the repo root, a few KB. Raw logs land in .testlogs/
# and are capped, so nothing can run away to gigabytes again.
set -u
cd "$(cd "$(dirname "$0")/.." && pwd)" || exit 1
REPO="$PWD"
AZZ="${1:-}"
LOGS="$REPO/.testlogs"; rm -rf "$LOGS"; mkdir -p "$LOGS"
R="$REPO/report.txt"; : > "$R"
CAP=20000000                       # 20 MB per raw log, hard ceiling

say(){ printf '%s\n' "$*" | tee -a "$R"; }
strip(){ sed 's/\x1b\[[0-9;]*m//g' | tr '\r' '\n'; }

# Kill whatever is listening on a port, by pid. Never pkill -f: the pattern
# matches the shell running this script.
freeport(){
    local p pid
    for p in "$@"; do
        pid=$(ss -ltnp 2>/dev/null | grep ":$p " | grep -oP 'pid=\K[0-9]+' | head -1)
        [ -n "${pid:-}" ] && { kill "$pid" 2>/dev/null; sleep 1; kill -9 "$pid" 2>/dev/null; }
    done
    sleep 1
}

say "webserv test report"
say "date    $(date '+%Y-%m-%d %H:%M')"
say "host    $(hostname)"
say "commit  $(git rev-parse --short HEAD 2>/dev/null) on $(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
say "user    $(id -un)$([ "$(id -u)" = 0 ] && echo '  *** ROOT: chmod 000 does not stop root, every 403 test will report 200 ***')"
say ""

# ── build ────────────────────────────────────────────────────────────────
say "== build =="
if make re > "$LOGS/build.log" 2>&1; then
    say "ok   $(grep -ciE ': warning' "$LOGS/build.log") warnings"
else
    say "FAILED -- everything below is meaningless"
    grep -iE ': (error|warning)' "$LOGS/build.log" | head -20 | sed 's/^/  /' | tee -a "$R"
    exit 1
fi

# ── unit suites ──────────────────────────────────────────────────────────
say ""; say "== make test (unit suites) =="
if make test > "$LOGS/unit.log" 2>&1; then
    say "ok   $(grep -c 'passed' "$LOGS/unit.log") suites reported passing"
else
    say "FAILED"; grep -iE 'fail|assert' "$LOGS/unit.log" | head -15 | sed 's/^/  /' | tee -a "$R"
fi

# ── our own suite ────────────────────────────────────────────────────────
say ""; say "== make fulltest (ours, 80 checks) =="
freeport 8080
if timeout 600 make fulltest > "$LOGS/fulltest.log" 2>&1; then :; fi
strip < "$LOGS/fulltest.log" | grep -E '[0-9]+/[0-9]+ passed' | tail -1 | sed 's/^ *//' | tee -a "$R"
strip < "$LOGS/fulltest.log" | grep -iE '^\s*(FAIL|✘)' | head -20 | sed 's/^ */  /' | tee -a "$R"

# ── school tester: the bar ───────────────────────────────────────────────
say ""; say "== school tester (config/tester.conf, port 8080) =="
if [ ! -x ./tester ]; then
    say "skipped -- ./tester not here (it is not in git, carry it yourself)"
elif [ ! -x YoupiBanane/cgi_tester ]; then
    say "skipped -- YoupiBanane/cgi_tester missing; every .bla test would 502"
else
    freeport 8080
    ( exec ./webserv config/tester.conf ) > "$LOGS/school-srv.log" 2>&1 &
    SRV=$!; sleep 2
    if kill -0 $SRV 2>/dev/null; then
        # It waits on "press enter to continue"; with no stdin it just sits
        # there until the timeout and reports nothing.
        printf '\n\n' | timeout 300 ./tester http://127.0.0.1:8080 > "$LOGS/school.log" 2>&1
        N=$(grep -c '^Test ' "$LOGS/school.log")
        if grep -qi 'FATAL' "$LOGS/school.log"; then
            say "$N tests run, FAILED on #$N"
            strip < "$LOGS/school.log" | grep -i 'FATAL' | head -2 | sed 's/^/  /' | tee -a "$R"
            say "  last test attempted:"
            grep '^Test ' "$LOGS/school.log" | tail -1 | cut -c1-88 | sed 's/^/    /' | tee -a "$R"
        elif [ "$N" -gt 0 ]; then
            say "$N/$N passed"
        else
            say "no tests ran -- check .testlogs/school.log"
            strip < "$LOGS/school.log" | grep -vE '^\s*$' | tail -4 | sed 's/^/  /' | tee -a "$R"
        fi
    else
        say "server did not start:"; head -5 "$LOGS/school-srv.log" | sed 's/^/  /' | tee -a "$R"
    fi
    kill $SRV 2>/dev/null; sleep 1; kill -9 $SRV 2>/dev/null
fi

# ── third-party ──────────────────────────────────────────────────────────
say ""; say "== EngineX third-party tester =="
[ -z "$AZZ" ] && for c in "$REPO/../web-serv-Tester" "$HOME/web-serv-Tester"; do
    [ -d "$c" ] && { AZZ="$c"; break; }
done
if [ -z "$AZZ" ] || [ ! -d "$AZZ" ]; then
    say "skipped -- pass its path: ./tools/schoolrun.sh /path/to/web-serv-Tester"
else
    AZZ="$(cd "$AZZ" && pwd)"
    cp -f "$REPO/webserv" "$AZZ/webserv"
    bash "$REPO/tools/enginex_fixtures.sh" "$AZZ/EngineX/www" > "$LOGS/fixtures.log" 2>&1 \
        && say "fixtures seeded" || { say "fixture setup FAILED"; cat "$LOGS/fixtures.log" | sed 's/^/  /' | tee -a "$R"; }
    [ -x "$AZZ/servTester.out" ] || ( cd "$AZZ" && make >/dev/null 2>&1 )
    if [ -x "$AZZ/servTester.out" ]; then
        freeport 1025 1026 1027
        ( cd "$AZZ" && exec ./webserv webserv-enginex.conf ) > "$LOGS/azz-srv.log" 2>&1 &
        SRV=$!; sleep 2
        if kill -0 $SRV 2>/dev/null; then
            # head -c caps it: when the cap is hit, SIGPIPE kills the tester.
            # That is what stops the menu spinning forever once stdin ends.
            ( cd "$AZZ" && printf '1-13\n14\n' | timeout 900 ./servTester.out 2>&1 ) \
                | head -c $CAP > "$LOGS/azz-raw.log"
            strip < "$LOGS/azz-raw.log" | grep -E '✔ PASS|✘ FAIL' > "$LOGS/azz.log"
            P=$(grep -c '✔ PASS' "$LOGS/azz.log"); F=$(grep -c '✘ FAIL' "$LOGS/azz.log")
            say "$P passed / $((P+F)) total"
            say "failures:"
            grep '✘ FAIL' "$LOGS/azz.log" | sed 's/.*✘ FAIL *//' | cut -c1-88 | sed 's/^/  /' | tee -a "$R"
        else
            say "server did not start:"; head -5 "$LOGS/azz-srv.log" | sed 's/^/  /' | tee -a "$R"
        fi
        kill $SRV 2>/dev/null; sleep 1; kill -9 $SRV 2>/dev/null
    else
        say "skipped -- servTester.out did not build"
    fi
fi

freeport 8080 1025 1026 1027
say ""
say "== end =="
say "raw logs in .testlogs/ (capped at $((CAP/1000000)) MB each)"
printf '\nreport written to %s (%s bytes) -- paste that\n' "$R" "$(stat -c%s "$R")"
