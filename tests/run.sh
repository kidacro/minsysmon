#!/usr/bin/env bash
# Run sysmon's regression tests. All binaries are built and executed in a
# temporary directory so this works on mounts that disallow execution.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sysmon-test.XXXXXX")
src="$tmp/src"
bin="$tmp/bin/sysmon"
stage="$tmp/stage"
session=""

cleanup() {
    if [[ -n $session ]]; then
        tmux kill-session -t "$session" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT

mkdir -p "$src" "$tmp/bin" "$stage"
cp "$root/sysmon.c" "$root/Makefile" "$src/"

pass=0
failed=0

ok() {
    printf 'ok   - %s\n' "$1"
    pass=$((pass + 1))
}

not_ok() {
    printf 'FAIL - %s\n' "$1" >&2
    failed=$((failed + 1))
}

CC=${CC:-cc}
LDLIBS=${LDLIBS:--lncurses -ltinfo}

# Strict build: catches missing POSIX feature macros and old-style prototypes.
build_log="$tmp/strict-build.log"
# shellcheck disable=SC2086
if $CC -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
        "$src/sysmon.c" -o "$bin" $LDLIBS >"$build_log" 2>&1; then
    ok "strict C11 build with -Werror"
else
    not_ok "strict C11 build with -Werror"
    cat "$build_log" >&2
fi

# Makefile build and staged install, both in the temporary directory.
if make -C "$src" >"$tmp/make.log" 2>&1; then
    ok "make build"
else
    not_ok "make build"
    cat "$tmp/make.log" >&2
fi

if make -C "$src" install DESTDIR="$stage" PREFIX=/usr \
        >"$tmp/install.log" 2>&1 && [[ -f $stage/usr/bin/sysmon ]]; then
    ok "make install honors DESTDIR/PREFIX"
else
    not_ok "make install honors DESTDIR/PREFIX"
    cat "$tmp/install.log" >&2
fi

out="$tmp/out"
err="$tmp/err"

# Success cases: help/version must go to stdout with an empty stderr.
if "$bin" -h >"$out" 2>"$err" && grep -Fq 'Usage:' "$out" &&
        grep -Fq -- '-V, --version' "$out" && [[ ! -s $err ]]; then
    ok "-h prints help to stdout"
else
    not_ok "-h prints help to stdout"
fi

if "$bin" --version >"$out" 2>"$err" &&
        grep -Fxq 'sysmon 0.1.0' "$out" && [[ ! -s $err ]]; then
    ok "--version prints the version"
else
    not_ok "--version prints the version"
fi

if "$bin" -V >"$out" 2>"$err" && grep -Fxq 'sysmon 0.1.0' "$out"; then
    ok "-V prints the version"
else
    not_ok "-V prints the version"
fi

if "$bin" -h -n 0 >"$out" 2>"$err" && grep -Fq 'Usage:' "$out"; then
    ok "help short-circuits later invalid options"
else
    not_ok "help short-circuits later invalid options"
fi

# Failure cases: non-zero exit, usage on stderr, nothing on stdout.
expect_fail() {
    local desc=$1
    shift
    local status=0
    if "$bin" "$@" >"$out" 2>"$err"; then
        status=0
    else
        status=$?
    fi
    if [[ $status -ne 0 ]] && grep -Fq 'Usage:' "$err" && [[ ! -s $out ]]; then
        ok "$desc"
    else
        not_ok "$desc (exit $status)"
    fi
}

expect_fail "rejects -n 0" -n 0
expect_fail "rejects -n 21" -n 21
expect_fail "rejects -t 0" -t 0
expect_fail "rejects -t 101" -t 101
expect_fail "rejects -d 0" -d 0
expect_fail "rejects non-numeric delay" -d abc
expect_fail "rejects unknown option" -x
expect_fail "rejects missing -l value" -l
expect_fail "rejects missing -s value" -s
expect_fail "rejects -l together with -s" -l /etc/hostname -s true

# TUI smoke tests. tmux supplies a pty for ncurses; skip gracefully if absent.
if command -v tmux >/dev/null 2>&1; then
    session="sysmon-test-$$"
    tmux kill-session -t "$session" 2>/dev/null || true
    tui_status="$tmp/tui.status"

    cat >"$tmp/tui.sh" <<EOF
#!/bin/sh
"$bin" -d 1 -n 3 -s 'printf "alpha\\nbeta\\n"'
printf '%s\n' "\$?" > "$tui_status"
sleep 30
EOF

    if tmux new-session -d -s "$session" -x 100 -y 24 "sh '$tmp/tui.sh'" \
            >/dev/null 2>&1; then
        sleep 2
        pane=$(tmux capture-pane -p -t "$session:0.0" 2>/dev/null || true)
        if grep -Fq 'CPU:' <<<"$pane" &&
                grep -Fq 'Memory:' <<<"$pane" &&
                grep -Fq 'Swap:' <<<"$pane" &&
                grep -Fq 'Top 3 CPU Processes:' <<<"$pane" &&
                grep -Fq 'Command (' <<<"$pane"; then
            ok "TUI renders stats, processes and shell tail"
        else
            not_ok "TUI renders stats, processes and shell tail"
            printf '%s\n' "$pane" >&2
        fi

        tmux send-keys -t "$session:0.0" q >/dev/null 2>&1 || true
        for _ in 1 2 3; do
            [[ -f $tui_status ]] && break
            sleep 1
        done
        if [[ -f $tui_status ]] && [[ $(cat "$tui_status") == 0 ]]; then
            ok "q quits promptly"
        else
            not_ok "q quits promptly"
        fi
        tmux kill-session -t "$session" 2>/dev/null || true
        session=""
    else
        not_ok "tmux could not start the TUI test"
    fi

    session="sysmon-test-sig-$$"
    tmux kill-session -t "$session" 2>/dev/null || true
    signal_status="$tmp/tui-signal.status"
    cat >"$tmp/tui-signal.sh" <<EOF
#!/bin/sh
"$bin" -d 5
printf '%s\n' "\$?" > "$signal_status"
sleep 30
EOF

    if tmux new-session -d -s "$session" -x 100 -y 24 \
            "sh '$tmp/tui-signal.sh'" >/dev/null 2>&1; then
        sleep 1
        tmux send-keys -t "$session:0.0" C-c >/dev/null 2>&1 || true
        for _ in 1 2 3; do
            [[ -f $signal_status ]] && break
            sleep 1
        done
        if [[ -f $signal_status ]] && [[ $(cat "$signal_status") == 0 ]]; then
            ok "SIGINT exits cleanly"
        else
            not_ok "SIGINT exits cleanly"
        fi
        tmux kill-session -t "$session" 2>/dev/null || true
        session=""
    else
        not_ok "tmux could not start the signal test"
    fi
else
    printf 'skip - TUI tests (tmux not found)\n'
fi

printf '\n%d passed, %d failed\n' "$pass" "$failed"
(( failed == 0 ))
