//
// Created by tsingson on 2026/7/31.
//

#include "drone.h"


void fill_drone(struct Drone* drone, int seq)
{
    memset(drone, 0, sizeof(*drone));

    drone->status = DRONE_STATUS_RISING;
    drone->position.longitude = 2000U + (uint32_t)seq;
    drone->position.latitude = 3000U + (uint32_t)seq;
    drone->position.altitude = 1080U + (uint32_t)seq;
    drone->flight.pose.yaw = 4321 + seq;
    drone->flight.pose.pitch = 1234 + seq;
    drone->flight.pose.roll = 5678 + seq;
    drone->flight.acceleration[0] = -1001 - seq;
    drone->flight.acceleration[1] = 1002 + seq;
    drone->flight.acceleration[2] = 1003 + seq;
    drone->power.is_charging = ((seq % 2) == 0);
    drone->power.battery = 98;
    drone->power.status = POWER_STATUS_ON;
    drone->propellers[0].id = 1;
    drone->propellers[0].direction = ROTATING_DIRECTION_CLOCK_WISE;
    drone->propellers[0].status = PROPELLER_STATUS_ROTATING;
    drone->network.signal = 15;
    drone->network.heartbeat_at = 1611280511628LL + seq;
    drone->landing_gear.status = LANDING_GEAR_STATUS_FOLDED;
}

int run_roundtrip_session(  char* srv_ip, uint16_t srv_port)
{
    int ret = 0;

    for (int seq = 0; seq < GPSEC_ROUNDS; seq++)
    {
        int sock;
        struct Drone drone = {0};
        struct Drone reply_drone = {0};
        uint8_t payload[BYTES_LENGTH_DRONE] = {0};
        uint8_t reply[BYTES_LENGTH_DRONE] = {0};
        size_t payload_len;
        size_t reply_len = 0;

        sock = connect_gpsec_socket(srv_ip, srv_port);
        if (sock < 0)
        {
            printk("round %d connect failed: %d\n", seq, sock);
            return sock;
        }

        fill_drone(&drone, seq);
        payload_len = EncodeDrone(&drone, (unsigned char*)payload);
        if (payload_len != BYTES_LENGTH_DRONE)
        {
            ret = -EINVAL;
            (void)zsock_close(sock);
            break;
        }

        ret = write_frame(sock, payload, payload_len);
        if (ret < 0)
        {
            printk("round %d write_frame failed: %d\n", seq, ret);
            (void)zsock_close(sock);
            break;
        }

        ret = read_frame(sock, reply, sizeof(reply), &reply_len);
        if (ret < 0)
        {
            printk("round %d read_frame failed: %d\n", seq, ret);
            (void)zsock_close(sock);
            break;
        }
        if (reply_len != payload_len || memcmp(reply, payload, payload_len) != 0)
        {
            printk("round %d payload mismatch tx=%u rx=%u\n", seq, (unsigned int)payload_len,
                   (unsigned int)reply_len);
            ret = -EIO;
            (void)zsock_close(sock);
            break;
        }

        if (DecodeDrone(&reply_drone, (unsigned char*)reply) != BYTES_LENGTH_DRONE)
        {
            ret = -EBADMSG;
            (void)zsock_close(sock);
            break;
        }

        printk("round %d ok: status=%u lat=%u lon=%u alt=%u\n", seq,
               (unsigned int)reply_drone.status,
               (unsigned int)reply_drone.position.latitude,
               (unsigned int)reply_drone.position.longitude,
               (unsigned int)reply_drone.position.altitude);
        (void)zsock_close(sock);
        break;
    }
    return ret;
}