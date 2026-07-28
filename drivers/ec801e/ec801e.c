#include "ec801e.h"

#define DT_DRV_COMPAT quectel_ec801e

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define EC801E_NODE DT_INST(0, quectel_ec801e)
#define EC801E_UART_NODE DT_PHANDLE(EC801E_NODE, mdm_uart)

#define EC801E_RX_BUF_LEN 3072
#define EC801E_CMD_BUF_LEN 256
#define EC801E_SHORT_TIMEOUT_MS 1500
#define EC801E_MEDIUM_TIMEOUT_MS 10000
#define EC801E_LONG_TIMEOUT_MS 60000
#define EC801E_BOOT_WAIT_MS 8000
#define EC801E_QHTTP_CONNECT_TIMEOUT_MS 125000
#define EC801E_REBOOT_HIT_THRESHOLD 3
#define EC801E_PDP_ACT_TIMEOUT_MS 120000

static const struct device *const ec801e_uart = DEVICE_DT_GET(EC801E_UART_NODE);
static const struct gpio_dt_spec ec801e_power_gpio = GPIO_DT_SPEC_INST_GET_OR(0, mdm_power_gpios,
							       {0});
static bool ec801e_uart_verified;
static bool ec801e_stack_ready;
static bool ec801e_socket_opened;
static char ec801e_rsp_buf[EC801E_RX_BUF_LEN];

#define EC801E_VLOG(...) \
	do { \
		if (IS_ENABLED(CONFIG_EC801E_VERBOSE_LOG)) { \
			printk(__VA_ARGS__); \
		} \
	} while (0)

static void ec801e_drain_rx(void);
static int ec801e_handshake_at(void);
static int ec801e_post_handshake_init(void);
static int ec801e_try_hard_recovery(void);

static int ec801e_uart_write(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(ec801e_uart, data[i]);
	}

	return 0;
}

static int ec801e_send_cmd(const char *cmd)
{
	char line[EC801E_CMD_BUF_LEN];
	/* Many cellular modules are stricter with CR-only AT termination. */
	int n = snprintk(line, sizeof(line), "%s\r", cmd);

	if (n <= 0 || n >= (int)sizeof(line)) {
		return -EINVAL;
	}

	EC801E_VLOG("-> %s\n", cmd);
	return ec801e_uart_write((const uint8_t *)line, (size_t)n);
}

static int ec801e_cmd_format(char *buf, size_t buf_len, const char *fmt, ...)
{
	va_list ap;
	int n;

	if ((buf == NULL) || (buf_len == 0U) || (fmt == NULL)) {
		return -EINVAL;
	}

	va_start(ap, fmt);
	n = vsnprintk(buf, buf_len, fmt, ap);
	va_end(ap);

	if ((n < 0) || (n >= (int)buf_len)) {
		return -EINVAL;
	}

	return 0;
}

static bool ec801e_power_gpio_available(void)
{
	return (ec801e_power_gpio.port != NULL) && device_is_ready(ec801e_power_gpio.port);
}

static bool ec801e_contains_any(const char *buf, const char *a, const char *b, const char *c)
{
	if (a != NULL && strstr(buf, a) != NULL) {
		return true;
	}
	if (b != NULL && strstr(buf, b) != NULL) {
		return true;
	}
	if (c != NULL && strstr(buf, c) != NULL) {
		return true;
	}

	return false;
}

static bool ec801e_contains_reboot_marker(const char *buf)
{
	if (buf == NULL) {
		return false;
	}

	return (strstr(buf, "RDY") != NULL) || (strstr(buf, "^boot.rom") != NULL) ||
	       (strstr(buf, "PB DONE") != NULL);
}

static int ec801e_wait_response(const char *ok1, const char *ok2, const char *ok3,
					const char *fail1, const char *fail2,
					int timeout_ms, char *out, size_t out_len)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	size_t pos = 0;
	size_t rx_count = 0;

	if (out != NULL && out_len > 0) {
		out[0] = '\0';
	}

	while (k_uptime_get() < deadline) {
		unsigned char ch;
		int ret = uart_poll_in(ec801e_uart, &ch);

		if (ret == 0) {
			rx_count++;
			if (out != NULL && out_len > 1) {
				if (pos < out_len - 1) {
					out[pos++] = (char)ch;
					out[pos] = '\0';
				} else {
					memmove(out, out + out_len / 2, out_len - out_len / 2 - 1);
					pos = out_len - out_len / 2 - 1;
					out[pos++] = (char)ch;
					out[pos] = '\0';
				}
			}

			if (out != NULL && ec801e_contains_any(out, ok1, ok2, ok3)) {
				return 0;
			}

			if (out != NULL && ec801e_contains_reboot_marker(out)) {
				/* Modem restarted in the middle of an exchange; recover at upper layer. */
				return -EHOSTDOWN;
			}

			if (out != NULL && ec801e_contains_any(out, fail1, fail2, "CME ERROR")) {
				return -EIO;
			}
		} else {
			k_sleep(K_MSEC(10));
		}
	}

	if (out != NULL) {
		printk("EC801E: wait timeout, rx_count=%u, buf='%s'\n", (unsigned int)rx_count, out);
	} else {
		printk("EC801E: wait timeout, rx_count=%u\n", (unsigned int)rx_count);
	}

	return -ETIMEDOUT;
}

static int ec801e_cmd_expect(const char *cmd, const char *ok1, const char *ok2, int timeout_ms)
{
	char *rsp = ec801e_rsp_buf;
	
	ec801e_drain_rx();
	int ret = ec801e_send_cmd(cmd);

	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response(ok1, ok2, NULL, "ERROR", "+QIOPEN: 0,", timeout_ms, rsp,
				  EC801E_RX_BUF_LEN);
	EC801E_VLOG("<- %s\n", rsp);
	return ret;
}

static int ec801e_cmd_expect_retry(const char *cmd, const char *ok1, const char *ok2,
					   int timeout_ms, int retries)
{
	int ret = -ETIMEDOUT;

	for (int i = 0; i < retries; i++) {
		ret = ec801e_cmd_expect(cmd, ok1, ok2, timeout_ms);
		if (ret == 0) {
			return 0;
		}

		if (ret == -EHOSTDOWN) {
			printk("EC801E: reboot marker while waiting '%s', cool down then retry\n", cmd);
			k_sleep(K_MSEC(EC801E_BOOT_WAIT_MS));
			continue;
		}

		if (ret != -ETIMEDOUT) {
			return ret;
		}

		k_sleep(K_MSEC(300));
	}

	return ret;
}

static int ec801e_wait_qiopen_result(int timeout_ms, char *out, size_t out_len)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	size_t pos = 0;

	if (out != NULL && out_len > 0U) {
		out[0] = '\0';
	}

	while (k_uptime_get() < deadline) {
		unsigned char ch;
		int ret = uart_poll_in(ec801e_uart, &ch);

		if (ret == 0) {
			if (out != NULL && out_len > 1U) {
				if (pos < out_len - 1U) {
					out[pos++] = (char)ch;
					out[pos] = '\0';
				}
			}

			if (out != NULL && (strstr(out, "ERROR") != NULL || strstr(out, "CME ERROR") != NULL)) {
				return -EIO;
			}

			if (out != NULL) {
				int result_code;
				char *p = strstr(out, "+QIOPEN: 0,");
				if (p != NULL && sscanf(p, "+QIOPEN: 0,%d", &result_code) == 1) {
					return (result_code == 0) ? 0 : -EIO;
				}
			}
		} else {
			k_sleep(K_MSEC(10));
		}
	}

	return -ETIMEDOUT;
}

static int ec801e_wait_cereg_registered(int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	char *rsp = ec801e_rsp_buf;
	int reboot_hits = 0;

	while (k_uptime_get() < deadline) {
		int ret = ec801e_send_cmd("AT+CEREG?");
		if (ret < 0) {
			return ret;
		}

		ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
					   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
		printk("<- %s\n", rsp);
		if (ret < 0) {
			if (ret == -EHOSTDOWN || ec801e_contains_reboot_marker(rsp)) {
				reboot_hits++;
				printk("EC801E: reboot marker during CEREG wait (%d), re-sync AT\n",
				       reboot_hits);
				/* Modem restarted mid-flow: wait stable, then redo full lightweight init. */
				k_sleep(K_MSEC(EC801E_BOOT_WAIT_MS));
				(void)ec801e_post_handshake_init();
				/* Re-apply network-side setup after reboot to avoid half-initialized state. */
				(void)ec801e_cmd_expect_retry("AT+CFUN=1", "OK", NULL,
						      EC801E_MEDIUM_TIMEOUT_MS, 2);
				(void)ec801e_cmd_expect_retry("AT+CEREG=1", "OK", NULL,
						      EC801E_SHORT_TIMEOUT_MS, 2);
				/* Give the modem extra time when reboot happened in the middle of attach. */
				deadline += 12000;

				if (reboot_hits >= EC801E_REBOOT_HIT_THRESHOLD) {
					int hret = ec801e_try_hard_recovery();
					if (hret == 0) {
						printk("EC801E: hard recovery done, restart CEREG wait\n");
						reboot_hits = 0;
						deadline = k_uptime_get() + timeout_ms;
						(void)ec801e_cmd_expect_retry("AT+CFUN=1", "OK", NULL,
								      EC801E_MEDIUM_TIMEOUT_MS, 2);
						(void)ec801e_cmd_expect_retry("AT+CEREG=1", "OK", NULL,
								      EC801E_SHORT_TIMEOUT_MS, 2);
					}
				}

				k_sleep(K_SECONDS(1));
				continue;
			}

			k_sleep(K_SECONDS(1));
			continue;
		}

		char *p = strstr(rsp, "+CEREG:");
		if (p != NULL) {
			char *comma = strchr(p, ',');
			if (comma != NULL) {
				int stat = atoi(comma + 1);
				printk("EC801E: CEREG stat=%d\n", stat);
				if (stat == 1 || stat == 5) {
					return 0;
				}
			}
		}

		k_sleep(K_SECONDS(2));
	}

	if (reboot_hits > 0) {
		printk("EC801E: CEREG timed out with %d modem reboot markers, check module power rail\n",
		       reboot_hits);
	}

	return -ETIMEDOUT;
}

static bool ec801e_qiact_rsp_has_ctx1_active(const char *rsp)
{
	const char *p;

	if (rsp == NULL) {
		return false;
	}

	p = strstr(rsp, "+QIACT:");
	while (p != NULL) {
		int cid = 0;
		if (sscanf(p, "+QIACT: %d", &cid) == 1 && cid == 1) {
			return true;
		}
		p = strstr(p + 7, "+QIACT:");
	}

	return false;
}

static int ec801e_ensure_pdp_activated(void)
{
	char *rsp = ec801e_rsp_buf;
	int ret;

	/* Query first to avoid re-activating an already-active context. */
	ret = ec801e_send_cmd("AT+QIACT?");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
				   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	printk("<- %s\n", rsp);
	if (ret == 0 && ec801e_qiact_rsp_has_ctx1_active(rsp)) {
		printk("EC801E: PDP ctx1 already active, skip QIACT\n");
		return 0;
	}

	printk("EC801E: activate PDP ctx1 (may take up to %d ms)\n", EC801E_PDP_ACT_TIMEOUT_MS);
	ret = ec801e_cmd_expect_retry("AT+QIACT=1", "OK", NULL, EC801E_PDP_ACT_TIMEOUT_MS, 3);
	if (ret == 0) {
		return 0;
	}

	printk("EC801E: QIACT returned %d, check current PDP state\n", ret);
	ret = ec801e_send_cmd("AT+QIACT?");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR", EC801E_MEDIUM_TIMEOUT_MS,
				   rsp, EC801E_RX_BUF_LEN);
	printk("<- %s\n", rsp);
	if (ret == 0 && ec801e_qiact_rsp_has_ctx1_active(rsp)) {
		printk("EC801E: PDP ctx1 already active, continue\n");
		return 0;
	}

	/* Clear stale PDP state then activate once more. */
	(void)ec801e_cmd_expect_retry("AT+QIDEACT=1", "OK", "ERROR", EC801E_PDP_ACT_TIMEOUT_MS, 1);
	k_sleep(K_SECONDS(1));

	ret = ec801e_cmd_expect_retry("AT+QIACT=1", "OK", NULL, EC801E_PDP_ACT_TIMEOUT_MS, 3);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int ec801e_send_raw(const char *payload)
{
	return ec801e_uart_write((const uint8_t *)payload, strlen(payload));
}

static void ec801e_drain_rx(void)
{
	unsigned char ch;

	while (uart_poll_in(ec801e_uart, &ch) == 0) {
		/* Drop stale bytes/noise before next AT exchange. */
	}
}

static int ec801e_handshake_at(void)
{
	int ret = -ETIMEDOUT;

	for (int i = 0; i < 12; i++) {
		ec801e_drain_rx();
		(void)ec801e_send_raw("\r\n");
		k_sleep(K_MSEC(80));

		ret = ec801e_cmd_expect("AT", "OK", NULL, EC801E_SHORT_TIMEOUT_MS);
		if (ret == 0) {
			return 0;
		}

		k_sleep(K_MSEC(300));
	}

	return ret;
}

static int ec801e_set_baud(uint32_t baudrate)
{
	struct uart_config cfg = {
		.baudrate = baudrate,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};

	return uart_configure(ec801e_uart, &cfg);
}

static int ec801e_probe_uart_only(void)
{
	static const uint32_t probe_bauds[] = {115200, 9600, 57600, 38400, 19200, 230400, 460800, 921600};
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(probe_bauds); i++) {
		ret = ec801e_set_baud(probe_bauds[i]);
		if (ret < 0) {
			printk("EC801E: uart set baud %u failed: %d\n", (unsigned int)probe_bauds[i], ret);
			continue;
		}

		printk("EC801E: UART probe AT at %u bps\n", (unsigned int)probe_bauds[i]);
		ret = ec801e_handshake_at();
		if (ret == 0) {
			printk("EC801E: UART AT probe success at %u bps\n", (unsigned int)probe_bauds[i]);
			return 0;
		}
	}

	return -ETIMEDOUT;
}

static int ec801e_post_handshake_init(void)
{
	int ret = -ETIMEDOUT;

	for (int attempt = 0; attempt < 5; attempt++) {
		/* Re-sync command channel first, because URCs/noise may desync line parsing. */
		ret = ec801e_handshake_at();
		if (ret < 0) {
			printk("EC801E: post-handshake AT sync failed (try %d): %d\n", attempt + 1, ret);
			k_sleep(K_MSEC(300));
			continue;
		}

		ret = ec801e_cmd_expect_retry("ATE0", "OK", NULL, EC801E_SHORT_TIMEOUT_MS, 2);
		if (ret < 0) {
			if (ret == -EHOSTDOWN) {
				k_sleep(K_MSEC(EC801E_BOOT_WAIT_MS));
				continue;
			}
			printk("EC801E: ATE0 failed (try %d): %d\n", attempt + 1, ret);
			k_sleep(K_MSEC(300));
			continue;
		}

		ret = ec801e_cmd_expect_retry("AT+CPIN?", "+CPIN: READY", "OK",
					      EC801E_MEDIUM_TIMEOUT_MS, 4);
		if (ret == 0) {
			return 0;
		}
		if (ret == -EHOSTDOWN) {
			k_sleep(K_MSEC(EC801E_BOOT_WAIT_MS));
			continue;
		}

		printk("EC801E: CPIN check failed (try %d): %d\n", attempt + 1, ret);
		if (ret == -EIO) {
			/* +CME ERROR here is often transient before SIM service is fully ready. */
			k_sleep(K_MSEC(1000));
			continue;
		}

		if (ret == -ETIMEDOUT) {
			/* If modem just rebooted or baud drifted, recover by probing AT again. */
			(void)ec801e_probe_uart_only();
		}

		k_sleep(K_MSEC(600));
	}

	return ret;
}

int ec801e_uart_rw_selftest(void)
{
	static const uint32_t probe_bauds[] = {115200, 9600, 57600, 38400, 19200, 230400, 460800, 921600};
	char rx[128];
	int ret;
	bool any_rx = false;

	if (!device_is_ready(ec801e_uart)) {
		printk("EC801E UART selftest: uart1 not ready\n");
		return -ENODEV;
	}

	for (size_t i = 0; i < ARRAY_SIZE(probe_bauds); i++) {
		ret = ec801e_set_baud(probe_bauds[i]);
		if (ret < 0) {
			printk("EC801E UART selftest: set baud %u failed: %d\n",
			       (unsigned int)probe_bauds[i], ret);
			continue;
		}

		ec801e_drain_rx();
		rx[0] = '\0';

		printk("EC801E UART selftest: TX 'AT' at %u bps\n", (unsigned int)probe_bauds[i]);
		(void)ec801e_send_raw("AT\r");

		ret = ec801e_wait_response("OK", "+CME ERROR", "ERROR", NULL, NULL,
					   1200, rx, sizeof(rx));

		if (rx[0] != '\0') {
			any_rx = true;
			printk("EC801E UART selftest: RX preview at %u bps: '%s'\n",
			       (unsigned int)probe_bauds[i], rx);
		} else {
			printk("EC801E UART selftest: no RX at %u bps\n", (unsigned int)probe_bauds[i]);
		}

		if (ret == 0 && strstr(rx, "OK") != NULL) {
			ec801e_uart_verified = true;
			printk("EC801E UART selftest: PASS at %u bps (AT OK)\n",
			       (unsigned int)probe_bauds[i]);
			return 0;
		}
	}

	ec801e_uart_verified = false;

	if (any_rx) {
		printk("EC801E UART selftest: partial PASS (RX exists, but no AT OK)\n");
		return -EAGAIN;
	}

	printk("EC801E UART selftest: FAIL (no RX on all bauds)\n");
	return -ETIMEDOUT;
}

static void ec801e_power_cycle(bool active_low)
{
	const struct device *gpio_dev = ec801e_power_gpio.port;
	gpio_pin_t gpio_pin = ec801e_power_gpio.pin;
	int inactive = active_low ? 1 : 0;
	int active = active_low ? 0 : 1;

	gpio_pin_set(gpio_dev, gpio_pin, inactive);
	k_sleep(K_MSEC(150));
	gpio_pin_set(gpio_dev, gpio_pin, active);
	k_sleep(K_MSEC(EC801E_BOOT_WAIT_MS));
}

static int ec801e_try_hard_recovery(void)
{
	int ret;

	if (!ec801e_power_gpio_available()) {
		return -ENOTSUP;
	}

	ret = gpio_pin_configure_dt(&ec801e_power_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	printk("EC801E: hard recovery via EN power-cycle\n");
	ec801e_power_cycle((ec801e_power_gpio.dt_flags & GPIO_ACTIVE_LOW) != 0U);

	ret = ec801e_probe_uart_only();
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_post_handshake_init();
	if (ret < 0) {
		return ret;
	}

	return 0;
}

int ec801e_boot(void)
{
	int ret;

	if (!device_is_ready(ec801e_uart)) {
		printk("EC801E device not ready\n");
		return -ENODEV;
	}

	if (ec801e_uart_verified) {
		printk("EC801E: UART already verified, skip EN/probe stage\n");
		goto post_handshake;
	}

	if (!ec801e_power_gpio_available()) {
		printk("EC801E: EN GPIO unavailable, continue with UART-only probe\n");
		goto probe_only;
	}

	ret = gpio_pin_configure_dt(&ec801e_power_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	/* First try pure UART AT probing without touching EN. This helps validate wiring quickly. */

probe_only:
	ret = ec801e_probe_uart_only();
	if (ret == 0) {
		goto post_handshake;
	}

	if (!ec801e_power_gpio_available()) {
		printk("EC801E: UART-only probe failed and EN GPIO unavailable\n");
		return ret;
	}

	printk("EC801E: UART-only probe failed, continue with EN power-cycle recovery\n");

	/* Try low-active EN first (common on modem power key rails), then fallback. */
	ec801e_power_cycle((ec801e_power_gpio.dt_flags & GPIO_ACTIVE_LOW) != 0U);
	ret = ec801e_probe_uart_only();
	if (ret < 0) {
		printk("EC801E: low-active EN handshake failed, retry high-active EN\n");
		ec801e_power_cycle((ec801e_power_gpio.dt_flags & GPIO_ACTIVE_LOW) == 0U);
		ret = ec801e_probe_uart_only();
	}

	if (ret < 0) {
		printk("EC801E: AT handshake timeout after power-on\n");
		return ret;
	}

post_handshake:

	ret = ec801e_post_handshake_init();
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_cmd_expect("AT+QICLOSE=0", "OK", "ERROR", EC801E_SHORT_TIMEOUT_MS);
	if (ret < 0) {
		printk("EC801E: ignore socket close result %d\n", ret);
	}

	return 0;
}

int ec801e_prepare_network(const char *apn)
{
	int ret;
	char cmd[EC801E_CMD_BUF_LEN];

	ret = ec801e_cmd_expect_retry("AT+CFUN=1", "OK", NULL, EC801E_MEDIUM_TIMEOUT_MS, 2);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_cmd_expect_retry("AT+CEREG=1", "OK", NULL, EC801E_SHORT_TIMEOUT_MS, 2);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_cereg_registered(90000);
	if (ret < 0) {
		printk("EC801E: network not registered yet\n");
		return ret;
	}

	ret = ec801e_cmd_format(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
	if (ret < 0) {
		return ret;
	}
	ret = ec801e_cmd_expect_retry(cmd, "OK", NULL, EC801E_SHORT_TIMEOUT_MS, 2);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_cmd_format(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\",1", apn);
	if (ret < 0) {
		return ret;
	}
	ret = ec801e_cmd_expect_retry(cmd, "OK", NULL, EC801E_SHORT_TIMEOUT_MS, 2);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_ensure_pdp_activated();
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_cmd_expect_retry("AT+CGPADDR=1", "OK", NULL, EC801E_MEDIUM_TIMEOUT_MS, 2);
	if (ret < 0) {
		printk("EC801E: warn - CGPADDR query failed, continue to socket stage\n");
	}

	return 0;
}

int ec801e_socket_open_ssl(const char *remote_host, uint16_t remote_port)
{
	int ret;
	char *rsp = ec801e_rsp_buf;
	char cmd[128];
	int ctx_candidates[] = {1, 0};
	int sslver_candidates[] = {4, 3, 1};
	int access_modes[] = {1, 0};
	int selected_ctx = -1;
	const char *host = remote_host;

	if ((host == NULL) || (host[0] == '\0') || (remote_port == 0U)) {
		return -EINVAL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(ctx_candidates); i++) {
		int ctx = ctx_candidates[i];
		bool sslver_ok = false;

		for (size_t v = 0; v < ARRAY_SIZE(sslver_candidates); v++) {
			snprintk(cmd, sizeof(cmd), "AT+QSSLCFG=\"sslversion\",%d,%d", ctx,
				 sslver_candidates[v]);
			ret = ec801e_cmd_expect_retry(cmd, "OK", NULL, EC801E_MEDIUM_TIMEOUT_MS, 2);
			if (ret < 0) {
				/* Some EC801E firmware variants use 2-argument QSSLCFG syntax. */
				snprintk(cmd, sizeof(cmd), "AT+QSSLCFG=\"sslversion\",%d",
					 sslver_candidates[v]);
				ret = ec801e_cmd_expect_retry(cmd, "OK", NULL, EC801E_MEDIUM_TIMEOUT_MS,
							  1);
			}

			if (ret == 0) {
				sslver_ok = true;
				selected_ctx = ctx;
				break;
			}
		}

		if (!sslver_ok) {
			printk("EC801E: sslversion unsupported on ctx=%d\n", ctx);
			continue;
		}

		snprintk(cmd, sizeof(cmd), "AT+QSSLCFG=\"seclevel\",%d,0", ctx);
		ret = ec801e_cmd_expect_retry(cmd, "OK", NULL, EC801E_MEDIUM_TIMEOUT_MS, 1);
		if (ret < 0) {
			snprintk(cmd, sizeof(cmd), "AT+QSSLCFG=\"seclevel\",0");
			(void)ec801e_cmd_expect_retry(cmd, "OK", "ERROR", EC801E_MEDIUM_TIMEOUT_MS, 1);
		}

		snprintk(cmd, sizeof(cmd), "AT+QSSLCFG=\"ignorelocaltime\",%d,1", ctx);
		ret = ec801e_cmd_expect_retry(cmd, "OK", NULL, EC801E_MEDIUM_TIMEOUT_MS, 1);
		if (ret < 0) {
			snprintk(cmd, sizeof(cmd), "AT+QSSLCFG=\"ignorelocaltime\",1");
			(void)ec801e_cmd_expect_retry(cmd, "OK", "ERROR", EC801E_MEDIUM_TIMEOUT_MS, 1);
		}

		/* Some firmware requires explicit SNI for Cloudflare endpoints. */
		snprintk(cmd, sizeof(cmd), "AT+QSSLCFG=\"sni\",%d,1", ctx);
		ret = ec801e_cmd_expect_retry(cmd, "OK", NULL, EC801E_MEDIUM_TIMEOUT_MS, 1);
		if (ret < 0) {
			snprintk(cmd, sizeof(cmd), "AT+QSSLCFG=\"sni\",1");
			(void)ec801e_cmd_expect_retry(cmd, "OK", "ERROR", EC801E_MEDIUM_TIMEOUT_MS, 1);
		}
	}

	if (selected_ctx < 0) {
		printk("EC801E: no usable QSSLCFG profile found, try direct SSL open\n");
	}

	for (size_t i = 0; i < ARRAY_SIZE(access_modes); i++) {
		snprintk(cmd, sizeof(cmd), "AT+QIOPEN=1,0,\"SSL\",\"%s\",%u,0,%d", host,
			 (unsigned int)remote_port, access_modes[i]);
		ret = ec801e_send_cmd(cmd);
		if (ret < 0) {
			continue;
		}

		ret = ec801e_wait_qiopen_result(EC801E_LONG_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
		printk("<- %s\n", rsp);
		if (ret == 0) {
			return 0;
		}
	}

	return -EIO;
}

static int ec801e_fetch_cloudflare_trace_qhttp_url(const char *url)
{
	int ret;
	char *rsp = ec801e_rsp_buf;
	char cmd[80];

	(void)ec801e_cmd_expect_retry("AT+QHTTPCFG=\"contextid\",1", "OK", "ERROR",
					EC801E_MEDIUM_TIMEOUT_MS, 1);
	(void)ec801e_cmd_expect_retry("AT+QHTTPCFG=\"requestheader\",0", "OK", "ERROR",
					EC801E_MEDIUM_TIMEOUT_MS, 1);

	snprintk(cmd, sizeof(cmd), "AT+QHTTPURL=%u,125", (unsigned int)strlen(url));
	ret = ec801e_send_cmd(cmd);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("CONNECT", NULL, NULL, "ERROR", NULL,
				   EC801E_QHTTP_CONNECT_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	printk("<- %s\n", rsp);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_send_raw(url);
	if (ret < 0) {
		return ret;
	}
	ret = ec801e_send_raw("\r");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR", EC801E_MEDIUM_TIMEOUT_MS,
				   rsp, EC801E_RX_BUF_LEN);
	printk("<- %s\n", rsp);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_send_cmd("AT+QHTTPGET=125");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("+QHTTPGET: 0,200", "+QHTTPGET: 0,206", "OK", "ERROR",
				   "CME ERROR", EC801E_QHTTP_CONNECT_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	printk("<- %s\n", rsp);
	if (ret < 0) {
		return ret;
	}

	/* Some firmware returns immediate OK, then reports final HTTP status via URC later. */
	if (strstr(rsp, "+QHTTPGET:") == NULL) {
		ret = ec801e_wait_response("+QHTTPGET: 0,200", "+QHTTPGET: 0,206", NULL,
					   "ERROR", "CME ERROR", EC801E_QHTTP_CONNECT_TIMEOUT_MS,
					   rsp, EC801E_RX_BUF_LEN);
		printk("<- %s\n", rsp);
		if (ret < 0) {
			printk("EC801E: no QHTTPGET URC, continue with QHTTPREAD attempt\n");
		}
	}

	k_sleep(K_MSEC(500));

	ret = ec801e_send_cmd("AT+QHTTPREAD=80");
	if (ret < 0) {
		printk("EC801E: QHTTPREAD=80 send failed, try QHTTPREAD\n");
		goto read_no_arg;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
				   EC801E_LONG_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	printk("QHTTP TRACE RAW:\n%s\n", rsp);
	if (ret == 0) {
		return 0;
	}

read_no_arg:
	ret = ec801e_send_cmd("AT+QHTTPREAD");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
				   EC801E_LONG_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	printk("QHTTP TRACE RAW:\n%s\n", rsp);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int ec801e_send_http_trace_request(void)
{
	int ret;
	const char *req = "GET /cdn-cgi/trace HTTP/1.1\r\n"
			  "Host: www.cloudflare.com\r\n"
			  "Connection: close\r\n"
			  "User-Agent: ec801e-zephyr\r\n\r\n";
	char cmd[64];

	char *rsp = ec801e_rsp_buf;
	snprintk(cmd, sizeof(cmd), "AT+QISEND=0,%u", (unsigned int)strlen(req));
	ret = ec801e_send_cmd(cmd);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response(">", NULL, NULL, "ERROR", NULL, EC801E_MEDIUM_TIMEOUT_MS, rsp,
				   EC801E_RX_BUF_LEN);
	printk("<- %s\n", rsp);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_send_raw(req);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("SEND OK", NULL, NULL, "SEND FAIL", "ERROR",
				   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	printk("<- %s\n", rsp);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int ec801e_read_trace_payload(void)
{
	char *rsp = ec801e_rsp_buf;
	int ret;

	for (int i = 0; i < 8; i++) {
		ret = ec801e_send_cmd("AT+QIRD=0,1024");
		if (ret < 0) {
			return ret;
		}

		ret = ec801e_wait_response("+QIRD:", NULL, NULL, "ERROR", NULL,
					  EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
		if (ret < 0) {
			return ret;
		}

		printk("TRACE CHUNK[%d]:\n%s\n", i, rsp);

		if (strstr(rsp, "h=") != NULL && strstr(rsp, "ip=") != NULL && strstr(rsp, "tls=") != NULL) {
			break;
		}

		k_sleep(K_MSEC(300));
	}

	return 0;
}

int ec801e_fetch_cloudflare_trace(void)
{
	int ret;

	ret = ec801e_socket_open_ssl("www.cloudflare.com", 443);
	if (ret < 0) {
		printk("EC801E open SSL socket failed: %d, fallback to QHTTP HTTPS\n", ret);
		ret = ec801e_fetch_cloudflare_trace_qhttp_url("https://www.cloudflare.com/cdn-cgi/trace");
		if (ret < 0) {
			printk("EC801E HTTPS QHTTP fallback failed: %d, retry plain HTTP\n", ret);
			ret = ec801e_fetch_cloudflare_trace_qhttp_url("http://www.cloudflare.com/cdn-cgi/trace");
			if (ret < 0) {
				printk("EC801E plain HTTP fallback failed: %d\n", ret);
			}
		}
		return ret;
	}

	ret = ec801e_send_http_trace_request();
	if (ret < 0) {
		printk("EC801E send request failed: %d\n", ret);
		goto out_close;
	}

	ret = ec801e_read_trace_payload();
	if (ret < 0) {
		printk("EC801E read payload failed: %d\n", ret);
	}

out_close:
	(void)ec801e_cmd_expect("AT+QICLOSE=0", "OK", "ERROR", EC801E_SHORT_TIMEOUT_MS);
	return ret;
}

static size_t ec801e_collect_uart(char *out, size_t out_len, int total_ms, int idle_break_ms)
{
	int64_t start = k_uptime_get();
	int64_t last_rx = start;
	size_t pos = 0;

	if (out_len == 0U) {
		return 0;
	}

	out[0] = '\0';
	while ((k_uptime_get() - start) < total_ms) {
		unsigned char ch;
		int ret = uart_poll_in(ec801e_uart, &ch);

		if (ret == 0) {
			if (pos < out_len - 1U) {
				out[pos++] = (char)ch;
				out[pos] = '\0';
			}
			last_rx = k_uptime_get();
		} else {
			if ((pos > 0U) && ((k_uptime_get() - last_rx) > idle_break_ms)) {
				break;
			}
			k_sleep(K_MSEC(10));
		}
	}

	return pos;
}

static int ec801e_extract_ipv4(const char *text, char *ip, size_t ip_len)
{
	if ((text == NULL) || (ip == NULL) || (ip_len < 8U)) {
		return -EINVAL;
	}

	for (size_t i = 0U; text[i] != '\0'; i++) {
		if (!isdigit((unsigned char)text[i])) {
			continue;
		}

		size_t j = i;
		int dots = 0;
		while (text[j] != '\0' && (isdigit((unsigned char)text[j]) || text[j] == '.')) {
			if (text[j] == '.') {
				dots++;
			}
			j++;
		}

		if (dots == 3) {
			size_t n = j - i;
			if (n < ip_len) {
				memcpy(ip, &text[i], n);
				ip[n] = '\0';
				return 0;
			}
		}
	}

	return -ENOENT;
}

int ec801e_socket_stack_init(const char *apn)
{
	int ret;

	if (ec801e_stack_ready) {
		return 0;
	}

	ret = ec801e_uart_rw_selftest();
	if ((ret < 0) && (ret != -EAGAIN)) {
		return ret;
	}

	ret = ec801e_boot();
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_prepare_network((apn != NULL) ? apn : CONFIG_EC801E_APN);
	if (ret < 0) {
		return ret;
	}

	ec801e_stack_ready = true;
	return 0;
}

int ec801e_resolve_ipv4(const char *host, char *ip, size_t ip_len)
{
	char cmd[EC801E_CMD_BUF_LEN];
	char *rsp = ec801e_rsp_buf;
	int ret = -EIO;
	const char *dns_cmd_candidates[] = {
		"AT+QIDNSGIP=\"%s\"",
		"AT+QIDNSGIP=1,\"%s\"",
		"AT+QIDNSGIP=1,\"%s\",1",
	};

	if ((host == NULL) || (ip == NULL) || (ip_len == 0U)) {
		return -EINVAL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(dns_cmd_candidates); i++) {
		ret = ec801e_cmd_format(cmd, sizeof(cmd), dns_cmd_candidates[i], host);
		if (ret < 0) {
			continue;
		}
		ec801e_drain_rx();
		ret = ec801e_send_cmd(cmd);
		if (ret < 0) {
			continue;
		}

		ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
					   EC801E_LONG_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
		printk("<- %s\n", rsp);
		if (ret < 0) {
			continue;
		}

		ret = ec801e_extract_ipv4(rsp, ip, ip_len);
		if (ret == 0) {
			return 0;
		}

		/* Some firmware reports DNS result in delayed URC, retry short receive window. */
		ret = ec801e_wait_response("+QIDNSGIP:", NULL, NULL, "ERROR", "CME ERROR",
					   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
		if (ret == 0) {
			printk("<- %s\n", rsp);
			if (ec801e_extract_ipv4(rsp, ip, ip_len) == 0) {
				return 0;
			}
		}
	}

	return -ENOENT;

}

int ec801e_socket_open_tcp(const char *remote_ip, uint16_t remote_port)
{
	char cmd[EC801E_CMD_BUF_LEN];
	char *rsp = ec801e_rsp_buf;
	int ret;

	if ((remote_ip == NULL) || (remote_port == 0U)) {
		return -EINVAL;
	}

	(void)ec801e_cmd_expect("AT+QICLOSE=0", "OK", "ERROR", EC801E_SHORT_TIMEOUT_MS);

	ret = ec801e_cmd_format(cmd, sizeof(cmd), "AT+QIOPEN=1,0,\"TCP\",\"%s\",%u,0,0",
			       remote_ip, (unsigned int)remote_port);
	if (ret < 0) {
		return ret;
	}
	ec801e_drain_rx();
	ret = ec801e_send_cmd(cmd);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_qiopen_result(EC801E_LONG_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	EC801E_VLOG("<- %s\n", rsp);
	if (ret < 0) {
		return ret;
	}

	ec801e_socket_opened = true;
	return 0;
}

int ec801e_socket_send(const uint8_t *data, size_t len)
{
	char cmd[64];
	char *rsp = ec801e_rsp_buf;
	int ret;

	if ((data == NULL) || (len == 0U)) {
		return -EINVAL;
	}
	if (!ec801e_socket_opened) {
		return -ENOTCONN;
	}

	snprintk(cmd, sizeof(cmd), "AT+QISEND=0,%u", (unsigned int)len);
	ec801e_drain_rx();
	ret = ec801e_send_cmd(cmd);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response(">", NULL, NULL, "ERROR", "CME ERROR", EC801E_MEDIUM_TIMEOUT_MS,
				   rsp, EC801E_RX_BUF_LEN);
	if (ret < 0) {
		EC801E_VLOG("<- %s\n", rsp);
		return ret;
	}

	ret = ec801e_uart_write(data, len);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("SEND OK", NULL, NULL, "SEND FAIL", "ERROR",
				   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	EC801E_VLOG("<- %s\n", rsp);
	if (ret < 0) {
		return ret;
	}

	return (int)len;
}

int ec801e_socket_recv(uint8_t *buf, size_t buf_len, size_t *out_len, int timeout_ms)
{
	char cmd[64];
	char *rsp = ec801e_rsp_buf;
	int ret;
	size_t collected;
	unsigned int n = 0U;
	char *payload_start;
	size_t available;

	if ((buf == NULL) || (buf_len == 0U) || (out_len == NULL)) {
		return -EINVAL;
	}
	if (!ec801e_socket_opened) {
		return -ENOTCONN;
	}

	*out_len = 0U;
	snprintk(cmd, sizeof(cmd), "AT+QIRD=0,%u", (unsigned int)MIN(buf_len, 1024U));
	ec801e_drain_rx();
	ret = ec801e_send_cmd(cmd);
	if (ret < 0) {
		return ret;
	}

	/* Collect a complete QIRD frame in one shot. */
	collected = ec801e_collect_uart(rsp, EC801E_RX_BUF_LEN, timeout_ms, 120);
	EC801E_VLOG("<- %s\n", rsp);

	if (strstr(rsp, "ERROR") != NULL || strstr(rsp, "CME ERROR") != NULL) {
		return -EIO;
	}

	payload_start = strstr(rsp, "+QIRD:");
	if (payload_start == NULL) {
		return -EAGAIN;
	}

	if (sscanf(payload_start, "+QIRD: %u", &n) != 1 || n == 0U) {
		return -EAGAIN;
	}

	payload_start = strstr(payload_start, "\r\n");
	if (payload_start == NULL) {
		return -EBADMSG;
	}
	payload_start += 2;

	available = collected - (size_t)(payload_start - rsp);
	if (available < n) {
		return -EBADMSG;
	}

	if (n > buf_len) {
		n = (unsigned int)buf_len;
	}

	memcpy(buf, payload_start, n);
	*out_len = n;
	return 0;
}

int ec801e_socket_close(void)
{
	int ret = ec801e_cmd_expect("AT+QICLOSE=0", "OK", "ERROR", EC801E_SHORT_TIMEOUT_MS);
	ec801e_socket_opened = false;
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static void ec801e_copy_digits(char *dst, size_t dst_len, const char *src)
{
	size_t w = 0U;

	if ((dst == NULL) || (dst_len == 0U)) {
		return;
	}

	dst[0] = '\0';
	if (src == NULL) {
		return;
	}

	for (size_t i = 0U; src[i] != '\0' && w < (dst_len - 1U); i++) {
		if (isdigit((unsigned char)src[i])) {
			dst[w++] = src[i];
		}
	}

	dst[w] = '\0';
}

static int ec801e_parse_cereg_stat(const char *rsp)
{
	char *p;
	char *comma;

	if (rsp == NULL) {
		return -EINVAL;
	}

	p = strstr(rsp, "+CEREG:");
	if (p == NULL) {
		return -ENOENT;
	}

	comma = strchr(p, ',');
	if (comma == NULL) {
		return -ENOENT;
	}

	return atoi(comma + 1);
}

int ec801e_get_module_info(struct ec801e_module_info *info)
{
	char *rsp = ec801e_rsp_buf;
	int ret;

	if (info == NULL) {
		return -EINVAL;
	}

	memset(info, 0, sizeof(*info));
	info->cereg_stat = -1;

	ret = ec801e_socket_stack_init(NULL);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_send_cmd("AT+GSN");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
				   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	if (ret < 0) {
		return ret;
	}

	ec801e_copy_digits(info->imei, sizeof(info->imei), rsp);
	if (info->imei[0] == '\0') {
		return -EBADMSG;
	}

	ret = ec801e_send_cmd("AT+CGPADDR=1");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
				   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	if (ret == 0) {
		if (ec801e_extract_ipv4(rsp, info->ip, sizeof(info->ip)) == 0) {
			info->ip_valid = true;
		}
	}

	ret = ec801e_send_cmd("AT+CEREG?");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
				   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_parse_cereg_stat(rsp);
	if (ret >= 0) {
		info->cereg_stat = ret;
	}

	return 0;
}

int ec801e_get_signal_info(struct ec801e_signal_info *info)
{
	char *rsp = ec801e_rsp_buf;
	int ret;
	int rssi = 99;
	int ber = 99;
	char *p;

	if (info == NULL) {
		return -EINVAL;
	}

	memset(info, 0, sizeof(*info));
	info->rssi_raw = 99;
	info->ber = 99;
	info->rssi_dbm = INT_MIN;

	ret = ec801e_socket_stack_init(NULL);
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_send_cmd("AT+CSQ");
	if (ret < 0) {
		return ret;
	}

	ret = ec801e_wait_response("OK", NULL, NULL, "ERROR", "CME ERROR",
				   EC801E_MEDIUM_TIMEOUT_MS, rsp, EC801E_RX_BUF_LEN);
	if (ret < 0) {
		return ret;
	}

	p = strstr(rsp, "+CSQ:");
	if ((p == NULL) || (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) != 2)) {
		return -EBADMSG;
	}

	info->rssi_raw = rssi;
	info->ber = ber;
	if (rssi >= 0 && rssi <= 31) {
		info->rssi_dbm = -113 + (2 * rssi);
	}

	return 0;
}

int ec801e_get_en_pin_state(int *level)
{
	int ret;

	if (level == NULL) {
		return -EINVAL;
	}

	if (ec801e_power_gpio.port == NULL) {
		return -ENOTSUP;
	}

	if (!device_is_ready(ec801e_power_gpio.port)) {
		return -ENODEV;
	}

	ret = gpio_pin_get_dt(&ec801e_power_gpio);
	if (ret < 0) {
		return ret;
	}

	*level = ret;
	return 0;
}
