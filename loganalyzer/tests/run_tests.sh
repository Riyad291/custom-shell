#!/bin/bash
# run_tests.sh - test suite for loganalyzer
# Run from the project root:  ./tests/run_tests.sh

set -u

BIN=./loganalyzer
PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); printf '  PASS  %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$1"; }

check_status() {   # name expected_status command...
    local name=$1 want=$2; shift 2
    "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then ok "$name (exit $got)"
    else bad "$name (wanted exit $want, got $got)"; fi
}

check_contains() { # name needle command...
    local name=$1 needle=$2; shift 2
    if "$@" 2>&1 | grep -qF -- "$needle"; then ok "$name"
    else bad "$name (output did not contain \"$needle\")"; fi
}

echo "Building..."
make --silent clean >/dev/null 2>&1
make --silent || { echo "build failed"; exit 1; }

echo "Preparing fixtures..."
mkdir -p tests
./tests/gen_log.sh 20 tests/small.log >/dev/null
: > tests/empty.log
printf '10.0.0.1 - - [12/Nov/2026:09:00:00] "GET /x HTTP/1.1" 200 5 INFO' \
    > tests/nonewline.log

echo
echo "Running tests:"

# 1 - normal file, counts correct
check_contains "1  normal file counts 20 lines" "Lines:     20" \
    $BIN -f tests/small.log

# 2 - empty file
check_contains "2  empty file reports 0 lines" "Lines:     0" \
    $BIN -f tests/empty.log
check_status   "2b empty file exits 0" 0 $BIN -f tests/empty.log

# 3 - no trailing newline still counts the last line
check_contains "3  file with no trailing newline" "Lines:     1" \
    $BIN -f tests/nonewline.log

# 4 - missing file
check_status   "4  missing file exits 1" 1 $BIN -f tests/nope.log
check_contains "4b missing file names itself" "loganalyzer:" \
    $BIN -f tests/nope.log

# 5 - no -f
check_status   "5  no -f exits 1" 1 $BIN

# 6 - unknown flag
check_status   "6  unknown flag exits 1" 1 $BIN -z -f tests/small.log

# 7 - bad -t
check_status   "7  -t abc exits 1" 1 $BIN -f tests/small.log -t abc
check_status   "7b -t 0 exits 1"   1 $BIN -f tests/small.log -t 0

# 8 - directory as input
check_status   "8  directory input exits 1" 1 $BIN -f /tmp

# 9 - pattern not found
check_contains "9  pattern not found" "Matched:   0" \
    $BIN -f tests/small.log -p zzzzzzzz

# 9b - pattern found
check_contains "9b pattern found" "Matched:" \
    $BIN -f tests/small.log -p GET

# 10 - level filter and path ranking work together
check_contains "10 level filter + path ranking" "by path" \
    $BIN -f tests/small.log -l ERROR -t 10 -k path

# 11 - bad level name rejected
check_status   "11 unknown level exits 1" 1 $BIN -f tests/small.log -l NOPE

# 12 - called twice in one process (the optind test)
echo "  ....  building shell_sim for test 12"
gcc -Wall -Wextra -O2 -o tests/shell_sim tests/shell_sim.c loganalyzer.c 2>/dev/null
if [ -x tests/shell_sim ]; then
    ./tests/shell_sim > tests/sim.out 2>&1
    first=$(sed -n '/=== first call ===/,/=== second call ===/p' tests/sim.out | grep "Lines:")
    second=$(sed -n '/=== second call ===/,$p'                 tests/sim.out | grep "Lines:")
    if [ -n "$first" ] && [ "$first" = "$second" ]; then
        ok "12 two calls in one process give identical output"
    else
        bad "12 two calls differ (optind was not reset): [$first] vs [$second]"
    fi
    rm -f tests/sim.out tests/shell_sim
else
    bad "12 could not build shell_sim"
fi

# 13 - comparison mode prints all three timings
check_contains "13  -c prints the mmap timing" "mmap + memchr" \
    $BIN -f tests/small.log -c
check_contains "13b -c prints the read timing" "read() + memchr" \
    $BIN -f tests/small.log -c
check_contains "13c -c prints the getline timing" "stdio getline()" \
    $BIN -f tests/small.log -c
check_contains "13d all three scans agree on the count" "mmap vs read()" \
    $BIN -f tests/small.log -c

# 14 - verbose prints the mapping info
check_contains "14 -v prints pages mapped" "page size" \
    $BIN -f tests/small.log -v

# 15 - help exits 0
check_status   "15 -h exits 0" 0 $BIN -h

echo
echo "-----------------------------"
printf "passed: %d   failed: %d\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
