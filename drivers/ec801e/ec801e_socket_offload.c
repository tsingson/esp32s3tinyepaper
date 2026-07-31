#include "ec801e_socket_offload.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/socket_offload.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ec801e.h"
LOG_MODULE_DECLARE(ec801e);

struct ec801e_socket_ctx {
	int fd;
	int protocol;
	bool allocated;
	bool connected;
	bool use_tls;
	struct net_sockaddr_in peer;
	k_timeout_t recv_timeout;
	k_timeout_t send_timeout;
	int tls_peer_verify;
	char tls_hostname[96];
};

struct ec801e_dns_result {
	struct zsock_addrinfo ai;
	struct sockaddr_in addr;
	char canonname[96];
};

static struct ec801e_socket_ctx g_ctx;
static struct ec801e_dns_result g_dns_result;
static const struct socket_op_vtable g_sock_vtable;
static const struct socket_dns_offload g_dns_ops;
static bool g_offload_registered;

#define EC801E_OFFLOAD_VLOG(...) \
	do { \
		if (IS_ENABLED(CONFIG_EC801E_VERBOSE_LOG)) { \
			LOG_DBG(__VA_ARGS__); \
		} \
	} while (0)

static int fail_with(int err)
{
	errno = err;
	return -1;
}

static bool ec801e_socket_is_supported(int family, int type, int protocol)
{
	if ((family != NET_AF_INET) || (type != NET_SOCK_STREAM)) {
		return false;
	}

	return (protocol == 0) || (protocol == NET_IPPROTO_TCP) ||
	       (protocol == NET_IPPROTO_TLS_1_0) || (protocol == NET_IPPROTO_TLS_1_1) ||
	       (protocol == NET_IPPROTO_TLS_1_2) || (protocol == NET_IPPROTO_TLS_1_3);
}

static int ec801e_socket_create(int family, int type, int protocol)
{
	EC801E_OFFLOAD_VLOG("EC801E offload socket_create family=%d type=%d proto=%d\n",
			   family, type, protocol);

	if (g_ctx.allocated) {
		errno = EMFILE;
		return -1;
	}

	int fd = zvfs_reserve_fd();
	if (fd < 0) {
		return -1;
	}

	g_ctx.fd = fd;
	g_ctx.protocol = (protocol == 0) ? NET_IPPROTO_TCP : protocol;
	g_ctx.allocated = true;
	g_ctx.connected = false;
	g_ctx.use_tls = (g_ctx.protocol >= NET_IPPROTO_TLS_1_0) &&
			(g_ctx.protocol <= NET_IPPROTO_TLS_1_3);
	g_ctx.recv_timeout = K_MSEC(3000);
	g_ctx.send_timeout = K_MSEC(3000);
	g_ctx.tls_peer_verify = ZSOCK_TLS_PEER_VERIFY_NONE;
	g_ctx.tls_hostname[0] = '\0';
	zvfs_finalize_typed_fd(fd, &g_ctx,
		(const struct fd_op_vtable *)&g_sock_vtable,
		ZVFS_MODE_IFSOCK);
	return fd;
}

static int ec801e_sock_connect(void *obj, const struct net_sockaddr *addr, net_socklen_t addrlen)
{
	struct ec801e_socket_ctx *ctx = obj;
	char ip[NET_IPV4_ADDR_LEN];
	int ret;

	EC801E_OFFLOAD_VLOG("EC801E offload connect addrlen=%u family=%d\n",
			   (unsigned int)addrlen, (addr != NULL) ? addr->sa_family : -1);

	if ((ctx == NULL) || !ctx->allocated || (addr == NULL) ||
	    (addrlen != sizeof(struct net_sockaddr_in)) ||
	    (addr->sa_family != NET_AF_INET)) {
		return fail_with(EINVAL);
	}

	if (ctx->connected) {
		/* Some upper socket paths may invoke connect more than once.
		 * Treat repeated connect on an already-connected single context as success.
		 */
		return 0;
	}

	ret = ec801e_socket_stack_init(NULL);
	if (ret < 0) {
		LOG_INF("EC801E offload stack init failed: %d\n", ret);
		return fail_with(-ret);
	}

	const struct net_sockaddr_in *peer = net_sin(addr);
	if (net_addr_ntop(NET_AF_INET, &peer->sin_addr, ip, sizeof(ip)) == NULL) {
		return fail_with(EINVAL);
	}

	if (ctx->use_tls) {
		const char *target = (ctx->tls_hostname[0] != '\0') ? ctx->tls_hostname : ip;
		ret = ec801e_socket_open_ssl(target, net_ntohs(peer->sin_port));
	} else {
		ret = ec801e_socket_open_tcp(ip, net_ntohs(peer->sin_port));
	}
	if (ret == -EISCONN) {
		ret = 0;
	}
	if (ret < 0) {
		LOG_INF("EC801E offload open failed: %d\n", ret);
		return fail_with(-ret);
	}

	ctx->peer = *peer;
	ctx->connected = true;
	return 0;
}

static ssize_t ec801e_sock_send(void *obj, const void *buf, size_t len, int flags,
	const struct net_sockaddr *dst, net_socklen_t dstlen)
{
	struct ec801e_socket_ctx *ctx = obj;
	int ret;

	if ((ctx == NULL) || !ctx->allocated || (buf == NULL) || (len == 0U)) {
		return fail_with(EINVAL);
	}
	if (!ctx->connected) {
		return fail_with(ENOTCONN);
	}
	if ((flags != 0) || (dst != NULL) || (dstlen != 0U)) {
		return fail_with(EOPNOTSUPP);
	}

	ret = ec801e_socket_send((const uint8_t *)buf, len);
	if (ret < 0) {
		return fail_with(-ret);
	}

	return ret;
}

static ssize_t ec801e_sock_recv(void *obj, void *buf, size_t len, int flags,
	struct net_sockaddr *src, net_socklen_t *srclen)
{
	struct ec801e_socket_ctx *ctx = obj;
	size_t out_len = 0U;
	int timeout_ms;
	int ret;

	if ((ctx == NULL) || !ctx->allocated || (buf == NULL) || (len == 0U)) {
		return fail_with(EINVAL);
	}
	if (!ctx->connected) {
		return fail_with(ENOTCONN);
	}
	if (flags != 0) {
		return fail_with(EOPNOTSUPP);
	}

	timeout_ms = (int)k_ticks_to_ms_floor32(ctx->recv_timeout.ticks);
	if (timeout_ms <= 0) {
		timeout_ms = 3000;
	}

	ret = ec801e_socket_recv((uint8_t *)buf, len, &out_len, timeout_ms);
	if (ret < 0) {
		if (ret == -EAGAIN) {
			return fail_with(EAGAIN);
		}
		return fail_with(-ret);
	}

	if ((src != NULL) && (srclen != NULL)) {
		net_socklen_t copy_len = MIN(*srclen, sizeof(ctx->peer));
		memcpy(src, &ctx->peer, copy_len);
		*srclen = sizeof(ctx->peer);
	}

	return (ssize_t)out_len;
}

static ssize_t ec801e_sock_read(void *obj, void *buf, size_t len)
{
	return ec801e_sock_recv(obj, buf, len, 0, NULL, NULL);
}

static ssize_t ec801e_sock_write(void *obj, const void *buf, size_t len)
{
	return ec801e_sock_send(obj, buf, len, 0, NULL, 0);
}

static int ec801e_sock_close(void *obj, int fd)
{
	struct ec801e_socket_ctx *ctx = obj;
	int ret = 0;

	if ((ctx == NULL) || !ctx->allocated) {
		errno = EBADF;
		return -1;
	}

	if (ctx->fd != fd) {
		EC801E_OFFLOAD_VLOG("EC801E offload close fd remap old=%d new=%d\n", ctx->fd, fd);
	}

	if (ctx->connected) {
		ret = ec801e_socket_close();
	}

	ctx->fd = -1;
	ctx->protocol = NET_IPPROTO_TCP;
	ctx->allocated = false;
	ctx->connected = false;
	ctx->use_tls = false;
	ctx->recv_timeout = K_MSEC(3000);
	ctx->send_timeout = K_MSEC(3000);
	ctx->tls_peer_verify = ZSOCK_TLS_PEER_VERIFY_NONE;
	ctx->tls_hostname[0] = '\0';
	memset(&ctx->peer, 0, sizeof(ctx->peer));

	if (ret < 0) {
		return fail_with(-ret);
	}
	return 0;
}

static int ec801e_sock_ioctl(void *obj, unsigned int req, va_list args)
{
	ARG_UNUSED(obj);
	ARG_UNUSED(args);

	if (req == ZFD_IOCTL_SET_LOCK) {
		return 0;
	}

	errno = EOPNOTSUPP;
	return -1;
}

static int ec801e_sock_setsockopt(void *obj, int level, int option,
	const void *optval, net_socklen_t optlen)
{
	struct ec801e_socket_ctx *ctx = obj;

	if ((ctx == NULL) || !ctx->allocated) {
		return fail_with(EBADF);
	}
	if (optval == NULL) {
		return fail_with(EINVAL);
	}

	if ((level == ZSOCK_SOL_SOCKET) &&
	    ((option == ZSOCK_SO_RCVTIMEO) || (option == ZSOCK_SO_SNDTIMEO))) {
		if (optlen != sizeof(struct zsock_timeval)) {
			return fail_with(EINVAL);
		}

		const struct zsock_timeval *tv = optval;
		if ((tv->tv_sec < 0) || (tv->tv_usec < 0) || (tv->tv_usec >= 1000000)) {
			return fail_with(EINVAL);
		}

		int64_t us = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
		k_timeout_t timeout = (us == 0) ? K_FOREVER : K_USEC(us);
		if (option == ZSOCK_SO_RCVTIMEO) {
			ctx->recv_timeout = timeout;
		} else {
			ctx->send_timeout = timeout;
		}
		return 0;
	}

	if ((level == ZSOCK_SOL_TLS) && (option == ZSOCK_TLS_HOSTNAME)) {
		size_t n = MIN((size_t)optlen, sizeof(ctx->tls_hostname) - 1U);
		memcpy(ctx->tls_hostname, optval, n);
		ctx->tls_hostname[n] = '\0';
		return 0;
	}

	if ((level == ZSOCK_SOL_TLS) && (option == ZSOCK_TLS_PEER_VERIFY)) {
		if (optlen != sizeof(int)) {
			return fail_with(EINVAL);
		}
		ctx->tls_peer_verify = *(const int *)optval;
		return 0;
	}

	return fail_with(EOPNOTSUPP);
}

static int ec801e_sock_getsockopt(void *obj, int level, int option,
	void *optval, net_socklen_t *optlen)
{
	int value;
	struct ec801e_socket_ctx *ctx = obj;

	if ((ctx == NULL) || !ctx->allocated || (optval == NULL) || (optlen == NULL) ||
	    (*optlen < sizeof(int))) {
		return fail_with(EINVAL);
	}
	if (level != ZSOCK_SOL_SOCKET) {
		return fail_with(EOPNOTSUPP);
	}

	switch (option) {
	case ZSOCK_SO_TYPE:
		value = NET_SOCK_STREAM;
		break;
	case ZSOCK_SO_PROTOCOL:
		value = ctx->protocol;
		break;
	case ZSOCK_SO_DOMAIN:
		value = NET_AF_INET;
		break;
	default:
		return fail_with(EOPNOTSUPP);
	}

	memcpy(optval, &value, sizeof(value));
	*optlen = sizeof(value);
	return 0;
}

static int ec801e_sock_getpeername(void *obj, struct net_sockaddr *addr,
	net_socklen_t *addrlen)
{
	struct ec801e_socket_ctx *ctx = obj;

	if ((ctx == NULL) || !ctx->allocated || (addr == NULL) || (addrlen == NULL)) {
		return fail_with(EINVAL);
	}
	if (!ctx->connected) {
		return fail_with(ENOTCONN);
	}

	net_socklen_t copy_len = MIN(*addrlen, sizeof(ctx->peer));
	memcpy(addr, &ctx->peer, copy_len);
	*addrlen = sizeof(ctx->peer);
	return 0;
}

static uint16_t ec801e_dns_parse_service_port(const char *service)
{
	char *end = NULL;
	long port;

	if ((service == NULL) || (service[0] == '\0')) {
		return 0U;
	}

	port = strtol(service, &end, 10);
	if ((end == NULL) || (*end != '\0') || (port < 0) || (port > 65535)) {
		return UINT16_MAX;
	}

	return (uint16_t)port;
}

static int ec801e_offload_getaddrinfo(const char *node, const char *service,
	const struct zsock_addrinfo *hints, struct zsock_addrinfo **res)
{
	char ip[NET_IPV4_ADDR_LEN] = {0};
	uint16_t port;
	int ret;

	if ((node == NULL) || (res == NULL)) {
		return DNS_EAI_FAIL;
	}

	if ((hints != NULL) && (hints->ai_family != AF_UNSPEC) && (hints->ai_family != AF_INET)) {
		return DNS_EAI_FAMILY;
	}
	if ((hints != NULL) && (hints->ai_socktype != 0) && (hints->ai_socktype != SOCK_STREAM)) {
		return DNS_EAI_SOCKTYPE;
	}

	port = ec801e_dns_parse_service_port(service);
	if (port == UINT16_MAX) {
		return DNS_EAI_SERVICE;
	}

	ret = ec801e_socket_stack_init(NULL);
	if (ret < 0) {
		return DNS_EAI_FAIL;
	}

	ret = ec801e_resolve_ipv4(node, ip, sizeof(ip));
	if (ret < 0) {
		return DNS_EAI_NONAME;
	}

	memset(&g_dns_result, 0, sizeof(g_dns_result));
	g_dns_result.ai.ai_family = AF_INET;
	g_dns_result.ai.ai_socktype = SOCK_STREAM;
	g_dns_result.ai.ai_protocol = NET_IPPROTO_TCP;
	g_dns_result.ai.ai_addrlen = sizeof(g_dns_result.addr);
	g_dns_result.ai.ai_addr = (struct sockaddr *)&g_dns_result.addr;
	g_dns_result.ai.ai_canonname = g_dns_result.canonname;
	(void)snprintk(g_dns_result.canonname, sizeof(g_dns_result.canonname), "%s", node);

	g_dns_result.addr.sin_family = AF_INET;
	g_dns_result.addr.sin_port = htons(port);
	if (net_addr_pton(AF_INET, ip, &g_dns_result.addr.sin_addr) != 0) {
		return DNS_EAI_NONAME;
	}

	*res = &g_dns_result.ai;
	return 0;
}

static void ec801e_offload_freeaddrinfo(struct zsock_addrinfo *res)
{
	ARG_UNUSED(res);
}

static const struct socket_dns_offload g_dns_ops = {
	.getaddrinfo = ec801e_offload_getaddrinfo,
	.freeaddrinfo = ec801e_offload_freeaddrinfo,
};

static const struct socket_op_vtable g_sock_vtable = {
	.fd_vtable = {
		.read = ec801e_sock_read,
		.write = ec801e_sock_write,
		.close2 = ec801e_sock_close,
		.ioctl = ec801e_sock_ioctl,
	},
	.connect = ec801e_sock_connect,
	.sendto = ec801e_sock_send,
	.recvfrom = ec801e_sock_recv,
	.getsockopt = ec801e_sock_getsockopt,
	.setsockopt = ec801e_sock_setsockopt,
	.getpeername = ec801e_sock_getpeername,
};

NET_SOCKET_OFFLOAD_REGISTER(ec801e, CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY,
	NET_AF_INET, ec801e_socket_is_supported, ec801e_socket_create);

int ec801e_socket_offload_init(void)
{
	if (g_offload_registered) {
		return 0;
	}

	memset(&g_ctx, 0, sizeof(g_ctx));
	g_ctx.fd = -1;
	socket_offload_dns_register(&g_dns_ops);
	g_offload_registered = true;
	return 0;
}

static int ec801e_socket_offload_sys_init(void)
{
	return ec801e_socket_offload_init();
}

SYS_INIT(ec801e_socket_offload_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
