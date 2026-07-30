//
// Created by tsingson on 2026/7/30.
//

#ifndef ESP32_CHIP_INFO_SOCKET_UTLS_H
#define ESP32_CHIP_INFO_SOCKET_UTLS_H
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>

#define GPSEC_ROUNDS 3
#define GPSEC_RETRIES 3
#define GPSEC_IO_TIMEOUT_MS 30000
#define GPSEC_RETRY_BACKOFF_MS 1000

int send_all(int sock, const uint8_t *buf, size_t len);
int recv_all(int sock, uint8_t *buf, size_t len);

int connect_gpsec_socket( const char* srv_ip, uint16_t srv_port);

#endif //ESP32_CHIP_INFO_SOCKET_UTLS_H
