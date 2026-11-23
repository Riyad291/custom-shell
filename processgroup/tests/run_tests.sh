#!/bin/bash
# test_processgroup.sh - automated test cases for processgroup
#
# Usage:  ./test_processgroup.sh [path/to/processgroup]
# Builds nothing; compile first:  gcc -Wall -Wextra -O2 -o processgroup processgroup.c
# Every test starts its own batch and cleans it up, so the script is safe to
# run repeatedly. Exit status is 0 only if every check passes.

PG_BIN=${1:-./processgroup}
TMP=$(mktemp -d)
PASS=0
FAIL=0
GROUPS_STARTED=""

if [ ! -x "$PG_BIN" ]; then
    echo "error: $PG_BIN not found or not executable (compile it first)" >&2
    exit 2
fi

cleanup() {
    for g in $GROUPS_STARTED; do
        kill -KILL -- "-$g" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT

ok()   { PASS=$((PASS + 1)); printf '  PASS  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL  %s\n' "$1"; [ -n "$2" ] && printf '        %s\n' "$2"; }

# check "description" expected_exit_code command...   (output kept in $OUT)
check_rc() {
    local desc=$1 want=$2
    shift 2
    OUT=$("$@" 2>&1)
    local rc=$?
    if [ "$rc" -eq "$want" ]; then ok "$desc"; else bad "$desc" "exit $rc, wanted $want: $OUT"; fi
}

expect_out() {   # expect_out "description" "regex"   (checks $OUT)
    if printf '%s' "$OUT" | grep -Eq "$2"; then ok "$1"; else bad "$1" "output was: $OUT"; fi
}

# live (non-zombie) processes in a group, via ps
live_count() { ps -eo pgid=,stat= | awk -v g="$1" '$1==g && $2 !~ /^Z/ {n++} END{print n+0}'; }
stopped_count() { ps -eo pgid=,stat= | awk -v g="$1" '$1==g && $2 ~ /^T/ {n++} END{print n+0}'; }

new_group() {    # new_group N cmd...  -> sets GID
    local n=$1
    shift
    GID=$("$PG_BIN" start -qd -n "$n" -- "$@")
    GROUPS_STARTED="$GROUPS_STARTED $GID"
}

wait_for() {     # wait_for "shell test" seconds
    local i
    for ((i = 0; i < $2 * 10; i++)); do
        if eval "$1"; then return 0; fi
        sleep 0.1
    done
    return 1
}

echo "processgroup tests (binary: $PG_BIN)"
echo

echo "[usage and argument errors]"
check_rc "no arguments -> usage, exit 2" 2 "$PG_BIN"
check_rc "help -> exit 0" 0 "$PG_BIN" help
expect_out "help lists the commands" "terminate"
check_rc "unknown subcommand -> exit 2" 2 "$PG_BIN" frobnicate
check_rc "start without command -> exit 2" 2 "$PG_BIN" start
check_rc "start -n 0 rejected -> exit 2" 2 "$PG_BIN" start -n 0 -- sleep 1
check_rc "start -n abc rejected -> exit 2" 2 "$PG_BIN" start -n abc -- sleep 1
check_rc "start -n 5000 rejected (over limit) -> exit 2" 2 "$PG_BIN" start -n 5000 -- sleep 1
check_rc "start unknown option -> exit 2" 2 "$PG_BIN" start -z -- sleep 1
check_rc "start -n with no value -> exit 2" 2 "$PG_BIN" start -n
printf 'sleep 30\n' > "$TMP/jobs.txt"
check_rc "start -f together with -n rejected -> exit 2" 2 "$PG_BIN" start -f "$TMP/jobs.txt" -n 2
check_rc "status without PGID -> exit 2" 2 "$PG_BIN" status
check_rc "status with non-numeric PGID -> exit 2" 2 "$PG_BIN" status abc
check_rc "signal with one argument -> exit 2" 2 "$PG_BIN" signal 12345
check_rc "signal with unknown signal name -> exit 2" 2 "$PG_BIN" signal 12345 BOGUS
check_rc "monitor -i 0 rejected -> exit 2" 2 "$PG_BIN" monitor -i 0 12345
check_rc "terminate -w -1 rejected -> exit 2" 2 "$PG_BIN" terminate -w -1 12345
echo

echo "[start]"
check_rc "start of a missing command fails, exit 1" 1 "$PG_BIN" start nosuchcommand_xyz
expect_out "missing command is named in the error" "nosuchcommand_xyz"
check_rc "start of a directory is rejected, exit 1" 1 "$PG_BIN" start /tmp
OUT=$("$PG_BIN" start -d -n 3 -- sleep 30)
GID=$(printf '%s\n' "$OUT" | sed -n 's/.*process group \([0-9]*\)\..*/\1/p')
GROUPS_STARTED="$GROUPS_STARTED $GID"
expect_out "start reports the process count" "Started 3 process"
if wait_for "[ \$(live_count $GID) -eq 3 ]" 3; then ok "three processes really exist in group $GID"; else bad "three processes in the group" "live=$(live_count "$GID")"; fi
PARENT_PGRP=$(ps -o pgid= -p $$ | tr -d ' ')
if [ "$GID" != "$PARENT_PGRP" ]; then ok "group is separate from the caller's group"; else bad "group is separate"; fi
new_group 2 sleep 30
if [[ "$GID" =~ ^[0-9]+$ ]]; then ok "start -q prints only a numeric PGID"; else bad "start -q output" "$GID"; fi
"$PG_BIN" terminate -w 2 "$GID" > /dev/null 2>&1
echo

echo "[status]"
new_group 3 sleep 30
wait_for "[ \$(live_count $GID) -eq 3 ]" 3
check_rc "status of a live group -> exit 0" 0 "$PG_BIN" status "$GID"
expect_out "status shows the sleep command" "sleep 30"
expect_out "status shows a total line" "total: 3 process"
check_rc "status of a group that does not exist -> exit 1" 1 "$PG_BIN" status 999999
expect_out "missing group is explained" "no processes found"
echo

echo "[signal]"
check_rc "signal 0 (existence test) -> exit 0" 0 "$PG_BIN" signal "$GID" 0
check_rc "signal STOP -> exit 0" 0 "$PG_BIN" signal "$GID" STOP
if wait_for "[ \$(stopped_count $GID) -eq 3 ]" 3; then ok "all 3 processes stopped by one signal"; else bad "all processes stopped" "stopped=$(stopped_count "$GID")"; fi
"$PG_BIN" status "$GID" > "$TMP/st.txt" 2>&1
if grep -q "stopped" "$TMP/st.txt"; then ok "status reports the stopped state"; else bad "status shows stopped" "$(cat "$TMP/st.txt")"; fi
check_rc "signal SIGCONT (with prefix, lower case) -> exit 0" 0 "$PG_BIN" signal "$GID" sigcont
if wait_for "[ \$(stopped_count $GID) -eq 0 ]" 3; then ok "all 3 processes continued"; else bad "all processes continued"; fi
check_rc "signal by number (0) -> exit 0" 0 "$PG_BIN" signal "$GID" 0
check_rc "signal to a group that does not exist -> exit 1" 1 "$PG_BIN" signal 999999 TERM
echo

echo "[safety checks]"
check_rc "refuses group 1" 1 "$PG_BIN" signal 1 TERM
check_rc "refuses terminate of group 1" 1 "$PG_BIN" terminate 1
OWN=$(ps -o pgid= -p $$ | tr -d ' ')
check_rc "refuses the caller's own group" 1 "$PG_BIN" signal "$OWN" TERM
expect_out "refusal explains why" "refusing"
echo

echo "[monitor]"
check_rc "monitor -c 2 prints two samples and exits 0" 0 "$PG_BIN" monitor -c 2 -i 0.2 "$GID"
expect_out "monitor shows a %CPU column" "%CPU"
expect_out "monitor numbers its samples" "sample 2"
check_rc "monitor of a missing group -> exit 1" 1 "$PG_BIN" monitor -c 1 999999
echo

echo "[terminate]"
check_rc "terminate a normal group -> exit 0" 0 "$PG_BIN" terminate "$GID"
expect_out "terminate reports the outcome" "3 exited on SIGTERM, 0 needed SIGKILL"
if [ "$(live_count "$GID")" -eq 0 ]; then ok "no live processes remain"; else bad "no live processes remain"; fi
check_rc "terminate of an already-gone group -> exit 1" 1 "$PG_BIN" terminate "$GID"

# A stopped group must still die: terminate sends SIGCONT after SIGTERM.
new_group 2 sleep 30
wait_for "[ \$(live_count $GID) -eq 2 ]" 3
"$PG_BIN" signal "$GID" STOP > /dev/null
wait_for "[ \$(stopped_count $GID) -eq 2 ]" 3
check_rc "terminate works on a STOPPED group" 0 "$PG_BIN" terminate -w 3 "$GID"
if [ "$(live_count "$GID")" -eq 0 ]; then ok "stopped processes were terminated"; else bad "stopped processes terminated" "live=$(live_count "$GID")"; fi

# A process that ignores SIGTERM must be escalated to SIGKILL.
new_group 2 bash -c 'trap "" TERM; while true; do sleep 1; done'
wait_for "[ \$(live_count $GID) -ge 2 ]" 3
sleep 0.5
check_rc "terminate escalates to SIGKILL for SIGTERM-ignoring jobs" 0 "$PG_BIN" terminate -w 1 "$GID"
expect_out "report says SIGKILL was needed" "needed SIGKILL"
if [ "$(live_count "$GID")" -eq 0 ]; then ok "stubborn processes are gone"; else bad "stubborn processes are gone" "live=$(live_count "$GID")"; fi
echo

echo "[batch file]"
cat > "$TMP/batch.txt" <<'EOF'
# three different commands in one group
sleep 30

sleep 31
   sleep   32
EOF
OUT=$("$PG_BIN" start -d -f "$TMP/batch.txt")
GID=$(printf '%s\n' "$OUT" | sed -n 's/.*process group \([0-9]*\)\..*/\1/p')
GROUPS_STARTED="$GROUPS_STARTED $GID"
expect_out "-f starts one process per command line (comments and blanks skipped)" "Started 3 process"
wait_for "[ \$(live_count $GID) -eq 3 ]" 3
"$PG_BIN" status "$GID" > "$TMP/st.txt" 2>&1
if grep -q "sleep 30" "$TMP/st.txt" && grep -q "sleep 31" "$TMP/st.txt" && grep -q "sleep 32" "$TMP/st.txt"; then
    ok "status lists each distinct command"
else
    bad "status lists each distinct command" "$(cat "$TMP/st.txt")"
fi
"$PG_BIN" terminate -w 2 "$GID" > /dev/null 2>&1
check_rc "-f with a missing file -> exit 1" 1 "$PG_BIN" start -f "$TMP/does_not_exist.txt"
printf '# only a comment\n\n' > "$TMP/empty.txt"
check_rc "-f with no commands -> exit 1" 1 "$PG_BIN" start -f "$TMP/empty.txt"
printf 'sleep 30\nnosuchcommand_xyz\n' > "$TMP/badbatch.txt"
check_rc "-f batch with one bad command starts nothing -> exit 1" 1 "$PG_BIN" start -d -f "$TMP/badbatch.txt"
echo

echo "[monitor lifecycle]"
new_group 2 sleep 1
wait_for "[ \$(live_count $GID) -eq 2 ]" 2
OUT=$(timeout 10 "$PG_BIN" monitor -i 0.3 "$GID" 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then ok "monitor exits by itself when the group finishes"; else bad "monitor exits by itself" "exit $rc"; fi
expect_out "monitor announces the group has ended" "no more processes"

new_group 2 sleep 30
wait_for "[ \$(live_count $GID) -eq 2 ]" 3
"$PG_BIN" monitor -i 0.2 "$GID" > "$TMP/mon.txt" 2>&1 &
MON=$!
sleep 1
kill -INT "$MON"
wait "$MON"
rc=$?
if [ "$rc" -eq 0 ]; then ok "Ctrl+C stops monitor cleanly (exit 0)"; else bad "SIGINT stops monitor cleanly" "exit $rc"; fi
if grep -q "left running" "$TMP/mon.txt"; then ok "monitor says the group was left running"; else bad "monitor leaves group running" "$(tail -3 "$TMP/mon.txt")"; fi
if [ "$(live_count "$GID")" -eq 2 ]; then ok "group still alive after monitor was interrupted"; else bad "group still alive" "live=$(live_count "$GID")"; fi
"$PG_BIN" terminate -w 2 "$GID" > /dev/null 2>&1
echo

echo "[embedded in a shell (shell_sim)]"
SIM="$(dirname "$0")/shell_sim"
if [ -x "$SIM" ]; then
    OUT=$(printf 'processgroup start -qd -n 2 -- sleep 30\nexit\n' | "$SIM" 2>&1)
    G=$(printf '%s\n' "$OUT" | grep -Eo '[0-9]{2,}' | head -1)
    GROUPS_STARTED="$GROUPS_STARTED $G"
    expect_out "processgroup_main() works when called from another program" "exit status 0"
    OUT=$(printf 'processgroup status %s\nprocessgroup status abc\nexit\n' "$G" | "$SIM" 2>&1)
    expect_out "repeated calls in one process keep working (getopt reset)" "exit status 2"
    "$PG_BIN" terminate -w 2 "$G" > /dev/null 2>&1
else
    echo "  skip  (build tests/shell_sim with 'make tests/shell_sim' to run these)"
fi
echo

echo "Result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
