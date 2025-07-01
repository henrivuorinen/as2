#!/bin/bash

# Start the bank server in background
./server &
SERVER_PID=$!

# Give server time to start
sleep 2

# Run the testbench
./as2_testbench ./client

# Kill the server
kill $SERVER_PID 2>/dev/null

echo "Test completed"