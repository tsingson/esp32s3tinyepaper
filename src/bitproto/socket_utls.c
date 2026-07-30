//
// Created by tsingson on 2026/7/30.
//

#include "socket_utls.h"

int send_all(int sock, const uint8_t* buf, size_t len)
{
    int64_t deadline = k_uptime_get() + GPSEC_IO_TIMEOUT_MS;

    while (len > 0U)
    {
        ssize_t n = zsock_send(sock, buf, len, 0);
        if (n < 0)
        {
            if (errno == EAGAIN && k_uptime_get() < deadline)
            {
                k_sleep(K_MSEC(100));
                continue;
            }
            return -EIO;
        }
        if (n == 0)
        {
            return -EIO;
        }
        buf += n;
        len -= (size_t)n;
    }

    return 0;
}

int recv_all(int sock, uint8_t* buf, size_t len)
{
    int64_t deadline = k_uptime_get() + GPSEC_IO_TIMEOUT_MS;

    while (len > 0U)
    {
        ssize_t n = zsock_recv(sock, buf, len, 0);
        if (n < 0)
        {
            if (errno == EAGAIN && k_uptime_get() < deadline)
            {
                k_sleep(K_MSEC(100));
                continue;
            }
            return -EIO;
        }
        if (n == 0)
        {
            return -EIO;
        }
        buf += n;
        len -= (size_t)n;
    }

    return 0;
}


int connect_gpsec_socket(const char* srv_ip, uint16_t srv_port)
{
    int sock;
    int ret;
    struct sockaddr_in peer = {0};
    struct zsock_timeval timeout = {
        .tv_sec = GPSEC_IO_TIMEOUT_MS / 1000,
        .tv_usec = 0,
    };

    peer.sin_family = NET_AF_INET;
    peer.sin_port = htons(srv_port);
    if (net_addr_pton(NET_AF_INET, srv_ip, &peer.sin_addr) != 0)
    {
        printk("Invalid server ip %s\n", srv_ip);
        return -EINVAL;
    }

    sock = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
    if (sock < 0)
    {
        printk("socket() failed errno=%d af=%d type=%d proto=%d\n", errno,
               NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
        return -errno;
    }

    (void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    ret = zsock_connect(sock, (const struct sockaddr*)&peer, sizeof(peer));
    if (ret < 0)
    {
        printk("connect() failed errno=%d peer_af=%d\n", errno, peer.sin_family);
        if (errno == EISCONN)
        {
            return sock;
        }
        ret = -errno;
        (void)zsock_close(sock);
        return ret;
    }

    return sock;
}
