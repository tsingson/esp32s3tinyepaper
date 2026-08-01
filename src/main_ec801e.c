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
#include "drone.h"


#define GPSEC_SERVER_IP "142.54.180.58"
#define GPSEC_SERVER_PORT 8061
#define GPSEC_ROUNDS 3
#define GPSEC_RETRIES 3
#define GPSEC_IO_TIMEOUT_MS 30000
#define GPSEC_RETRY_BACKOFF_MS 1000


int main(void)
{
    k_sleep(K_MSEC(500));
    printk("\n=== EC801E-CN Bitproto TCP Demo ====\n");
    int ret;
    struct ec801e_module_info mod = {0};
    struct ec801e_signal_info sig = {0};
    int en_level = -1;
    int qret;

    k_sleep(K_MSEC(800));


    qret = ec801e_get_module_info(&mod);
    if (qret == 0)
    {
        printk("EC801E info: imei=%s cereg=%d ip=%s\n", mod.imei, mod.cereg_stat,
               mod.ip_valid ? mod.ip : "N/A");
    }
    else
    {
        printk("EC801E info query failed: %d\n", qret);
    }

    qret = ec801e_get_signal_info(&sig);
    if (qret == 0)
    {
        printk("EC801E signal: csq=%d ber=%d rssi_dbm=%d\n", sig.rssi_raw, sig.ber,
               sig.rssi_dbm);
    }
    else
    {
        printk("EC801E signal query failed: %d\n", qret);
    }

    qret = ec801e_get_en_pin_state(&en_level);
    if (qret == 0)
    {
        printk("EC801E EN pin level: %d\n", en_level);
    }
    else
    {
        printk("EC801E EN pin query unavailable: %d\n", qret);
    }

    for (int attempt = 1; attempt <= GPSEC_RETRIES; attempt++)
    {
        ret = run_roundtrip_session(GPSEC_SERVER_IP, GPSEC_SERVER_PORT);
        if (ret == 0)
        {
            printk("GPSEC tcpip example done rounds=%d attempt=%d\n", GPSEC_ROUNDS, attempt);
            break;
        }

        printk("GPSEC attempt %d/%d failed: %d\n", attempt, GPSEC_RETRIES, ret);
        if (attempt < GPSEC_RETRIES)
        {
            k_sleep(K_MSEC(GPSEC_RETRY_BACKOFF_MS));
        }
    }


    while (1)
    {
        k_sleep(K_SECONDS(10));
    }

    return 0;
}
