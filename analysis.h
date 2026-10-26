#pragma once
#include "cJSON.h"
#include <iostream>
#include <ctime>
#include <vector>
#include <map>
#include <mutex>
#include <string>
#include <algorithm>
#include <netinet/in.h>

using namespace std;

#define COLOR_RED   "\033[1;31m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_RESET "\033[0m"

#define USERS_FILE_PATH "../users.txt"

// UDP 中继端口（客户端 register_rtp 后按这个端口收发媒体）
#define UDP_PORT 10001

typedef struct {
    string user_name;
    int    user_id;
    int    client_fd;
    int    user_room;          // -1 表示未加入房间
    string session_token;      // 用于后续校验，不改变外部行为
    string rtp_ip;
    int    rtp_port;
} User_Info;

typedef struct {
    int    room_id;
    string room_name;
    string room_master;
    vector<string> members;
} Room_Info;

void user_db_load(const char *path = USERS_FILE_PATH);
void handle_client_json(int client_fd, cJSON *root, string &user);
int get_user_room_by_id(int user_id);
int room_of_udp_sender(const sockaddr_in &sender, int &pub_uid);
std::vector<User_Info> get_room_members_snapshot(int room_id);