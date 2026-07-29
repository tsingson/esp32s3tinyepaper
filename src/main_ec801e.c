#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>

#include "ec801e.h"
#include "bitproto/drone_bp.h"
#include "bitproto/frameheader_bp.h"

#define GPSEC_SERVER_IP "142.54.180.58"
#define GPSEC_SERVER_PORT 8061
#define FRAME_MAGIC 0xB1
#define GPSEC_ROUNDS 3
#define GPSEC_RETRIES 3
#define GPSEC_IO_TIMEOUT_MS 30000
#define GPSEC_RETRY_BACKOFF_MS 1000

static void fill_drone(struct Drone *drone, int seq)
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

static int send_all(int sock, const uint8_t *buf, size_t len)
{
	int64_t deadline = k_uptime_get() + GPSEC_IO_TIMEOUT_MS;

	while (len > 0U) {
		ssize_t n = zsock_send(sock, buf, len, 0);
		if (n < 0) {
			if (errno == EAGAIN && k_uptime_get() < deadline) {
				k_sleep(K_MSEC(100));
				continue;
			}
			return -EIO;
		}
		if (n == 0) {
			return -EIO;
		}
		buf += n;
		len -= (size_t)n;
	}

	return 0;
}

static int recv_all(int sock, uint8_t *buf, size_t len)
{
	int64_t deadline = k_uptime_get() + GPSEC_IO_TIMEOUT_MS;

	while (len > 0U) {
		ssize_t n = zsock_recv(sock, buf, len, 0);
		if (n < 0) {
			if (errno == EAGAIN && k_uptime_get() < deadline) {
				k_sleep(K_MSEC(100));
				continue;
			}
			return -EIO;
		}
		if (n == 0) {
			return -EIO;
		}
		buf += n;
		len -= (size_t)n;
	}

	return 0;
}

static int connect_gpsec_socket(void)
{
	int sock;
	int ret;
	struct sockaddr_in peer = {0};
	struct zsock_timeval timeout = {
		.tv_sec = GPSEC_IO_TIMEOUT_MS / 1000,
		.tv_usec = 0,
	};

	peer.sin_family = NET_AF_INET;
	peer.sin_port = htons(GPSEC_SERVER_PORT);
	if (net_addr_pton(NET_AF_INET, GPSEC_SERVER_IP, &peer.sin_addr) != 0) {
		printk("Invalid server ip %s\n", GPSEC_SERVER_IP);
		return -EINVAL;
	}

	sock = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
	if (sock < 0) {
		printk("socket() failed errno=%d af=%d type=%d proto=%d\n", errno,
		       NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
		return -errno;
	}

	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	ret = zsock_connect(sock, (const struct sockaddr *)&peer, sizeof(peer));
	if (ret < 0) {
		printk("connect() failed errno=%d peer_af=%d\n", errno, peer.sin_family);
		if (errno == EISCONN) {
			return sock;
		}
		ret = -errno;
		(void)zsock_close(sock);
		return ret;
	}

	return sock;
}

static int write_frame(int sock, const uint8_t *payload, size_t payload_len)
{
	struct FrameHeader hdr = {0};
	uint8_t hdr_buf[BYTES_LENGTH_FRAMEHEADER] = {0};

	if (payload_len > UINT16_MAX) {
		return -EINVAL;
	}

	hdr.magic = FRAME_MAGIC;
	hdr.payload_type = PAYLOAD_TYPE_DRONE;
	hdr.payload_length = (uint16_t)payload_len;
	if (EncodeFrameHeader(&hdr, (unsigned char *)hdr_buf) != BYTES_LENGTH_FRAMEHEADER) {
		return -EBADMSG;
	}

	if (send_all(sock, hdr_buf, sizeof(hdr_buf)) < 0) {
		return -EIO;
	}

	return send_all(sock, payload, payload_len);
}

static int read_frame(int sock, uint8_t *payload, size_t payload_capacity, size_t *out_len)
{
	uint8_t hdr_buf[BYTES_LENGTH_FRAMEHEADER] = {0};
	struct FrameHeader hdr = {0};

	if (recv_all(sock, hdr_buf, sizeof(hdr_buf)) < 0) {
		return -EIO;
	}

	if (DecodeFrameHeader(&hdr, (unsigned char *)hdr_buf) != BYTES_LENGTH_FRAMEHEADER) {
		return -EBADMSG;
	}
	if (hdr.magic != FRAME_MAGIC || hdr.payload_type != PAYLOAD_TYPE_DRONE) {
		return -EBADMSG;
	}
	if (hdr.payload_length > payload_capacity) {
		return -EMSGSIZE;
	}

	if (recv_all(sock, payload, hdr.payload_length) < 0) {
		return -EIO;
	}

	*out_len = hdr.payload_length;
	return 0;
}

static int run_roundtrip_session(void)
{
	int ret = 0;

	for (int seq = 0; seq < GPSEC_ROUNDS; seq++) {
		int sock;
		struct Drone drone = {0};
		struct Drone reply_drone = {0};
		uint8_t payload[BYTES_LENGTH_DRONE] = {0};
		uint8_t reply[BYTES_LENGTH_DRONE] = {0};
		size_t payload_len;
		size_t reply_len = 0;

		sock = connect_gpsec_socket();
		if (sock < 0) {
			printk("round %d connect failed: %d\n", seq, sock);
			return sock;
		}

		fill_drone(&drone, seq);
		payload_len = EncodeDrone(&drone, (unsigned char *)payload);
		if (payload_len != BYTES_LENGTH_DRONE) {
			ret = -EINVAL;
			(void)zsock_close(sock);
			break;
		}

		ret = write_frame(sock, payload, payload_len);
		if (ret < 0) {
			printk("round %d write_frame failed: %d\n", seq, ret);
			(void)zsock_close(sock);
			break;
		}

		ret = read_frame(sock, reply, sizeof(reply), &reply_len);
		if (ret < 0) {
			printk("round %d read_frame failed: %d\n", seq, ret);
			(void)zsock_close(sock);
			break;
		}
		if (reply_len != payload_len || memcmp(reply, payload, payload_len) != 0) {
			printk("round %d payload mismatch tx=%u rx=%u\n", seq, (unsigned int)payload_len,
			       (unsigned int)reply_len);
			ret = -EIO;
			(void)zsock_close(sock);
			break;
		}

		if (DecodeDrone(&reply_drone, (unsigned char *)reply) != BYTES_LENGTH_DRONE) {
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
	}
	return ret;
}

int main(void)
{
	int ret;
	struct ec801e_module_info mod = {0};
	struct ec801e_signal_info sig = {0};
	int en_level = -1;
	int qret;

	k_sleep(K_MSEC(800));
	printk("\n=== EC801E-CN Bitproto TCP Demo ===\n");

	qret = ec801e_get_module_info(&mod);
	if (qret == 0) {
		printk("EC801E info: imei=%s cereg=%d ip=%s\n", mod.imei, mod.cereg_stat,
		       mod.ip_valid ? mod.ip : "N/A");
	} else {
		printk("EC801E info query failed: %d\n", qret);
	}

	qret = ec801e_get_signal_info(&sig);
	if (qret == 0) {
		printk("EC801E signal: csq=%d ber=%d rssi_dbm=%d\n", sig.rssi_raw, sig.ber,
		       sig.rssi_dbm);
	} else {
		printk("EC801E signal query failed: %d\n", qret);
	}

	qret = ec801e_get_en_pin_state(&en_level);
	if (qret == 0) {
		printk("EC801E EN pin level: %d\n", en_level);
	} else {
		printk("EC801E EN pin query unavailable: %d\n", qret);
	}

	for (int attempt = 1; attempt <= GPSEC_RETRIES; attempt++) {
		ret = run_roundtrip_session();
		if (ret == 0) {
			printk("GPSEC tcpip example done rounds=%d attempt=%d\n", GPSEC_ROUNDS, attempt);
			break;
		}

		printk("GPSEC attempt %d/%d failed: %d\n", attempt, GPSEC_RETRIES, ret);
		if (attempt < GPSEC_RETRIES) {
			k_sleep(K_MSEC(GPSEC_RETRY_BACKOFF_MS));
		}
	}

	if (ret != 0) {
		printk("GPSEC tcpip example failed after %d attempts: %d\n", GPSEC_RETRIES, ret);
	}

	while (1) {
		k_sleep(K_SECONDS(10));
	}

	return 0;
}
