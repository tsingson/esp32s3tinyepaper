//
// Created by tsingson on 2026/7/30.
//

#include "protocol_utls.h"


int write_frame(int sock, const uint8_t* payload, size_t payload_len)
{
    struct FrameHeader hdr = {0};
    uint8_t hdr_buf[BYTES_LENGTH_FRAMEHEADER] = {0};

    if (payload_len > UINT16_MAX)
    {
        return -EINVAL;
    }

    hdr.magic = FRAME_MAGIC;
    hdr.payload_type = PAYLOAD_TYPE_DRONE;
    hdr.payload_length = (uint16_t)payload_len;
    if (EncodeFrameHeader(&hdr, (unsigned char*)hdr_buf) != BYTES_LENGTH_FRAMEHEADER)
    {
        return -EBADMSG;
    }

    if (send_all(sock, hdr_buf, sizeof(hdr_buf)) < 0)
    {
        return -EIO;
    }

    return send_all(sock, payload, payload_len);
}

int read_frame(int sock, uint8_t* payload, size_t payload_capacity, size_t* out_len)
{
    uint8_t hdr_buf[BYTES_LENGTH_FRAMEHEADER] = {0};
    struct FrameHeader hdr = {0};

    if (recv_all(sock, hdr_buf, sizeof(hdr_buf)) < 0)
    {
        return -EIO;
    }

    if (DecodeFrameHeader(&hdr, (unsigned char*)hdr_buf) != BYTES_LENGTH_FRAMEHEADER)
    {
        return -EBADMSG;
    }
    if (hdr.magic != FRAME_MAGIC || hdr.payload_type != PAYLOAD_TYPE_DRONE)
    {
        return -EBADMSG;
    }
    if (hdr.payload_length > payload_capacity)
    {
        return -EMSGSIZE;
    }

    if (recv_all(sock, payload, hdr.payload_length) < 0)
    {
        return -EIO;
    }

    *out_len = hdr.payload_length;
    return 0;
}

