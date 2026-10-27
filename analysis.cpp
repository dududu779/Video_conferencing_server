#include "analysis.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <string>
#include <fstream>
#include <map>
#include <mutex>
#include "cJSON.h"

#define USERS_FILE_PATH "../users.txt"

// ==================== 全局状态与锁 ====================
// 账号信息：<用户名, 密码>
static map<string, string> g_users_map;
static mutex g_user_mutex;

// 在线用户信息：<用户名, 用户详细信息>，记录用户的ID、套接字、所在房间等
static map<string, User_Info> g_id_map;
static mutex g_id_mutex;

// 房间信息：<房间号, 房间详细信息>，记录房间名称、房主、成员列表
static map<int, Room_Info> g_room_map;
static mutex g_room_mutex;

// ==================== 基础工具函数 ====================

// 发送 JSON 响应，并附带日志打印
static bool send_json_response(int fd, cJSON *obj)
{
    char *str = cJSON_PrintUnformatted(obj);
    if (str == nullptr)
        return false;

    string send_str = string(str) + '\n'; // 协议规定以 \n 结尾

    int ret = write(fd, send_str.c_str(), send_str.length());
#if 1
    // 除了心跳包，其他的响应都在终端打印出来方便调试
    cJSON *type_item = cJSON_GetObjectItem(obj, "type");
    if (type_item != nullptr && cJSON_IsString(type_item))
    {
        if (strcmp(type_item->valuestring, "heartbeat_resp") != 0)
        {
            cout << COLOR_GREEN << __LINE__ << "发送:" << send_str << COLOR_RESET;
        }
    }
#endif
    free(str);
    return ret > 0;
}

// 获取空闲用户id (从1001开始，如果1001被占用则顺延到1002)
// 注意：调用此函数前必须持有 g_id_mutex 锁
static int get_user_id(void)
{
    int id = 1001;
    bool is_user = false;
    do
    {
        is_user = false;
        for (auto &trav : g_id_map)
        {
            if (trav.second.user_id == id)
            {
                is_user = true;
                id++;
                break;
            }
        }
    } while (is_user);
    return id;
}

// 获取空闲房间号 (从1开始)
// 注意：调用此函数前必须持有 g_room_mutex 锁
static int get_room_id(void)
{
    int id = 1;
    bool is_using = false;
    do
    {
        is_using = false;
        for (auto &trav : g_room_map)
        {
            if (trav.second.room_id == id)
            {
                is_using = true;
                id++;
                break;
            }
        }
    } while (is_using);
    return id;
}

// ==================== 房间广播与通知 ====================

// 推送房间成员列表给指定用户 (通常在刚进房间时调用)
static void send_room_users_to(int client_fd, int room_id, const vector<string> &members)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "room_users");
    cJSON_AddNumberToObject(root, "room_id", room_id);

    cJSON *arr = cJSON_CreateArray();
    for (auto &un : members)//轮流取出用户名
    {
        int uid = 0;
        {
            lock_guard<mutex> lk(g_id_mutex);
            auto it = g_id_map.find(un);
            if (it != g_id_map.end())
                uid = it->second.user_id;//用un当作键找到id
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "user_id", uid);
        cJSON_AddStringToObject(o, "username", un.c_str());
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "users", arr);

    send_json_response(client_fd, root);
    cJSON_Delete(root);
}

// 广播有新成员加入 (只发给房间里除自己外的其他人)
static void broadcast_user_joined(const string &joiner, int room_id, const vector<string> &members)
{
    int joiner_id = 0;
    {
        lock_guard<mutex> lk(g_id_mutex);
        auto it = g_id_map.find(joiner);
        if (it != g_id_map.end())
            joiner_id = it->second.user_id;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "user_joined");
    cJSON_AddNumberToObject(root, "room_id", room_id);
    cJSON_AddNumberToObject(root, "user_id", joiner_id);
    cJSON_AddStringToObject(root, "username", joiner.c_str());

    for (auto &m : members)
    {
        if (m == joiner) // 不给自己发
            continue;
        int fd = -1;
        // 短临界区：只查fd，发网络包在锁外做
        {
            lock_guard<mutex> lk(g_id_mutex);
            auto it = g_id_map.find(m);
            if (it != g_id_map.end())
                fd = it->second.client_fd;
        }
        if (fd >= 0)
            send_json_response(fd, root);
    }
    cJSON_Delete(root);
}

// 广播有成员离开
static void broadcast_user_left(const string &leaver, int room_id, const vector<string> &remaining_members)
{
    int leaver_id = 0;
    {
        lock_guard<mutex> lk(g_id_mutex);
        auto it = g_id_map.find(leaver);
        if (it != g_id_map.end())
            leaver_id = it->second.user_id;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "user_left");
    cJSON_AddNumberToObject(root, "room_id", room_id);
    cJSON_AddNumberToObject(root, "user_id", leaver_id);
    cJSON_AddStringToObject(root, "username", leaver.c_str());

    for (auto &m : remaining_members)
    {
        int fd = -1;
        {
            lock_guard<mutex> lk(g_id_mutex);
            auto it = g_id_map.find(m);
            if (it != g_id_map.end())
                fd = it->second.client_fd;
        }
        if (fd >= 0)
            send_json_response(fd, root);
    }
    cJSON_Delete(root);
}

// ==================== 报文组装模块 ====================

static void reply_msg(int client_fd, const char *type, int code)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "code", code);
    send_json_response(client_fd, root);
    cJSON_Delete(root);
}

static void reply_login(int client_fd, const char *type, int code, int user_id, const char *session_token, int expires_in, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "user_id", user_id);
    cJSON_AddStringToObject(root, "session_token", session_token);
    cJSON_AddNumberToObject(root, "expires_in", expires_in);
    cJSON_AddStringToObject(root, "message", message);
    send_json_response(client_fd, root);
    cJSON_Delete(root);
}

static void reply_create_room(int client_fd, const char *type, int code, int room_id, const char *room_name)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "room_id", room_id);
    cJSON_AddStringToObject(root, "room_name", room_name);
    send_json_response(client_fd, root);
    cJSON_Delete(root);
}

static void reply_room_list(int fd, int code, const vector<string> &rooms)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "room_list_resp");
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", code == 0 ? "ok" : "error");
    cJSON *arr = cJSON_CreateArray();
    for (auto &s : rooms)
        cJSON_AddItemToArray(arr, cJSON_CreateString(s.c_str()));
    cJSON_AddItemToObject(root, "user_list", arr); // 协议规定的奇葩字段名：user_list
    send_json_response(fd, root);
    cJSON_Delete(root);
}

static void reply_join_room(int fd, int code, int room_id, int rtp_port, const char *rtp_host)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "join_room_resp");
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "room_id", room_id);
    cJSON_AddNumberToObject(root, "rtp_port", rtp_port); // 告诉客户端 UDP 往哪个端口发
    cJSON_AddStringToObject(root, "rtp_host", rtp_host); // 告诉客户端 UDP 往哪个 IP 发
    send_json_response(fd, root);
    cJSON_Delete(root);
}

static void reply_leave_room(int fd, int code, int room_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "leave_room_resp");
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "room_id", room_id);
    send_json_response(fd, root);
    cJSON_Delete(root);
}

static void reply_register_rtp(int client_fd, int code, int relay_port)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "register_rtp_resp");
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "relay_port", relay_port);
    send_json_response(client_fd, root);
    cJSON_Delete(root);
}

static void reply_broadcast(int client_fd, int code, const char *message, const char *error_msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "broadcast_resp");
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddStringToObject(root, "error_msg", error_msg);
    send_json_response(client_fd, root);
    cJSON_Delete(root);
}

// ==================== 账号文件操作 ====================

// 注册账户   0 成功 / 1 已存在 / 2 参数非法 / 3 写文件失败
static int user_register(const string &un, const string &pwd)
{
    if (un.empty() || pwd.empty() ||
        un.find('\n') != string::npos || pwd.find('\n') != string::npos ||
        un.find(':') != string::npos || pwd.find(':') != string::npos)
    {
        return 2;
    }

    lock_guard<mutex> lk(g_user_mutex);
    if (g_users_map.count(un))
        return 1;

    ofstream fout(USERS_FILE_PATH, ios::app);
    if (!fout.is_open())
    {
        cout << "文件打开失败" << endl;
    }
    fout << un << ':' << pwd << '\n';
    if (!fout)
        return 3;

    g_users_map[un] = pwd;
    return 0;
}

// 校验密码   0 成功 / 1 未找到用户名 / 2 密码错误
static int user_login_check(const string un, const string pwd)
{
    lock_guard<mutex> lk(g_user_mutex);
    auto temp = g_users_map.find(un);
    if (temp == g_users_map.end())
        return 1;
    if (temp->second == pwd)
        return 0;
    return 2;
}

// ==================== 业务处理模块 ====================

// 处理注册
static void handle_register_msg(int client_fd, cJSON *root)
{
    cJSON *username = cJSON_GetObjectItem(root, "username");
    cJSON *password = cJSON_GetObjectItem(root, "password_hash");

    if (!cJSON_IsString(username) || !cJSON_IsString(password))
    {
        cout << "数据校验错误:" << __LINE__ << endl;
        return;
    }

    string un = username->valuestring;
    string pwd = password->valuestring;

    switch (user_register(un, pwd))
    {
    case 0:
        reply_msg(client_fd, "register_resp", 0);
        break;
    case 1:
        cout << "用户已存在" << endl;
        reply_msg(client_fd, "register_resp", 5);
        break;
    case 2:
        cout << "参数非法" << endl;
        break;
    case 3:
        cout << "写文件失败" << endl;
        break;
    default:
        cout << __LINE__ << ":未知错误" << endl;
        break;
    }
}

// 处理登录
static void handle_login_msg(int client_fd, cJSON *root, string &user)
{
    cJSON *username = cJSON_GetObjectItem(root, "username");
    cJSON *password = cJSON_GetObjectItem(root, "password_hash");

    if (!cJSON_IsString(username) || !cJSON_IsString(password))
    {
        cout << "数据校验错误:" << __LINE__ << endl;
        return;
    }

    string un = username->valuestring;
    string pwd = password->valuestring;

    switch (user_login_check(un, pwd))
    {
    case 0:
    {
        lock_guard<mutex> lock(g_id_mutex);
        int user_id = get_user_id(); // 获取分配的空闲ID

        User_Info user_info;
        user_info.user_name = un;
        user_info.user_id = user_id;
        user_info.client_fd = client_fd;
        user_info.user_room = -1; // 初始不在任何房间

        // 记录进在线用户表，并把当前连接的用户名写回给线程私有变量
        g_id_map[un] = user_info;
        user = un;

        // 生成 token 凭证
        long long current_time = time(NULL);
        char token_buf[256];
        snprintf(token_buf, sizeof(token_buf), "token-%d-%lld", user_id, current_time);
        reply_login(client_fd, "login_resp", 0, user_id, token_buf, 1800, "ok");
        break;
    }
    case 1:
        cout << "未找到用户名" << endl;
        break;
    case 2:
        cout << __LINE__ << ":未知错误:2" << endl;
        break;
    default:
        cout << __LINE__ << ":未知错误" << endl;
        break;
    }
}

// 处理心跳（保活）
static void handle_heartbeat_msg(int client_fd, cJSON *root)
{
    reply_msg(client_fd, "heartbeat_resp", 0);
}

// 处理创建房间
static void handle_create_room_msg(int client_fd, cJSON *root, string &user)
{
    cJSON *session_token = cJSON_GetObjectItem(root, "session_token");
    cJSON *room_name = cJSON_GetObjectItem(root, "room_name");

    if (!cJSON_IsString(session_token) || !cJSON_IsString(room_name))
    {
        cout << "数据校验错误:" << __LINE__ << endl;
        return;
    }

    string rn = room_name->valuestring;
    int room_id = 0;

    // 创建并初始化房间数据
    {
        lock_guard<mutex> lock(g_room_mutex);
        room_id = get_room_id();

        Room_Info room_info;
        room_info.room_id = room_id;
        room_info.room_name = rn;
        room_info.room_master = user;      // 创建者即为房主
        room_info.members.push_back(user); // 把自己加入房间成员表
        g_room_map[room_id] = room_info;
    }

    // 更新用户状态中的房间号
    {
        lock_guard<mutex> lock(g_id_mutex);
        g_id_map[user].user_room = room_id;
    }

    reply_create_room(client_fd, "create_room_resp", 0, room_id, rn.c_str());

    // 紧接着推送一份房间成员列表（因为刚建完房间，里面只有自己，直接塞进去）
    cJSON *resp_users = cJSON_CreateObject();
    cJSON_AddStringToObject(resp_users, "type", "room_users");
    cJSON_AddNumberToObject(resp_users, "room_id", room_id);

    cJSON *users_array = cJSON_CreateArray();
    cJSON *my_obj = cJSON_CreateObject();

    int user_id = 0;
    {
        lock_guard<mutex> lock(g_id_mutex);
        user_id = g_id_map[user].user_id; // 取真实ID，不能填0
    }
    cJSON_AddNumberToObject(my_obj, "user_id", user_id);
    cJSON_AddStringToObject(my_obj, "username", user.c_str());
    cJSON_AddItemToArray(users_array, my_obj);

    cJSON_AddItemToObject(resp_users, "users", users_array);

    send_json_response(client_fd, resp_users);
    cJSON_Delete(resp_users);
}

// 处理刷新房间
static void handle_list_rooms_msg(int client_fd, cJSON *root, const string &user)
{
    vector<string> list;
    {
        lock_guard<mutex> lk(g_room_mutex);
        for (auto &kv : g_room_map)
        {
            // 拼装成 "房间号:房间名" 格式
            list.push_back(to_string(kv.second.room_id) + ":" + kv.second.room_name);
        }
    }
    reply_room_list(client_fd, 0, list);
}

// 获取本端 IP
static string local_ip_of(int fd)
{
    sockaddr_in a{};
    socklen_t l = sizeof(a);
    if (getsockname(fd, (sockaddr *)&a, &l) < 0)
        return "127.0.0.1";
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return buf;
}

// 处理加入房间
static void handle_join_room_msg(int client_fd, cJSON *root, const string &user)
{
    cJSON *room_item = cJSON_GetObjectItem(root, "room_id");
    if (!cJSON_IsNumber(room_item))
    {
        reply_join_room(client_fd, 1, 0, 0, "");
        return;
    }
    int room_id = room_item->valueint;

    vector<string> members_snapshot;
    {
        lock_guard<mutex> lk(g_room_mutex);
        auto it = g_room_map.find(room_id);
        if (it == g_room_map.end())
        {
            reply_join_room(client_fd, 20, room_id, 0, ""); // 房间不存在
            return;
        }
        // 检查是否已经在这个房间里了
        for (auto &m : it->second.members)
        {
            if (m == user)
            {
                reply_join_room(client_fd, 22, room_id, 0, ""); // 已在该房间
                return;
            }
        }
        // 真正加入房间
        it->second.members.push_back(user);
        members_snapshot = it->second.members; // 拷贝一份最新的成员列表，锁外使用
    }

    // 更新用户所在房间
    {
        lock_guard<mutex> lk(g_id_mutex);
        g_id_map[user].user_room = room_id;
    }

    // 回执告诉客户端加入成功，并带上 UDP 服务器地址供发流使用
    reply_join_room(client_fd, 0, room_id, UDP_PORT, local_ip_of(client_fd).c_str());

    // 给新加入者推送当前房间里所有人的列表
    send_room_users_to(client_fd, room_id, members_snapshot);

    // 给原本就在房间里的人广播，有新成员进来了
    broadcast_user_joined(user, room_id, members_snapshot);
}

// 处理离开房间
static void handle_leave_room_msg(int client_fd, cJSON *root, const string &user)
{
    cJSON *room_item = cJSON_GetObjectItem(root, "room_id");
    if (!cJSON_IsNumber(room_item))
    {
        reply_leave_room(client_fd, 1, 0);
        return;
    }
    int room_id = room_item->valueint;

    bool is_master = false;
    vector<string> remaining_snapshot;

    // 阶段1：在房间表上操作（持 g_room_mutex）
    {
        lock_guard<mutex> lk(g_room_mutex);
        auto it = g_room_map.find(room_id);
        if (it == g_room_map.end())
        {
            reply_leave_room(client_fd, 20, room_id); // 房间不存在
            return;
        }
        auto &members = it->second.members;
        auto mit = find(members.begin(), members.end(), user);
        if (mit == members.end())
        {
            reply_leave_room(client_fd, 21, room_id); // 不在该房间中
            return;
        }

        is_master = (it->second.room_master == user);
        members.erase(mit);           // 从成员表移除
        remaining_snapshot = members; // 锁外使用的剩余名单

        if (is_master)
        {
            // 房主离开，直接销毁房间（后续成员被动踢出）
            g_room_map.erase(it);
        }
    }

    // 阶段2：更新相关用户的 user_room（持 g_id_mutex，与 g_room_mutex 不嵌套防止死锁）
    {
        lock_guard<mutex> lk(g_id_mutex);
        auto it = g_id_map.find(user);
        if (it != g_id_map.end())
            it->second.user_room = -1; // 自己的房间号置空

        if (is_master)
        {
            // 房间销毁 → 剩余成员被动离开，把他们的房间号也置空
            for (auto &m : remaining_snapshot)
            {
                auto mit = g_id_map.find(m);
                if (mit != g_id_map.end())
                    mit->second.user_room = -1;
            }
        }
    }

    // 阶段3：回执 + 广播（锁已释放，进行网络发送）
    reply_leave_room(client_fd, 0, room_id);

    if (!remaining_snapshot.empty())
        broadcast_user_left(user, room_id, remaining_snapshot);
}

// 处理 RTP 端口注册
static void handle_register_rtp_msg(int client_fd, cJSON *root, const string &user)
{
    cJSON *port_item = cJSON_GetObjectItem(root, "rtp_port");
    if (!cJSON_IsNumber(port_item))
    {
        reply_register_rtp(client_fd, 1, 0); // 参数缺失
        return;
    }

    int rtp_port = port_item->valueint;

    // 记下客户端的 IP：用 TCP 对端地址，保证中继时回传地址正确
    char ip[INET_ADDRSTRLEN] = {0};
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    if (getpeername(client_fd, (sockaddr *)&peer, &plen) == 0)
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

    {
        lock_guard<mutex> lk(g_id_mutex);
        auto it = g_id_map.find(user);
        if (it != g_id_map.end())
        {
            it->second.rtp_ip = ip;
            it->second.rtp_port = rtp_port;
        }
    }

    reply_register_rtp(client_fd, 0, UDP_PORT); // 告诉客户端 UDP 中继端口是 10001
}

// 处理聊天广播
static void handle_broadcast_msg(int client_fd, cJSON *root, const string &user)
{
    cJSON *room_item = cJSON_GetObjectItem(root, "room_id");
    cJSON *msg_item = cJSON_GetObjectItem(root, "message");

    if (!cJSON_IsNumber(room_item) || !cJSON_IsString(msg_item))
    {
        reply_broadcast(client_fd, 1, "error", "参数错误");
        return;
    }

    int room_id = room_item->valueint;
    string text = msg_item->valuestring;

    if (user.empty())
    {
        reply_broadcast(client_fd, 30, "error", "用户名为空");
        return;
    }

    // 取房间成员快照
    vector<string> members_snapshot;
    {
        lock_guard<mutex> lk(g_room_mutex);
        auto it = g_room_map.find(room_id);
        if (it == g_room_map.end())
        {
            reply_broadcast(client_fd, 20, "error", "会议室不存在");
            return;
        }
        members_snapshot = it->second.members;
    }

    // 验证自己是否在这个房间
    if (find(members_snapshot.begin(), members_snapshot.end(), user) == members_snapshot.end())
    {
        reply_broadcast(client_fd, 30, "error", "不在该会议室");
        return;
    }

    // 1. 给发送者回执成功
    reply_broadcast(client_fd, 0, "ok", "广播成功");

    // 2. 转发给房间里的其他人（from 换成用户名）
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "broadcast");
    cJSON_AddStringToObject(msg, "from", user.c_str());
    cJSON_AddNumberToObject(msg, "room_id", room_id);
    cJSON_AddStringToObject(msg, "message", text.c_str());

    for (auto &m : members_snapshot)
    {
        if (m == user) // 不给自己发
            continue;
        int fd = -1;
        {
            lock_guard<mutex> lk(g_id_mutex);
            auto it = g_id_map.find(m);
            if (it != g_id_map.end())
                fd = it->second.client_fd;
        }
        if (fd >= 0)
            send_json_response(fd, msg);
    }
    cJSON_Delete(msg);
}

// ==================== 总路由分发 ====================
void handle_client_json(int client_fd, cJSON *root, string &user)
{
    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (type_item != nullptr && cJSON_IsString(type_item))
    {
        string msg_type = type_item->valuestring;

        if (msg_type == "register")
            handle_register_msg(client_fd, root);
        else if (msg_type == "login")
            handle_login_msg(client_fd, root, user);
        else if (msg_type == "heartbeat")
            handle_heartbeat_msg(client_fd, root);
        else if (msg_type == "create_room")
            handle_create_room_msg(client_fd, root, user);
        else if (msg_type == "list_rooms")
            handle_list_rooms_msg(client_fd, root, user);
        else if (msg_type == "join_room")
            handle_join_room_msg(client_fd, root, user);
        else if (msg_type == "leave_room")
            handle_leave_room_msg(client_fd, root, user);
        else if (msg_type == "register_rtp")
            handle_register_rtp_msg(client_fd, root, user);
        else if (msg_type == "broadcast")
            handle_broadcast_msg(client_fd, root, user);
    }
}

// ==================== 供 UDP 中继使用的数据接口 ====================

// 供 UDP 中继查询：这个用户ID在哪个房间
int get_user_room_by_id(int user_id)
{
    lock_guard<mutex> lk(g_id_mutex);
    for (auto &kv : g_id_map)
    {
        if (kv.second.user_id == user_id)
            return kv.second.user_room;
    }
    return -1;
}

// 供 UDP 中继查询：获取某房间的所有成员信息快照
std::vector<User_Info> get_room_members_snapshot(int room_id)
{
    std::vector<User_Info> ret;
    lock_guard<mutex> lk(g_id_mutex);
    for (auto &kv : g_id_map)//遍历在线用户
    {
        if (kv.second.user_room == room_id)//用户是该房间的成员
            ret.push_back(kv.second);
    }
    return ret;
}

// 从文件加载已注册用户到内存
void user_db_load(const char *path)
{
    lock_guard<mutex> lk(g_user_mutex);
    g_users_map.clear();

    ifstream fin(path);
    if (!fin)
    {
        cout << "账号文件打开失败: " << path << endl;
        return;
    }

    string line;
    while (getline(fin, line))
    {
        if (line.empty())
            continue;
        auto pos = line.find(':');
        if (pos == string::npos)
            continue;
        g_users_map[line.substr(0, pos)] = line.substr(pos + 1);
    }
}

// 核心难点：这个 UDP 发送者属于哪个房间？
// 背景：客户端发 RTP 的 socket 和它注册接收的 socket 可能不是同一个，无法直接靠 IP:Port 匹配。
// 处理方式：先精确匹配注册的 IP:Port，匹配不上再按“同一 IP 上的房主”兜底。
int room_of_udp_sender(const sockaddr_in &sender, int &pub_uid)
{
    pub_uid = -1;

    // 1) 精确匹配 (客户端发送源端口和注册接收端口一致的情况)
    {
        lock_guard<mutex> lk(g_id_mutex);
        for (auto &kv : g_id_map)
        {
            if (kv.second.rtp_port <= 0 || kv.second.rtp_ip.empty())
                continue;
            if (kv.second.rtp_port == ntohs(sender.sin_port) &&
                inet_addr(kv.second.rtp_ip.c_str()) == sender.sin_addr.s_addr)
            {
                pub_uid = kv.second.user_id;
                return kv.second.user_room;
            }
        }
    }

    // 2) 兜底逻辑（针对同一 IP 上同时跑多个端，且端口不匹配的情况）
    struct Cand
    {
        string name;
        int uid;
        int room;
    };
    vector<Cand> cand;
    {
        lock_guard<mutex> lk(g_id_mutex);
        for (auto &kv : g_id_map)
        {
            if (kv.second.rtp_ip.empty() || kv.second.user_room == -1)
                continue;
            if (inet_addr(kv.second.rtp_ip.c_str()) != sender.sin_addr.s_addr)
                continue;
            Cand c;
            c.name = kv.first;
            c.uid = kv.second.user_id;
            c.room = kv.second.user_room;
            cand.push_back(c);
        }
    }

    // 匹配同 IP 下的房主。必须把 user_id 带出去，防止把学生当成了发送者导致跳过。
    lock_guard<mutex> lk(g_room_mutex);
    for (auto &c : cand)
    {
        auto it = g_room_map.find(c.room);
        if (it != g_room_map.end() && it->second.room_master == c.name)
        {
            pub_uid = c.uid;
            return c.room;
        }
    }

    pub_uid = -1;
    return -1;
}