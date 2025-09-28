#include "header.h"
using namespace std;
struct peer {
    string port;
    string ip;
    string filepath;
};
vector<peer> parseClientData(string& message) {
    vector<peer> peers;
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
            peer cli = {port, ip, filepath};
            peers.push_back(cli);
        }
    }
    return peers;
}
string calculateSHA1(const std::string& input) {
    unsigned char hash[20];
    string str="";
    SHA1(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);
    stringstream ss;
    for (int i = 0; i < 20; ++i) {
        char buf[3];
            sprintf(buf, "%02x", hash[i]&0xff);
            str += string(buf);
    }
    
    return str;
}

void handle_client(int client_sock){   
    
     //cout<<"on hc";
   // while (true){      
        char buffer[512*1024]={0};
        int valread = read(client_sock, buffer, sizeof(buffer));//read file name-chunkno
        string request=string(buffer); //send data from that file

        size_t one = request.find('$');
        size_t two = request.find('$', one + 1);

        string filepath = request.substr(0, one);
        int chunkno = stoi(request.substr(one + 1, two - one - 1));
        int nchunks = stoi(request.substr(two + 1));
        ssize_t totalsize=(512 * 1024*nchunks);
        //char BUFFER[buffsize]={0};
        // cout<<"got filepath from peer -"<<filepath<<endl;
       // memset(buffer, 0, sizeof(buffer));
        int file = open(filepath.c_str(), O_RDONLY);
        if (file < 0){
            cout << "Error opening file with file path "<<filepath<<endl;
            return;
        }
        if (lseek(file, 512*1024*chunkno, SEEK_SET) < 0) {
            cerr << "Error seeking to start byte\n";
            close(file);
            return;
        }
        ssize_t bytesRead,totalbytesread=0;
        while ((bytesRead = read(file, buffer, sizeof(buffer))) > 0 && totalbytesread<totalsize){
           // cout<<"bytesRead"<<bytesRead<<endl;
           totalbytesread+=bytesRead;
            ssize_t bytesSent = send(client_sock, buffer, bytesRead, 0);
            //cout<<"bytesSent"<<bytesSent<<endl;
            if (bytesSent < 0){
                cout << "Error sending file data\n";
                close(file);
                return;
            }
            memset(buffer, 0, sizeof(buffer));
    }
        // send(client_sock, "END_OF_FILE", 11, 0);
    //cout<<"data sent"<<endl;
        string filename=filepath.substr(filepath.find_last_of('/') + 1);
        cout<<"sent chunk of file: "<<filename<<" to peer "<<endl;
        close(client_sock);
        close(file);
    //}
}
void start_listening(int listen_port){
    int listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_sock == -1) {
        cerr << "Failed to create listening socket." << endl;
        return;
    }

    sockaddr_in listener_addr;
    listener_addr.sin_family = AF_INET;
    listener_addr.sin_addr.s_addr = INADDR_ANY;
    listener_addr.sin_port = htons(listen_port);

    if (bind(listener_sock, (struct sockaddr*)&listener_addr, sizeof(listener_addr)) < 0){
        cerr << "Binding failed on port " << listen_port << endl;
        return;
    }
    if (listen(listener_sock, 5) < 0) {
        cerr << "Failed to listen on socket." << endl;
        return;
    }
    cout << "Listening for peer connections on port " << listen_port << endl;

    while (true) {
        sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int peer_sock = accept(listener_sock, (struct sockaddr*)&peer_addr, &peer_len);
        if (peer_sock < 0) {
            cerr << "Failed to accept peer connection." << endl;
            continue;
        }
        thread(handle_client, peer_sock).detach();
    }
}
int main(int argc, char *argv[]) {
   
    if (argc != 3) {
        cout << "Invalid parameter Format: ./client <IP>:<PORT> tracker_info.txt" << endl;
        return 0;
    }
    string ipandport = argv[1];
    string trackerfilename = argv[2];
    int i = ipandport.find(':');
    if (i == string::npos || i == 0) {
        cout << "Invalid format for <IP>:<PORT>" << endl;
        return 0;
    }

    string ip_address = ipandport.substr(0, i);
    int listen_port = stoi(ipandport.substr(i + 1));
    trackerfilename="../Tracker/"+trackerfilename;
    int fd = open(trackerfilename.c_str(), O_RDONLY);
    if (fd < 0) {
        cerr << "Failed to open " << trackerfilename << endl;
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
    i = firstLine.find(':');
    if (i == string::npos || i == 0) {
        cout << "Invalid format for <IP>:<PORT>" << endl;
        return 0;
    }

    string track_ip1 = firstLine.substr(0, i);
    int port1 = stoi(firstLine.substr(i + 1));

    i = secondLine.find(':');
    if (i == string::npos || i == 0) {
        cout << "Invalid format for <IP>:<PORT>" << endl;
        return 0;
    }

    string track_ip2 = secondLine.substr(0, i);
    int port2 = stoi(secondLine.substr(i + 1));

    cout << "Connecting to Tracker...\n";
    

    
    thread listening_thread(start_listening, listen_port);
    listening_thread.detach();

    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        cout << "Failed to create socket." << endl;
        return 0;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port1);
    string track_ip=track_ip1;
    int port=port1;
   
    if (inet_pton(AF_INET, track_ip1.c_str(), &server_addr.sin_addr) <= 0) {
        track_ip=track_ip2;
        if (inet_pton(AF_INET, track_ip2.c_str(), &server_addr.sin_addr) <= 0){
            cout << "Invalid IP address." << endl;
            return 0;
        } 
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
        server_addr.sin_port = htons(port2);
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
            cout << "Connection failed." << endl;
            return 0;
        }
        track_ip=track_ip2;
        port=port2;
    }
    cout << "Connected to server " << track_ip << " on port " << port << endl;
    
    ssize_t bytessend = send(sock, ipandport.c_str(), ipandport.length(), 0);//sending client ip and port
     if(bytessend<0){
            cout<<"failed to send client portno"<<endl;
            return 0;
    }
      
    while (true) {
    string str;
    getline(cin, str);
    ssize_t bytes_sent;
    if(str.substr(0,12)=="upload_file "){
        //open file cal the chunk value and append in str
        size_t one = str.find(' ');
        size_t two = str.find(' ', one + 1);
        string filePath = str.substr(one + 1, two - one - 1);
        int file = open(filePath.c_str(), O_RDONLY);
        if (file < 0) {
            cout << "File not found" << endl;
            continue;
        }
        char buffer[512 * 1024]={0};
        ssize_t bytesRead;
        const unsigned char* shval;
        string sh1="";
        string fullfiledata="";
        long long nochunks=0;
        // Read the file in 512 KB chunks
        while ((bytesRead = read(file, buffer, sizeof(buffer))) > 0) {
             string Chunkdata(buffer, bytesRead);
            //sh1 += calculateSHA1(Chunkdata) + "@";
            fullfiledata+=string(buffer);
            memset(buffer, 0, sizeof(buffer));
            nochunks++;
         }
        sh1+=calculateSHA1(fullfiledata);
        //cout<<sh1<<endl;
        //cout<<"no of chunks: "<<nochunks<<endl;
        
         str=str+" "+sh1+" "+to_string(nochunks);
         bytes_sent = send(sock, str.c_str(), str.length(), 0);
    }else{
         bytes_sent = send(sock, str.c_str(), str.length(), 0);
    }    
    if (bytes_sent < 0) {
        cout << "failed to send client" << endl;
    } else {
        if (str.substr(0, 14) == "download_file "){        ///download file case
            //char buffer[512 * 1024] = {0};
            int noofseeders, noofchunks;
            string destination_path;
            memset(buffer, 0, sizeof(buffer));
            ssize_t valread = read(sock, buffer, sizeof(buffer));
            string peerstr = string(buffer);//noofseeders-noofchunks-destination_path-noofchunks-port-ip-file_path
            //cout<<peerstr<<endl;
            if(peerstr=="invalid arguments to download files"|| peerstr=="login to download files" || peerstr=="group doesn't exist" || peerstr=="you are not member of grp" || peerstr=="file doesn't exist"){
                
                continue;
            }else if (valread > 0){

                size_t one = peerstr.find('-');
                size_t two = peerstr.find('-', one + 1);
                size_t three = peerstr.find('-', two + 1);

                noofseeders = stoi(peerstr.substr(0, one));
                noofchunks = stoi(peerstr.substr(one + 1, two - one - 1));
                string destination_path = peerstr.substr(two + 1, three - two - 1);

                peerstr = peerstr.substr(three + 1);
                int chunksperseeder=noofchunks/noofseeders;
                int remainingchunks= noofchunks % noofseeders;
               
               
                int seederno=0;
                
                vector<peer> seeders = parseClientData(peerstr);
                //handle no of chunks<seeders
                char mainbuffer[512*1024*noofchunks]={0};i=0;
                string fullfiledata="";
                string sh1="";
                //cout<<"seeders vec size "<<seeders.size()<<endl;
                for (const auto& client : seeders){
                    //cout<<"connection : "<<i<<endl;
                    int readchunks,chunksreaded=0;
                    if(chunksreaded>=noofchunks){
                        break;
                    }
                    if(noofseeders-1==seederno && remainingchunks!=0){
                        if(noofseeders>noofchunks){
                            readchunks=1;
                        }
                        else{
                            readchunks=remainingchunks;
                        }
                    }else{
                        if(noofseeders>noofchunks){
                            readchunks=1;
                        }
                        else{
                            readchunks=chunksperseeder;
                        }   
                    }
                    sockaddr_in client_add;
                    socklen_t client_l = sizeof(client_add);
                    client_add.sin_family = AF_INET;
                    client_add.sin_port = htons(stoi(client.port));
                    string client_ip = client.ip;
                    int client_sock = socket(AF_INET, SOCK_STREAM, 0);
                    
                    if (client_sock == -1) {
                        cout << "Failed to create socket." << endl;
                        continue;
                    }

                    if (inet_pton(AF_INET, client_ip.c_str(), &client_add.sin_addr) <= 0) {
                        cout << "Invalid IP address." << endl;
                        continue;
                    }

                    if (connect(client_sock, (struct sockaddr*)&client_add, sizeof(client_add)) < 0) {
                        cout << "Connection failed." << endl;
                        continue;
                    }

                    string message = client.filepath+"$"+to_string(chunksreaded)+"$"+to_string(readchunks);//filepath$readchunks$seederno
                    //cout << "calling peer with " << message << endl;
                    ssize_t bytes_sent = send(client_sock, message.c_str(), message.length(), 0); // sending file path
                    
                    if (bytes_sent < 0) {
                        perror("Failed to send message to client");
                        continue;
                    }
                   
                    ssize_t bytesReceived,startadd=512*1024*chunksreaded;

                    while ((bytesReceived = read(client_sock, buffer, sizeof(buffer))) > 0 ){
                        fullfiledata+=string(buffer);
                        memcpy(mainbuffer + startadd, buffer, bytesReceived);
                        startadd += bytesReceived;
                        memset(buffer, 0, sizeof(buffer));
                    }
                    cout<<"Got chunk "<<i<<" from peer with hash: "<<calculateSHA1(string(buffer))<<endl;
                    //bytes_sent = send(sock, sh1.c_str(), sh1.length(), 0);
                    memset(buffer, 0, sizeof(buffer));i++;
                    // ssize_t valread = read(sock, buffer, sizeof(buffer));
                    // string result=string(buffer);
                    // if(result=="yes"){
                    //     result="download completed ";

                    // }else{
                    //     result="download completed ";
                    // }
                    // cout<<result<<endl;
                    chunksreaded+=readchunks;
                }
                sh1=calculateSHA1(fullfiledata);
                bytes_sent = send(sock, sh1.c_str(), sh1.length(), 0);
                    int file = open(destination_path.c_str(), O_CREAT | O_WRONLY, 0666);
                    if (file < 0) {
                        std::cerr << "Error opening file\n";
                        continue;
                    }
                   ssize_t bytesWritten = write(file, mainbuffer, 512 * 1024 * noofchunks);
                   //cout<<"file saved"<<endl;
                    close(file);
                     ssize_t valread = read(sock, buffer, sizeof(buffer));
                    string result=string(buffer);
                    if(result=="yes"){
                        result="download completed ";

                    }else{
                        result="download falied ";
                    }
                    cout<<result<<endl;i++;
            } else {
                cout << "Tracker Went Down2!" << endl;
            }
        } else {
            char buffer[1024] = {0};
            ssize_t valread = read(sock, buffer, sizeof(buffer));
            
            if (valread > 0) {
                cout << buffer<<"\n"<< endl;
            } else {
                cout << "Tracker Went Down!" << endl;
            }
        }
    }
}

    close(sock);
    return 0;
}
