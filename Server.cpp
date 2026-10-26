#include <sys/socket.h>
#include <iostream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <pthread.h>
#include <signal.h>
#include "cJSON.h"
#include "analysis.h"
#include "U_Server.h"

// using namespace std;

#define SERVER_PORT 8888
#define RECV_LEN 1024
#define SEND_LEN 1024

class TCP_Server
{
public:
    TCP_Server()
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            perror("创建套接字失败");
        }

        // 开启端口复用
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(SERVER_PORT);
        addr.sin_addr.s_addr = inet_addr("0.0.0.0");

        int ret = bind(fd, (const sockaddr *)&addr, sizeof(addr));
        if (ret < 0)
        {
            perror("绑定失败");
        }

        ret = listen(fd, 2);
        if (ret < 0)
        {
            perror("监听失败");
        }

        cout << "服务器启动成功" << endl;
    }

    // 服务器连接客户端线程，循环
    void server_accept()
    {
        while (1)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(fd, (sockaddr *)&client_addr, &client_len);
            if (client_fd < 0)
            {
                perror("连接服务器失败");
                continue;
            }

            pthread_t tid;

            // 为客户端的fd申请堆空间
            int *p_fd = new int(client_fd);
            pthread_create(&tid, NULL, client_pthread, p_fd);
        }
    }

private:
    int fd;
    struct sockaddr_in addr;

    // 客户端线程，循环
    static void *client_pthread(void *arg)
    {
        // 把 new 出来的内存 delete 掉
        int client_fd = *(int *)arg;
        delete (int *)arg;

        char buf[RECV_LEN] = {0}; // 接收缓冲区
        string current_username;  // 记录当前线程连接的用户名（当前为空）

        // 设置read超时属性
        struct timeval tv;
        tv.tv_sec = 15; // 15秒超时
        tv.tv_usec = 0; // 微秒
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (1)
        {
            memset(buf, 0, sizeof(buf)); // 缓冲区置零

            int ret = read(client_fd, buf, RECV_LEN - 1); // 读取数据
            if (ret > 0)
            {
                buf[ret] = '\0';
#if 1
                cJSON *root = cJSON_Parse(buf);
                cJSON *type_item = cJSON_GetObjectItem(root, "type");
                if (strcmp(type_item->valuestring, "heartbeat") != 0)
                {

                    cout << COLOR_RED << __LINE__ << "收到:" << buf << COLOR_RESET << endl;
                }
                cJSON_Delete(root);
#endif
            }
            else if (ret == 0)
            {
                cout << "客户端断开连接" << endl;
                close(client_fd);
                break;
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    cout << "read超时" << endl;
                }
                else
                {
                    cout << "read出错，断开连接" << endl;
                }
                close(client_fd);
                break;
            }

            char *newline = strchr(buf, '\n'); // 将换行符换为\0
            if (newline != nullptr)
            {
                *newline = '\0';
            }

            // 解析 Json
            cJSON *root = cJSON_Parse(buf);
            if (root == nullptr)
            {
                cout << "cjson_parse: error" << endl;
                continue;
            }
            handle_client_json(client_fd, root, current_username); // 处理解析出来的键值对
            cJSON_Delete(root);                                    // 解析完释放内存
        }

        close(client_fd); // 最后关闭 socket
        return NULL;
    }
};

int main()
{
    signal(SIGPIPE, SIG_IGN);
    user_db_load();

    // 启动UDP中继线程
    pthread_t udp_tid;
    pthread_create(&udp_tid, nullptr, udp_relay_thread, nullptr);
    pthread_detach(udp_tid);

    TCP_Server server;
    server.server_accept();
}