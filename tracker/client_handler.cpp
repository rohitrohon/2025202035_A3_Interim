#include "tracker.h"
#include "client_handler.h"
#include "data_structures.h"
#include <sstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <openssl/sha.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/err.h>
#include <sstream>
#include <thread>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <errno.h>
#include <mutex>
#include <map>

// External declarations from data_structures.cpp
extern std::unordered_map<std::string, std::map<long long, std::string>> group_join_times;
extern pthread_mutex_t group_join_times_mutex;

// Forward declarations for socket functions to avoid conflicts
extern "C" {
    int accept(int, struct sockaddr*, socklen_t*);
    int bind(int, const struct sockaddr*, socklen_t);
    int connect(int, const struct sockaddr*, socklen_t);
    int socket(int, int, int);
    int close(int);
}

// Using declarations for standard library components we use
using std::string;
using std::vector;

// Map socket fd -> client's listening port (from PORT command)
static std::unordered_map<int,int> socket_listen_port;
static pthread_mutex_t socket_listen_port_mutex = PTHREAD_MUTEX_INITIALIZER;

// Send a response to a client
void send_response(int client_sock, const std::string& response) {
    // Add newline if not present
    std::string response_with_newline = response;
    if (!response_with_newline.empty() && response_with_newline.back() != '\n') {
        response_with_newline += "\n";
    }
    
    // Send the response
    ssize_t bytes_sent = send(client_sock, response_with_newline.c_str(), response_with_newline.length(), 0);
    if (bytes_sent < 0) {
        perror("Error sending response");
    }
}

// Get client address string from user_id (ip:port)
std::string get_client_address(const std::string& user_id) {
    // Lock the user IP/port map
    pthread_mutex_lock(&user_ip_port_mutex);
    
    // Find the user in the map
    auto it = user_ip_port.find(user_id);
    if (it == user_ip_port.end()) {
        pthread_mutex_unlock(&user_ip_port_mutex);
        return "";
    }
    
    // Format the address as ip:port
    std::string address = it->second.second + ":" + std::to_string(it->second.first);
    
    pthread_mutex_unlock(&user_ip_port_mutex);
    return address;
}
using std::istringstream;
using std::cout;
using std::cerr;
using std::endl;

// Handle client connection
void handle_client(int client_sock, struct sockaddr_in client_addr) {
    char tmpbuf[4096];
    string client_user_id;
    string client_ip = inet_ntoa(client_addr.sin_addr);
    int client_port = ntohs(client_addr.sin_port);
    std::string inbuf;

    while (true) {
        ssize_t bytes_received = recv(client_sock, tmpbuf, sizeof(tmpbuf), 0);

        if (bytes_received <= 0) {
            // Client disconnected or error occurred
            if (!client_user_id.empty()) {
                // Update user status to offline
                pthread_mutex_lock(&login_mutex);
                is_logged_in[client_user_id] = false;
                pthread_mutex_unlock(&login_mutex);
                
                // Remove user's IP and port
                pthread_mutex_lock(&user_ip_port_mutex);
                user_ip_port.erase(client_user_id);
                pthread_mutex_unlock(&user_ip_port_mutex);
                
                cout << "User " << client_user_id << " disconnected" << endl;
            }
            break;
        }

        // Append received bytes to buffer
        inbuf.append(tmpbuf, bytes_received);

        // Extract full lines terminated by '\n'
        size_t pos;
        while ((pos = inbuf.find('\n')) != std::string::npos) {
            std::string line = inbuf.substr(0, pos);
            // Remove optional CR
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // Trim whitespace at ends
            size_t first = line.find_first_not_of(" \t\r\n");
            size_t last = line.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last != std::string::npos) {
                std::string cmd = line.substr(first, last - first + 1);
                if (!cmd.empty()) {
                    process_client_request(cmd, client_sock, client_user_id, client_ip, client_port);
                }
            } else {
                // empty or whitespace-only line: ignore
            }

            // Erase processed line including the newline
            inbuf.erase(0, pos + 1);
        }
        
        // Prevent unbounded buffer growth
        if (inbuf.size() > 16 * 1024 * 1024) {
            // Too much buffered data without newline; reset to avoid OOM
            inbuf.clear();
        }
    }
    
    // Close the client socket
    close(client_sock);
}

// Process individual commands
void process_client_request(const string& command, int client_sock, 
                           string& client_user_id, const string& client_ip, int client_port) {
    // Debug: Print raw command received
    std::cout << "DEBUG: Raw command received: '" << command << "' (length: " << command.length() << ")" << std::endl;
    std::cout << "DEBUG: Hex dump: ";
    for (char c : command) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)c << " ";
    }
    std::cout << std::dec << std::endl;

    // Debug: Print the raw command with length
    std::cout << "DEBUG: Raw command before trim: '" << command << "' (length: " << command.length() << ")" << std::endl;
    
    // Trim any leading/trailing whitespace including newlines
    auto cmd_trimmed = command;
    cmd_trimmed.erase(cmd_trimmed.find_last_not_of(" \t\n\r\f\v") + 1);
    cmd_trimmed.erase(0, cmd_trimmed.find_first_not_of(" \t\n\r\f\v"));
    
    std::cout << "DEBUG: Command after trim: '" << cmd_trimmed << "' (length: " << cmd_trimmed.length() << ")" << std::endl;
    
    std::istringstream ss(cmd_trimmed);
    std::vector<std::string> tokens;
    std::string token;
    
    while (std::getline(ss, token, ' ')) {
        // Trim each token to remove any remaining whitespace
        token.erase(token.find_last_not_of(" \t\n\r\f\v") + 1);
        token.erase(0, token.find_first_not_of(" \t\n\r\f\v"));
        
        if (!token.empty()) {
            tokens.push_back(token);
            std::cout << "DEBUG: Token: ['" << token << "'] (length: " << token.length() << ")" << std::endl;
        }
    }
    
    // Debug: Print all tokens
    std::cout << "DEBUG: Found " << tokens.size() << " tokens" << std::endl;
    
    if (tokens.empty()) {
        const char* response = "Invalid command";
        ::send(client_sock, response, std::strlen(response), 0);
        return;
    }
    
    // Handle commands with spaces in them (e.g., "login user" -> "login_user")
    std::string cmd = tokens[0];
    
    // First check if the command is already in the combined format (e.g., "list_groups")
    if (cmd == "list_groups" || cmd == "create_user" || cmd == "create_group" || 
        cmd == "join_group" || cmd == "list_requests" || cmd == "accept_request" || 
        cmd == "leave_group") {
        // Command is already in the correct format, use as is
    }
    // Handle multi-word commands by checking the second token (e.g., "list groups" -> "list_groups")
    else if (tokens.size() > 1) {
        std::string potential_cmd = tokens[0] + "_" + tokens[1];
        // Check if this is a known command with underscore
        if (potential_cmd == "create_user" || 
            potential_cmd == "create_group" ||
            potential_cmd == "list_groups" ||
            potential_cmd == "join_group" ||
            potential_cmd == "list_requests" ||
            potential_cmd == "accept_request" ||
            potential_cmd == "leave_group") {
            
            // Remove the second token and update the command
            cmd = potential_cmd;
            tokens.erase(tokens.begin() + 1);
            // The first token is now the combined command
            tokens[0] = cmd;
        }
    }
    
    // Route to appropriate command handler
    if (cmd == "PORT" && tokens.size() == 2) {
        // Handle PORT command (from client connection)
        int client_listen_port = std::stoi(tokens[1]);
        std::cout << "Client connected with port: " << client_port 
                  << " (listening on port: " << client_listen_port << ")" << std::endl;
        
        // Store the client's listening port
        pthread_mutex_lock(&user_ip_port_mutex);
        if (!client_user_id.empty()) {
            user_ip_port[client_user_id] = std::make_pair(client_listen_port, client_ip);
        }
        pthread_mutex_unlock(&user_ip_port_mutex);
        
        std::string response = "PORT_ACK\n";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    } else if (cmd == "create_user") {
        create_user(tokens, client_sock);
    } else if (cmd == "login") {
        login_user(tokens, client_sock, client_user_id, client_port, client_ip);
    } else if (cmd == "create_group") {
        create_group(tokens, client_sock, client_user_id);
    } else if (cmd == "list_groups") {
        list_groups(tokens, client_sock, client_user_id);
    } else if (cmd == "logout") {
        logout(tokens, client_sock, client_user_id);
    } else if (cmd == "join_group") {
        join_group(tokens, client_sock, client_user_id);
    } else if (cmd == "list_requests") {
        list_requests(tokens, client_sock, client_user_id);
    } else if (cmd == "accept_request") {
        accept_request(tokens, client_sock, client_user_id);
    } else if (cmd == "leave_group") {
        leave_group(tokens, client_sock, client_user_id);
    } else if (cmd == "upload_file") {
        upload_file(tokens, client_sock, client_user_id);
    } else if (cmd == "download_file") {
        download_file(tokens, client_sock, client_user_id);
    } else if (cmd == "list_files") {
        list_files(tokens, client_sock, client_user_id);
    } else if (cmd == "stop_share") {
        stop_share(tokens, client_sock, client_user_id);
    } else if (cmd == "get_peers") {
        get_peers(tokens, client_sock, client_user_id);
    } else if (cmd == "update_file_metadata") {
        update_file_metadata(tokens, client_sock, client_user_id);
    } else {
        std::string response = "Unknown command: " + cmd;
        ::send(client_sock, response.c_str(), response.length(), 0);
    }
}

/* tracker_loop is defined in tracker.cpp */

// File sharing command implementations
void upload_file(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    // Format: upload_file <group_id> <file_path>
    if (tokens.size() != 3) {
        send_response(client_sock, "ERROR: Invalid command format. Use: upload_file <group_id> <file_path>");
        return;
    }

    string group_id = tokens[1];
    string file_path = tokens[2];
    
    // Get client's IP and port for peer information
    string client_addr = get_client_address(client_user_id);
    if (client_addr.empty()) {
        send_response(client_sock, "ERROR: Could not determine client address");
        return;
    }

    // Get file name from path
    size_t last_slash = file_path.find_last_of("/\\");
    string file_name = (last_slash == string::npos) ? file_path : file_path.substr(last_slash + 1);

    // Check if user is in the group
    bool user_in_group = false;
    {
        pthread_mutex_lock(&group_members_mutex);
        user_in_group = (group_members[group_id].find(client_user_id) != group_members[group_id].end());
        pthread_mutex_unlock(&group_members_mutex);
    }

    if (!user_in_group) {
        send_response(client_sock, "ERROR: You are not a member of this group");
        return;
    }
    
    // Get file name from path (already done above, no need to do it again)
    
    // Generate a unique file ID (will be used until client provides the actual hash)
    string temp_file_id = "temp_" + to_string(time(nullptr)) + "_" + client_user_id;

    // Create temporary file info
    FileInfo file_info;
    file_info.file_name = file_name;
    file_info.file_path = file_path;
    file_info.owner_id = client_user_id;
    
    // Get the actual file size
    struct stat stat_buf;
    if (stat(file_path.c_str(), &stat_buf) == 0) {
        file_info.file_size = stat_buf.st_size;
        cout << "DEBUG: Got file size: " << file_info.file_size << " bytes" << endl;
    } else {
        cerr << "WARNING: Could not get file size for " << file_path << ": " << strerror(errno) << endl;
        file_info.file_size = 0;
    }
    
    // Store the client (by user id) as a potential peer for this file
    // The client will provide actual chunk information later
    if (!client_user_id.empty()) {
        file_info.chunk_owners[0].insert(client_user_id);  // Using chunk 0 as a placeholder
    }

    // Update data structures with temporary file ID - lock in consistent order to prevent deadlocks
    pthread_mutex_lock(&file_metadata_mutex);
    pthread_mutex_lock(&group_files_mutex);
    pthread_mutex_lock(&user_files_mutex);
    
    try {
        file_metadata[temp_file_id] = file_info;
        group_files[group_id].insert(temp_file_id);
        user_files[client_user_id].insert(temp_file_id);
        
        pthread_mutex_unlock(&user_files_mutex);
        pthread_mutex_unlock(&group_files_mutex);
        pthread_mutex_unlock(&file_metadata_mutex);
    } catch (...) {
        // Ensure mutexes are always unlocked, even if an exception occurs
        pthread_mutex_unlock(&user_files_mutex);
        pthread_mutex_unlock(&group_files_mutex);
        pthread_mutex_unlock(&file_metadata_mutex);
        throw; // Re-throw the exception
    }

    // Instruct client to process the file and provide metadata
    // Include the file name and size in the response
    // Format: PROCESS_FILE <temp_file_id> <file_name> <file_size>
    string response = "PROCESS_FILE " + temp_file_id + " " + file_name + " " + to_string(file_info.file_size);
    cout << "DEBUG: Sending response to client: " << response << endl;
    send_response(client_sock, response);
}

void download_file(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() < 3) {
        send_response(client_sock, "ERROR: Invalid command format. Use: download_file <group_id> <file_hash>");
        return;
    }

    string group_id = tokens[1];
    string file_hash = tokens[2];

    // Check if user is in the group
    bool user_in_group = false;
    {
        pthread_mutex_lock(&group_members_mutex);
        user_in_group = (group_members[group_id].find(client_user_id) != group_members[group_id].end());
        pthread_mutex_unlock(&group_members_mutex);
    }

    if (!user_in_group) {
        send_response(client_sock, "ERROR: You are not a member of this group");
        return;
    }

    // Get file info
    FileInfo file_info;
    bool found = false;
    
    // Lock the metadata mutex for the entire lookup
    pthread_mutex_lock(&file_metadata_mutex);
    
    // First try to find by hash
    auto file_it = file_metadata.find(file_hash);
    
    // If not found by hash, try to find by filename
    if (file_it == file_metadata.end()) {
        cout << "DEBUG: File not found by hash, trying to find by name: " << file_hash << endl;
        for (const auto& [hash, info] : file_metadata) {
            if (info.file_name == file_hash) {  // Using file_hash as filename in this case
                cout << "DEBUG: Found file by name: " << info.file_name 
                     << " with hash: " << hash << endl;
                file_it = file_metadata.find(hash);
                found = true;
                break;
            }
        }
        if (!found) {
            pthread_mutex_unlock(&file_metadata_mutex);
            cout << "DEBUG: File not found by name either: " << file_hash << endl;
            send_response(client_sock, "ERROR: File not found");
            return;
        }
    }
    
    // Check if file is still in temp state
    if (file_it->first.find("temp_") == 0) {
        pthread_mutex_unlock(&file_metadata_mutex);
        cout << "DEBUG: File found but still in temp state: " << file_it->first << endl;
        send_response(client_sock, "ERROR: File is still being processed. Please try again in a moment.");
        return;
    }
    
    file_info = file_it->second; // Make a copy while holding the lock
    file_hash = file_it->first;  // Update file_hash to the actual hash
    pthread_mutex_unlock(&file_metadata_mutex);

    // Get list of peers who have this file
    vector<string> peers;
    {
        // Lock the file metadata again to safely access chunk_owners
        pthread_mutex_lock(&file_metadata_mutex);
        auto file_it = file_metadata.find(file_hash);
        if (file_it != file_metadata.end()) {
            // If file is still in temp state, it's not ready for download
            if (file_it->first.find("temp_") == 0) {
                pthread_mutex_unlock(&file_metadata_mutex);
                send_response(client_sock, "ERROR: File is still being processed. Please try again in a moment.");
                return;
            }
            
            for (const auto& [chunk_num, clients] : file_it->second.chunk_owners) {
                for (const string& peer_id : clients) {
                    string peer_addr = get_client_address(peer_id);
                    if (!peer_addr.empty()) {
                        peers.push_back(peer_addr);
                    }
                }
            }
            
            // If no peers found, the file might not be available for download
            if (peers.empty()) {
                pthread_mutex_unlock(&file_metadata_mutex);
                send_response(client_sock, "ERROR: No peers currently have this file available for download");
                return;
            }
        } else {
            pthread_mutex_unlock(&file_metadata_mutex);
            send_response(client_sock, "ERROR: File not found in metadata");
            return;
        }
        pthread_mutex_unlock(&file_metadata_mutex);
    }

    // Remove duplicates
    sort(peers.begin(), peers.end());
    peers.erase(unique(peers.begin(), peers.end()), peers.end());

    // Send response with file info, peers, and chunk hashes (if available)
    string response = "FILE_INFO " + file_info.file_name + " " + 
                     to_string(file_info.file_size) + " " + 
                     to_string(file_info.total_chunks) + " " + 
                     to_string(peers.size());
    
    for (const string& peer : peers) {
        response += " " + peer;
    }

    // Append chunk hashes so clients can verify downloaded chunks
    if (!file_info.chunk_hashes.empty()) {
        for (const auto& ch : file_info.chunk_hashes) {
            response += " " + ch;
        }
    }

    send_response(client_sock, response);
}

void list_files(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() < 2) {
        send_response(client_sock, "ERROR: Invalid command format. Use: list_files <group_id>");
        return;
    }

    string group_id = tokens[1];
    string response = "FILES ";
    int file_count = 0;
    string file_list;

    // Lock the necessary mutexes
    pthread_mutex_lock(&group_files_mutex);
    pthread_mutex_lock(&file_metadata_mutex);
    
    try {
        auto group_it = group_files.find(group_id);
        if (group_it != group_files.end()) {
            for (const auto& file_hash : group_it->second) {
                auto file_it = file_metadata.find(file_hash);
                if (file_it == file_metadata.end()) {
                    cerr << "WARNING: File hash found in group but not in metadata: " << file_hash << endl;
                    continue;
                }
                
                const FileInfo& file_info = file_it->second;
                
                // Skip if file is still in temp state
                if (file_hash.find("temp_") == 0) {
                    cout << "DEBUG: Skipping temp file: " << file_hash << endl;
                    continue;
                }
                
                // Add file info to the response
                if (!file_list.empty()) {
                    file_list += " ";
                }
                file_list += file_info.file_name + ":" + file_hash + ":" + to_string(file_info.file_size);
                file_count++;
                
                // Debug output
                cout << "DEBUG: Listing file - "
                     << "Name: " << file_info.file_name
                     << ", Size: " << file_info.file_size
                     << ", Hash: " << file_info.file_hash << endl;
            }
        }
        
        response = to_string(file_count) + " " + file_list;
        send_response(client_sock, response);
        
    } catch (const std::exception& e) {
        cerr << "ERROR in list_files: " << e.what() << endl;
        send_response(client_sock, "ERROR: Internal server error");
    }
    
    pthread_mutex_unlock(&file_metadata_mutex);
    pthread_mutex_unlock(&group_files_mutex);
}

void stop_share(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() < 3) {
        send_response(client_sock, "ERROR: Invalid command format. Use: stop_share <group_id> <file_hash>");
        return;
    }

    string group_id = tokens[1];
    string file_hash = tokens[2];

    // Check if user is the owner of the file
    pthread_mutex_lock(&file_metadata_mutex);
    auto file_it = file_metadata.find(file_hash);
    if (file_it == file_metadata.end() || file_it->second.owner_id != client_user_id) {
        pthread_mutex_unlock(&file_metadata_mutex);
        send_response(client_sock, "ERROR: You are not the owner of this file or file does not exist");
        return;
    }
    pthread_mutex_unlock(&file_metadata_mutex);

    // Remove from group files
    pthread_mutex_lock(&group_files_mutex);
    group_files[group_id].erase(file_hash);
    pthread_mutex_unlock(&group_files_mutex);

    // Remove from user files
    pthread_mutex_lock(&user_files_mutex);
    user_files[client_user_id].erase(file_hash);
    pthread_mutex_unlock(&user_files_mutex);

    // If no more references, remove file metadata
    bool has_references = false;
    pthread_mutex_lock(&group_files_mutex);
    for (const auto& [_, files] : group_files) {
        if (files.find(file_hash) != files.end()) {
            has_references = true;
            break;
        }
    }
    pthread_mutex_unlock(&group_files_mutex);

    if (!has_references) {
        pthread_mutex_lock(&file_metadata_mutex);
        file_metadata.erase(file_hash);
        pthread_mutex_unlock(&file_metadata_mutex);
    }

    send_response(client_sock, "SUCCESS: Stopped sharing file");
}

void update_file_metadata(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    // Format: update_file_metadata <temp_file_id> <file_hash> <file_size> <total_chunks> [<chunk_hash1> <chunk_hash2> ...]
    cout << "DEBUG: update_file_metadata called with tokens: ";
    for (const auto& t : tokens) {
        cout << t << " ";
    }
    cout << endl;

    if (tokens.size() < 5) {
        string error = "ERROR: Invalid command format. Expected at least 5 tokens, got " + to_string(tokens.size());
        cout << error << endl;
        send_response(client_sock, error);
        return;
    }

    string temp_file_id = tokens[1];
    string file_hash = tokens[2];
    uint64_t file_size;
    int total_chunks;

    try {
        file_size = stoull(tokens[3]);
        total_chunks = stoi(tokens[4]);
        cout << "DEBUG: Parsed file_size: " << file_size << ", total_chunks: " << total_chunks << endl;
    } catch (const std::exception& e) {
        string error = "ERROR: Invalid file size or chunk count: " + string(e.what());
        cout << error << endl;
        send_response(client_sock, error);
        return;
    }

    // Lock all necessary mutexes in a consistent order to prevent deadlocks
    pthread_mutex_lock(&file_metadata_mutex);
    
    try {
        // Find the temporary file
        cout << "DEBUG: Looking for temp file ID: " << temp_file_id << endl;
        auto it = file_metadata.find(temp_file_id);
        if (it == file_metadata.end()) {
            cout << "DEBUG: Temp file not found in file_metadata, current keys: " << endl;
            for (const auto& entry : file_metadata) {
                cout << "  - " << entry.first << endl;
            }
            pthread_mutex_unlock(&file_metadata_mutex);
            send_response(client_sock, "ERROR: File not found or already updated");
            return;
        }

        // Get the file info
        FileInfo file_info = it->second;
        
        cout << "DEBUG: Found file info - "
             << "Name: " << file_info.file_name
             << ", Old Size: " << file_info.file_size
             << ", New Size: " << file_size
             << ", Old Hash: " << file_info.file_hash
             << ", New Hash: " << file_hash << endl;
        
        // Update with actual metadata
        file_info.file_hash = file_hash;
        file_info.file_size = file_size;
        file_info.total_chunks = total_chunks;

        // Store chunk hashes sent by client (if any)
        file_info.chunk_hashes.clear();
        if ((int)tokens.size() >= 5 + total_chunks) {
            for (int i = 0; i < total_chunks; ++i) {
                file_info.chunk_hashes.push_back(tokens[5 + i]);
            }
        } else if ((int)tokens.size() > 5) {
            // Store whatever hashes were provided
            for (size_t i = 5; i < tokens.size(); ++i) file_info.chunk_hashes.push_back(tokens[i]);
        }

        // Remove the old entry
        cout << "DEBUG: Removing temp file entry: " << temp_file_id << endl;
        file_metadata.erase(it);
        
        // Add with the actual file hash as the key
        cout << "DEBUG: Adding new file entry with hash: " << file_hash << endl;
        file_metadata[file_hash] = file_info;
        
        // Debug output
        std::cout << "DEBUG: Updated file metadata - "
                  << "Name: " << file_info.file_name
                  << ", Size: " << file_info.file_size
                  << ", Hash: " << file_info.file_hash
                  << ", Chunks: " << file_info.total_chunks << std::endl;
        
        // Unlock file_metadata_mutex before locking others to prevent deadlocks
        pthread_mutex_unlock(&file_metadata_mutex);
        
        // Update user files mapping first
        pthread_mutex_lock(&user_files_mutex);
        try {
            bool user_updated = false;
            for (auto& [user_id, files] : user_files) {
                if (files.erase(temp_file_id) > 0) {
                    files.insert(file_hash);
                    user_updated = true;
                    cout << "DEBUG: Updated user files mapping for user: " << user_id << endl;
                }
            }
            
            if (!user_updated) {
                cout << "WARNING: File " << temp_file_id << " not found in any user's file list" << endl;
                // Add to the owner's file list if not already there
                if (!file_info.owner_id.empty()) {
                    user_files[file_info.owner_id].insert(file_hash);
                    cout << "DEBUG: Added file " << file_hash << " to owner's file list: " 
                         << file_info.owner_id << endl;
                }
            }
            
            pthread_mutex_unlock(&user_files_mutex);
        } catch (const std::exception& e) {
            pthread_mutex_unlock(&user_files_mutex);
            throw;
        }
        
        // Update group files mapping with the new hash
        pthread_mutex_lock(&group_files_mutex);
        try {
            bool group_updated = false;
            for (auto& [group_id, files] : group_files) {
                if (files.erase(temp_file_id) > 0) {
                    files.insert(file_hash);
                    group_updated = true;
                    cout << "DEBUG: Updated group files mapping for group: " << group_id << endl;
                }
            }
            
            if (!group_updated) {
                cout << "WARNING: File " << temp_file_id << " not found in any group's file list" << endl;
            }
            
            pthread_mutex_unlock(&group_files_mutex);
        } catch (const std::exception& e) {
            pthread_mutex_unlock(&group_files_mutex);
            throw;
        }
        
        // At this point, all mappings have been updated
        cout << "DEBUG: Successfully updated all mappings for file " << file_info.file_name 
             << " (old temp ID: " << temp_file_id << ", new hash: " << file_hash << ")" << endl;
        
        // Send success response
        send_response(client_sock, "SUCCESS: File metadata updated successfully");
    } catch (const std::exception& e) {
        // Ensure file_metadata_mutex is always unlocked
        pthread_mutex_unlock(&file_metadata_mutex);
        string error = "ERROR: " + string(e.what());
        cerr << error << endl;
        send_response(client_sock, error);
    }
}

// Old function - keeping for reference but it's no longer used
void update_file_info(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() != 5) {
        send_response(client_sock, "ERROR: Invalid command format. Use: update_file_info <file_id> <file_size> <file_hash> <total_chunks>");
        return;
    }

    string file_id = tokens[1];
    uint64_t file_size;
    string file_hash = tokens[3];
    int total_chunks;

    try {
        file_size = stoull(tokens[2]);
        total_chunks = stoi(tokens[4]);
    } catch (const std::exception& e) {
        send_response(client_sock, "ERROR: Invalid file size or chunk count");
        return;
    }

    // Lock the file metadata for update
    pthread_mutex_lock(&file_metadata_mutex);
    
    // Find the file by ID
    auto it = file_metadata.find(file_id);
    if (it == file_metadata.end()) {
        pthread_mutex_unlock(&file_metadata_mutex);
        send_response(client_sock, "ERROR: File not found or already updated");
        return;
    }

    // Update file info
    FileInfo& file_info = it->second;
    
    // If this is a temporary file (starts with 'temp_'), we need to update the ID to use the hash
    if (file_id.find("temp_") == 0) {
        // Remove the old entry
        FileInfo updated_info = file_info;  // Copy existing info
        file_metadata.erase(it);
        
        // Update the file info
        updated_info.file_size = file_size;
        updated_info.file_hash = file_hash;
        updated_info.total_chunks = total_chunks;
        
        // Insert with new key (file_hash)
        file_metadata[file_hash] = updated_info;
        
        // Update group files mapping
        pthread_mutex_lock(&group_files_mutex);
        for (auto& [group_id, files] : group_files) {
            if (files.erase(file_id) > 0) {
                files.insert(file_hash);
            }
        }
        pthread_mutex_unlock(&group_files_mutex);
        
        // Update user files mapping
        pthread_mutex_lock(&user_files_mutex);
        for (auto& [user_id, files] : user_files) {
            if (files.erase(file_id) > 0) {
                files.insert(file_hash);
            }
        }
        pthread_mutex_unlock(&user_files_mutex);
    } else {
        // For non-temporary files, just update the info
        file_info.file_size = file_size;
        file_info.file_hash = file_hash;
        file_info.total_chunks = total_chunks;
    }
    
    pthread_mutex_unlock(&file_metadata_mutex);
    send_response(client_sock, "SUCCESS: File info updated successfully");
}

void get_peers(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() < 2) {
        send_response(client_sock, "ERROR: Invalid command format. Use: get_peers <file_hash>");
        return;
    }

    string file_hash = tokens[1];
    vector<string> peers;

    pthread_mutex_lock(&file_metadata_mutex);
    auto file_it = file_metadata.find(file_hash);
    if (file_it != file_metadata.end()) {
        for (const auto& [chunk_num, clients] : file_it->second.chunk_owners) {
            for (const string& peer_id : clients) {
                string peer_addr = get_client_address(peer_id);
                if (!peer_addr.empty()) {
                    peers.push_back(peer_addr);
                }
            }
        }
    }
    pthread_mutex_unlock(&file_metadata_mutex);

    // Remove duplicates
    sort(peers.begin(), peers.end());
    peers.erase(unique(peers.begin(), peers.end()), peers.end());

    string response = "PEERS " + to_string(peers.size());
    for (const string& peer : peers) {
        response += " " + peer;
    }
    
    send_response(client_sock, response);
}

// Command handler implementations
void create_user(const vector<string>& tokens, int client_sock) {
    if (tokens.size() != 3) {
        string response = "Invalid command. Usage: create_user <user_id> <password>";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    const string& user_id = tokens[1];
    const string& password = tokens[2];
    string response;

    pthread_mutex_lock(&user_data_mutex);
    if (user_data.find(user_id) != user_data.end()) {
        response = "User already exists";
    } else {
        user_data[user_id] = password;
        response = "User created successfully";
    }
    pthread_mutex_unlock(&user_data_mutex);

    ::send(client_sock, response.c_str(), response.length(), 0);
}

void login_user(const vector<string>& tokens, int client_sock, 
               string& client_user_id, int client_port, const string& client_ip) {
    if (tokens.size() != 3) {
        string response = "Invalid command. Usage: login <user_id> <password>";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    const string& user_id = tokens[1];
    const string& password = tokens[2];
    string response;

    pthread_mutex_lock(&user_data_mutex);
    pthread_mutex_lock(&login_mutex);
    
    // Check if client is already logged in as another user
    if (!client_user_id.empty()) {
        response = "Please logout first before logging in as another user";
        pthread_mutex_unlock(&login_mutex);
        pthread_mutex_unlock(&user_data_mutex);
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }
    
    auto it = user_data.find(user_id);
    if (it == user_data.end()) {
        response = "User does not exist";
    } else if (it->second != password) {
        response = "Incorrect password";
    } else if (is_logged_in[user_id]) {
        response = "User already logged in from another client";
    } else {
        client_user_id = user_id;
        is_logged_in[user_id] = true;
        
        // Store user's IP and port
        pthread_mutex_lock(&user_ip_port_mutex);
        user_ip_port[user_id] = make_pair(client_port, client_ip);
        pthread_mutex_unlock(&user_ip_port_mutex);
        
        response = "Login successful";
    }
    
    pthread_mutex_unlock(&login_mutex);
    pthread_mutex_unlock(&user_data_mutex);
    
    ::send(client_sock, response.c_str(), response.length(), 0);
}

void create_group(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() != 2) {
        string response = "Invalid command. Usage: create_group <group_id>";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    const string& group_id = tokens[1];
    string response;

    pthread_mutex_lock(&all_groups_mutex);
    pthread_mutex_lock(&group_admin_mutex);
    
    if (find(all_groups.begin(), all_groups.end(), group_id) != all_groups.end()) {
        response = "Group already exists";
    } else {
        all_groups.push_back(group_id);
        group_admin[group_id] = client_user_id;
        
        // Get current time in milliseconds since epoch
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        
        // Add creator as first member
        pthread_mutex_lock(&group_members_mutex);
        group_members[group_id].insert(client_user_id);
        pthread_mutex_unlock(&group_members_mutex);
        
        // Record join time for the creator
        pthread_mutex_lock(&group_join_times_mutex);
        group_join_times[group_id][millis] = client_user_id;
        pthread_mutex_unlock(&group_join_times_mutex);
        
        response = "Group created successfully";
    }
    
    pthread_mutex_unlock(&group_admin_mutex);
    pthread_mutex_unlock(&all_groups_mutex);
    
    ::send(client_sock, response.c_str(), response.length(), 0);
}

void list_groups(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() != 1) {
        string response = "Invalid command. Usage: list_groups";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    string response;
    
    pthread_mutex_lock(&all_groups_mutex);
    if (all_groups.empty()) {
        response = "No groups exist";
    } else {
        for (const auto& group : all_groups) {
            response += group + "\n";
        }
    }
    pthread_mutex_unlock(&all_groups_mutex);
    
    ::send(client_sock, response.c_str(), response.length(), 0);
}

void logout(const vector<string>& tokens, int client_sock, string& client_user_id) {
    if (tokens.size() != 1) {
        string response = "Invalid command. Usage: logout";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    if (client_user_id.empty()) {
        string response = "Not logged in";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    pthread_mutex_lock(&login_mutex);
    is_logged_in[client_user_id] = false;
    
    // Remove user's IP and port
    pthread_mutex_lock(&user_ip_port_mutex);
    user_ip_port.erase(client_user_id);
    pthread_mutex_unlock(&user_ip_port_mutex);
    
    client_user_id = "";
    pthread_mutex_unlock(&login_mutex);
    
    string response = "Logged out successfully";
    ::send(client_sock, response.c_str(), response.length(), 0);
}

void join_group(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() != 2) {
        string response = "Invalid command. Usage: join_group <group_id>";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    if (client_user_id.empty()) {
        string response = "Please login first";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    const string& group_id = tokens[1];
    string response;

    pthread_mutex_lock(&all_groups_mutex);
    auto it = find(all_groups.begin(), all_groups.end(), group_id);
    if (it == all_groups.end()) {
        response = "Group does not exist";
        pthread_mutex_unlock(&all_groups_mutex);
    } else {
        pthread_mutex_unlock(&all_groups_mutex);
        
        pthread_mutex_lock(&group_members_mutex);
        auto& members = group_members[group_id];
        if (members.find(client_user_id) != members.end()) {
            response = "Already a member of this group";
        } else {
            pthread_mutex_lock(&pending_requests_mutex);
            pending_requests[group_id].insert(client_user_id);
            pthread_mutex_unlock(&pending_requests_mutex);
            response = "Join request sent to group admin";
        }
        pthread_mutex_unlock(&group_members_mutex);
    }
    
    ::send(client_sock, response.c_str(), response.length(), 0);
}

void list_requests(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() != 2) {
        string response = "Invalid command. Usage: list_requests <group_id>";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    if (client_user_id.empty()) {
        string response = "Please login first";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    const string& group_id = tokens[1];
    string response;

    pthread_mutex_lock(&group_admin_mutex);
    auto it = group_admin.find(group_id);
    if (it == group_admin.end() || it->second != client_user_id) {
        response = "You are not the admin of this group";
        pthread_mutex_unlock(&group_admin_mutex);
    } else {
        pthread_mutex_unlock(&group_admin_mutex);
        
        pthread_mutex_lock(&pending_requests_mutex);
        auto& requests = pending_requests[group_id];
        if (requests.empty()) {
            response = "No pending requests";
        } else {
            for (const auto& user : requests) {
                response += user + "\n";
            }
        }
        pthread_mutex_unlock(&pending_requests_mutex);
    }
    
    ::send(client_sock, response.c_str(), response.length(), 0);
}

void accept_request(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() != 3) {
        string response = "Invalid command. Usage: accept_request <group_id> <user_id>";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    if (client_user_id.empty()) {
        string response = "Please login first";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    const string& group_id = tokens[1];
    const string& user_id = tokens[2];
    string response;

    pthread_mutex_lock(&group_admin_mutex);
    auto it = group_admin.find(group_id);
    if (it == group_admin.end() || it->second != client_user_id) {
        response = "You are not the admin of this group";
        pthread_mutex_unlock(&group_admin_mutex);
    } else {
        pthread_mutex_unlock(&group_admin_mutex);
        
        pthread_mutex_lock(&pending_requests_mutex);
        auto& requests = pending_requests[group_id];
        auto req_it = requests.find(user_id);
        if (req_it == requests.end()) {
            response = "No such join request found";
        } else {
            // Remove the request from pending_requests
            requests.erase(req_it);
            
            // If this was the last request for the group, remove the group from pending_requests
            if (requests.empty()) {
                pending_requests.erase(group_id);
            }
            
            // The pending_requests change will be synchronized with the other tracker
            // in the next sync cycle. The sync happens every few seconds, so the request
            // will be removed from the other tracker soon.
            
            // Get current time in milliseconds since epoch
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            
            // Add user to the group members
            pthread_mutex_lock(&group_members_mutex);
            group_members[group_id].insert(user_id);
            pthread_mutex_unlock(&group_members_mutex);
            
            // Record join time
            pthread_mutex_lock(&group_join_times_mutex);
            group_join_times[group_id][millis] = user_id;
            pthread_mutex_unlock(&group_join_times_mutex);
            
            // The pending_requests change will be synchronized in the next sync cycle
            
            response = "User added to the group";
        }
        pthread_mutex_unlock(&pending_requests_mutex);
    }
    
    ::send(client_sock, response.c_str(), response.length(), 0);
}

void leave_group(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    if (tokens.size() != 2) {
        string response = "Invalid command. Usage: leave_group <group_id>";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    if (client_user_id.empty()) {
        string response = "Please login first";
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }

    const string& group_id = tokens[1];
    string response;

    pthread_mutex_lock(&group_admin_mutex);
    auto admin_it = group_admin.find(group_id);
    if (admin_it == group_admin.end()) {
        response = "Group does not exist";
        pthread_mutex_unlock(&group_admin_mutex);
    } else if (admin_it->second == client_user_id) {
        // Admin is leaving, need to transfer admin rights to the next oldest member
        pthread_mutex_lock(&group_members_mutex);
        auto& members = group_members[group_id];
        
        if (members.size() <= 1) {
            // Only admin is in the group, remove the group
            
            // Remove from all_groups first
            pthread_mutex_lock(&all_groups_mutex);
            auto it = find(all_groups.begin(), all_groups.end(), group_id);
            if (it != all_groups.end()) {
                all_groups.erase(it);
            }
            pthread_mutex_unlock(&all_groups_mutex);
            
            // Clean up other group data
            group_admin.erase(admin_it);
            members.clear();
            
            // Clean up join times
            pthread_mutex_lock(&group_join_times_mutex);
            group_join_times.erase(group_id);
            pthread_mutex_unlock(&group_join_times_mutex);
            
            // Clean up pending requests
            pthread_mutex_lock(&pending_requests_mutex);
            pending_requests.erase(group_id);
            pthread_mutex_unlock(&pending_requests_mutex);
            
            response = "You were the last member. Group has been deleted.";
        } else {
            // Find the next oldest member
            string new_admin;
            pthread_mutex_lock(&group_join_times_mutex);
            auto& join_times = group_join_times[group_id];
            
            // The map is sorted by time, so the first entry is the oldest member
            for (const auto& [time, user_id] : join_times) {
                if (user_id != client_user_id) { // Skip the current admin
                    new_admin = user_id;
                    break;
                }
            }
            
            if (!new_admin.empty()) {
                // Transfer admin rights
                admin_it->second = new_admin;
                
                // Remove the leaving admin from members and join times
                members.erase(client_user_id);
                
                // Find and remove the admin's join time
                for (auto it = join_times.begin(); it != join_times.end(); ) {
                    if (it->second == client_user_id) {
                        it = join_times.erase(it);
                        break;
                    } else {
                        ++it;
                    }
                }
                
                response = "You have left the group. Admin rights transferred to " + new_admin;
            } else {
                response = "Error: Could not find a new admin. Group may be in an inconsistent state.";
            }
            pthread_mutex_unlock(&group_join_times_mutex);
        }
        pthread_mutex_unlock(&group_members_mutex);
        pthread_mutex_unlock(&group_admin_mutex);
    } else {
        // Regular member is leaving
        pthread_mutex_unlock(&group_admin_mutex);
        
        pthread_mutex_lock(&group_members_mutex);
        auto& members = group_members[group_id];
        auto member_it = members.find(client_user_id);
        if (member_it == members.end()) {
            response = "You are not a member of this group";
        } else {
            members.erase(member_it);
            
            // Remove from join times
            pthread_mutex_lock(&group_join_times_mutex);
            auto& join_times = group_join_times[group_id];
            for (auto it = join_times.begin(); it != join_times.end(); ) {
                if (it->second == client_user_id) {
                    it = join_times.erase(it);
                    break;
                } else {
                    ++it;
                }
            }
            pthread_mutex_unlock(&group_join_times_mutex);
            
            response = "Left the group successfully";
        }
        pthread_mutex_unlock(&group_members_mutex);
    }
    
    ::send(client_sock, response.c_str(), response.length(), 0);
}