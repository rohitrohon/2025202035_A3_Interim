#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <cstdint>

using namespace std;

// Structure to pass client data to the thread function
struct ClientData {
    int client_sock;
    struct sockaddr_in client_addr;
};

// Thread function for handling client connections
void* client_thread_func(void* arg);

// Handle client requests
void handle_client(int client_sock, struct sockaddr_in client_addr);

// Process individual commands
void process_client_request(const string& command, int client_sock, 
                           string& client_user_id, const string& client_ip, int client_port);

// Command handlers
void create_user(const vector<string>& tokens, int client_sock);
void login_user(const vector<string>& tokens, int client_sock, 
                string& client_user_id, int client_port, const string& client_ip);
void create_group(const vector<string>& tokens, int client_sock, const string& client_user_id);
void list_groups(const vector<string>& tokens, int client_sock, const string& client_user_id);
void logout(const vector<string>& tokens, int client_sock, string& client_user_id);
void join_group(const vector<string>& tokens, int client_sock, const string& client_user_id);
void list_requests(const vector<string>& tokens, int client_sock, const string& client_user_id);
void accept_request(const vector<string>& tokens, int client_sock, const string& client_user_id);
void leave_group(const vector<string>& tokens, int client_sock, const string& client_user_id);

// File sharing commands
void upload_file(const vector<string>& tokens, int client_sock, const string& client_user_id);
void download_file(const vector<string>& tokens, int client_sock, const string& client_user_id);
void list_files(const vector<string>& tokens, int client_sock, const string& client_user_id);
void stop_share(const vector<string>& tokens, int client_sock, const string& client_user_id);
void get_peers(const vector<string>& tokens, int client_sock, const string& client_user_id);
void update_file_metadata(const vector<string>& tokens, int client_sock, const string& client_user_id);

// Helper functions
string get_client_address(const string& client_id);
void send_response(int sock, const string& response);

#endif