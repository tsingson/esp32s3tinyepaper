#include "serv_addr.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(serv_addr, LOG_LEVEL_INF);

#define SERV_ADDR_MAX_LEN 128
#define ACTIVE_MAGIC 0x53415256u
#define TCP_PROBE_TIMEOUT_MS 3000

#define ACTIVE_PARTITION activeaddr_partition
#define ACTIVE_PARTITION_ID PARTITION_ID(ACTIVE_PARTITION)

struct active_addr_record {
	uint32_t magic;
	uint32_t crc;
	char addr[SERV_ADDR_MAX_LEN];
};

static char g_active_addr[SERV_ADDR_MAX_LEN];
static bool g_active_valid;

static void trim_line_end(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) {
		s[--n] = '\0';
	}
}

static bool parse_host_port(const char *input, char *host, size_t host_len, uint16_t *port)
{
	const char *colon;
	char port_str[8];
	long p;
	size_t host_part_len;
	size_t port_part_len;

	if (input == NULL || host == NULL || port == NULL || host_len < 2U) {
		return false;
	}

	colon = strrchr(input, ':');
	if (colon == NULL || colon == input || colon[1] == '\0') {
		return false;
	}

	host_part_len = (size_t)(colon - input);
	if (host_part_len >= host_len) {
		return false;
	}

	port_part_len = strlen(colon + 1);
	if (port_part_len == 0U || port_part_len >= sizeof(port_str)) {
		return false;
	}

	memcpy(host, input, host_part_len);
	host[host_part_len] = '\0';

	memcpy(port_str, colon + 1, port_part_len + 1);
	p = strtol(port_str, NULL, 10);
	if (p <= 0 || p > 65535) {
		return false;
	}

	*port = (uint16_t)p;
	return true;
}

static uint32_t calc_crc(const struct active_addr_record *rec)
{
	uint32_t h = 2166136261u;
	size_t i;

	for (i = 0; i < sizeof(rec->addr); ++i) {
		h ^= (uint8_t)rec->addr[i];
		h *= 16777619u;
	}

	return h;
}

static int save_active_addr_to_flash(const char *addr)
{
	struct active_addr_record rec;
	const struct flash_area *fa;
	int ret;

	memset(&rec, 0, sizeof(rec));
	rec.magic = ACTIVE_MAGIC;
	strncpy(rec.addr, addr, sizeof(rec.addr) - 1);
	rec.addr[sizeof(rec.addr) - 1] = '\0';
	rec.crc = calc_crc(&rec);

	ret = flash_area_open(ACTIVE_PARTITION_ID, &fa);
	if (ret < 0) {
		return ret;
	}

	ret = flash_area_erase(fa, 0, fa->fa_size);
	if (ret == 0) {
		ret = flash_area_write(fa, 0, &rec, sizeof(rec));
	}

	flash_area_close(fa);
	return ret;
}

static int load_active_addr_from_flash(char *out, size_t out_len)
{
	struct active_addr_record rec;
	const struct flash_area *fa;
	int ret;

	if (out_len == 0U) {
		return -EINVAL;
	}

	ret = flash_area_open(ACTIVE_PARTITION_ID, &fa);
	if (ret < 0) {
		return ret;
	}

	ret = flash_area_read(fa, 0, &rec, sizeof(rec));
	flash_area_close(fa);
	if (ret < 0) {
		return ret;
	}

	if (rec.magic != ACTIVE_MAGIC) {
		return -ENOENT;
	}

	if (rec.crc != calc_crc(&rec)) {
		return -EINVAL;
	}

	rec.addr[sizeof(rec.addr) - 1] = '\0';
	if (rec.addr[0] == '\0') {
		return -ENOENT;
	}

	strncpy(out, rec.addr, out_len - 1);
	out[out_len - 1] = '\0';
	return 0;
}

static bool tcp_reachable(const char *addr)
{
	char host[80];
	uint16_t port;
	char port_str[8];
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
	};
	struct zsock_addrinfo *res = NULL;
	struct zsock_timeval tv = {
		.tv_sec = TCP_PROBE_TIMEOUT_MS / 1000,
		.tv_usec = (TCP_PROBE_TIMEOUT_MS % 1000) * 1000,
	};
	int fd = -1;
	int ret;
	bool ok = false;

	if (!parse_host_port(addr, host, sizeof(host), &port)) {
		LOG_WRN("地址格式无效: %s", addr);
		return false;
	}

	snprintk(port_str, sizeof(port_str), "%u", (unsigned int)port);
	ret = zsock_getaddrinfo(host, port_str, &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_WRN("DNS 解析失败: %s:%s (%d)", host, port_str, ret);
		return false;
	}

	fd = zsock_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		LOG_WRN("socket 创建失败: %d", errno);
		zsock_freeaddrinfo(res);
		return false;
	}

	(void)zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	(void)zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	ret = zsock_connect(fd, res->ai_addr, res->ai_addrlen);
	if (ret == 0) {
		ok = true;
	}

	zsock_close(fd);
	zsock_freeaddrinfo(res);
	return ok;
}

int serv_addr_init(const char *initial_addr)
{
	char init_copy[SERV_ADDR_MAX_LEN];
	int ret;

	if (initial_addr == NULL || initial_addr[0] == '\0') {
		return -EINVAL;
	}

	strncpy(init_copy, initial_addr, sizeof(init_copy) - 1);
	init_copy[sizeof(init_copy) - 1] = '\0';
	trim_line_end(init_copy);

	ret = load_active_addr_from_flash(g_active_addr, sizeof(g_active_addr));
	if (ret == 0) {
		g_active_valid = true;
		LOG_INF("激活地址已加载: %s", g_active_addr);
		return 0;
	}

	strncpy(g_active_addr, init_copy, sizeof(g_active_addr) - 1);
	g_active_addr[sizeof(g_active_addr) - 1] = '\0';

	ret = save_active_addr_to_flash(g_active_addr);
	if (ret < 0) {
		LOG_ERR("首次保存激活地址失败 (%d)", ret);
		return ret;
	}

	g_active_valid = true;
	LOG_INF("首次写入激活地址: %s", g_active_addr);
	return 0;
}

int serv_addr_try_promote(const char *candidate_addr)
{
	char candidate[SERV_ADDR_MAX_LEN];
	int ret;

	if (!g_active_valid || candidate_addr == NULL) {
		return -EINVAL;
	}

	strncpy(candidate, candidate_addr, sizeof(candidate) - 1);
	candidate[sizeof(candidate) - 1] = '\0';
	trim_line_end(candidate);
	if (candidate[0] == '\0') {
		return -EINVAL;
	}

	if (strcmp(candidate, g_active_addr) == 0) {
		return 0;
	}

	if (!tcp_reachable(candidate)) {
		LOG_WRN("候选地址不可达，保留激活地址: %s", g_active_addr);
		return -EHOSTUNREACH;
	}

	ret = save_active_addr_to_flash(candidate);
	if (ret < 0) {
		LOG_ERR("更新激活地址失败 (%d)", ret);
		return ret;
	}

	strncpy(g_active_addr, candidate, sizeof(g_active_addr) - 1);
	g_active_addr[sizeof(g_active_addr) - 1] = '\0';
	LOG_INF("激活地址已更新: %s", g_active_addr);
	return 0;
}

const char *get_serv_addr(void)
{
	if (!g_active_valid) {
		return "";
	}

	return g_active_addr;
}
