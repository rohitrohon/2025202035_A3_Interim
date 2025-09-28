# Tracker with Synchronization

This is a distributed tracker system with synchronization capabilities between multiple tracker instances.

## Building the Tracker

1. Ensure you have the following dependencies installed:
   - C++17 compatible compiler (GCC 7+ or Clang 6+)
   - CMake 3.10+
   - OpenSSL development libraries

2. Build the tracker:
   ```bash
   make clean
   make all
   ```

## Running the Trackers

You need to run at least two tracker instances for synchronization to work.

### Terminal 1 - Tracker 1
```bash
./tracker tracker_info.txt 1
```

### Terminal 2 - Tracker 2
```bash
./tracker tracker_info.txt 2
```

## Tracker Commands

When running, you can use the following commands in the tracker console:

- `status` - Show current tracker status and sync version
- `sync` - Force immediate synchronization with other tracker
- `quit` - Shut down the tracker gracefully

## Synchronization Details

- Trackers synchronize their state every 5 seconds by default
- The sync interval can be configured in the code
- State includes all user data, groups, and file information
- Uses a versioning system to track changes and resolve conflicts

## Configuration

Edit `tracker_info.txt` to configure the tracker addresses:
```
<tracker1_ip>:<tracker1_port>
<tracker2_ip>:<tracker2_port>
```

## Testing

1. Start both trackers as described above
2. Make changes on one tracker (e.g., add a user)
3. Verify the changes appear on the other tracker within the sync interval
4. Use the `sync` command to force immediate synchronization
5. Test network partition recovery by stopping and restarting a tracker

## Troubleshooting

- If trackers can't connect, check firewall settings and ensure the sync ports (8001 and 8002 by default) are open
- Check logs for any error messages
- Verify both trackers are using the same configuration

## Cleanup

To clean up build artifacts:
```bash
make clean
```
