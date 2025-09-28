#ifndef HEADER_H
#define HEADER_H
#include<bits/stdc++.h>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
using namespace std;

extern unordered_map<string,string> user_data;
extern unordered_map<string, bool> isloggedin;
extern vector<string> allgrps;
extern unordered_map<string,string> grpadm;
extern unordered_map<string,set<string>> grpmem;
extern unordered_map<string,set<string>> pendingreq;
extern unordered_map<string, pair<int,string>> user_ipport;
extern unordered_map<string, vector<pair<string,string>> > grpdata;
extern unordered_map<string,vector<string>> grpfiles;



void createuser(vector<string> tokens,int client_sock);
void doclientreq(string str,int client_sock,string& clientuserid,string clientip, int clientport);
void loginuser(vector<string> tokens,int client_sock,string& clientuserid,int clientport,string clientip);
void creategroup(vector<string> tokens,int client_sock,string &clientuserid);
void listgroups(vector<string> tokens,int client_sock,string &clientuserid);
void logout(vector<string> tokens,int client_sock,string &clientuserid);
void joingroup(vector<string> tokens,int client_sock,string& clientuserid);
void listrequests(vector<string> tokens,int client_sock,string& clientuserid);
void acceptrequest(vector<string> tokens,int client_sock,string& clientuserid);
void leavegroup(vector<string> tokens,int client_sock,string& clientuserid);
void sendtoclient(vector<string> tokens,int client_sock,string &clientuserid);
void uploadfile(vector<string> tokens,int client_sock,string &clientuserid);
void  listfiles(vector<string> tokens,int client_sock,string &clientuserid);
void downloadfile(vector<string> tokens,int client_sock,string &clientuserid);
void showdownloads(vector<string>  tokens,int client_sock, string clientuserid);
void stopsharing(vector<string>  tokens,int client_sock, string clientuserid);
#endif