#include "header.h"
using namespace std;
unordered_map<string, string> user_data;//{username->password}
unordered_map<string, bool> isloggedin;//{username->islogin?}
vector<string> allgrps;//vector of all groups
unordered_map<string,string> grpadm;//{grpid->clientuserid}
unordered_map<string,set<string>> grpmem;//{grpid->set(userids)}
unordered_map<string,set<string>> pendingreq;//{grpid->set(userids)}
unordered_map<string, pair<int,string>> user_ipport;//{username-><port,ip>}
unordered_map<string, vector<pair<string,string>> > grpdata;//{grpid->vector(filepath,owner})}
unordered_map<string,vector<string>> grpfiles;//{grpid->vector<filenames>}
//unordered_map<string,vector<string>> hashvals;//{filepath->vector(vector{chunk number]= sh1val}//last sh1 value is full file value
unordered_map<string,vector<pair<string,pair<string,string>>>> dwnldrslist;//{filename->vector<pair<userid,pair<filepath,grpid>>>)}
unordered_map<string,pair<string,int>> nochunks;//{filename-><grpid,noofchunks>}
unordered_map<string,string> filehash;//{filepath->hashstring}

pthread_mutex_t userdatamutex;
pthread_mutex_t loginmutex;
pthread_mutex_t allgrpsmutex;
pthread_mutex_t groupadmutex;
pthread_mutex_t grpmemmutex;
pthread_mutex_t penreqmutex;
pthread_mutex_t useripport;
pthread_mutex_t grpdatamutex;
pthread_mutex_t grpfilesmutex;
pthread_mutex_t dwnldrslistmutex;
pthread_mutex_t nochunksmutex;
pthread_mutex_t filehashmutex;
pthread_mutex_t dwnldvecmutex;
struct download_stat {
    string progress;
    string grpid;
    string filename;
};
vector<download_stat> dwnldvec;
void doclientreq(string str,int client_sock ,string& clientuserid ,string clientip ,int clientport){
    vector<string> tokens;
    stringstream commands(str);
    string token;

    while(commands >> token){
        tokens.push_back(token);
    }
        if(tokens[0]=="create_user"){
            createuser(tokens,client_sock);
        }else if(tokens[0]=="login"){
            loginuser(tokens,client_sock,clientuserid,clientport,clientip);
            cout<<endl;
        }else if(tokens[0]=="create_group"){
        //    cout<<"on creating grp";
            creategroup(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="join_group"){
            joingroup(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="leave_group"){
            leavegroup(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="list_requests"){
            listrequests(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="accept_request"){
           acceptrequest(tokens,client_sock,clientuserid);
           cout<<endl;
        }else if(tokens[0]=="list_groups"){
            listgroups(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="logout"){
            logout(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="list_files"){
            listfiles(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="upload_file"){
            uploadfile(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="download_file"){
            downloadfile(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="show_downloads"){
            showdownloads(tokens,client_sock,clientuserid);
            cout<<endl;
        }else if(tokens[0]=="stop_share"){
            stopsharing(tokens,client_sock,clientuserid);
            cout<<endl;
        }else{
            string result="invalid Command";
            cout<<result<<"\n"<<endl;
            ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
            if (bytes_sent < 0) {
                perror("Failed to send message to client");
            }
        }
}
void createuser(vector<string> tokens,int client_sock){
    //Create User Account: create_user <user_id> <passwd>
    string result;
    if(tokens.size()!=3){
        result="invalid arguments to create user";
    }else if(user_data.find(tokens[1])!=user_data.end()){
        result="user "+tokens[1]+" already exist";
    }else{
        pthread_mutex_lock(&userdatamutex);
        pthread_mutex_lock(&loginmutex);
        user_data[tokens[1]]=tokens[2];
        result="Account create sucessfully with user id:"+tokens[1];
        isloggedin[tokens[1]]=false;
        pthread_mutex_unlock(&userdatamutex);
        pthread_mutex_unlock(&loginmutex);
    }
     cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
        if (bytes_sent < 0) {
            perror("Failed to send message to client");
        }
}

void loginuser(vector<string> tokens,int client_sock,string& clientuserid,int clientport ,string clientip){
    //Login: login <user_id> <passwd> 
    string result;
    if(tokens.size()!=3){
        for(auto i:tokens)
        cout<<i<<endl;
        result="invalid arguments for loginuser";
    }else if(user_data.find(tokens[1])==user_data.end()){
        result="user doesn't exist";
    }else if(isloggedin[tokens[1]]==true){
        result="user already logged in ";
    }else if(clientuserid!=""){
        result="user already logged in account "+clientuserid;
    }else{
        pthread_mutex_lock(&userdatamutex);
        pthread_mutex_lock(&loginmutex);
        if(tokens[2]==user_data[tokens[1]]){//if admin
             isloggedin[tokens[1]]=true;
             result="loggedin sucesfully";
             clientuserid=tokens[1];
        }else{
            result="invalid Passsword";
        }  
        user_ipport[clientuserid]=make_pair(clientport,clientip);     
        pthread_mutex_unlock(&userdatamutex);
        pthread_mutex_unlock(&loginmutex);
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}
void creategroup(vector<string> tokens,int client_sock,string &clientuserid){
    //Create Group: create_group <group_id>
    string result;
    int flag=0;
    if(tokens.size()!=2){
        result="invalid arguments for create group";
    }else if(clientuserid==""){
        result="login to create group";
    }else if(find(allgrps.begin(), allgrps.end(), tokens[1]) != allgrps.end()){
        result="group id already exist";
    }else{
         pthread_mutex_lock(&allgrpsmutex);
        pthread_mutex_lock(&groupadmutex);
         pthread_mutex_lock(&grpmemmutex);
        result="group created sucessfully";
            allgrps.push_back(tokens[1]);
            grpadm[tokens[1]]=clientuserid;
            grpmem[tokens[1]].insert(clientuserid);
             pthread_mutex_unlock(&allgrpsmutex);
        pthread_mutex_unlock(&groupadmutex);
         pthread_mutex_unlock(&grpmemmutex);
    }
     cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}
void listgroups(vector<string> tokens,int client_sock,string &clientuserid){
    //List All Group In Network: list_groups
    string result;
    if(tokens.size()!=1){
        result="invalid arguments for list groups";
    }else if(clientuserid==""){
        result="login to list groups";
    }else if(allgrps.size()==0){
        result="no groups exist";
    }else{
        result="************************Existing groups*******************************\n";
        for(auto i:allgrps){
            cout<<i<<endl;
            result+=i+"\n";
        }
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}

void logout(vector<string> tokens,int client_sock,string &clientuserid){
    //Logout: logout
    string result;
    if(tokens.size()!=1){
        result="invalid arguments for login";
    }else if(clientuserid==""){
        result="Not logged in yet!";
    }
    else if(isloggedin[clientuserid]==false){
        result="already logged out";
    }else{
        pthread_mutex_lock(&loginmutex);
        pthread_mutex_lock(&useripport);
        isloggedin[clientuserid]=false;
        clientuserid="";
        result="logged out sucessfully";
        user_ipport[clientuserid]=make_pair(0,"");
        pthread_mutex_unlock(&loginmutex);
        pthread_mutex_unlock(&useripport);
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}
void joingroup(vector<string> tokens,int client_sock,string& clientuserid){
    //Join Group: join_group <group_id>
    string result;
    if(tokens.size()!=2){
        result="invalid arguments for join group";
    }else if(clientuserid==""){
        result="login to join group!";
    }
    else if(find(allgrps.begin(), allgrps.end(), tokens[1]) == allgrps.end()){
        result="group doesn't exist";
    }
    else if(grpmem[tokens[1]].find(clientuserid)!=grpmem[tokens[1]].end()){
        result="you are already member of that group";
    }else if(pendingreq[tokens[1]].find(clientuserid)!=pendingreq[tokens[1]].end()){
        result="request sent already";
    }else{
        pthread_mutex_lock(&penreqmutex);
        pendingreq[tokens[1]].insert(clientuserid);
        pthread_mutex_unlock(&penreqmutex);
        result="request sent to join group with group id "+tokens[1];
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}
void listrequests(vector<string> tokens,int client_sock,string& clientuserid){
    //List pending join: list_requests <group_id>
    string result;
    if(tokens.size()!=2){
        result="invalid arguments for list requests";
    }else if(clientuserid==""){
        result="login to list requests";
    }else if(find(allgrps.begin(), allgrps.end(), tokens[1]) == allgrps.end()){
        result="groups doesn't exist";
    }else if(grpadm[tokens[1]]!=clientuserid){
        result ="you are not admin of group "+tokens[1];
    }else{
        result="************************pending requests*******************************\n";
        for (auto it : pendingreq[tokens[1]]){
        result+=it+"\n";
        }
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}
void acceptrequest(vector<string> tokens,int client_sock,string& clientuserid){
    //Accept Group Joining Request: accept_request <group_id> <user_id>
    string result;
    if(tokens.size()!=3){
        result="invalid arguments for accepting requests";
    }else if(clientuserid==""){
        result="login to accept requests";
    }else if(find(allgrps.begin(), allgrps.end(), tokens[1]) == allgrps.end()){
        result="groups doesn't exist";
    }else if(grpadm[tokens[1]]!=clientuserid){
        result ="you are not admin of group "+tokens[1];
    }else if(pendingreq[tokens[1]].find(tokens[2])==pendingreq[tokens[1]].end()){
        //no request from userid
        result="no request from user "+tokens[2];
    }else{
        pthread_mutex_lock(&grpmemmutex);
        pthread_mutex_lock(&penreqmutex);
        grpmem[tokens[1]].insert(tokens[2]);
        pendingreq[tokens[1]].erase(tokens[2]);
        pthread_mutex_unlock(&grpmemmutex);
        pthread_mutex_unlock(&penreqmutex);
        result="request accepted sucesfully";
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}
void leavegroup(vector<string> tokens,int client_sock,string& clientuserid){
    //Leave Group: leave_group <group_id>
    string result;
    if(tokens.size()!=2){
        result="invalid arguments for leave group";
    }else if(clientuserid==""){
        result="login to leave group!";
    }
    else if(find(allgrps.begin(), allgrps.end(), tokens[1]) == allgrps.end()){
        result="group doesn't exist";
    }
    else if(grpmem[tokens[1]].find(clientuserid)==grpmem[tokens[1]].end()){
        result="you are not member of the group "+tokens[1];
    }else {
        pthread_mutex_lock(&userdatamutex);
        pthread_mutex_lock(&allgrpsmutex);
        pthread_mutex_lock(&groupadmutex);
        pthread_mutex_lock(&grpmemmutex);
        pthread_mutex_lock(&penreqmutex);
        pthread_mutex_lock(&grpdatamutex);
        pthread_mutex_lock(&dwnldrslistmutex);
        //remove from the grpmem
        //if admin change admin grpid->clientuserid
        if(grpadm[tokens[1]]==clientuserid){
            grpmem[tokens[1]].erase(clientuserid);
            if(!grpmem[tokens[1]].empty()){
                string newadm=*grpmem[tokens[1]].begin();
                grpadm[tokens[1]]=newadm;
                cout<<newadm<<" is the new admin of the group"<<endl;
            }
        }else{
            grpmem[tokens[1]].erase(clientuserid);        
        }
        if(grpmem[tokens[1]].empty()){
                //delete group
                allgrps.erase(find(allgrps.begin(),allgrps.end(),tokens[1]));
                grpadm.erase(tokens[1]);
                grpmem.erase(tokens[1]);
                pendingreq.erase(tokens[1]);
        }
        vector<string> filesToRemove;
        if (grpdata.find(tokens[1]) != grpdata.end()) {
            auto& grpDataVec = grpdata[tokens[1]];
            for (auto it = grpDataVec.begin(); it != grpDataVec.end(); ) {
                if (it->second == clientuserid) {
                    filesToRemove.push_back(it->first); 
                    it = grpDataVec.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (grpfiles.find(tokens[1]) != grpfiles.end()) {
                auto& grpFilesVec = grpfiles[tokens[1]];
                grpFilesVec.erase(remove_if(grpFilesVec.begin(), grpFilesVec.end(),[&filesToRemove](const string& fileName) {
                return any_of(filesToRemove.begin(), filesToRemove.end(),
                    [&fileName](const string& filePath) {
                        return filePath.find(fileName) != string::npos;
                    });
            }),
            grpFilesVec.end()
        );
        }
        for (auto& [filename, seedersList] : dwnldrslist) {
            for (auto it = seedersList.begin(); it != seedersList.end(); ) {
                const auto& [userId, info] = *it;
                const auto& [filePath, gid] = info;
                if (userId == clientuserid && gid == tokens[1] &&
                    find(filesToRemove.begin(), filesToRemove.end(), filePath) != filesToRemove.end()) {
                    it = seedersList.erase(it); 
                } else {
                    ++it;
                }
            }
        }
        pthread_mutex_unlock(&userdatamutex);
        pthread_mutex_unlock(&allgrpsmutex);
        pthread_mutex_unlock(&groupadmutex);
        pthread_mutex_unlock(&grpmemmutex);
        pthread_mutex_unlock(&penreqmutex);
        pthread_mutex_unlock(&grpdatamutex);
        pthread_mutex_unlock(&dwnldrslistmutex);
        result="removed "+clientuserid+" from "+tokens[1];
        }
        cout<<result<<endl;
        ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
        if (bytes_sent < 0) {
            perror("Failed to send message to client");
        }

}

void uploadfile(vector<string> tokens,int client_sock,string &clientuserid){
    //upload_file <file_path> <group_id> <ssh> <noofchunks>
    string result;
    cout<<"tokens size"<<tokens.size()<<endl;
    if(tokens.size()!=5){
        result="invalid arguments for uploading files";
    }else if(clientuserid==""){
        result="login to upload files";
    } else if(find(allgrps.begin(), allgrps.end(), tokens[2]) == allgrps.end()){
        result="group doesn't exist";
    }else if(grpmem[tokens[2]].find(clientuserid)==grpmem[tokens[2]].end()){
        result="you are not member of grp ";
    }else{
        pthread_mutex_lock(&grpdatamutex);
        pthread_mutex_lock(&grpfilesmutex);
        pair<string,string> reqpair = {tokens[1],clientuserid};
        auto itr=find_if(grpdata[tokens[2]].begin(), grpdata[tokens[2]].end(),
                           [&](const pair<string, string>& p) {
                               return p == reqpair;
                           });
        if(itr!=grpdata[tokens[2]].end()){
            result="you have already uploaded the file";
        }else{
            
            //cal hash val of the file and upload along with path
            grpdata[tokens[2]].push_back(make_pair(tokens[1],clientuserid));
            result="uploaded the file sucesfully";
            //extract file name add to grpfiles
            size_t pos = tokens[1].find_last_of("/");
            string filename;
            if((pos != string::npos)){
                filename=tokens[1].substr(pos + 1);
            }else{
                filename=tokens[1];
            }
            grpfiles[tokens[2]].push_back(filename);
            dwnldrslist[filename].push_back(make_pair(clientuserid,make_pair(tokens[1],tokens[2])));
            // vector<string> hashes;        
            // istringstream ssh_stream(tokens[3]);
            // string hash;
            // while (getline(ssh_stream, hash, '@')){
            //     hashes.push_back(hash);
            // }
            nochunks[filename]=make_pair(tokens[2],stoi(tokens[4]));
            //hashvals[filename] = hashes;
            filehash[tokens[1]]= tokens[3];
            //cout<<"hash: "<< tokens[3]<<endl;
            pthread_mutex_unlock(&grpdatamutex);
            pthread_mutex_unlock(&grpfilesmutex);
            }
            
   }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}
void  listfiles(vector<string> tokens,int client_sock,string &clientuserid){
    //list_files <group_id>
     string result;
    if(tokens.size()!=2){
        result="invalid arguments for list files";
    }else if(clientuserid==""){
        result="login to list files!";
    }
    else if(find(allgrps.begin(), allgrps.end(), tokens[1]) == allgrps.end()){
        result="group doesn't exist";
    }
    else if(grpmem[tokens[1]].find(clientuserid)==grpmem[tokens[1]].end()){
        result="you are not a member of that group";
    }else{
        result="************************list files*******************************\n";
        for(auto i:grpfiles[tokens[1]]){
            result+=i+"\n";
        }
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }
}
string pieceselectionalgo(vector<string> tokens,int client_sock,string &clientuserid){
    cout<<"inpca"<<endl;
        string message;
        int noofseeders=0;//no of seeders
        int noofchunks;

        if(nochunks[tokens[2]].first==tokens[1])
        noofchunks=nochunks[tokens[2]].second;
        for(auto i:dwnldrslist[tokens[2]]){
            if(isloggedin[i.first]){
                message+=to_string(user_ipport[i.first].first)+"-"+user_ipport[i.first].second+"-"+i.second.first+"@";//port-ip-file_path
                noofseeders++;
            }
        }
        cout<<"no of chunks: "<<noofchunks<<"no of seeders: "<<noofseeders<<endl;
        message=to_string(noofseeders)+"-"+to_string(noofchunks)+"-"+tokens[3]+"-"+message;//noofseeders-noofchunks-destination_path-noofchunks-port-ip-file_path
        //cout<<"psa string"<<message;
        return message;
}
void downloadfile(vector<string> tokens,int client_sock,string &clientuserid){
   // download_file <group_id> <file_name> <destination_path>
   //cout<<"enter downloads"<<endl;
    string result;
    if(tokens.size()!=4){
        result="invalid arguments to download files";
        ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    }else if(clientuserid==""){
        result="login to download files";
        ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    } else if(find(allgrps.begin(), allgrps.end(), tokens[1]) == allgrps.end()){
        result="group doesn't exist";
        ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    }else if(grpmem[tokens[1]].find(clientuserid)==grpmem[tokens[1]].end()){
        result="you are not member of grp";
        ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    }else if(find(grpfiles[tokens[1]].begin(), grpfiles[tokens[1]].end(), tokens[2]) == grpfiles[tokens[1]].end()){
        result="file doesn't exist";
        ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    }else{
        pthread_mutex_lock(&dwnldvecmutex);
        //cout<<"send details to sender"<<endl;
        string message=pieceselectionalgo(tokens,client_sock,clientuserid);
        ssize_t bytes_sent = send(client_sock, message.c_str(), message.length(), 0);//sending seeder details
        if (bytes_sent < 0){
                perror("Failed to send message to client");
        }
        download_stat prog = { "[D]",tokens[1],tokens[2]};
        dwnldvec.push_back(prog);
        char buffer[1024] = {0};

        int valread = read(client_sock, buffer, sizeof(buffer));//read hash value
        
        string hash=string(buffer);
        message="no";
        // while (getline(ssh_stream, hash, '@')) {
        //        hashes.push_back(hash);
        // }
        for(auto i:grpdata[tokens[1]]){
            if(i.first.find(tokens[2])!=-1){
                if(hash==filehash[i.first]){
                    message="yes";
                }
            }
        }       
       // cout<<"fullfile Hash: "<<hash<<endl;message="yes";
        bytes_sent = send(client_sock, message.c_str(), message.length(), 0);
        if(message=="yes"){
            result="download completed ";
          //keep download status in completed
            dwnldvec.erase(remove_if(dwnldvec.begin(), dwnldvec.end(), [&](const download_stat& prog) {
            return ((prog.progress == "[D]" && prog.grpid == tokens[1]) && prog.filename == tokens[2]);
            }), dwnldvec.end());
             prog = { "[c]",tokens[1],tokens[2]};
            dwnldvec.push_back(prog);
            //add in seeder list
             grpdata[tokens[1]].push_back(make_pair(tokens[3],clientuserid));
             dwnldrslist[tokens[2]].push_back(make_pair(clientuserid,make_pair(tokens[3],tokens[1])));
            
        }else{
             
             dwnldvec.erase(remove_if(dwnldvec.begin(), dwnldvec.end(), [&](const download_stat& prog) {
            return ((prog.progress == "[D]" && prog.grpid == tokens[1]) && prog.filename == tokens[2]);
            }), dwnldvec.end());
            result="download failed ";
        }
        pthread_mutex_unlock(&dwnldvecmutex);
    }
    cout<<result<<endl;
}
void showdownloads(vector<string>  tokens,int client_sock, string clientuserid){
    string result="";
    if(dwnldvec.size()==0){
        result="no downloads yet!";
    }
    else{
        for(auto i:dwnldvec){
            result+=i.progress+" "+i.grpid+" "+i.filename+"\n";
        }
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }

}
void stopsharing(vector<string>  tokens,int client_sock, string clientuserid){
    //stop_share <group_id> <file_name>
    //cout<<"on stop sharing"<<endl;
    string result;
    if(tokens.size()!=3){
        result="invalid arguments to stop sharing";
    }else if(clientuserid==""){
        result="login to stop sharing";
    } else if(find(allgrps.begin(), allgrps.end(), tokens[1]) == allgrps.end()){
        result="group doesn't exist";
    }else if(grpmem[tokens[1]].find(clientuserid)==grpmem[tokens[1]].end()){
        result="you are not member of grp ";
    }else if(find(grpfiles[tokens[1]].begin(), grpfiles[tokens[1]].end(), tokens[2]) == grpfiles[tokens[1]].end()){
        result="file doesn't exist";
    }else{
        pthread_mutex_lock(&dwnldrslistmutex);
        for(auto it = dwnldrslist[tokens[2]].begin(); it != dwnldrslist[tokens[2]].end(); ) {
            if (it->first == clientuserid && it->second.first.find(tokens[2]) != string::npos) {
                it = dwnldrslist[tokens[2]].erase(it);
                result="sharing stopped for file "+tokens[2]+" in group "+tokens[1];
                break;
            } else {
                ++it; 
            }
        }
        pthread_mutex_unlock(&dwnldrslistmutex);
    }
    cout<<result<<endl;
    ssize_t bytes_sent = send(client_sock, result.c_str(), result.length(), 0);
    if (bytes_sent < 0) {
            perror("Failed to send message to client");
    }


}