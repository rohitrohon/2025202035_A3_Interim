
# Peer to Peer Distributed File Sharing System
## ASSIGNMENT 3 Final Submission

### Submission Format:
2024201017_A3

├── Client

    ├── client.cpp
    ├── hrader.h
    ├── makefile

├── Tracker

    ├── tracker.cpp
    ├──clientrequest.cpp
    ├── header.h
    ├── makefile
├── readme.md



### implementation instructions
open tracker folder and execute these commands
1. make
2. ./tracker <tracker_info.txt> <tracker_no>
open client folder in another new terminal as many clients wanted and run these commands
1. make
2. ./client <ipaddress:portno> <tracker_info.txt> 

now each client is conected to a tracker thread
each client can communicate with tracker independently 

### Commands Executed
1.create_user <user_id> <passwd>

2.login <user_id> <passwd>

3.create_group <group_id>

4.join_group <group_id>

5.leave_group <group_id>

6.list_requests <group_id>

7.accept_request <group_id> <user_id>

8.list_groups

9.logout

10.list_files   <group_id>

11.upload_file  <file_path> <group_id>

12.download_file    <group_id> <file_name> <destination_path>

13.show_downloads

14.stop_share   <group_id> <file_name>

### approach
#### tracker.cpp
tracker will open a lisining port selected from the tracker_info.txt using tracker_no 

when ever a new client request came it binds client to a new thread and the thread executes all client commands 

tracker consists all data related to userid groups and accepts the valid requests from client

download file will download the file from more than one peer concurrently

tracker do not have any access to clients data 

#### client.cpp
client will connect to tracker by sending request to all trackers in tracker_info.txt 

clent will take command and send it to tracker for processing 

tracker sends response after executing command







