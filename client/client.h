#ifndef CLIENT_H
#define CLIENT_H

#include <string>
using namespace std;

// Client main functions
void start_listening(int listen_port);
void handle_peer_connection(int peer_sock);
int connect_to_tracker(const string& ip, int port, const string& client_ip_port);

#endif