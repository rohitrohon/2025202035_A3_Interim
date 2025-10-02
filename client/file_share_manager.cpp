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

const size_t CHUNK_SIZE = 512 * 1024; // 512 KB fixed chunk size
const int SOCKET_TIMEOUT_SEC = 10; // Socket timeout in seconds

// Global control socket (if the interactive client registered one)
static int g_control_socket = -1;

// Set socket timeout
static void set_socket_timeout(int sock, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Constructor
FileShareManager::FileShareManager(const std::string& client_id, 
                                 const std::string& tracker_ip, 
                                 int tracker_port)
    : client_id(client_id), 
      tracker_ip(tracker_ip), 
      tracker_port(tracker_port) {
    // Create chunks directory if it doesn't exist
    mkdir("./chunks", 0777);
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
    extern int connect_to_tracker(const std::string&, int, const std::string&);
    return connect_to_tracker(tracker_ip, tracker_port, "");
}

// Destructor
FileShareManager::~FileShareManager() {
    // Clean up any active downloads
    {
        std::lock_guard<std::mutex> lock(downloads_mutex);
        for (const auto& [id, info] : active_downloads) {
            // Notify any waiting threads
            cv_downloads.notify_all();
        }
    }
    
    // Wait for all downloads to complete or timeout
    std::this_thread::sleep_for(std::chrono::seconds(1));
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
bool FileShareManager::send_to_tracker(const std::string& message) const {
    int sock = connect_to_tracker();
    if (sock < 0) {
        return false;
    }

    // Send a newline-terminated message and close
    std::string tosend = message;
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
bool FileShareManager::register_share_with_tracker(const std::string& group_id, 
                                                 const std::string& file_name) {
    std::string message = "SHARE " + group_id + " " + client_id + " " + file_name;
    return send_to_tracker(message);
}

// Unregister a file share with the tracker
bool FileShareManager::unregister_share_with_tracker(const std::string& group_id, 
                                                   const std::string& file_name) {
    std::string message = "STOP_SHARE " + group_id + " " + client_id + " " + file_name;
    return send_to_tracker(message);
}

// Upload a file to share
bool FileShareManager::upload_file(const std::string& group_id, 
                                 const std::string& file_path,
                                 ProgressCallback callback) {
    // Step 1: Split file into chunks and calculate hashes
    FileMetadata metadata;
    try {
    std::cout << "Splitting file into chunks..." << std::endl;
    metadata = split_file_into_chunks(file_path);
    std::cout << "Calculating file hash..." << std::endl;
    metadata.file_hash = calculate_file_hash(file_path);
        
        // Store metadata locally
        {
            std::lock_guard<std::mutex> lock(shared_files_mutex);
            file_metadata[metadata.file_name] = metadata;
            shared_files[group_id].insert(metadata.file_name);
        }
        
        // Step 2: Register with tracker
        bool sock_owned = true;
        int sock = connect_to_tracker(sock_owned);
        if (sock < 0) {
            throw std::runtime_error("Failed to connect to tracker");
        }
        // Send the upload command to the tracker (newline-terminated)
        std::string command = "upload_file " + group_id + " " + file_path + "\n";
        if (send(sock, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            close(sock);
            throw std::runtime_error("Failed to send upload command to tracker");
        }

        // Read one line of response
        auto recv_line = [&](int s)->std::string {
            std::string buf;
            char rbuf[4096];
            while (true) {
                ssize_t n = recv(s, rbuf, sizeof(rbuf), 0);
                if (n <= 0) break;
                buf.append(rbuf, n);
                size_t pos = buf.find('\n');
                if (pos != std::string::npos) {
                    std::string line = buf.substr(0, pos);
                    return line;
                }
                if (buf.size() > 10 * 1024) break; // too long
            }
            return std::string();
        };

        std::string response = recv_line(sock);
        if (response.empty()) {
            close(sock);
            throw std::runtime_error("No response or invalid response from tracker");
        }

        std::vector<std::string> tokens;
        std::istringstream iss(response);
        std::string token;
        while (iss >> token) tokens.push_back(token);
        if (tokens.empty() || tokens[0] != "PROCESS_FILE") {
            close(sock);
            throw std::runtime_error("Unexpected response from tracker: " + response);
        }
        
        // Display upload information
        std::cout << "\n=== File Upload Started ===" << std::endl;
        std::cout << "File: " << metadata.file_name << std::endl;
        std::cout << "Size: " << metadata.file_size << " bytes" << std::endl;
        std::cout << "Hash: " << metadata.file_hash << std::endl;
        std::cout << "Chunks: " << metadata.total_chunks << std::endl;
        std::cout << "Group: " << group_id << std::endl;
        
        // Send the file metadata to the tracker
        // Build update command and include per-chunk hashes so tracker can verify later
        command = "update_file_metadata " + tokens[1] + " " +
                 metadata.file_hash + " " +
                 std::to_string(metadata.file_size) + " " +
                 std::to_string(metadata.total_chunks);

        // Append each chunk's hash
        for (const auto& c : metadata.chunks) {
            command += " " + c.hash;
        }

        std::cout << "DEBUG: Sending update_file_metadata command: " << command << std::endl;
        if (send(sock, (command + "\n").c_str(), command.length() + 1, MSG_NOSIGNAL) <= 0) {
            close(sock);
            throw std::runtime_error("Failed to send file metadata");
        }

        response = recv_line(sock);
        std::cout << "DEBUG: Received response to update_file_metadata: " << response << std::endl;
    if (sock_owned) close(sock);
        
    if (response.find("SUCCESS") == 0) {
            // Convert file size to KB with 2 decimal places
            double file_size_kb = static_cast<double>(metadata.file_size) / 1024.0;
            std::cout << "\nFile Upload Complete. " 
                      << metadata.file_name << " " 
                      << std::fixed << std::setprecision(2) << file_size_kb 
                      << " KB" << std::endl;
            return true;
        } else {
            throw std::runtime_error("Failed to complete file upload: " + response);
        }
        
    } catch (const std::exception& e) {
        // Cleanup on failure
        std::cerr << "Upload failed: " << e.what() << std::endl;
        if (!metadata.file_name.empty()) {
            std::lock_guard<std::mutex> lock(shared_files_mutex);
            shared_files[group_id].erase(metadata.file_name);
            file_metadata.erase(metadata.file_name);
        }
        return false;
    }
}

// List files in a group from tracker
std::vector<std::string> FileShareManager::list_files(const std::string& group_id) const {
    std::vector<std::string> files;
    
    try {
        // Connect to tracker
        int sock = connect_to_tracker();
        if (sock < 0) {
            std::cerr << "Failed to connect to tracker" << std::endl;
            return {};
        }

        std::string command = "list_files " + group_id + "\n";
        if (send(sock, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            close(sock);
            return {};
        }

        // Read one line response
        std::string buf;
        char rbuf[4096];
        while (true) {
            ssize_t n = recv(sock, rbuf, sizeof(rbuf), 0);
            if (n <= 0) break;
            buf.append(rbuf, n);
            size_t pos = buf.find('\n');
            if (pos != std::string::npos) {
                std::string line = buf.substr(0, pos);
                // parse line: count <files...> or directly files
                std::istringstream iss(line);
                std::string token;
                while (std::getline(iss, token, ' ')) {
                    if (!token.empty()) files.push_back(token);
                }
                break;
            }
            if (buf.size() > 16 * 1024) break;
        }
        close(sock);
    } catch (const std::exception& e) {
        std::cerr << "Error listing files: " << e.what() << std::endl;
    }
    
    return files;
}

// Download a file from the network
bool FileShareManager::download_file(const std::string& group_id, 
                                   const std::string& file_name, 
                                   const std::string& dest_path) {
    try {
        // Step 1: Get file info from tracker (reuse control socket if registered)
        bool sock_owned = true;
        int sock = connect_to_tracker(sock_owned);
        if (sock < 0) {
            throw std::runtime_error("Failed to connect to tracker");
        }

        // Use tracker download_file to get FILE_INFO and peers
        std::string command = "download_file " + group_id + " " + file_name + "\n";
        if (send(sock, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            if (sock_owned) close(sock);
            throw std::runtime_error("Failed to request file info");
        }

        // Read one line of response
        std::string buf;
        char rbuf[8192];
        ssize_t n = recv(sock, rbuf, sizeof(rbuf), 0);
        if (n <= 0) {
            if (sock_owned) close(sock);
            throw std::runtime_error("No response from tracker");
        }
        buf.append(rbuf, n);
        // Stop at newline
        size_t pos = buf.find('\n');
        std::string line = (pos == std::string::npos) ? buf : buf.substr(0, pos);
        if (sock_owned) close(sock);

        // Expected format: FILE_INFO <file_name> <file_size> <total_chunks> <num_peers> <peer1> <peer2> ...
        std::istringstream iss(line);
        std::string header;
        iss >> header;
        if (header != "FILE_INFO") {
            throw std::runtime_error("Invalid FILE_INFO from tracker: " + line);
        }
        uint64_t file_size;
        std::string file_hash_or_name;
        int total_chunks;
        int num_peers;
        iss >> file_hash_or_name >> file_size >> total_chunks >> num_peers;
        std::vector<std::string> peers;
        for (int i = 0; i < num_peers; ++i) {
            std::string p; if (!(iss >> p)) break; peers.push_back(p);
        }
        
        // Step 2: Create download directory if it doesn't exist
        std::filesystem::path dest_dir = std::filesystem::path(dest_path).parent_path();
        if (!dest_dir.empty() && !std::filesystem::exists(dest_dir)) {
            std::filesystem::create_directories(dest_dir);
        }
        
        // Step 3: Prepare download info
        std::string download_id = group_id + ":" + file_name + ":" + std::to_string(std::time(nullptr));
        
        DownloadInfo info;
        info.file_name = file_name;
        info.dest_path = dest_path;
        info.total_chunks = total_chunks;
        info.downloaded_chunks = 0;
        info.start_time = std::chrono::system_clock::now();
        
        // Add to active downloads
        {
            std::lock_guard<std::mutex> lock(downloads_mutex);
            active_downloads[download_id] = info;
        }
        
        // Step 4: Get list of peers with the file
        // Reuse control socket if we have one registered
        bool sock2_owned = true;
        int sock2 = connect_to_tracker(sock2_owned);
        if (sock2 < 0) {
            throw std::runtime_error("Failed to connect to tracker for peer list");
        }

        command = "get_peers " + group_id + " " + file_hash_or_name + "\n";
        if (send(sock2, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            if (sock2_owned) close(sock2);
            throw std::runtime_error("Failed to request peer list");
        }

        // Read response lines until newline
        std::string response;
        char rbuf2[8192];
        while (true) {
            ssize_t rn = recv(sock2, rbuf2, sizeof(rbuf2), 0);
            if (rn <= 0) break;
            response.append(rbuf2, rn);
            size_t p = response.find('\n');
            if (p != std::string::npos) {
                response = response.substr(0, p);
                break;
            }
            if (response.size() > 64 * 1024) break;
        }
    if (sock2_owned) close(sock2);
        
        // Parse peer list
        std::istringstream peer_stream(response);
        std::string peer_addr;
        while (std::getline(peer_stream, peer_addr)) {
            if (!peer_addr.empty()) {
                peers.push_back(peer_addr);
            }
        }
        
        if (peers.empty()) {
            throw std::runtime_error("No peers available for this file");
        }
        
    // Step 5: Parse optional chunk hashes appended after peers
        std::vector<std::string> chunk_hashes;
        // Any remaining tokens in 'line' after peers are chunk hashes
        // We already parsed peers from the first FILE_INFO line; try to extract hashes from that line too
        {
            // Re-parse the FILE_INFO line to extract everything after peers
            std::istringstream iss2(line);
            std::string tok;
            std::vector<std::string> all_tokens;
            while (iss2 >> tok) all_tokens.push_back(tok);
            // FILE_INFO name size total_chunks num_peers peer1 ... peerN [hash1 ...]
            int header_count = 5 + num_peers; // FILE_INFO + name + size + total_chunks + num_peers + peers
            if ((int)all_tokens.size() > header_count) {
                for (int i = header_count; i < (int)all_tokens.size(); ++i) {
                    chunk_hashes.push_back(all_tokens[i]);
                }
            }
        }

        // Debug: print parsed info
        std::cout << "DEBUG: Parsed FILE_INFO - file='" << file_name << "' size=" << file_size
                  << " total_chunks=" << total_chunks << " num_peers=" << num_peers << std::endl;
        std::cout << "DEBUG: Peers list (" << peers.size() << "):";
        for (const auto &p : peers) std::cout << " '" << p << "'";
        std::cout << std::endl;
        if (!chunk_hashes.empty()) {
            std::cout << "DEBUG: Received " << chunk_hashes.size() << " chunk hashes" << std::endl;
        } else {
            std::cout << "DEBUG: No per-chunk hashes provided by tracker" << std::endl;
        }

        // Prepare temp dir for chunks
        std::filesystem::path temp_dir = std::filesystem::path("./chunks") / (file_name + "_chunks_tmp_") / std::to_string(std::time(nullptr));
        std::filesystem::create_directories(temp_dir);

        // Helper to download a single chunk from a peer
        auto download_chunk_from_peer = [&](const std::string& peer, int chunkno, const std::string& expected_hash)->bool {
            // peer format ip:port
            size_t colon = peer.find(':');
            if (colon == std::string::npos) return false;
            std::string ip = peer.substr(0, colon);
            int port = stoi(peer.substr(colon + 1));
            std::cout << "DEBUG: Attempting connection to peer " << ip << ":" << port << " for chunk " << chunkno << std::endl;
            int psock = connect_to_server(ip, port);
            if (psock < 0) {
                std::cout << "DEBUG: connect_to_server failed for " << ip << ":" << port << std::endl;
                return false;
            }

            // Request one chunk at a time: filepath$chunkno$1
            std::string request = file_name + "$" + std::to_string(chunkno) + "$1";
            std::cout << "DEBUG: Sending peer request: '" << request << "'" << std::endl;
            if (send(psock, request.c_str(), request.length(), 0) <= 0) {
                std::cout << "DEBUG: Failed to send request to peer " << ip << ":" << port << std::endl;
                close(psock);
                return false;
            }

            std::string chunk_path = (temp_dir / ("chunk_" + std::to_string(chunkno))).string();
            std::ofstream out(chunk_path, std::ios::binary);
            if (!out) {
                std::cout << "DEBUG: Failed to open chunk file for writing: " << chunk_path << std::endl;
                close(psock);
                return false;
            }

            char buffer[CHUNK_SIZE];
            ssize_t received;
            ssize_t total_received = 0;
            // Read until peer closes or until we see no more data
            while ((received = recv(psock, buffer, sizeof(buffer), 0)) > 0) {
                out.write(buffer, received);
                total_received += received;
            }
            out.close();
            if (received == 0) {
                // peer closed connection normally
            } else if (received < 0) {
                std::cout << "DEBUG: recv() error while downloading chunk " << chunkno << ": " << strerror(errno) << std::endl;
            }
            close(psock);

            std::cout << "DEBUG: Received " << total_received << " bytes for chunk " << chunkno << " from " << ip << ":" << port << std::endl;

            // Verify chunk if expected hash provided
            if (!expected_hash.empty()) {
                bool ok = verify_chunk(chunk_path, expected_hash);
                std::cout << "DEBUG: Chunk " << chunkno << " verification result: " << (ok ? "OK" : "FAIL") << std::endl;
                if (!ok) {
                    // remove bad file
                    std::error_code ec;
                    std::filesystem::remove(chunk_path, ec);
                    if (ec) std::cout << "DEBUG: Failed to remove bad chunk file: " << ec.message() << std::endl;
                    return false;
                }
            }
            return total_received > 0;
        };

        // Download each chunk by trying peers in round-robin until success
        for (int chunkno = 0; chunkno < total_chunks; ++chunkno) {
            bool chunk_ok = false;
            std::string expected_hash = (chunkno < (int)chunk_hashes.size()) ? chunk_hashes[chunkno] : std::string();

            for (const auto& peer : peers) {
                if (download_chunk_from_peer(peer, chunkno, expected_hash)) {
                    chunk_ok = true;
                    break;
                }
            }

            if (!chunk_ok) {
                throw std::runtime_error("Failed to download chunk " + std::to_string(chunkno));
            }

            // Update progress
            {
                std::lock_guard<std::mutex> lock(downloads_mutex);
                auto it = active_downloads.find(download_id);
                if (it != active_downloads.end()) {
                    it->second.downloaded_chunks++;
                }
            }
        }

        // Combine chunks
        std::string final_temp_dir = temp_dir.string();
        if (!combine_chunks(dest_path, final_temp_dir, total_chunks)) {
            throw std::runtime_error("Failed to combine chunks");
        }

        // Verify final file hash if provided (some trackers may send file hash in other responses)
        if (!file_hash_or_name.empty()) {
            try {
                if (!verify_file_integrity(dest_path, file_hash_or_name)) {
                    throw std::runtime_error("Final file hash mismatch");
                }
            } catch (...) {
                throw;
            }
        }

        // Cleanup active download entry
        {
            std::lock_guard<std::mutex> lock(downloads_mutex);
            active_downloads.erase(download_id);
        }

        std::cout << "Download complete: " << dest_path << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Download failed: " << e.what() << std::endl;
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
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
bool FileShareManager::send_message(int sock, const std::string& message) const {
    // Send message length (network byte order)
    uint32_t len = htonl(message.length());
    if (!send_with_retry(sock, &len, sizeof(len))) {
        return false;
    }
    
    // Send message data
    return send_with_retry(sock, message.data(), message.length());
}

std::string FileShareManager::receive_message(int sock) const {
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
    std::string message(len, '\0');
    if (!recv_with_retry(sock, &message[0], len)) {
        return "";
    }
    
    return message;
}

// Show current downloads
void FileShareManager::show_downloads() const {
    std::lock_guard<std::mutex> lock(downloads_mutex);
    
    if (active_downloads.empty()) {
        std::cout << "No active downloads" << std::endl;
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    
    std::cout << "\nActive Downloads:";
    std::cout << "\n--------------------------------------------------" << std::endl;
    
    for (const auto& [id, info] : active_downloads) {
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - info.start_time).count();
            
        double speed = (info.downloaded_chunks * CHUNK_SIZE) / 
                      (duration > 0 ? duration : 1) / 1024.0; // KB/s
        
        double progress = (info.downloaded_chunks * 100.0) / info.total_chunks;
        
        std::cout << "File: " << info.file_name << "\n"
                  << "  Progress: " << std::fixed << std::setprecision(1) << progress 
                  << "% (" << info.downloaded_chunks << "/" << info.total_chunks << " chunks)\n"
                  << "  Speed: " << std::fixed << std::setprecision(1) << speed << " KB/s\n"
                  << "  Time: " << duration << "s\n"
                  << "  Destination: " << info.dest_path << "\n"
                  << std::endl;
    }
    
    std::cout << "--------------------------------------------------" << std::endl;
}

// Get file metadata
const FileMetadata* FileShareManager::get_file_metadata(const std::string& file_name) const {
    auto it = file_metadata.find(file_name);
    if (it == file_metadata.end()) {
        return nullptr;
    }
    return &(it->second);
}

// Check if a file is shared in a group
bool FileShareManager::is_file_shared(const std::string& group_id, 
                                    const std::string& file_name) const {
    auto group_it = shared_files.find(group_id);
    if (group_it == shared_files.end()) {
        return false;
    }
    
    return group_it->second.find(file_name) != group_it->second.end();
}

// Stop sharing a file
bool FileShareManager::stop_share(const std::string& group_id, 
                                const std::string& file_name) {
    try {
        // Step 1: Verify the file is being shared by this client
        bool file_found = false;
        {
            std::lock_guard<std::mutex> lock(shared_files_mutex);
            auto group_it = shared_files.find(group_id);
            if (group_it != shared_files.end()) {
                auto& files = group_it->second;
                file_found = (files.find(file_name) != files.end());
            }
        }
        
        if (!file_found) {
            std::cerr << "File not being shared by this client: " << file_name << std::endl;
            return false;
        }
        
        // Step 2: Notify the tracker
        int sock = connect_to_tracker();
        if (sock < 0) {
            throw std::runtime_error("Failed to connect to tracker");
        }
        
        std::string command = "stop_share " + group_id + " " + client_id + " " + file_name + "\n";
        if (send(sock, command.c_str(), command.length(), MSG_NOSIGNAL) <= 0) {
            close(sock);
            throw std::runtime_error("Failed to send stop_share command");
        }

        // Read one line of response
        std::string response;
        char rbuf3[2048];
        ssize_t rn = recv(sock, rbuf3, sizeof(rbuf3), 0);
        if (rn > 0) {
            response.append(rbuf3, rn);
            size_t p = response.find('\n');
            if (p != std::string::npos) response = response.substr(0, p);
        }
        close(sock);
        
        if (response.find("SUCCESS") != 0) {
            throw std::runtime_error("Failed to stop sharing: " + response);
        }
        
        // Step 3: Update local state
        {
            std::lock_guard<std::mutex> lock(shared_files_mutex);
            
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
        
        std::cout << "Stopped sharing file: " << file_name << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Failed to stop sharing file: " << e.what() << std::endl;
        return false;
    }
}
