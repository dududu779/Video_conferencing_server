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

#define COLOR_RED "\033[1;31m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_RESET "\033[0m"

#define USERS_FILE_PATH "../users.txt"

// UDP 中继端口（客户端 register_rtp 后按这个端口收发媒体）
#define UDP_PORT 10001

// ==================== 用户信息结构体 ====================
typedef struct
{
    string user_name; // 用户名
    int user_id;      // 用户唯一 ID
    int client_fd;    // 该用户对应的 TCP 套接字描述符（服务端主动给他推信令时需要用到这个 fd）
    int user_room;    // 用户当前所在的房间号（初始为 -1，表示未加入任何房间）

    string session_token; // 会话凭证

    string rtp_ip; // 客户端接收音视频流的 IP 地址（通常根据 TCP 连接的对端 IP 获取）
    int rtp_port;  // 客户端期望接收音视频流的 UDP 端口（由 register_rtp 上报，中继转发时的目标端口）
} User_Info;

// ==================== 房间信息结构体 ====================
typedef struct
{
    int room_id;        // 房间唯一 ID（从 1 开始自增分配，创建/加入房间时使用）
    string room_name;   // 房间名称（例如 "会议室A"，用于列表展示）
    string room_master; // 房主的用户名（记录是谁创建的房间，房主退出时会触发房间销毁逻辑）

    vector<string> members; // 房间内的所有成员列表（存储用户名，用于广播通知，以及在 UDP 中继时查找同房间的人）
} Room_Info;

void user_db_load(const char *path = USERS_FILE_PATH);
void handle_client_json(int client_fd, cJSON *root, string &user);
int get_user_room_by_id(int user_id);
int room_of_udp_sender(const sockaddr_in &sender, int &pub_uid);
std::vector<User_Info> get_room_members_snapshot(int room_id);