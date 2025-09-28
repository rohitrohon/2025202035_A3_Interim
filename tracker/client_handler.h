#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

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

#endif