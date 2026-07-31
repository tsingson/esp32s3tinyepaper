//
// Created by tsingson on 2026/7/31.
//

#ifndef ESP32_CHIP_INFO_DRONE_H
#define ESP32_CHIP_INFO_DRONE_H
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>

#include "ec801e.h"
#include "bitproto/drone_bp.h"
#include "bitproto/socket_utls.h"
#include "bitproto/protocol_utls.h"

#define GPSEC_ROUNDS 3
#define GPSEC_RETRIES 3
#define GPSEC_IO_TIMEOUT_MS 30000
#define GPSEC_RETRY_BACKOFF_MS 1000


void fill_drone(struct Drone* drone, int seq);
int run_roundtrip_session(const char* srv_ip, uint16_t srv_port);

#endif //ESP32_CHIP_INFO_DRONE_H
