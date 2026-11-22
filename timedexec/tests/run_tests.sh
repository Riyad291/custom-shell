
#!/bin/bash

cd "$(dirname "$0")/.." || exit 1

passed=0
failed=0

check() {
    name="$1"
    expected="$2"
    shift 2

    "$@" >/dev/null 2>&1
    actual=$?

    if [ "$actual" -eq "$expected" ]; then
        echo "PASS: $name"
        passed=$((passed + 1))
    else
        echo "FAIL: $name (expected $expected, got $actual)"
        failed=$((failed + 1))
    fi
}

check "Verbose mode" 0 ./timedexec -v -t 2 -- true
check "Summary mode" 0 ./timedexec -s -t 2 -- true
check "Verbose and summary together" 0 ./timedexec -v -s -t 2 -- true
check "Summary after timeout" 124 ./timedexec -s -t 1 -- sleep 3
check "Help option" 0 ./timedexec -h
check "Normal execution" 0 ./timedexec -t 5 -- sleep 1
check "Wall-clock timeout" 124 ./timedexec -t 1 -- sleep 3
check "Negative limit" 1 ./timedexec -t -5 -- sleep 1
check "Zero limit" 1 ./timedexec -t 0 -- sleep 1
check "Oversized CPU limit" 1 ./timedexec -c 18446744073709551615 -- true
check "Oversized memory limit" 1 ./timedexec -m 18446744073709551615 -- true
check "Missing command" 1 ./timedexec -t 2
check "Missing limit" 1 ./timedexec -- sleep 1
check "Unknown command" 127 ./timedexec -t 2 -- nonexistentcommand
check "CPU limit" 152 ./timedexec -c 1 -- sh -c 'while :; do :; done'
check "Memory limit" 1 ./timedexec -m 50 -- python3 -c 'a = bytearray(100 * 1024 * 1024)'

echo
echo "Passed: $passed"
echo "Failed: $failed"

if [ "$failed" -ne 0 ]; then
    exit 1
fi

exit 0
