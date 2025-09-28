#!/bin/bash

# Check if trackers are already running
if pgrep -f "./tracker" > /dev/null; then
    echo "Trackers are already running. Please stop them first."
    exit 1
fi

# Build the tracker
cd tracker
make clean
make all
if [ $? -ne 0 ]; then
    echo "Build failed"
    exit 1
fi

# Create log directory
mkdir -p logs

# Start trackers in separate terminals
# Terminal 1 - Tracker 1
echo "Starting Tracker 1..."
./tracker tracker_info.txt 1 > logs/tracker1.log 2>&1 &
TRACKER1_PID=$!

# Give it a moment to start
sleep 2

# Terminal 2 - Tracker 2
echo "Starting Tracker 2..."
./tracker tracker_info.txt 2 > logs/tracker2.log 2>&1 &
TRACKER2_PID=$!

echo "Trackers started with PIDs: $TRACKER1_PID, $TRACKER2_PID"
echo "Logs are being written to logs/tracker1.log and logs/tracker2.log"

# Function to clean up
cleanup() {
    echo "Shutting down trackers..."
    kill $TRACKER1_PID $TRACKER2_PID 2>/dev/null
    wait $TRACKER1_PID $TRACKER2_PID 2>/dev/null
    echo "Done."
    exit 0
}

# Set up trap to catch Ctrl+C
trap cleanup INT

# Keep script running
while true; do
    sleep 1
    # Check if either tracker has died
    if ! kill -0 $TRACKER1_PID 2>/dev/null || ! kill -0 $TRACKER2_PID 2>/dev/null; then
        echo "One of the trackers has stopped unexpectedly."
        cleanup
        exit 1
    fi
done
