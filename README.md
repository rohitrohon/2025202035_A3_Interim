# Peer-to-Peer Distributed File Sharing System
## Assignment 3 Interim Submission

### Submission Format:
2025202035_A3_Interim.zip

### Implementation Overview

This interim submission focuses on the multi-tracker synchronization and user/group management functionality of the distributed file sharing system.

#### Architecture

The system consists of:
1. **Tracker Server**: Maintains metadata about users, groups, and file locations
2. **Client Application**: Allows users to interact with the file sharing network

#### Multi-Tracker Synchronization

**Approach**: 
We've implemented a state-based synchronization mechanism where each tracker maintains a complete copy of the system state. When a tracker receives an update (user creation, group modification, etc.), it propagates this change to the other tracker using a custom synchronization protocol over TCP sockets.

**Implementation Details**:
- Each tracker runs a synchronization thread that periodically checks for state changes
- When changes are detected, the tracker connects to the other tracker and sends the updated state
- The receiving tracker validates and applies the state changes
- Conflict resolution follows a "last write wins" strategy with timestamps

**Handling New Connections**:
- Clients can connect to either tracker
- The tracker a client connects to becomes its primary tracker
- All client operations are processed by the primary tracker and then synchronized

#### Metadata Organization

We've organized metadata using the following data structures:

1. **User Management**:
   - `user_data`: Maps user IDs to passwords
   - `is_logged_in`: Tracks user login status
   - `user_ip_port`: Maps users to their IP:port combinations

2. **Group Management**:
   - `all_groups`: List of all groups in the system
   - `group_admin`: Maps group IDs to admin user IDs
   - `group_members`: Maps group IDs to sets of member user IDs
   - `pending_requests`: Maps group IDs to sets of users with pending join requests

All data structures are protected by mutexes to ensure thread safety in the multi-threaded tracker environment.

#### Compilation and Execution

**Tracker**:
cd Tracker
make
./tracker tracker_info.txt tracker_no


**Client**:
cd Client
make
./client <IP>:<PORT> tracker_info.txt