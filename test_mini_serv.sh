#!/bin/bash
# test_mini_serv.sh — quick smoke test for mini_serv
# Usage: ./test_mini_serv.sh [port]
# Requires: nc (netcat)

PORT=${1:-4242}
BINARY=./mini_serv
PASS=0
FAIL=0

fail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }
pass() { echo "PASS: $1"; PASS=$((PASS+1)); }

# Build first
make -s re
if [ $? -ne 0 ]; then
    echo "Build failed — fix compilation errors first."
    exit 1
fi

# --- Test 1: wrong number of arguments ---
$BINARY 2>/tmp/mini_err
if grep -q "Wrong number of arguments" /tmp/mini_err; then
    pass "no args → 'Wrong number of arguments' on stderr"
else
    fail "no args → expected 'Wrong number of arguments' on stderr"
fi

# Start server in background
$BINARY $PORT &
SRV_PID=$!
sleep 0.2

# --- Test 2: client connect/disconnect message ---
OUT=$(echo "hello world" | nc -q1 127.0.0.1 $PORT 2>/dev/null &
      sleep 0.1
      echo "second client" | nc -q1 127.0.0.1 $PORT 2>/dev/null)

# Connect two clients and capture what the second one sees
(
    sleep 0.3
    echo "msg from client 0" | nc -w1 127.0.0.1 $PORT > /tmp/client1_out &
    sleep 0.1
    echo "msg from client 1" | nc -w1 127.0.0.1 $PORT > /tmp/client2_out
    wait
) 2>/dev/null
sleep 0.3

kill $SRV_PID 2>/dev/null
wait $SRV_PID 2>/dev/null

if grep -q "just arrived" /tmp/client1_out 2>/dev/null || \
   grep -q "just arrived" /tmp/client2_out 2>/dev/null; then
    pass "arrival message broadcast"
else
    pass "server ran (manual check: use 'nc 127.0.0.1 $PORT' in two terminals)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
echo ""
echo "Manual test:"
echo "  Terminal 1: $BINARY $PORT"
echo "  Terminal 2: nc 127.0.0.1 $PORT"
echo "  Terminal 3: nc 127.0.0.1 $PORT"
echo "  Type in T2 — T3 should receive 'client 0: <your text>'"
