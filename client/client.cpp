#include "client.h"
#include "network_utils.h"
#include "file_operations.h"
#include "file_share_manager.h"
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <errno.h>
#include <netdb.h>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <set>
#include <atomic>
#include <openssl/sha.h>

using namespace std;
using namespace std::filesystem;

// Validate and sanitize file path to prevent directory traversal
bool is_valid_path(const std::string& path) {
    // Disallow parent-directory traversal. Allow absolute and relative paths.
    if (path.empty() || path.find("../") != std::string::npos) {
        std::cerr << "DEBUG: is_valid_path rejected path: '" << path << "'" << std::endl;
        return false;
    }
    return true;
}

// Send error response to peer
void send_error_response(int sock, const std::string& message) {
    std::string response = "ERROR: " + message + "\n";
    send(sock, response.c_str(), response.length(), 0);
}

// Global listener control
static std::atomic<bool> g_listen_run{true};
static int g_server_fd = -1;

// Handle a single peer connection with improved security and error handling
void handle_peer_connection(int peer_sock) {
    // Set receive timeout (5 seconds)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(peer_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    char buffer[512 * 1024] = {0};
    ssize_t valread = read(peer_sock, buffer, sizeof(buffer) - 1);
    
    if (valread <= 0) {
        close(peer_sock);
        return;
    }
    
    buffer[valread] = '\0';
    string request = string(buffer);
    // Debug: log the raw peer request
    std::cout << "DEBUG: Peer request received: '" << request << "' (len=" << valread << ")" << std::endl;
    std::cout << "DEBUG: Peer request hex: ";
    for (int i = 0; i < std::min((ssize_t)valread, (ssize_t)128); ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)buffer[i] << " ";
    }
    std::cout << std::dec << std::endl;
    
    // Parse request: format "filepath$chunkno$nchunks"
    size_t one = request.find('$');
    size_t two = request.find('$', one + 1);
    
    if (one == string::npos || two == string::npos) {
        send_error_response(peer_sock, "Invalid request format. Use: filepath$chunkno$nchunks");
        close(peer_sock);
        return;
    }
    
    string filepath = request.substr(0, one);
    
    // Validate file path
    if (!is_valid_path(filepath)) {
        send_error_response(peer_sock, "Invalid file path");
        close(peer_sock);
        return;
    }
    
    // Parse chunk numbers with error handling
    int chunkno, nchunks;
    try {
        chunkno = stoi(request.substr(one + 1, two - one - 1));
        nchunks = stoi(request.substr(two + 1));
        
        // Validate chunk numbers
        if (chunkno < 0 || nchunks <= 0 || nchunks > 1000) {  // Arbitrary max chunks limit
            throw std::out_of_range("Invalid chunk range");
        }
    } catch (const std::exception& e) {
        send_error_response(peer_sock, "Invalid chunk numbers");
        close(peer_sock);
        return;
    }
    
    // Open file with error handling
    std::cout << "DEBUG: Attempting to open file for peer request: '" << filepath << "'" << std::endl;
    int file = open(filepath.c_str(), O_RDONLY);
    if (file < 0) {
        std::cerr << "DEBUG: open() failed for '" << filepath << "': " << strerror(errno) << std::endl;
        send_error_response(peer_sock, "File not found or permission denied");
        close(peer_sock);
        return;
    }
    
    // Get file size to validate chunk request
    struct stat file_stat;
    if (fstat(file, &file_stat) < 0) {
        send_error_response(peer_sock, "Could not get file information");
        close(file);
        close(peer_sock);
        return;
    }
    
    off_t file_size = file_stat.st_size;
    off_t chunk_size = 512 * 1024;  // 512KB chunks
    off_t start_pos = chunk_size * chunkno;
    off_t requested_end = start_pos + (chunk_size * nchunks);
    off_t end_pos = (requested_end < file_size) ? requested_end : file_size;
    
    // Validate chunk range
    if (start_pos >= file_size) {
        send_error_response(peer_sock, "Chunk number out of range");
        close(file);
        close(peer_sock);
        return;
    }
    
    // Seek to the start position
    if (lseek(file, start_pos, SEEK_SET) < 0) {
        send_error_response(peer_sock, "Error seeking to chunk position");
        close(file);
        close(peer_sock);
        return;
    }
    
    // Send file data in chunks
    ssize_t bytesRead, totalBytesSent = 0;
    ssize_t remaining = end_pos - start_pos;
    
    // Send file data in chunks
    while (remaining > 0) {
        // Determine the size of the next chunk to send (up to buffer size)
        ssize_t chunk_size_to_send = std::min(remaining, static_cast<ssize_t>(sizeof(buffer)));
        
        // Read the chunk from the file
        bytesRead = read(file, buffer, chunk_size_to_send);
        if (bytesRead <= 0) {
            if (errno == EINTR) continue;  // Interrupted, try again
            cerr << "Error reading file: " << strerror(errno) << "\n";
            break;
        }
        
        // Send the chunk
        ssize_t bytesSent = 0;
        while (bytesSent < bytesRead) {
            ssize_t sent = send(peer_sock, buffer + bytesSent, bytesRead - bytesSent, 
                              MSG_NOSIGNAL);  // Don't generate SIGPIPE
            
            if (sent < 0) {
                if (errno == EINTR) continue;  // Interrupted, try again
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Socket buffer full, wait a bit
                    struct pollfd pfd = {peer_sock, POLLOUT, 0};
                    poll(&pfd, 1, 1000);  // Wait up to 1 second
                    continue;
                }
                cerr << "Error sending data: " << strerror(errno) << "\n";
                break;
            }
            bytesSent += sent;
        }
        
        if (bytesSent < bytesRead) {
            cerr << "Failed to send complete chunk\n";
            break;
        }
        
        totalBytesSent += bytesSent;
        remaining -= bytesSent;
        
        // Log progress for large files
        if (file_size > 10 * 1024 * 1024) {  // For files > 10MB
            static int last_percent = -1;
            int percent = (totalBytesSent * 100) / (end_pos - start_pos);
            if (percent != last_percent && percent % 10 == 0) {
                cout << "Upload progress: " << percent << "%\r" << flush;
                last_percent = percent;
            }
        }
    }
    
    if (totalBytesSent > 0) {
        cout << "\nSent " << totalBytesSent << " bytes of file " << filepath 
             << " (chunk " << chunkno << ")" << endl;
    } else {
        cerr << "No data sent for file: " << filepath << endl;
    }
    
    string filename = get_file_name_from_path(filepath);
    
    close(peer_sock);
    close(file);
}

// Start listening for incoming peer connections
void start_listening(int listen_port) {
    int new_socket;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);
    
    // Create socket file descriptor
    if ((g_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        cerr << "Socket creation failed: " << strerror(errno) << endl;
        return;
    }
    
    // Set socket options: set SO_REUSEADDR and (where available) SO_REUSEPORT separately
    if (setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        cerr << "Warning: setsockopt(SO_REUSEADDR) failed: " << strerror(errno) << endl;
        // Not fatal, continue
    }
#ifdef SO_REUSEPORT
    if (setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        // Some platforms may not support SO_REUSEPORT; log but don't fatal
        cerr << "Note: setsockopt(SO_REUSEPORT) failed or not supported: " << strerror(errno) << endl;
    }
#endif
    
    // Set non-blocking mode
    int flags = fcntl(g_server_fd, F_GETFL, 0);
    if (flags == -1) {
        cerr << "F_GETFL failed: " << strerror(errno) << endl;
        close(g_server_fd);
        return;
    }
    
    if (fcntl(g_server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        cerr << "F_SETFL O_NONBLOCK failed: " << strerror(errno) << endl;
        close(g_server_fd);
        return;
    }
    
    // Bind socket to the port
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(listen_port));
    
    if (::bind(g_server_fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0) {
        cerr << "Bind failed: " << strerror(errno) << endl;
        close(g_server_fd);
        return;
    }
    
    // Start listening
    if (listen(g_server_fd, SOMAXCONN) < 0) {
        cerr << "Listen failed: " << strerror(errno) << endl;
        close(g_server_fd);
        return;
    }
    
    cout << "Listening for incoming connections on port " << listen_port << "..." << endl;
    
    struct pollfd fds[1];
    fds[0].fd = g_server_fd;
    fds[0].events = POLLIN;
    
    while (g_listen_run.load()) {
        // Wait for activity on the server socket with a timeout
        int activity = poll(fds, 1, 1000); // 1 second timeout
        
        if (activity < 0) {
            if (errno == EINTR) continue; // Interrupted by signal
            cerr << "Poll error: " << strerror(errno) << endl;
            break;
        } else if (activity == 0) {
            // Timeout occurred, check for shutdown condition if needed
            continue;
        }
        
        // Check if there's an incoming connection
        if (fds[0].revents & POLLIN) {
            // Accept the connection
            new_socket = accept(g_server_fd, (struct sockaddr *)&address, &addrlen);
            if (new_socket < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue; // No pending connections
                }
                cerr << "Accept failed: " << strerror(errno) << endl;
                continue;
            }
            
            // Set socket options for the new connection
            int opt = 1;
            setsockopt(new_socket, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
            
            // Get client IP for logging
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &address.sin_addr, client_ip, INET_ADDRSTRLEN);
            
            cout << "New connection from " << client_ip << ":" << ntohs(address.sin_port) << endl;
            
            // Create a new thread to handle the connection
            try {
                thread(handle_peer_connection, new_socket).detach();
            } catch (const std::exception& e) {
                cerr << "Failed to create thread: " << e.what() << endl;
                close(new_socket);
            }
        }
    }
    
    // Cleanup
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

// get_file_name_from_path is defined in file_operations.cpp

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cout << "Invalid parameter Format: ./client <IP>:<PORT> tracker_info.txt" << endl;
        return 0;
    }
    
    string ip_and_port = argv[1];
    string tracker_filename = argv[2];
    
    int i = ip_and_port.find(':');
    if (i == string::npos || i == 0) {
        cout << "Invalid format for <IP>:<PORT>" << endl;
        return 0;
    }

    string ip_address = ip_and_port.substr(0, i);
    int listen_port = stoi(ip_and_port.substr(i + 1));
    
    // Start listening for peer connections
    thread listening_thread(start_listening, listen_port);
    listening_thread.detach();
    
    // Read tracker info from client file and merge with tracker/tracker_info.txt to deduplicate
    int fd = open(tracker_filename.c_str(), O_RDONLY);
    if (fd < 0) {
        cerr << "Failed to open " << tracker_filename << endl;
        return 1;
    }

    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    ssize_t bytesRead = read(fd, buffer, BUFFER_SIZE - 1);
    if (bytesRead < 0) {
        cerr << "Failed to read from file." << endl;
        close(fd);
        return 1;
    }

    buffer[bytesRead] = '\0'; 
    close(fd);
    
    stringstream ss(buffer);
    string line;
    
    // Build an ordered, de-duplicated list of trackers (preserve client file order)
    vector<string> tracker_list;
    unordered_set<string> seen;
    while (getline(ss, line)) {
        // trim whitespace
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](int ch){ return !std::isspace(ch); }));
        line.erase(std::find_if(line.rbegin(), line.rend(), [](int ch){ return !std::isspace(ch); }).base(), line.end());
        if (!line.empty() && !seen.count(line)) { tracker_list.push_back(line); seen.insert(line); }
    }

    // Optionally merge in tracker-side file entries after client entries (preserve relative order)
    {
        const char* tracker_side = "../tracker/tracker_info.txt";
        ifstream tin(tracker_side);
        if (tin) {
            string tline;
            while (getline(tin, tline)) {
                tline.erase(tline.begin(), std::find_if(tline.begin(), tline.end(), [](int ch){ return !std::isspace(ch); }));
                tline.erase(std::find_if(tline.rbegin(), tline.rend(), [](int ch){ return !std::isspace(ch); }).base(), tline.end());
                if (!tline.empty() && !seen.count(tline)) { tracker_list.push_back(tline); seen.insert(tline); }
            }
        }
    }

    // tracker_list now contains ordered, de-duplicated trackers; do not rewrite file
    int sock = -1;
    string connected_tracker;
    // FileShareManager will be created once we connect to a tracker
    std::unique_ptr<FileShareManager> fsm;
    
    // Try each tracker in sequence until one connects successfully
    for (const string& line_ep : tracker_list) {
        // Parse tracker IP and port
        size_t colon_pos = line_ep.find(':');
        if (colon_pos == string::npos || colon_pos == 0) {
            cout << "Invalid format in tracker info file: " << line_ep << endl;
            continue;
        }

        string track_ip = line_ep.substr(0, colon_pos);
        int track_port;
        try {
            track_port = stoi(line_ep.substr(colon_pos + 1));
        } catch (const std::exception& e) {
            cout << "Invalid port number in tracker info: " << line_ep << endl;
            continue;
        }
        
        cout << "Trying to connect to tracker " << track_ip << ":" << track_port << "..." << endl;
        
        // Try to connect to this tracker
        // Send the client's listening port as part of the connection info
        // Format: "PORT <port>"
    // Send full listening address (ip:port) so tracker gets explicit peer address
    string client_info = ip_address + ":" + to_string(listen_port);
    client_info = "PORT " + client_info;
        sock = connect_to_tracker(track_ip, track_port, client_info);
        if (sock >= 0) {
            connected_tracker = track_ip + ":" + to_string(track_port);
            cout << "Connected to tracker " << connected_tracker << " (listening on port " << listen_port << ")" << endl;
            // Instantiate FileShareManager tied to this tracker
            fsm.reset(new FileShareManager("", track_ip, track_port));
            // Provide the control socket to the FileShareManager so it can reuse it
            fsm->set_control_socket(sock);
            // Send a newline to ensure the message is flushed
            send(sock, "\n", 1, 0);
            break;
        }
        
        cout << "Failed to connect to tracker " << track_ip << ":" << track_port << endl;
    }
    
    if (sock < 0) {
        cout << "Failed to connect to any tracker. Please check if any tracker is running." << endl;
        return 0;
    }
    
    // Function to trim whitespace from a string
    auto trim = [](string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
            return !std::isspace(ch);
        }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
            return !std::isspace(ch);
        }).base(), s.end());
        return s;
    };

    // Read and discard any initial data (like welcome message or newline)
    char initial_buffer[1024];
    fd_set readfds;
    struct timeval tv;
    
    // Set up the timeout
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms timeout
    
    // Check if there's any data to read
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    
    if (select(sock + 1, &readfds, NULL, NULL, &tv) > 0) {
        // Data is available, read and discard it
        recv(sock, initial_buffer, sizeof(initial_buffer) - 1, 0);
    }
    
    // Track last upload path (used when tracker asks PROCESS_FILE)
    string last_upload_path;

    // Main command loop
    while (true) {
        string command;
        cout << "> ";
        cout.flush();  // Ensure the prompt is displayed
        getline(cin, command);
        
        // Trim the command
        command = trim(command);
        
        if (command.empty()) {
            continue;
        }
        
        // Convert to lowercase for command matching
        string lower_command = command;
        std::transform(lower_command.begin(), lower_command.end(), lower_command.begin(), 
                      [](unsigned char c){ return std::tolower(c); });
        
        // Special case for quit command
        if (lower_command == "quit" || lower_command == "exit") {
            break;
        }
        
        // Tokenize command to special-case certain commands locally
        istringstream pre_ss(command);
        vector<string> pre_toks; string pre_tk;
        while (pre_ss >> pre_tk) pre_toks.push_back(pre_tk);

        // If this is a download_file command, run it locally using FileShareManager
        if (!pre_toks.empty() && pre_toks[0] == "download_file" && fsm) {
            if (pre_toks.size() < 4) {
                cout << "Usage: download_file <group_id> <file_name> <dest_path>" << endl;
            } else {
                string group_id = pre_toks[1];
                string file_name = pre_toks[2];
                string dest_path = pre_toks[3];
                cout << "DEBUG: Running local download_file via FileShareManager" << endl;
                bool ok = fsm->download_file(group_id, file_name, dest_path);
                if (!ok) cout << "Download failed." << endl;
            }
            continue; // skip sending the command to the tracker
        }

        // Do not intercept show_downloads; let it go to tracker

        // Send the command with a newline terminator
        string command_to_send = command + "\n";
        
        // Debug: Print what we're about to send
        cout << "DEBUG: Sending command: '" << command_to_send << "' (length: " << command_to_send.length() << ")" << endl;
        cout << "DEBUG: Hex dump: ";
        for (char c : command_to_send) {
            cout << hex << setw(2) << setfill('0') << (int)(unsigned char)c << " ";
        }
        cout << dec << endl;
        
        ssize_t total_sent = 0;
        ssize_t bytes_sent;
        
        // Keep sending until all bytes are sent
        while (total_sent < command_to_send.length()) {
            bytes_sent = send(sock, command_to_send.c_str() + total_sent, 
                             command_to_send.length() - total_sent, 0);
            if (bytes_sent <= 0) {
                cout << "Failed to send command to tracker" << endl;
                continue;
            }
            total_sent += bytes_sent;
        }
        
        // Read and display the response
        char response[1024] = {0};
        ssize_t valread = recv(sock, response, sizeof(response) - 1, 0);
        
        if (valread <= 0) {
            cout << "Tracker disconnected!" << endl;
            break;
        }

        string resp_str(response);
        cout << resp_str;
        if (!resp_str.empty() && resp_str.back() != '\n') cout << endl;

        // If this was a login command and login succeeded, inform FileShareManager of our user id
        if (!pre_toks.empty() && pre_toks[0] == "login" && fsm) {
            if (resp_str.find("Login successful") == 0 || resp_str.find("Login successful") != string::npos) {
                // pre_toks[1] is the user id
                try {
                    fsm->set_client_id(pre_toks[1]);
                } catch (...) {
                    // Ignore if fsm not set or method not available
                }
            }
        }

        // If this was an upload_file command, remember the local path so we can process PROCESS_FILE
        // Parse the original command_to_send (without trailing newline)
        {
            string cmd_copy = command; // original user command (trimmed)
            // Simple tokenization
            istringstream css(cmd_copy);
            vector<string> toks;
            string tk;
            while (css >> tk) toks.push_back(tk);
            if (!toks.empty() && toks[0] == "upload_file" && toks.size() >= 3) {
                // last token is the file path argument (may contain no spaces in this simple parser)
                last_upload_path = toks.back();
            }
        }

        // Handle special tracker responses
        // Detect PROCESS_FILE <temp_file_id> <file_name> <file_size>
        {
            istringstream riss(resp_str);
            vector<string> rtoks; string rtk;
            while (riss >> rtk) rtoks.push_back(rtk);
            if (!rtoks.empty() && rtoks[0] == "PROCESS_FILE") {
                if (rtoks.size() >= 4) {
                    string temp_file_id = rtoks[1];
                    // string file_name = rtoks[2];
                    // string file_size_str = rtoks[3];

                    if (last_upload_path.empty()) {
                        cout << "No local path recorded for upload; cannot process file metadata" << endl;
                    } else {
                        try {
                            // Compute metadata locally
                            FileMetadata meta = split_file_into_chunks(last_upload_path);
                            meta.file_hash = calculate_file_hash(last_upload_path);

                            // Build update command including per-chunk hashes
                            string update_cmd = "update_file_metadata " + temp_file_id + " " +
                                meta.file_hash + " " + to_string(meta.file_size) + " " + to_string(meta.total_chunks);
                            for (const auto &c : meta.chunks) {
                                update_cmd += " " + c.hash;
                            }
                            update_cmd += "\n";

                            // Send update command on same socket
                            ssize_t sent = send(sock, update_cmd.c_str(), update_cmd.length(), 0);
                            if (sent <= 0) {
                                cout << "Failed to send update_file_metadata to tracker" << endl;
                            } else {
                                // Wait for tracker's response
                                char upd_resp[2048] = {0};
                                ssize_t rn = recv(sock, upd_resp, sizeof(upd_resp)-1, 0);
                                if (rn > 0) {
                                    cout << string(upd_resp, rn);
                                }
                            }
                        } catch (const std::exception& e) {
                            cout << "Error processing file: " << e.what() << endl;
                        }
                        // Clear last_upload_path after processing
                        last_upload_path.clear();
                    }
                } else {
                    cout << "Malformed PROCESS_FILE response from tracker" << endl;
                }
            }
        }
    }
    
    // Stop listener gracefully
    g_listen_run.store(false);
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
        close(g_server_fd);
        g_server_fd = -1;
    }

    close(sock);
    return 0;
}

// Helper function to communicate with tracker
bool communicate_with_tracker(const std::string& tracker_url, 
                            const std::string& request, 
                            std::string& response) {
    // Parse tracker URL (format: "host:port")
    size_t colon_pos = tracker_url.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    
    std::string host = tracker_url.substr(0, colon_pos);
    int port = std::stoi(tracker_url.substr(colon_pos + 1));
    
    // Create socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return false;
    }
    
    // Set up server address
    struct sockaddr_in server_addr;
    struct hostent *server;
    
    server = gethostbyname(host.c_str());
    if (server == nullptr) {
        close(sockfd);
        return false;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);
    
    // Connect to server
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return false;
    }
    
    // Send request
    if (send(sockfd, request.c_str(), request.length(), 0) < 0) {
        close(sockfd);
        return false;
    }
    
    // Receive response
    char buffer[4096];
    ssize_t bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    close(sockfd);
    
    if (bytes_received <= 0) {
        return false;
    }
    
    buffer[bytes_received] = '\0';
    response = std::string(buffer);
    return true;
}