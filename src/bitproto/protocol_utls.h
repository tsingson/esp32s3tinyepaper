//
// Created by tsingson on 2026/7/30.
//

#ifndef ESP32_CHIP_INFO_FRAME_UTLS_H
#define ESP32_CHIP_INFO_FRAME_UTLS_H

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>
#include "frameheader_bp.h"
#include "socket_utls.h"


int read_frame(int sock, uint8_t *payload, size_t payload_capacity, size_t *out_len);
int write_frame(int sock, const uint8_t *payload, size_t payload_len);


#endif //ESP32_CHIP_INFO_FRAME_UTLS_H
