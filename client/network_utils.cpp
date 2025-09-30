#include "network_utils.h"
#include <string>
#include <sstream>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>

using namespace std;

vector<PeerInfo> parse_peer_data(const string& message) {
    vector<PeerInfo> peers;
    stringstream ss(message);
    string client_info;
    
    while (getline(ss, client_info, '@')) {
        if (client_info.empty()) continue;
        
        size_t one = client_info.find('-');
        size_t two = client_info.find('-', one + 1);
        
        if (one != string::npos && two != string::npos) {
            string port = client_info.substr(0, one);
            string ip = client_info.substr(one + 1, two - one - 1);
            string filepath = client_info.substr(two + 1);
            
            PeerInfo peer;
            peer.peer_id = "";  // Will be set by the caller if needed
            peer.ip = ip;
            peer.port = static_cast<uint16_t>(stoi(port));
            peer.downloaded = 0;
            peer.uploaded = 0;
            peer.left = 0;
            peer.event = "";
            
            peers.push_back(peer);
        }
    }
    
    return peers;
}

int create_socket_and_bind(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return -1;
    }
    
    // Set SO_REUSEADDR to allow reuse of local addresses
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sock);
        return -1;
    }
    
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (::bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    
    // Set the socket to listen for incoming connections
    if (listen(sock, SOMAXCONN) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }
    
    return sock;
}

int connect_to_server(const string& ip, int port, bool send_client_info, const string& client_info) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        return -1;
    }
    
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

int connect_to_tracker(const string& ip, int port, const string& client_info) {
    // Create a socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return -1;
    }

    // Set up the server address structure
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // Convert IP address from text to binary form
    if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return -1;
    }

    // Connect to the tracker
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    // If client_info is provided, send it to the tracker
    if (!client_info.empty()) {
        // Add newline to terminate the client info message
        string message = client_info + "\n";
        ssize_t bytes_sent = send(sock, message.c_str(), message.length(), 0);
        if (bytes_sent < 0) {
            perror("send");
            close(sock);
            return -1;
        }
        
        // Wait for PORT_ACK response
        char ack_buffer[32] = {0};
        ssize_t bytes_received = recv(sock, ack_buffer, sizeof(ack_buffer) - 1, 0);
        if (bytes_received <= 0) {
            close(sock);
            return -1;
        }
        
            // Check if we got a PORT_ACK
        if (strncmp(ack_buffer, "PORT_ACK", 7) != 0) {
            std::cerr << "Did not receive PORT_ACK from tracker" << std::endl;
            close(sock);
            return -1;
        }
    }

    return sock;
}