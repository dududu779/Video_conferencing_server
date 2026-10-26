#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <iostream>
#include "analysis.h"

#define UDP_BUF_SIZE 2048

// UDP 中继线程：收到谁发来的媒体，就原样转发给同房间的其他人
void *udp_relay_thread(void *arg)
{
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0)
    {
        perror("udp socket");
        return nullptr;
    }

    int opt = 1;
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udp_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("udp bind");
        close(udp_fd);
        return nullptr;
    }

    std::cout << "UDP中继启动，端口 " << UDP_PORT << std::endl;

    char buf[UDP_BUF_SIZE];

    while (true)
    {
        sockaddr_in sender{};
        socklen_t sender_len = sizeof(sender);

        ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0, (sockaddr *)&sender, &sender_len);
        if (n <= 0)
            continue;

        // 这个包属于哪个房间、谁发的
        int pub_uid = -1;
        int room_id = room_of_udp_sender(sender, pub_uid);
        if (room_id == -1)
        {
            static int warn = 0;
            if (warn++ < 5)
                std::cout << "未识别的RTP来源: " << inet_ntoa(sender.sin_addr)
                          << ":" << ntohs(sender.sin_port) << std::endl;
            continue;
        }

        // 转发给同房间的其他人
        std::vector<User_Info> members = get_room_members_snapshot(room_id);

        // 诊断日志：前 3 个包 + 之后每 100 个包打一次
        static unsigned long long pkt = 0;
        ++pkt;
        if (pkt <= 3 || pkt % 100 == 0)
        {
            std::cout << "[中继] RTP包#" << pkt << " 来自 "
                      << inet_ntoa(sender.sin_addr) << ":" << ntohs(sender.sin_port)
                      << " → 房间 " << room_id << "，成员 " << members.size() << " 人"
                      << std::endl;
        }
        for (auto &m : members)
        {
            if (m.user_id == pub_uid)
                continue; // 不发给自己
            if (m.rtp_port <= 0 || m.rtp_ip.empty())
                continue;

            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(m.rtp_port);
            dst.sin_addr.s_addr = inet_addr(m.rtp_ip.c_str());
            sendto(udp_fd, buf, n, 0, (sockaddr *)&dst, sizeof(dst));
        }
    }

    close(udp_fd);
    return nullptr;
}