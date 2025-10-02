#include "tracker.h"
#include "client_handler.h"
#include "synchronization.h"
#include "data_structures.h"
#include <sstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <chrono>
#include <iomanip>
#include <unordered_set>
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

using namespace std;

// External declarations from data_structures.cpp
extern unordered_map<string, map<long long, string>> group_join_times;
extern pthread_mutex_t group_join_times_mutex;

// Using declarations for standard library components we use

// Map socket fd -> client's listening port (from PORT command)
static unordered_map<int,int> socket_listen_port;
static unordered_map<int,string> socket_listen_ip;
static pthread_mutex_t socket_listen_port_mutex = PTHREAD_MUTEX_INITIALIZER;

// Send a response to a client
void send_response(int client_sock, const string& response) {
    // Add newline if not present
    string response_with_newline = response;
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
string get_client_address(const string& user_id) {
    // Lock the user IP/port map
    pthread_mutex_lock(&user_ip_port_mutex);
    
    // Find the user in the map
    auto it = user_ip_port.find(user_id);
    if (it == user_ip_port.end()) {
        pthread_mutex_unlock(&user_ip_port_mutex);
        return "";
    }
    
    // Format the address as ip:port
    string address = it->second.second + ":" + to_string(it->second.first);
    
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
    string inbuf;

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
        while ((pos = inbuf.find('\n')) != string::npos) {
            string line = inbuf.substr(0, pos);
            // Remove optional CR
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // Trim whitespace at ends
            size_t first = line.find_first_not_of(" \t\r\n");
            size_t last = line.find_last_not_of(" \t\r\n");
            if (first != string::npos && last != string::npos) {
                string cmd = line.substr(first, last - first + 1);
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
    // Cleanup socket -> listen port mapping
    pthread_mutex_lock(&socket_listen_port_mutex);
    socket_listen_port.erase(client_sock);
    socket_listen_ip.erase(client_sock);
    pthread_mutex_unlock(&socket_listen_port_mutex);

    close(client_sock);
}

// Process individual commands
void process_client_request(const string& command, int client_sock,
                           string& client_user_id, const string& client_ip, int client_port) {
    // Trim leading/trailing whitespace
    string cmd_trimmed = command;
    cmd_trimmed.erase(0, cmd_trimmed.find_first_not_of(" \t\n\r\f\v"));
    if (cmd_trimmed.empty()) return;
    if (cmd_trimmed.find_last_not_of(" \t\n\r\f\v") != string::npos)
        cmd_trimmed.erase(cmd_trimmed.find_last_not_of(" \t\n\r\f\v") + 1);

    // Tokenize by spaces
    istringstream iss(cmd_trimmed);
    vector<string> tokens;
    string tk;
    while (iss >> tk) tokens.push_back(tk);
    if (tokens.empty()) {
        send_response(client_sock, "Invalid command");
        return;
    }

    // Normalize two-word commands like "create user" -> "create_user"
    string cmd = tokens[0];
    if (tokens.size() > 1) {
        string combined = tokens[0] + "_" + tokens[1];
        static const unordered_set<string> known = {
            "create_user","create_group","list_groups","join_group","list_requests",
            "accept_request","leave_group"
        };
        if (known.count(combined)) {
            cmd = combined;
            tokens.erase(tokens.begin() + 1);
            tokens[0] = cmd;
        }
    }

    // Dispatch
    if (cmd == "PORT" && tokens.size() == 2) {
        string provided = tokens[1];
        string provided_ip = client_ip;
        int client_listen_port = 0;
        size_t colon = provided.find(':');
        if (colon != string::npos) {
            provided_ip = provided.substr(0, colon);
            client_listen_port = stoi(provided.substr(colon + 1));
        } else {
            client_listen_port = stoi(provided);
        }

        cout << "Client connected with source port: " << client_port
             << " (listening on: " << provided_ip << ":" << client_listen_port << ")" << endl;

        pthread_mutex_lock(&socket_listen_port_mutex);
        socket_listen_port[client_sock] = client_listen_port;
        socket_listen_ip[client_sock] = provided_ip;
        pthread_mutex_unlock(&socket_listen_port_mutex);

        if (!client_user_id.empty()) {
            pthread_mutex_lock(&user_ip_port_mutex);
            user_ip_port[client_user_id] = make_pair(client_listen_port, provided_ip);
            pthread_mutex_unlock(&user_ip_port_mutex);
        }

        send_response(client_sock, "PORT_ACK");
        return;
    }

    if (cmd == "create_user") {
        create_user(tokens, client_sock);
        return;
    }
    if (cmd == "login") {
        login_user(tokens, client_sock, client_user_id, client_port, client_ip);
        return;
    }
    if (cmd == "create_group") {
        create_group(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "list_groups") {
        list_groups(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "logout") {
        logout(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "join_group") {
        join_group(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "list_requests") {
        list_requests(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "accept_request") {
        accept_request(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "leave_group") {
        leave_group(tokens, client_sock, client_user_id);
        return;
    }

    // File-sharing related
    if (cmd == "upload_file") {
        upload_file(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "download_file") {
        download_file(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "list_files") {
        list_files(tokens, client_sock, client_user_id);
        return;
    }
    if (cmd == "stop_share") {
        stop_share(tokens, client_sock, client_user_id);
        return;
    }

    if (cmd == "show_downloads") {
        // Return the download list for the logged-in user
        if (client_user_id.empty()) {
            send_response(client_sock, "ERROR: Please login first");
            return;
        }
        pthread_mutex_lock(&user_downloads_mutex);
        string resp;
        auto it = user_downloads.find(client_user_id);
        if (it != user_downloads.end()) {
            for (const auto &d : it->second) {
                string line;
                line += (d.status == 'D') ? "[D] " : "[C] ";
                line += "[" + d.group_id + "] ";
                line += d.file_name;
                if (!resp.empty()) resp += "\n";
                resp += line;
            }
        }
        pthread_mutex_unlock(&user_downloads_mutex);
        if (resp.empty()) resp = "No downloads";
        send_response(client_sock, resp);
        return;
    }

    if (cmd == "I_HAVE") {
        // Format: I_HAVE <group_id> <client_id> <file_hash> <file_path>
        if (tokens.size() < 5) { send_response(client_sock, "ERROR: Invalid I_HAVE format"); return; }
        string group_id = tokens[1];
        string announcing_client = tokens[2];
        string file_hash = tokens[3];
        string file_path = tokens[4];

        pthread_mutex_lock(&user_files_mutex);
        user_files[announcing_client].insert(file_hash);
        pthread_mutex_unlock(&user_files_mutex);

        pthread_mutex_lock(&group_files_mutex);
        group_files[group_id].insert(file_hash);
        pthread_mutex_unlock(&group_files_mutex);

        pthread_mutex_lock(&file_metadata_mutex);
        auto fit = file_metadata.find(file_hash);
        if (fit != file_metadata.end()) {
            FileInfo &fi = fit->second;
            int tc = fi.total_chunks > 0 ? fi.total_chunks : 1;
            for (int i = 0; i < tc; ++i) fi.chunk_owners[i].insert(announcing_client);
            if (fi.file_path.empty()) fi.file_path = file_path;
        } else {
            FileInfo fi;
            fi.file_hash = file_hash;
            fi.file_path = file_path;
            fi.file_name = file_path.substr(file_path.find_last_of("/\\") + 1);
            fi.owner_id = announcing_client;
            fi.file_size = 0;
            fi.total_chunks = 1;
            fi.chunk_owners[0].insert(announcing_client);
            file_metadata[file_hash] = fi;
        }
        pthread_mutex_unlock(&file_metadata_mutex);

        // Mark as completed in user_downloads if present
        pthread_mutex_lock(&user_downloads_mutex);
        try {
            auto &vec = user_downloads[announcing_client];
            for (auto &e : vec) {
                if (e.group_id == group_id) {
                    pthread_mutex_lock(&file_metadata_mutex);
                    string name_lookup;
                    auto fm = file_metadata.find(file_hash);
                    if (fm != file_metadata.end()) name_lookup = fm->second.file_name;
                    pthread_mutex_unlock(&file_metadata_mutex);
                    if (!name_lookup.empty() && e.file_name == name_lookup) e.status = 'C';
                }
            }
        } catch (...) {}
        pthread_mutex_unlock(&user_downloads_mutex);

        force_sync();
        send_response(client_sock, "SUCCESS: Announced availability");
        return;
    }

    if (cmd == "get_peers") { get_peers(tokens, client_sock, client_user_id); return; }
    if (cmd == "update_file_metadata") { update_file_metadata(tokens, client_sock, client_user_id); return; }

    // Unknown
    send_response(client_sock, string("Unknown command: ") + cmd);
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
    
    // Force sync so membership and pending requests changes propagate immediately
    force_sync();
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
        } else {
        cerr << "WARNING: Could not get file size for " << file_path << ": " << strerror(errno) << endl;
        file_info.file_size = 0;
    }
    
    // Store the client (by user id) as a potential peer for this file
    // The client will provide actual chunk information later
    if (!client_user_id.empty()) {
        file_info.chunk_owners[0].insert(client_user_id);  // Using chunk 0 as a placeholder
    }

    // Update data structures with temporary file ID
    // Important: do NOT insert temp into group_files/user_files to avoid leaking temps
    pthread_mutex_lock(&file_metadata_mutex);
    try {
        file_metadata[temp_file_id] = file_info;
        pthread_mutex_unlock(&file_metadata_mutex);
    } catch (...) {
        pthread_mutex_unlock(&file_metadata_mutex);
        throw;
    }
    // Record temp associations for finalize step
    pthread_mutex_lock(&temp_maps_mutex);
    temp_to_group[temp_file_id] = group_id;
    temp_to_owner[temp_file_id] = client_user_id;
    pthread_mutex_unlock(&temp_maps_mutex);
    // Recorded temp mapping for finalize step

    // Instruct client to process the file and provide metadata
    // Include the file name and size in the response
    // Format: PROCESS_FILE <temp_file_id> <file_name> <file_size>
    string response = "PROCESS_FILE " + temp_file_id + " " + file_name + " " + to_string(file_info.file_size);
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
    
    // If not found by hash, try to find by filename, preferring finalized entries
    if (file_it == file_metadata.end()) {
    // File not found by hash, trying to find by name
        string chosen_hash;
        // First pass: non-temp entries only
        for (const auto& [hash, info] : file_metadata) {
            if (hash.rfind("temp_", 0) != 0 && info.file_name == file_hash) {
                chosen_hash = hash;
                break;
            }
        }
        // Second pass: allow temp if no finalized found
        if (chosen_hash.empty()) {
            for (const auto& [hash, info] : file_metadata) {
                if (info.file_name == file_hash) {
                    chosen_hash = hash;
                    break;
                }
            }
        }
        if (!chosen_hash.empty()) {
            // Found file by name
            file_it = file_metadata.find(chosen_hash);
            found = true;
        }
        if (!found) {
            pthread_mutex_unlock(&file_metadata_mutex);
            // File not found by name either
            send_response(client_sock, "ERROR: File not found");
            return;
        }
    }
    
    // Check if file is still in temp state; try to resolve to finalized entry by same name
    if (file_it->first.find("temp_") == 0) {
        string temp_name = file_it->second.file_name;
    // File found in temp state, attempting resolve by name
        bool resolved = false;
        for (const auto &kv : file_metadata) {
            if (kv.first.rfind("temp_", 0) != 0 && kv.second.file_name == temp_name) {
                // Found a finalized entry for same name
                file_it = file_metadata.find(kv.first);
                resolved = true;
                break;
            }
        }
        if (!resolved) {
            // Try forcing a sync and re-check once
            pthread_mutex_unlock(&file_metadata_mutex);
            force_sync();
            pthread_mutex_lock(&file_metadata_mutex);
            for (const auto &kv : file_metadata) {
                if (kv.first.rfind("temp_", 0) != 0 && kv.second.file_name == temp_name) {
                    file_it = file_metadata.find(kv.first);
                    resolved = true;
                    break;
                }
            }
        }
        if (!resolved) {
            pthread_mutex_unlock(&file_metadata_mutex);
            send_response(client_sock, "ERROR: File not yet available; metadata not finalized");
            return;
        }
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

    // Before responding, record this user's download status as 'D' (downloading)
    {
        pthread_mutex_lock(&user_downloads_mutex);
        bool found = false;
        auto &vec = user_downloads[client_user_id];
        for (auto &e : vec) {
            if (e.group_id == group_id && e.file_name == file_info.file_name) { e.status = 'D'; found = true; break; }
        }
        if (!found) vec.push_back(DownloadEntry{group_id, file_info.file_name, 'D'});
        pthread_mutex_unlock(&user_downloads_mutex);
    }

    // Send response with file info, peers, and chunk hashes (if available)
    // Format: FILE_INFO <file_path> <file_name> <file_hash> <file_size> <total_chunks> <num_peers> <peer1> ... [chunk_hashes...]
    // Ensure we send a placeholder for file_hash if missing to keep token positions stable
    string safe_file_hash = file_info.file_hash.empty() ? string("-") : file_info.file_hash;
    string response = "FILE_INFO " + file_info.file_path + " " + file_info.file_name + " " + safe_file_hash + " " +
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
            // Deduplicate by filename to avoid duplicate rows if both temp and finalized exist
            unordered_set<string> seen_names;
            // Snapshot hashes to avoid mutating during iteration
            vector<string> hashes(group_it->second.begin(), group_it->second.end());
            for (const auto& h : hashes) {
                auto it = file_metadata.find(h);
                if (it == file_metadata.end()) {
                    cerr << "WARNING: File hash found in group but not in metadata: " << h << endl;
                    continue;
                }

                const FileInfo* info_ptr = &it->second;
                string name = info_ptr->file_name;

                // If temp, try to resolve to finalized entry by same name
                if (h.rfind("temp_", 0) == 0) {
                    // Skipping temp file
                    for (const auto &kv : file_metadata) {
                        if (kv.first.rfind("temp_", 0) != 0 && kv.second.file_name == name) {
                            info_ptr = &kv.second;
                            name = info_ptr->file_name;
                            break;
                        }
                    }
                }

                // If already listed by name, skip to avoid duplicates
                if (seen_names.count(name)) continue;
                seen_names.insert(name);

                if (!file_list.empty()) file_list += "\n";
                file_list += name + " " + to_string(info_ptr->file_size) + " " + to_string(info_ptr->total_chunks);
                file_count++;

             // Listing file - name/size/chunks
            }
        }
        
        // Response: first line is the count, followed by one file per line with: <name> <size> <total_chunks>
        response = to_string(file_count) + "\n" + file_list;
        send_response(client_sock, response);
        
    } catch (const exception& e) {
        cerr << "ERROR in list_files: " << e.what() << endl;
        send_response(client_sock, "ERROR: Internal server error");
    }
    
    pthread_mutex_unlock(&file_metadata_mutex);
    pthread_mutex_unlock(&group_files_mutex);
}

void stop_share(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    // Accept: stop_share <group_id> <file_name_or_hash>
    if (tokens.size() < 3) {
        send_response(client_sock, "ERROR: Invalid command format. Use: stop_share <group_id> <file_name>");
        return;
    }

    string group_id = tokens[1];
    string file_key = tokens[2]; // can be hash or name

    // Ensure user is a member of the group
    bool user_in_group = false;
    pthread_mutex_lock(&group_members_mutex);
    user_in_group = (group_members[group_id].find(client_user_id) != group_members[group_id].end());
    pthread_mutex_unlock(&group_members_mutex);
    if (!user_in_group) {
        send_response(client_sock, "ERROR: You are not a member of this group");
        return;
    }

    // Resolve file: try by hash, else by name within metadata and group
    string file_hash = file_key;
    pthread_mutex_lock(&file_metadata_mutex);
    auto it = file_metadata.find(file_key);
    if (it == file_metadata.end()) {
        // find by name: ensure it's part of this group
        // check group_files for candidates
        pthread_mutex_unlock(&file_metadata_mutex);
        pthread_mutex_lock(&group_files_mutex);
        auto gf_it = group_files.find(group_id);
        if (gf_it == group_files.end()) {
            pthread_mutex_unlock(&group_files_mutex);
            send_response(client_sock, "ERROR: File not found in this group");
            return;
        }
        // try to match filename
        bool found = false;
        pthread_mutex_lock(&file_metadata_mutex);
        for (const auto& fh : gf_it->second) {
            auto fm_it = file_metadata.find(fh);
            if (fm_it != file_metadata.end() && fm_it->second.file_name == file_key) {
                file_hash = fh;
                it = fm_it;
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&file_metadata_mutex);
        pthread_mutex_unlock(&group_files_mutex);
        if (!found) {
            send_response(client_sock, "ERROR: File not found in metadata");
            return;
        }
    }

    // Remove this user from seeders list (chunk_owners) for all chunks
    try {
        FileInfo &fi = it->second;
        for (auto &entry : fi.chunk_owners) {
            entry.second.erase(client_user_id);
        }
    } catch (...) {
        // Ensure mutex gets unlocked below
    }
    pthread_mutex_unlock(&file_metadata_mutex);

    // Remove mapping from user_files
    pthread_mutex_lock(&user_files_mutex);
    user_files[client_user_id].erase(file_hash);
    pthread_mutex_unlock(&user_files_mutex);

    // If no seeders remain for any chunk, remove file from group and metadata
    bool any_seeder = false;
    pthread_mutex_lock(&file_metadata_mutex);
    auto fit2 = file_metadata.find(file_hash);
    if (fit2 != file_metadata.end()) {
        for (const auto &entry : fit2->second.chunk_owners) {
            if (!entry.second.empty()) { any_seeder = true; break; }
        }
    }
    pthread_mutex_unlock(&file_metadata_mutex);

    if (!any_seeder) {
        pthread_mutex_lock(&group_files_mutex);
        group_files[group_id].erase(file_hash);
        pthread_mutex_unlock(&group_files_mutex);

        pthread_mutex_lock(&file_metadata_mutex);
        file_metadata.erase(file_hash);
        pthread_mutex_unlock(&file_metadata_mutex);
    }

    send_response(client_sock, "SUCCESS: Stopped sharing file");
}

void update_file_metadata(const vector<string>& tokens, int client_sock, const string& client_user_id) {
    // Format: update_file_metadata <temp_file_id> <file_hash> <file_size> <total_chunks> [<chunk_hash1> <chunk_hash2> ...]
    // update_file_metadata called

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
    } catch (const exception& e) {
        string error = "ERROR: Invalid file size or chunk count: " + string(e.what());
        cout << error << endl;
        send_response(client_sock, error);
        return;
    }

    // Lock all necessary mutexes in a consistent order to prevent deadlocks
    pthread_mutex_lock(&file_metadata_mutex);
    
    try {
        // Find the temporary file
        // Looking for temp file ID
        auto it = file_metadata.find(temp_file_id);
        if (it == file_metadata.end()) {
            // Temp file not found in file_metadata
            pthread_mutex_unlock(&file_metadata_mutex);
            send_response(client_sock, "ERROR: File not found or already updated");
            return;
        }

        // Get the file info
        FileInfo file_info = it->second;
        
        // Found file info - updating metadata
        
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
        // Removing temp file entry
        file_metadata.erase(it);
        
        // Add with the actual file hash as the key
        // Adding new file entry with hash
        file_metadata[file_hash] = file_info;
        
        // Debug output
        // Updated file metadata
        // Option A cleanup: purge any lingering temp_* entries with the same file_name
        {
            vector<string> temps_to_remove;
            for (const auto &kv : file_metadata) {
                if (kv.first.rfind("temp_", 0) == 0 && kv.second.file_name == file_info.file_name) {
                    temps_to_remove.push_back(kv.first);
                }
            }
            if (!temps_to_remove.empty()) {
                // Purging temp entries for this file_name
            }
            // Remove from mappings first, then erase from metadata
                if (!temps_to_remove.empty()) {
                pthread_mutex_lock(&user_files_mutex);
                for (auto &uf : user_files) {
                    for (const string &tk : temps_to_remove) {
                        if (uf.second.erase(tk) > 0) {
                            uf.second.insert(file_hash);
                        }
                    }
                }
                pthread_mutex_unlock(&user_files_mutex);

                pthread_mutex_lock(&group_files_mutex);
                for (auto &gf : group_files) {
                    bool touched = false;
                    for (const string &tk : temps_to_remove) {
                        if (gf.second.erase(tk) > 0) {
                            touched = true;
                        }
                    }
                    if (touched) {
                        gf.second.insert(file_hash);
                    }
                }
                pthread_mutex_unlock(&group_files_mutex);

                // Finally, erase from file_metadata (keep current finalized entry)
                for (const string &tk : temps_to_remove) {
                    if (tk != temp_file_id) {
                        file_metadata.erase(tk);
                    }
                }
            }
        }
        
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
                    if (user_updated) {
                        // Updated user files mapping for user
                    }
                }
            }
            
            if (!user_updated) {
                // Add to the owner's file list if not already there
                string owner = file_info.owner_id;
                if (owner.empty()) {
                    pthread_mutex_lock(&temp_maps_mutex);
                    auto to = temp_to_owner.find(temp_file_id);
                    if (to != temp_to_owner.end()) owner = to->second;
                    pthread_mutex_unlock(&temp_maps_mutex);
                }
                if (!owner.empty()) {
                    user_files[owner].insert(file_hash);
                }
            }
            
            pthread_mutex_unlock(&user_files_mutex);
        } catch (const exception& e) {
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
                }
            }
            
            if (!group_updated) {
                // Insert into the original group from temp mapping (always add finalized)
                string gins;
                pthread_mutex_lock(&temp_maps_mutex);
                auto tgit = temp_to_group.find(temp_file_id);
                if (tgit != temp_to_group.end()) gins = tgit->second;
                pthread_mutex_unlock(&temp_maps_mutex);
                if (!gins.empty()) {
                    group_files[gins].insert(file_hash);
                } else {
                    // File not found in any group's file list and no temp mapping
                }
            }
            
            pthread_mutex_unlock(&group_files_mutex);
        } catch (const exception& e) {
            pthread_mutex_unlock(&group_files_mutex);
            throw;
        }

        // Erase temp-to-* mappings for this temp_id now that finalize is done
        pthread_mutex_lock(&temp_maps_mutex);
        temp_to_group.erase(temp_file_id);
        temp_to_owner.erase(temp_file_id);
        pthread_mutex_unlock(&temp_maps_mutex);
        
        // At this point, all mappings have been updated
        // Successfully updated all mappings for file
        
        // Send success response
        send_response(client_sock, "SUCCESS: File metadata updated successfully");

        // Trigger immediate sync so other tracker sees the finalized entry and seeders
        force_sync();
    } catch (const exception& e) {
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
    } catch (const exception& e) {
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
        
        // Store user's IP and listening port. Prefer the port and ip provided earlier via PORT command
        int listen_port_to_store = client_port; // fallback to the source port
        string ip_to_store = client_ip; // fallback to source IP

        pthread_mutex_lock(&socket_listen_port_mutex);
        auto spit = socket_listen_port.find(client_sock);
        if (spit != socket_listen_port.end()) {
            listen_port_to_store = spit->second;
        }
        auto sipit = socket_listen_ip.find(client_sock);
        if (sipit != socket_listen_ip.end()) {
            ip_to_store = sipit->second;
        }
        pthread_mutex_unlock(&socket_listen_port_mutex);

        pthread_mutex_lock(&user_ip_port_mutex);
        user_ip_port[user_id] = make_pair(listen_port_to_store, ip_to_store);
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
        auto now = chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto millis = chrono::duration_cast<chrono::milliseconds>(duration).count();
        
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
    
    // Force sync so other tracker sees the new group immediately
    force_sync();

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
            response = "Join request sent";
        }
        pthread_mutex_unlock(&group_members_mutex);
        // Force sync so the pending request is visible on the other tracker immediately
        force_sync();
        
        ::send(client_sock, response.c_str(), response.length(), 0);
        return;
    }
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
            auto now = chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            auto millis = chrono::duration_cast<chrono::milliseconds>(duration).count();
            
            // Add user to the group members
            pthread_mutex_lock(&group_members_mutex);
            group_members[group_id].insert(user_id);
            pthread_mutex_unlock(&group_members_mutex);
            
            // Record join time
            pthread_mutex_lock(&group_join_times_mutex);
            group_join_times[group_id][millis] = user_id;
            pthread_mutex_unlock(&group_join_times_mutex);
            
            // Force sync so the addition and pending_requests removal propagate immediately
            force_sync();
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
            // Propagate deletion immediately
            force_sync();
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
                // Propagate admin transfer immediately
                force_sync();
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