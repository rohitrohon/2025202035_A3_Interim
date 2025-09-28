#ifndef NETWORK_UTILS_H
#define NETWORK_UTILS_H

#include <string>
#include <vector>
using namespace std;

struct PeerInfo {
    string port;
    string ip;
    string file_path;
};

// Network utility functions
vector<PeerInfo> parse_peer_data(const string& message);
int create_socket_and_bind(int port);
int connect_to_server(const string& ip, int port, bool send_client_info = false, const string& client_info = "");
int connect_to_tracker(const string& ip, int port, const string& client_info = "");

#endif