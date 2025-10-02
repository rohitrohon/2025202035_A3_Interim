#include "file_share_manager.h"
#include "network_utils.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>
#include <fcntl.h>
#include <sys/time.h>
#include <errno.h>
#include <openssl/sha.h>
#include <iostream>
#include <thread>
#include <sys/types.h>
#include <vector>
#include <dirent.h>
#include <filesystem>
#include <queue>
#include <functional>
#include <condition_variable>
#include <atomic>

using namespace std;

const size_t CHUNK_SIZE = 512 * 1024; // 512 KB fixed chunk size

// Global control socket (if the interactive client registered one)
static int g_control_socket = -1;


// Constructor
FileShareManager::FileShareManager(const string& client_id, 
                                 const string& tracker_ip, 
                                 int tracker_port)
    : client_id(client_id), 
      tracker_ip(tracker_ip), 
      tracker_port(tracker_port) {
    // Create chunks directory if it doesn't exist
    mkdir("./chunks", 0777);
}

// Simple fixed-size thread pool for parallel chunk downloads
class ThreadPool {
public:
    ThreadPool(size_t n) : stop(false) {
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([this]() {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    try { task(); } catch (...) { }
                }
            });
        }
    }

    ~ThreadPool() {
        {
            unique_lock<mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (auto &t : workers) if (t.joinable()) t.join();
    }

    void submit(function<void()> f) {
        {
            lock_guard<mutex> lock(queue_mutex);
            tasks.push(std::move(f));
        }
        condition.notify_one();
    }

    // wait until queue is empty
    void wait_empty() {
        while (true) {
            {
                lock_guard<mutex> lock(queue_mutex);
                if (tasks.empty()) break;
            }
            this_thread::sleep_for(chrono::milliseconds(50));
        }
    }

private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mutex;
    condition_variable condition;
    bool stop;
};

void FileShareManager::set_client_id(const string& id) {
    this->client_id = id;
}

bool FileShareManager::announce_share(const string& group_id, const string& file_hash, const string& file_path) const {
    // Send a simple I_HAVE command to tracker: I_HAVE <group_id> <client_id> <file_hash> <file_path>
    if (client_id.empty()) return false;

    bool owned = true;
    int sock = connect_to_tracker(owned);
    if (sock < 0) return false;

    string message = "I_HAVE " + group_id + " " + client_id + " " + file_hash + " " + file_path + "\n";
    ssize_t total = 0;
    const char* buf = message.c_str();
    ssize_t len = (ssize_t)message.length();
    while (total < len) {
        ssize_t sent = send(sock, buf + total, len - total, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EINTR) continue;
            if (owned) close(sock);
            return false;
        }
        total += sent;
    }

    // Read a short acknowledgement line (optional)
    char rbuf[1024];
    ssize_t n = recv(sock, rbuf, sizeof(rbuf)-1, 0);
    if (n > 0) {
        rbuf[n] = '\0';
        // ignore content here
    }

    if (owned) close(sock);
    return true;
}

void FileShareManager::set_control_socket(int sock) {
    // Store the provided control socket at file scope so other methods can reuse it
    // Note: not thread-safe; only one interactive control connection is expected.
    g_control_socket = sock;
}

int FileShareManager::connect_to_tracker(bool &out_owned) const {
    // If a control socket was registered, reuse it and mark as not owned
    if (g_control_socket > 0) {
        out_owned = false;
        return g_control_socket;
    }
    out_owned = true;
    // Use the helper in network_utils to open a new connection
    extern int connect_to_tracker(const string&, int, const string&);
    return connect_to_tracker(tracker_ip, tracker_port, "");
}

// Destructor
FileShareManager::~FileShareManager() {
    // Clean up any active downloads
    {
        lock_guard<mutex> lock(downloads_mutex);
        if (!active_downloads.empty()) {
            cv_downloads.notify_all();
        }
    }
    
    // Wait for all downloads to complete or timeout
    this_thread::sleep_for(chrono::seconds(1));
}

// Helper function to create a TCP socket and connect to the tracker
int FileShareManager::connect_to_tracker() const {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    
    // Set socket to non-blocking for connect timeout
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(tracker_port);
    
    if (inet_pton(AF_INET, tracker_ip.c_str(), &server_addr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }
    
    // Start non-blocking connect
    int res = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (res < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }
    
    // Set up timeout
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    struct timeval timeout;
    timeout.tv_sec = SOCKET_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    
    // Wait for connect with timeout
    res = select(sock + 1, nullptr, &set, nullptr, &timeout);
    if (res <= 0) {
        close(sock);
        return -1; // Timeout or error
    }
    
    // Check if connection was successful
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        close(sock);
        return -1;
    }
    
    // Set back to blocking mode
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    
    // Set socket options
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    
    return sock;
}

// Helper function to send a message to the tracker
bool FileShareManager::send_to_tracker(const string& message) const {
    int sock = connect_to_tracker();
    if (sock < 0) {
        return false;
    }

    // Send a newline-terminated message and close
    string tosend = message;
    if (tosend.empty() || tosend.back() != '\n') tosend.push_back('\n');

    ssize_t total = 0;
    const char* buf = tosend.c_str();
    ssize_t len = (ssize_t)tosend.length();
    while (total < len) {
        ssize_t sent = send(sock, buf + total, len - total, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EINTR) continue;
            close(sock);
            return false;
        }
        total += sent;
    }

    // Optionally read an acknowledgement line (non-blocking short wait) - not required here
    close(sock);
    return true;
}

// Register a file share with the tracker
bool FileShareManager::register_share_with_tracker(const string& group_id, 
                                                 const string& file_name) {
    string message = "SHARE " + group_id + " " + client_id + " " + file_name;
    return send_to_tracker(message);
}

// Unregister a file share with the tracker
bool FileShareManager::unregister_share_with_tracker(const string& group_id, 
                                                   const string& file_name) {
    string message = "STOP_SHARE " + group_id + " " + client_id + " " + file_name;
    return send_to_tracker(message);
}

// Upload a file to share
bool FileShareManager::upload_file(const string& group_id, 
                                 const string& file_path,
                                 ProgressCallback callback) {
    // Step 1: Split file into chunks and calculate hashes
    FileMetadata metadata;
    try {
    cout << "Splitting file into chunks..." << endl;
    metadata = split_file_into_chunks(file_path);
    cout << "Calculating file hash..." << endl;
    metadata.file_hash = calculate_file_hash(file_path);
        
        // Store metadata locally
        {
            lock_guard<mutex> lock(shared_files_mutex);
            file_metadata[metadata.file_name] = metadata;
            shared_files[group_id].insert(metadata.file_name);
        }
        
        // Step 2: Register with tracker
        bool sock_owned = true;
        int sock = connect_to_tracker(sock_owned);
        if (sock < 0) {
            throw runtime_error("Failed to connect to tracker");
        }
        // Send the upload command to the tracker (newline-terminated)
        string command = "upload_file " + group_id + " " + file_path + "\n";
        if (send(sock, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            close(sock);
            throw runtime_error("Failed to send upload command to tracker");
        }

        // Read one line of response
        auto recv_line = [&](int s)->string {
            string buf;
            char rbuf[4096];
            while (true) {
                ssize_t n = recv(s, rbuf, sizeof(rbuf), 0);
                if (n <= 0) break;
                buf.append(rbuf, n);
                size_t pos = buf.find('\n');
                if (pos != string::npos) {
                    string line = buf.substr(0, pos);
                    return line;
                }
                if (buf.size() > 10 * 1024) break; // too long
            }
            return string();
        };

        string response = recv_line(sock);
        if (response.empty()) {
            close(sock);
            throw runtime_error("No response or invalid response from tracker");
        }

        vector<string> tokens;
        istringstream iss(response);
        string token;
        while (iss >> token) tokens.push_back(token);
        if (tokens.empty() || tokens[0] != "PROCESS_FILE") {
            close(sock);
            throw runtime_error("Unexpected response from tracker: " + response);
        }
        
        // Display upload information
        cout << "\n=== File Upload Started ===" << endl;
        cout << "File: " << metadata.file_name << endl;
        cout << "Size: " << metadata.file_size << " bytes" << endl;
        cout << "Hash: " << metadata.file_hash << endl;
        cout << "Chunks: " << metadata.total_chunks << endl;
        cout << "Group: " << group_id << endl;
        
        // Send the file metadata to the tracker
        // Build update command and include per-chunk hashes so tracker can verify later
        command = "update_file_metadata " + tokens[1] + " " +
                 metadata.file_hash + " " +
                 to_string(metadata.file_size) + " " +
                 to_string(metadata.total_chunks);

        // Append each chunk's hash
        for (const auto& c : metadata.chunks) {
            command += " " + c.hash;
        }

        cout << "DEBUG: Sending update_file_metadata command: " << command << endl;
        if (send(sock, (command + "\n").c_str(), command.length() + 1, MSG_NOSIGNAL) <= 0) {
            close(sock);
            throw runtime_error("Failed to send file metadata");
        }

        response = recv_line(sock);
        cout << "DEBUG: Received response to update_file_metadata: " << response << endl;
    if (sock_owned) close(sock);
        
        if (response.find("SUCCESS") == 0) {
            // Convert file size to KB with 2 decimal places
            double file_size_kb = static_cast<double>(metadata.file_size) / 1024.0;
            cout << "\nFile Upload Complete. " 
                 << metadata.file_name << " " 
                 << fixed << setprecision(2) << file_size_kb 
                 << " KB";
            // Also show number of chunks uploaded (requested UX change)
            cout << " (chunks: " << metadata.total_chunks << ")" << endl;
            return true;
        } else {
            throw runtime_error("Failed to complete file upload: " + response);
        }
        
    } catch (const exception& e) {
        // Cleanup on failure
        cerr << "Upload failed: " << e.what() << endl;
        if (!metadata.file_name.empty()) {
            lock_guard<mutex> lock(shared_files_mutex);
            shared_files[group_id].erase(metadata.file_name);
            file_metadata.erase(metadata.file_name);
        }
        return false;
    }
}

// List files in a group from tracker
vector<string> FileShareManager::list_files(const string& group_id) const {
    vector<string> files;
    
    try {
        // Connect to tracker
        int sock = connect_to_tracker();
        if (sock < 0) {
            cerr << "Failed to connect to tracker" << endl;
            return {};
        }

        string command = "list_files " + group_id + "\n";
        if (send(sock, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            close(sock);
            return {};
        }

        // Read full response until the server closes the connection
        string buf;
        char rbuf[4096];
        while (true) {
            ssize_t n = recv(sock, rbuf, sizeof(rbuf), 0);
            if (n <= 0) break; // either closed or error
            buf.append(rbuf, n);
            // guard against extremely large responses
            if (buf.size() > 1 * 1024 * 1024) break;
        }
        close(sock);

        if (buf.empty()) return {};

        // Split into lines
        istringstream resp(buf);
        string line;
        // First line expected to be a count (optional). We'll try to read it and then
        // treat each subsequent non-empty line as a file entry in the format:
        // <file_name> <file_size> <total_chunks>
        if (!getline(resp, line)) return {};
        // Try to parse count, but if it's not a number we will treat the first line
        // as a file entry as well.
        try {
            (void)stoi(line); // parse count but we don't strictly need it
        } catch (...) {
            // treat the first line as a file entry
            if (!line.empty()) files.push_back(line);
        }

        // Read remaining lines
        while (getline(resp, line)) {
            if (line.empty()) continue;
            files.push_back(line);
        }
    } catch (const exception& e) {
        cerr << "Error listing files: " << e.what() << endl;
    }
    
    return files;
}

// Download a file from the network
bool FileShareManager::download_file(const string& group_id, 
                                   const string& file_name, 
                                   const string& dest_path) {
    try {
        // Step 1: Get file info from tracker (reuse control socket if registered)
        bool sock_owned = true;
        int sock = connect_to_tracker(sock_owned);
        if (sock < 0) {
            throw runtime_error("Failed to connect to tracker");
        }

        // Use tracker download_file to get FILE_INFO and peers
        string command = "download_file " + group_id + " " + file_name + "\n";
        if (send(sock, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            if (sock_owned) close(sock);
            throw runtime_error("Failed to request file info");
        }

        // Read one line of response
        string buf;
        char rbuf[8192];
        ssize_t n = recv(sock, rbuf, sizeof(rbuf), 0);
        if (n <= 0) {
            if (sock_owned) close(sock);
            throw runtime_error("No response from tracker");
        }
        buf.append(rbuf, n);
        // Stop at newline
        size_t pos = buf.find('\n');
        string line = (pos == string::npos) ? buf : buf.substr(0, pos);
        if (sock_owned) close(sock);

        // Expected format (new): FILE_INFO <file_path> <file_name> <file_size> <total_chunks> <num_peers> <peer1> ... [chunk_hashes...]
        istringstream iss(line);
        string header;
        iss >> header;
        if (header != "FILE_INFO") {
            throw runtime_error("Invalid FILE_INFO from tracker: " + line);
        }
    string file_path_recv;
    string file_name_recv;
    string file_hash_recv;
    uint64_t file_size;
    int total_chunks;
    int num_peers;
    iss >> file_path_recv >> file_name_recv >> file_hash_recv >> file_size >> total_chunks >> num_peers;
        vector<string> peers;
        for (int i = 0; i < num_peers; ++i) {
            string p; if (!(iss >> p)) break; peers.push_back(p);
        }
        
        // Step 2: Create download directory if it doesn't exist
        filesystem::path dest_dir = filesystem::path(dest_path).parent_path();
        if (!dest_dir.empty() && !filesystem::exists(dest_dir)) {
            filesystem::create_directories(dest_dir);
        }
        
        // Step 3: Prepare download info
        string download_id = group_id + ":" + file_name + ":" + to_string(time(nullptr));
        
        DownloadInfo info;
        info.file_name = file_name;
        info.dest_path = dest_path;
    info.group_id = group_id;
        info.total_chunks = total_chunks;
        info.downloaded_chunks = 0;
        info.start_time = chrono::system_clock::now();
        
        // Add to active downloads
        {
            lock_guard<mutex> lock(downloads_mutex);
            active_downloads[download_id] = info;
        }
        
        // Peers were provided in FILE_INFO; no extra get_peers call required
        
        if (peers.empty()) {
            throw runtime_error("No peers available for this file");
        }
        
    // Step 5: Parse optional chunk hashes appended after peers
        vector<string> chunk_hashes;
        // Any remaining tokens in 'line' after peers are chunk hashes
        // We already parsed peers from the first FILE_INFO line; try to extract hashes from that line too
        {
            // Re-parse the FILE_INFO line to extract everything after peers
            {
                istringstream iss2(line);
                string tok;
                vector<string> all_tokens;
                while (iss2 >> tok) all_tokens.push_back(tok);
                // Token layout (indices):
                // [0]=FILE_INFO [1]=file_path [2]=file_name [3]=file_hash [4]=file_size [5]=total_chunks [6]=num_peers [7..6+num_peers]=peers [after]=chunk_hashes
                int header_count = 7 + num_peers; // up to and including peers
                if ((int)all_tokens.size() > header_count) {
                    for (int i = header_count; i < (int)all_tokens.size(); ++i) {
                        // Treat placeholder '-' as missing
                        if (all_tokens[i] == "-") continue;
                        chunk_hashes.push_back(all_tokens[i]);
                    }
                }
            }
        }

        // Debug: print parsed info
    cout << "DEBUG: Parsed FILE_INFO - file='" << file_name_recv << "' size=" << file_size
                  << " total_chunks=" << total_chunks << " num_peers=" << num_peers << endl;
        cout << "DEBUG: Peers list (" << peers.size() << "):";
        for (const auto &p : peers) cout << " '" << p << "'";
        cout << endl;
        if (!chunk_hashes.empty()) {
            cout << "DEBUG: Received " << chunk_hashes.size() << " chunk hashes" << endl;
        } else {
            cout << "DEBUG: No per-chunk hashes provided by tracker" << endl;
        }

        // Prepare temp dir for chunks
        filesystem::path temp_dir = filesystem::path("./chunks") / (file_name_recv + "_chunks_tmp_") / to_string(time(nullptr));
        filesystem::create_directories(temp_dir);

        // Thread pool sizing: use hardware concurrency but cap to 16 threads
        unsigned int hw = thread::hardware_concurrency();
        if (hw == 0) hw = 2;
        unsigned int pool_size = min<unsigned int>((unsigned int)total_chunks, min<unsigned int>(hw, 16));
        ThreadPool pool(pool_size);

        atomic<int> failed_chunks{0};
        atomic<int> completed_chunks{0};
        atomic<bool> cancel_flag{false};

        // For each chunk, submit a download task
        for (int chunkno = 0; chunkno < total_chunks; ++chunkno) {
            string expected_hash = (chunkno < (int)chunk_hashes.size()) ? chunk_hashes[chunkno] : string();

            // capture file_path_recv and file_name_recv by value for use inside lambda
            pool.submit([this, &peers, chunkno, expected_hash, &temp_dir, &completed_chunks, &failed_chunks, &cancel_flag, &download_id, file_path_recv, file_name_recv]() {
                if (cancel_flag.load()) return;

                bool chunk_ok = false;
                // Try peers in order
                for (const auto& peer : peers) {
                    if (cancel_flag.load()) break;
                    // peer format ip:port
                    size_t colon = peer.find(':');
                    if (colon == string::npos) continue;
                    string ip = peer.substr(0, colon);
                    int port = stoi(peer.substr(colon + 1));
                    int psock = connect_to_server(ip, port);
                    if (psock < 0) continue;

                    // Request one chunk at a time: <file_path>$chunkno$1  (use file_path received from tracker if present)
                    string request_file = !file_path_recv.empty() ? file_path_recv : file_name_recv;
                    string request = request_file + "$" + to_string(chunkno) + "$1";
                    if (send(psock, request.c_str(), request.length(), 0) <= 0) {
                        close(psock);
                        continue;
                    }

                    string chunk_path = (temp_dir / ("chunk_" + to_string(chunkno))).string();
                    ofstream out(chunk_path, ios::binary);
                    if (!out) { close(psock); continue; }

                    vector<char> buffer(CHUNK_SIZE);
                    ssize_t received;
                    ssize_t total_received = 0;
                    while ((received = recv(psock, buffer.data(), buffer.size(), 0)) > 0) {
                        out.write(buffer.data(), received);
                        total_received += received;
                    }
                    out.close();
                    close(psock);

                    if (total_received <= 0) {
                        // nothing received
                        error_code ec;
                        filesystem::remove(chunk_path, ec);
                        continue;
                    }

                    string expected = expected_hash;
                    if (expected == "-") expected.clear();
                    if (!expected.empty()) {
                        bool ok = verify_chunk(chunk_path, expected);
                        if (!ok) {
                            error_code ec; filesystem::remove(chunk_path, ec);
                            continue;
                        }
                    }

                    // success
                    chunk_ok = true;
                    break;
                }

                if (!chunk_ok) {
                    failed_chunks++;
                    cancel_flag.store(true);
                } else {
                    completed_chunks++;
                    // update shared progress info
                    lock_guard<mutex> lock(downloads_mutex);
                    auto it = active_downloads.find(download_id);
                    if (it != active_downloads.end()) it->second.downloaded_chunks++;
                }
            });
        }

        // Wait for all tasks to finish
        pool.wait_empty();

        // After queue drained, destruct pool to join workers

        if (failed_chunks.load() > 0) {
            throw runtime_error("Failed to download one or more chunks");
        }

        // Combine chunks
        string final_temp_dir = temp_dir.string();
        if (!combine_chunks(dest_path, final_temp_dir, total_chunks)) {
            throw runtime_error("Failed to combine chunks");
        }

        // Verify final file hash if provided by FILE_INFO
        if (!file_hash_recv.empty()) {
            if (!verify_file_integrity(dest_path, file_hash_recv)) {
                throw runtime_error("Final file hash mismatch");
            }
        }

        // Cleanup active download entry
        {
            lock_guard<mutex> lock(downloads_mutex);
            // Move to completed downloads for history
            auto it = active_downloads.find(download_id);
            if (it != active_downloads.end()) {
                completed_downloads[download_id] = it->second;
                active_downloads.erase(it);
            }
        }

        cout << "Download complete: " << dest_path << endl;
        return true;
        
    } catch (const exception& e) {
        cerr << "Download failed: " << e.what() << endl;
        return false;
    }
    // 4. Verifying chunk hashes
    // 5. Combining chunks into the final file
    
    return true;
}

// Reliable send with retries
bool FileShareManager::send_with_retry(int sock, const void* buf, size_t len, int flags) const {
    int retries = 0;
    ssize_t total_sent = 0;
    
    while (total_sent < static_cast<ssize_t>(len) && retries < MAX_RETRIES) {
        ssize_t sent = send(sock, static_cast<const char*>(buf) + total_sent, 
                           len - total_sent, flags);
        if (sent <= 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                retries++;
                this_thread::sleep_for(chrono::milliseconds(100));
                continue;
            }
            return false;
        }
        total_sent += sent;
        retries = 0; // Reset retry counter on successful send
    }
    return total_sent == static_cast<ssize_t>(len);
}

// Reliable receive with retries
bool FileShareManager::recv_with_retry(int sock, void* buf, size_t len, int flags) const {
    int retries = 0;
    ssize_t total_received = 0;
    
    while (total_received < static_cast<ssize_t>(len) && retries < MAX_RETRIES) {
        ssize_t received = recv(sock, static_cast<char*>(buf) + total_received, 
                               len - total_received, flags);
        if (received <= 0) {
            if (received == 0 || errno == ECONNRESET) {
                // Connection closed by peer
                return false;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                retries++;
                this_thread::sleep_for(chrono::milliseconds(100));
                continue;
            }
            return false;
        }
        total_received += received;
        retries = 0; // Reset retry counter on successful receive
    }
    return total_received == static_cast<ssize_t>(len);
}

// Message protocol: [4-byte length][message]
bool FileShareManager::send_message(int sock, const string& message) const {
    // Send message length (network byte order)
    uint32_t len = htonl(message.length());
    if (!send_with_retry(sock, &len, sizeof(len))) {
        return false;
    }
    
    // Send message data
    return send_with_retry(sock, message.data(), message.length());
}

string FileShareManager::receive_message(int sock) const {
    // Receive message length
    uint32_t len_net;
    if (!recv_with_retry(sock, &len_net, sizeof(len_net))) {
        return "";
    }
    
    // Convert length to host byte order
    uint32_t len = ntohl(len_net);
    if (len > 10 * 1024 * 1024) { // Sanity check: max 10MB message
        return "";
    }
    
    // Receive message data
    string message(len, '\0');
    if (!recv_with_retry(sock, &message[0], len)) {
        return "";
    }
    
    return message;
}

// Show current downloads
void FileShareManager::show_downloads() const {
    lock_guard<mutex> lock(downloads_mutex);
    // Print currently downloading entries with [D]
    for (const auto& [id, info] : active_downloads) {
        double progress = (info.total_chunks > 0) ? ((info.downloaded_chunks * 100.0) / info.total_chunks) : 0.0;
        cout << "[D] [" << info.group_id << "] " << info.file_name << " - "
             << fixed << setprecision(1) << progress << "% (" << info.downloaded_chunks << "/" << info.total_chunks << ")" << endl;
    }

    // Print completed entries with [C]
    for (const auto& [id, info] : completed_downloads) {
        cout << "[C] [" << info.group_id << "] " << info.file_name << " - Completed" << endl;
    }
}

// Get file metadata
const FileMetadata* FileShareManager::get_file_metadata(const string& file_name) const {
    auto it = file_metadata.find(file_name);
    if (it == file_metadata.end()) {
        return nullptr;
    }
    return &(it->second);
}

// Check if a file is shared in a group
bool FileShareManager::is_file_shared(const string& group_id, 
                                    const string& file_name) const {
    auto group_it = shared_files.find(group_id);
    if (group_it == shared_files.end()) {
        return false;
    }
    
    return group_it->second.find(file_name) != group_it->second.end();
}

// Stop sharing a file
bool FileShareManager::stop_share(const string& group_id, 
                                const string& file_name) {
    try {
        // Step 1: Verify the file is being shared by this client
        bool file_found = false;
        {
            lock_guard<mutex> lock(shared_files_mutex);
            auto group_it = shared_files.find(group_id);
            if (group_it != shared_files.end()) {
                auto& files = group_it->second;
                file_found = (files.find(file_name) != files.end());
            }
        }
        
        if (!file_found) {
            cerr << "File not being shared by this client: " << file_name << endl;
            return false;
        }
        
        // Step 2: Notify the tracker
        int sock = connect_to_tracker();
        if (sock < 0) {
            throw runtime_error("Failed to connect to tracker");
        }
        
        string command = "stop_share " + group_id + " " + client_id + " " + file_name + "\n";
        if (send(sock, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            close(sock);
            throw runtime_error("Failed to send stop_share command");
        }

        // Read one line of response
        string response;
        char rbuf3[2048];
        ssize_t rn = recv(sock, rbuf3, sizeof(rbuf3), 0);
        if (rn > 0) {
            response.append(rbuf3, rn);
            size_t p = response.find('\n');
            if (p != string::npos) response = response.substr(0, p);
        }
        close(sock);
        
        if (response.find("SUCCESS") != 0) {
            throw runtime_error("Failed to stop sharing: " + response);
        }
        
        // Step 3: Update local state
        {
            lock_guard<mutex> lock(shared_files_mutex);
            
            // Remove from shared files
            auto group_it = shared_files.find(group_id);
            if (group_it != shared_files.end()) {
                group_it->second.erase(file_name);
                if (group_it->second.empty()) {
                    shared_files.erase(group_it);
                }
            }
            
            // Note: We don't remove file_metadata as it might be needed for active downloads
        }
        
        cout << "Stopped sharing file: " << file_name << endl;
        return true;
        
    } catch (const exception& e) {
        cerr << "Failed to stop sharing file: " << e.what() << endl;
        return false;
    }
}
