#include "tracker.h"
#include "client_handler.h"
#include "synchronization.h"
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
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
    int listen(int, int);
    ssize_t recv(int, void*, size_t, int);
    ssize_t send(int, const void*, size_t, int);
    int socket(int, int, int);
    int close(int);
}

// Using declarations for standard library components we use
using std::string;
using std::vector;
using std::istringstream;
using std::cout;
using std::cerr;
using std::endl;

// Handle client connection
void handle_client(int client_sock, struct sockaddr_in client_addr) {
    char buffer[1024] = {0};
    string client_user_id;
    string client_ip = inet_ntoa(client_addr.sin_addr);
    int client_port = ntohs(client_addr.sin_port);

    while (true) {
        // Clear the buffer
        memset(buffer, 0, sizeof(buffer));
        
        // Read data from client
        ssize_t bytes_received = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
        
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
        
        // Process the received command
        string command(buffer);
        process_client_request(command, client_sock, client_user_id, client_ip, client_port);
    }
    
    // Close the client socket
    close(client_sock);
}

// Implementation of the client thread function
void* client_thread_func(void* arg) {
    ClientData* data = static_cast<ClientData*>(arg);
    handle_client(data->client_sock, data->client_addr);
    return nullptr;
}

void process_client_request(const std::string& command, int client_sock, 
                           std::string& client_user_id, const std::string& client_ip, int client_port) {
    std::istringstream ss(command);
    std::vector<std::string> tokens;
    std::string token;
    
    while (std::getline(ss, token, ' ')) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    
    if (tokens.empty()) {
        const char* response = "Invalid command";
        ::send(client_sock, response, std::strlen(response), 0);
        return;
    }
    
    // Handle commands with spaces in them (e.g., "login user" -> "login_user")
    std::string cmd = tokens[0];
    
    // Handle multi-word commands by checking the second token
    if (tokens.size() > 1) {
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
    } else {
        std::string response = "Unknown command: " + cmd;
        ::send(client_sock, response.c_str(), response.length(), 0);
    }
}

/* tracker_loop is defined in tracker.cpp */



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