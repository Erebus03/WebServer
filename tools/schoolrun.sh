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
DEADLINE=900   # hard stop for the EngineX tester
QUIET=90       # ...or stop once no new result line has appeared for this long

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
        # It stops at "press enter to continue" FIVE times, not once (counted
        # from a real run). Two newlines left it depending on how its reader
        # behaves at EOF. Ten is bounded and covers every prompt -- never use
        # `yes ''` here, that is what produced 8.7M lines once.
        printf '\n%.0s' $(seq 10) | timeout 300 ./tester http://127.0.0.1:8080 > "$LOGS/school.log" 2>&1
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
            # Do NOT cap by bytes: the tester calls signal(SIGPIPE, SIG_IGN) as
            # the first statement in main(), so closing its stdout cannot stop
            # it -- it ran the full 900s after the cap fired. Instead filter as
            # it streams (nothing large is ever written) and stop it by pid when
            # the results go quiet. pgrep -x matches the binary only; pkill -f
            # would match this script.
            ( cd "$AZZ" && printf '1-13\n14\n' | ./servTester.out 2>&1 ) \
                | sed -u 's/\x1b\[[0-9;]*m//g' \
                | grep -a --line-buffered -E '✔ PASS|✘ FAIL' > "$LOGS/azz.log" &
            PIPE=$!
            start=$SECONDS; last_size=0; last_change=$SECONDS
            while kill -0 $PIPE 2>/dev/null; do
                sleep 5
                size=$(stat -c%s "$LOGS/azz.log" 2>/dev/null || echo 0)
                [ "$size" != "$last_size" ] && { last_size=$size; last_change=$SECONDS; }
                if { [ "$size" -gt 0 ] && [ $((SECONDS-last_change)) -ge $QUIET ]; } \
                   || [ $((SECONDS-start)) -ge $DEADLINE ]; then
                    t=$(pgrep -x servTester.out | head -1)
                    [ -n "${t:-}" ] && kill -9 "$t" 2>/dev/null
                    break
                fi
            done
            wait $PIPE 2>/dev/null
            t=$(pgrep -x servTester.out | head -1); [ -n "${t:-}" ] && kill -9 "$t" 2>/dev/null
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
say "logs in .testlogs/ (results are filtered as they stream, so nothing grows)"
printf '\nreport written to %s (%s bytes) -- paste that\n' "$R" "$(stat -c%s "$R")"
