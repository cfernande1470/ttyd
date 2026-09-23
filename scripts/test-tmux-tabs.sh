#!/bin/sh
# Functional tests for the --tmux-tabs mode.
#
# usage: scripts/test-tmux-tabs.sh [path-to-ttyd]
# requires: tmux, curl, and (for the ws test) node >= 21 (global WebSocket)
set -u

TTYD=${1:-build/ttyd}
HOST=127.0.0.1
TABS_PORT=17691
CLASSIC_PORT=17692
WORKDIR=$(mktemp -d)
SOCK="$WORKDIR/tmux.sock"
PASS=0
FAIL=0

say() {
    printf '%s\n' "$*"
}

check() {
    # check <desc> <expected> <actual>
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1))
        say "ok    - $1"
    else
        FAIL=$((FAIL + 1))
        say "FAIL  - $1 (expected [$2], got [$3])"
    fi
}

wait_for_http() {
    # wait_for_http <url>
    i=0
    while [ $i -lt 50 ]; do
        if curl -s -o /dev/null "$1"; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.2
    done
    return 1
}

cleanup() {
    [ -n "${TTYD_PID:-}" ] && kill "$TTYD_PID" 2>/dev/null
    [ -n "${CLASSIC_PID:-}" ] && kill "$CLASSIC_PID" 2>/dev/null
    tmux -S "$SOCK" kill-server 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

say "== tmux-tabs API tests =="
"$TTYD" --tmux-tabs --tmux-socket "$SOCK" -p "$TABS_PORT" -W bash -l > "$WORKDIR/ttyd.log" 2>&1 &
TTYD_PID=$!
if ! wait_for_http "http://$HOST:$TABS_PORT/"; then
    say "FAIL  - ttyd did not start (see $WORKDIR/ttyd.log)"
    exit 1
fi
B="http://$HOST:$TABS_PORT"

check "GET /api/tabs on empty server" "[]" "$(curl -s "$B/api/tabs")"

ID1=$(curl -s -X POST "$B/api/tabs" | sed -n 's/.*"id": *\([0-9]*\).*/\1/p')
check "POST /api/tabs creates a session" "1" "$ID1"
tmux -S "$SOCK" has-session -t "ttyd-$ID1" 2>/dev/null
check "tmux session ttyd-$ID1 exists" "0" "$?"

ID2=$(curl -s -X POST "$B/api/tabs" | sed -n 's/.*"id": *\([0-9]*\).*/\1/p')
check "second POST creates the next id" "2" "$ID2"

check "GET lists both sessions" "[{\"id\":1," "$(curl -s "$B/api/tabs" | cut -c1-9)"
check "DELETE invalid id is rejected" "400" "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$B/api/tabs/abc")"
check "DELETE unknown id is 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$B/api/tabs/999")"
check "DELETE $ID2 returns 200" "200" "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$B/api/tabs/$ID2")"

# explicit re-checks of the two session states
tmux -S "$SOCK" has-session -t "ttyd-$ID1" 2>/dev/null
check "session $ID1 survived the DELETE of $ID2" "0" "$?"
tmux -S "$SOCK" has-session -t "ttyd-$ID2" 2>/dev/null
check "session $ID2 was killed" "1" "$?"

say "== websocket attach tests =="
if command -v node >/dev/null 2>&1; then
    if node scripts/test-ws-tab.mjs "$TABS_PORT" "$ID1" "TABS-MARK"; then
        check "ws client attaches to tab and types into it" "0" "0"
    else
        check "ws client attaches to tab and types into it" "0" "1"
    fi
else
    say "skip  - node not found, ws attach test not run"
fi

say "== ttyd restart test (persistence) =="
kill "$TTYD_PID" 2>/dev/null
wait "$TTYD_PID" 2>/dev/null
tmux -S "$SOCK" has-session -t "ttyd-$ID1" 2>/dev/null
check "session survives ttyd shutdown" "0" "$?"

"$TTYD" --tmux-tabs --tmux-socket "$SOCK" -p "$TABS_PORT" -W bash -l > "$WORKDIR/ttyd2.log" 2>&1 &
TTYD_PID=$!
wait_for_http "http://$HOST:$TABS_PORT/"
TABLIST=$(curl -s "$B/api/tabs")
if echo "$TABLIST" | grep -q "\"id\": *$ID1"; then
    check "tabs reappear after ttyd restart" "0" "0"
else
    check "tabs reappear after ttyd restart (got $TABLIST)" "0" "1"
fi

say "== classic mode regression tests =="
"$TTYD" -p "$CLASSIC_PORT" -W bash -l > "$WORKDIR/classic.log" 2>&1 &
CLASSIC_PID=$!
wait_for_http "http://$HOST:$CLASSIC_PORT/"
CB="http://$HOST:$CLASSIC_PORT"
check "classic: /api/tabs is 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' "$CB/api/tabs")"
check "classic: / serves index.html" "200" "$(curl -s -o /dev/null -w '%{http_code}' "$CB/")"
check "classic: /token works" "0" "$(curl -s "$CB/token" | grep -c 'token' >/dev/null; echo $?)"

say ""
say "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
