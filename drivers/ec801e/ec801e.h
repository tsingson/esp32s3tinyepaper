#ifndef EC801E_DRIVER_H
#define EC801E_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EC801E_IMEI_MAX_LEN 24
#define EC801E_IP_MAX_LEN 40

struct ec801e_module_info {
	char imei[EC801E_IMEI_MAX_LEN];
	char ip[EC801E_IP_MAX_LEN];
	int cereg_stat;
	bool ip_valid;
};

struct ec801e_signal_info {
	int rssi_raw;
	int ber;
	int rssi_dbm;
};

int ec801e_boot(void);
int ec801e_uart_rw_selftest(void);
int ec801e_prepare_network(const char *apn);
int ec801e_fetch_cloudflare_trace(void);

int ec801e_socket_stack_init(const char *apn);
int ec801e_resolve_ipv4(const char *host, char *ip, size_t ip_len);
int ec801e_socket_open_tcp(const char *remote_ip, uint16_t remote_port);
int ec801e_socket_open_ssl(const char *remote_host, uint16_t remote_port);
int ec801e_socket_send(const uint8_t *data, size_t len);
int ec801e_socket_recv(uint8_t *buf, size_t buf_len, size_t *out_len, int timeout_ms);
int ec801e_socket_close(void);

int ec801e_get_module_info(struct ec801e_module_info *info);
int ec801e_get_signal_info(struct ec801e_signal_info *info);
int ec801e_get_en_pin_state(int *level);

#endif
