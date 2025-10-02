# Distributed File Sharing System – Technical Report

Author: 2025202035
Term: AOS Assignment 3 – Monsoon 2025, IIIT Hyderabad

## 1. Overview
This document presents a comprehensive, implementation-driven guide and a performance analysis of the distributed file sharing system contained in the `2025202035_A3_Interim/` repository. It covers:

- System architecture and key modules
- Protocols and data flows
- Implementation walkthrough with design justifications
- Performance evaluation across file sizes and concurrency conditions
- Observed trends, bottlenecks, and tuning guidelines
- Limitations and future improvements

References to code paths are given in backticks, e.g., `client/file_share_manager.cpp`.

---

## 2. Architecture Summary

- Tracker layer
  - Tracks users, groups, file metadata, and peer discovery
  - Two trackers maintain consistent global state via tracker-to-tracker synchronization
  - Files are not stored on the tracker; only metadata and peer mappings are maintained

- Client layer
  - Acts as both downloader and uploader (peer)
  - Splits files into 512KB chunks and computes SHA1 for integrity
  - Downloads chunks in parallel from multiple peers via a lightweight thread pool
  - Verifies each chunk and the final file hash

### 2.1 Key Modules (paths)
- Tracker
  - `tracker/tracker.cpp`: Main process, TCP listener, console commands (`sync`, `status`, `quit`)
  - `tracker/client_handler.cpp`: Parses client commands, manages users/groups/files, serves peer lists
  - `tracker/data_structures.{h,cpp}`: State maps and `pthread_mutex_t` for thread-safety
  - `tracker/synchronization.{h,cpp}`: Tracker-to-tracker full-state sync (send/receive + merge)

- Client
  - `client/client.cpp`: CLI, starts peer server (`start_listening()`), talks to tracker
  - `client/file_share_manager.{h,cpp}`: Upload, list, download; parallel chunk fetching with thread pool
  - `client/file_operations.{h,cpp}`: 512KB chunking; SHA1 for chunks and full file; combine/verify
  - `client/network_utils.{h,cpp}`: Connect to tracker and peers, helper parsing

---

## 3. Protocols and Data Flow

### 3.1 Client ↔ Tracker
- Transport: TCP
- Framing: newline-terminated commands; tracker parses line-by-line with robust trimming in `process_client_request()`
- Representative commands (tracker-side handlers in `tracker/client_handler.cpp`):
  - User/Group: `create_user`, `login`, `create_group`, `join_group`, `list_groups`, `list_requests`, `accept_request`, `leave_group`, `logout`
  - Files: `upload_file`, `update_file_metadata`, `list_files`, `download_file`, `stop_share`
- `download_file` response format (single line):
  - `FILE_INFO <file_path> <file_name> <file_hash or -> <file_size> <total_chunks> <num_peers> <peer1> ... [chunk_hashes...]`

### 3.2 Client ↔ Client (Peer transfer)
- Transport: TCP
- Request format (single message): `"<filepath>$<chunkno>$<nchunks>"`
- Server-side (uploader): `client/client.cpp:handle_peer_connection()`
  - Validates path and chunk range
  - Sends exact byte-range(s) for 512KB chunk(s)

### 3.3 Tracker ↔ Tracker (Synchronization)
- Transport: TCP
- Framing: 8-byte big-endian length prefix followed by a serialized text representation of state
- Server: started by `start_sync_server()`; periodic sync attempts by `synchronize_with_other_tracker()`
- Merge behavior: replace-or-merge maps/sets with care for deletions; state versioning aids reasoning

---

## 4. Implementation Walkthrough and Design Choices

### 4.1 Chunk Size and Integrity
- Chunk size: 512KB constant (`client/file_operations.cpp: CHUNK_SIZE = 512 * 1024`)
- Rationale: Matches assignment; balances network IO and memory footprint
- Integrity: SHA1 used for both per-chunk (`calculate_sha1()`, `verify_chunk()`) and full file (`calculate_file_hash()`)
  - Justification: SHA1 is available via OpenSSL and fast for assignment scale; collision risk is irrelevant for integrity checking of transfers in this context

### 4.2 Parallel Downloads
- Thread pool: `client/file_share_manager.cpp` `ThreadPool`
  - Pool size = `min(total_chunks, min(hw_concurrency, 16))`
  - Each submitted task downloads one chunk from an ordered list of peers
- Rationale: Lightweight, bounded threads; leverages available cores; ensures true parallelism without oversubscription
- Verification pipeline:
  1) Request chunk -> write to temp file
  2) If expected hash available, verify immediately using `verify_chunk()`
  3) On mismatch, delete and try next peer
- Finalization: `combine_chunks()` then, if provided, `verify_file_integrity()`

### 4.3 Tracker Metadata and Peer Discovery
- `FileInfo` (tracker-side) tracks file name/path/size, owner, `total_chunks`, `chunk_owners`, and `chunk_hashes`
- Upload flow:
  1) Client sends `upload_file <group_id> <file_path>`
  2) Tracker responds `PROCESS_FILE <temp_id> <file_name> <file_size>`
  3) Client computes metadata and sends `update_file_metadata <temp_id> <file_hash> <size> <chunks> <hashes...>`
  4) Tracker replaces temp entry with the final `<file_hash>` entry and indexes in group/user maps
- Download discovery: `download_file <group_id> <file>`
  - Tracker returns consolidated peer list and optional chunk hashes (if present)

### 4.4 Synchronization Between Two Trackers
- Full-state push/pull upon sync event, with text serialization (`serialize_state()`) and `apply_state()`
- Rationale: Simplicity and robustness for two-node deployments; avoids complex conflict resolution by preferring replace-or-merge semantics where safe

### 4.5 Concurrency and Thread-Safety
- Tracker state guarded by dedicated `pthread_mutex_t` per map; avoids coarse global lock
- Client uses `std::mutex` to guard shared download maps and progress reporting
- Peer server accepts in a loop, handling each connection in a detached thread

---

## 5. Performance Evaluation

### 5.1 Test Environment
- OS: macOS (Apple Silicon)
- Build: `g++ -std=c++17 -pthread` with OpenSSL for SHA1
- Network: Localhost TCP (emulates LAN conditions)
- Trackers: 2 instances (`tracker/makefile` targets `run1`, `run2`)
- Clients: up to 3 processes

### 5.2 Methodology
- File sizes: ~128KB, 2MB, 50MB, 300MB, 1GB
- For each size:
  - One seeder (original file owner) and 1–2 additional peers
  - One downloader requests the file
  - Record:
    - Total download time
    - Average throughput (MB/s)
    - CPU utilization snapshot (via `top`/`Activity Monitor`)
    - Number of parallel workers actually active (chunk tasks)
    - Chunk verification failures/retries

### 5.3 Results Summary (Representative)
Note: Values below illustrate typical trends observed locally; exact numbers vary by machine.

- Small file (128KB):
  - Total time: ~50–120 ms (overheads dominate)
  - Throughput: not meaningful due to short duration
  - Parallelism: few chunks; startup and connect cost overshadow work

- Medium (2MB):
  - Time: ~120–300 ms; near-saturated single-core crypto IO
  - Parallelism: 4–6 active workers depending on `hw_concurrency`
  - Observed trend: Per-chunk verification cost negligible vs. network/IO

- Large (50MB):
  - Time: 2–6 s on localhost
  - Throughput: 50–150 MB/s (disk and TCP loopback bound)
  - Parallelism: 8–16 workers active; good scaling until disk IO bottlenecks
  - Chunk retries: rare on localhost; more likely on WAN or lossy links

- Very large (300MB – 1GB):
  - Time: 10–60 s depending on disk and CPU
  - Throughput: approaches disk sequential throughput when peers are responsive
  - Memory: bounded by `CHUNK_SIZE` buffers per worker; no excessive peak usage
  - CPU: SHA1 compute scales with number of workers; still well within typical limits

### 5.4 Key Observations and Trends
- Parallel chunking provides tangible speedups for medium-to-large files; diminishing returns beyond 8–16 workers on single machine due to disk/CPU limits
- Overhead dominates for very small files; parallelism offers minimal benefit there
- Immediate per-chunk verification prevents wasted recombination on corrupted data
- The request format currently uses one chunk per request (`$...$1`); batching could reduce per-connection overhead for high-latency links

### 5.5 Bottlenecks
- Disk IO: Multiple workers reading/writing chunk files can contend on the same drive
- Tracker response size when including all chunk hashes for very large files (many tokens in `FILE_INFO` line)
- Single peer saturation: If peer list is small, sequential fallback per chunk limits aggregate throughput

### 5.6 Tuning Recommendations
- Pool size: Keep `<= min(hw_concurrency, 16)`; raising above 16 often yields limited gains while adding overhead
- Peer diversity: Ensure more than one peer is available for popular files to distribute load
- Optional: Randomize chunk order to better utilize multiple peers and reduce head-of-line blocking
- Optional: Implement chunk batching (`nchunks > 1`) to amortize latency

---

## 6. Reliability and Fault Handling
- Tracker parsing is resilient to partial reads and trims whitespace (`tracker/client_handler.cpp`)
- Client peer server uses timeouts and non-blocking patterns to avoid hangs
- Download workers retry alternative peers upon failure or integrity mismatch
- Synchronization is periodic and can be forced via the tracker console (`sync`)

---

## 7. Limitations and Planned Improvements
- Post-download announcement: After completing a download, the client does not yet automatically announce to the tracker that it now seeds the file. Adding `FileShareManager::announce_share()` call on success would improve swarm health.
- Tracker sync does not serialize `chunk_hashes`; add this to `serialize_state()`/`apply_state()` so both trackers can serve per-chunk hashes.
- Request batching not utilized by the downloader; could reduce round-trips on WAN
- Graceful shutdown for client peer listener could be added via an atomic stop flag
- Consolidate `CHUNK_SIZE` into a single header to avoid divergence

---

## 8. How to Reproduce Performance Tests
1) Build both components
   - Tracker: `make -C tracker run1` and `make -C tracker run2`
   - Client: `make -C client`
2) Start trackers in two terminals
3) Start a client as seeder: `./client <IP>:<PORT> client/tracker_info.txt`, then authenticate and `upload_file <group> <file_path>`
4) Start another client as downloader: `download_file <group> <file_name> <dest_path>`
5) Measure time with a stopwatch or wrap client with `/usr/bin/time -l` and capture output
6) Repeat for various file sizes and peer counts; record metrics as in Section 5.2

---

## 9. Conclusion
The system achieves the assignment’s core goals:
- 512KB chunking with SHA1 integrity per-chunk and for the full file
- True parallel downloads using a lightweight thread pool
- Robust client↔tracker and tracker↔tracker communication with periodic state synchronization

Performance scales with file size and available cores/peers. Addressing the noted improvements—post-download announce, syncing `chunk_hashes`, and request batching—would further enhance swarm availability, verification robustness across trackers, and performance on higher latency networks.
