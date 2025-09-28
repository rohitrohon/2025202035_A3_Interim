#include "header.h"
using namespace std;
bool threadrun=true;
void handleClient(int client_sock, sockaddr_in client_addr){
    string clientuserid="";
    char buffer[1024] = {0};
    int valread = read(client_sock, buffer, sizeof(buffer));
    string clientipport=string(buffer);
    int  i = clientipport.find(':');
    string clientip = clientipport.substr(0, i);
    int clientport = stoi(clientipport.substr(i + 1));
    memset(buffer, 0, sizeof(buffer));
    while (true){
        int valread = read(client_sock, buffer, sizeof(buffer));
        if (valread < 0) {
            cerr << "Failed to read from client." << endl;
            break; 
        } else if (valread == 0){            
            cout << "Client disconnected." << endl;
            string str="logout";
            doclientreq(str, client_sock, clientuserid,clientip, clientport);
            break;
        }
        string str = string(buffer);
        doclientreq(str, client_sock, clientuserid,clientip, clientport);
        memset(buffer, 0, sizeof(buffer));
    }
   close(client_sock);
}
void trackerLoop(int tracker_sock){
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (threadrun) {
        int client_sock = accept(tracker_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            cout << "Failed to accept client connection." << endl;
            continue;
        }
        cout << "Client connected with port: " << ntohs(client_addr.sin_port) << endl;

        thread client_thread(handleClient, client_sock, client_addr);
        client_thread.detach();
    }
    close(tracker_sock);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cout << "Invalid Arguments Format: ./tracker tracker_info.txt tracker_no " << endl;
        return 1;
    }

    string tracker_file = argv[1];
    int tracker_no = atoi(argv[2]);

    string ip_address;
    int port;
    int fd = open(tracker_file.c_str(), O_RDONLY);
    if (fd < 0) {
        cerr << "Failed to open " << tracker_file << endl;
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
    string firstLine, secondLine;
    getline(ss, firstLine);
    getline(ss, secondLine);

    if (tracker_no == 1) {
        size_t i = firstLine.find(':');
        if (i != string::npos) {
            ip_address = firstLine.substr(0, i);
            port = stoi(firstLine.substr(i + 1));
        }

        const char* filepath = "../client/tracker_info.txt";
        fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0) {
            cerr << "Failed to open file for writing: " << filepath << endl;
            return 1;
        }
        write(fd, firstLine.c_str(), firstLine.size());
        close(fd);

    } else if (tracker_no == 2) {
        size_t i = secondLine.find(':');
        if (i != string::npos) {
            ip_address = secondLine.substr(0, i);
            port = stoi(secondLine.substr(i + 1));
        }

        const char* filepath = "../client/tracker_info.txt";
        fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0) {
            cerr << "Failed to open file for writing: " << filepath << endl;
            return 1;
        }
        write(fd, secondLine.c_str(), secondLine.size());
        close(fd);
    }

     if (ip_address.empty()) {
        cout << "Tracker IP Address not found in the tracker info file." << endl;
        return 1;
    }

    cout << "Using IP Address: " << ip_address << " and Port: " << port << endl;

    int tracker_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tracker_sock == -1) {
        cout << "Failed to create socket." << endl;
        return 1;
    }

    int no = 1;
    if (setsockopt(tracker_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &no, sizeof(no))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    sockaddr_in tracker_addr;
    tracker_addr.sin_family = AF_INET;
    tracker_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip_address.c_str(), &tracker_addr.sin_addr) <= 0) {
        cout << "Invalid IP address." << endl;
        return 1;
    }

    if (bind(tracker_sock, (struct sockaddr*)&tracker_addr, sizeof(tracker_addr)) < 0) {
        cout << "Bind failed." << endl;
        return 1;
    }

    if (listen(tracker_sock, 5) < 0) {
        cout << "Failed to listen on socket." << endl;
        return 1;
    }

    cout << "Tracker is listening on IP Address: " << ip_address << " and Port NO: " << port << endl;

    // sockaddr_in client_addr;
    // socklen_t client_len = sizeof(client_addr);
    // while (true) {
    //     int client_sock = accept(tracker_sock, (struct sockaddr*)&client_addr, &client_len);
    //     if (client_sock < 0) {
    //         cout << "Failed to accept client connection." << endl;
    //         continue;
    //     }

    //     cout << "Client connected with port: " << ntohs(client_addr.sin_port) << endl;

    //     thread client_thread(handleClient, client_sock, client_addr);
    //     client_thread.detach();
    // }
    thread trackerThread(trackerLoop, tracker_sock);
    string input;
    while (true) {
        cin >> input;
        if (input == "quit") {
            threadrun = false;
            _exit(0);
        }
    }

    
    return 0;
}
