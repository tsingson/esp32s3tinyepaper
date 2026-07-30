#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/usb/usbd_msg.h>
#include <zephyr/usb/usbd.h>

#include <ff.h>

#include "bitproto/drone_bp.h"
#include "bitproto/frameheader_bp.h"
#include "ec801e.h"
#include "serv_addr.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define DISK_NAME "FLASH"
#define MOUNT_POINT "/FLASH:"
#define TARGET_FILE_PATH "/FLASH:/hosts.txt"

#define HOSTS_BUF_SIZE 128
#define DRONE_REPORT_INTERVAL_MS (4 * 60 * 1000)
#define MAIN_LOOP_SLEEP_MS 1000
#define GPSEC_IO_TIMEOUT_MS 30000
#define GPSEC_RETRY_BACKOFF_MS 1000
#define GPSEC_RETRIES 3

static const char* const default_host = "192.168.1.100:8080\n";
static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = MOUNT_POINT,
};

USBD_DEVICE_DEFINE(my_flash_usbd,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   0x16c0,
                   0x05e3);
USBD_DESC_LANG_DEFINE (usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(usb_mfr, "Qinshen");
USBD_DESC_PRODUCT_DEFINE(usb_product, "ESP32S3 Tiny MSC+TCP");
USBD_DESC_CONFIG_DEFINE(usb_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(usb_hs_cfg_desc, "HS Configuration");
USBD_CONFIGURATION_DEFINE(usb_fs_config, 0, 100, &usb_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(usb_hs_config, 0, 100, &usb_hs_cfg_desc);
USBD_DEFINE_MSC_LUN(flash, DISK_NAME, "Zephyr", "HostsDisk", "1.00");

static const char* const usb_class_blocklist[] = {
    "dfu_dfu",
    NULL,
};

static char last_host_value[HOSTS_BUF_SIZE];
static bool host_value_valid;
static bool usb_host_connected;
static bool hosts_refresh_pending;
static uint32_t g_seq;

void usbd_msc_medium_event(const char* disk, bool medium_loaded)
{
    if (disk == NULL || strcmp(disk, DISK_NAME) != 0)
    {
        return;
    }

    if (!medium_loaded)
    {
        hosts_refresh_pending = true;
        printk("[HID] host safely removed disk, refresh pending\n");
    }
    else
    {
        printk("[HID] host loaded disk\n");
    }
}

static void usb_msg_cb(struct usbd_context* const ctx,
                       const struct usbd_msg* const msg)
{
    (void)ctx;

    if (msg == NULL)
    {
        return;
    }

    switch (msg->type)
    {
    case USBD_MSG_VBUS_READY:
    case USBD_MSG_CONFIGURATION:
    case USBD_MSG_RESUME:
        usb_host_connected = true;
        break;
    case USBD_MSG_SUSPEND:
    case USBD_MSG_RESET:
        break;
    case USBD_MSG_VBUS_REMOVED:
        usb_host_connected = false;
        break;
    default:
        break;
    }
}

static void trim_line_end(char* s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n'))
    {
        s[--n] = '\0';
    }
}

static int parse_host_port(const char* input, char* host, size_t host_len, uint16_t* port)
{
    const char* colon;
    char port_str[8];
    long p;
    size_t host_part_len;
    size_t port_part_len;

    if (input == NULL || host == NULL || port == NULL || host_len < 2U)
    {
        return -EINVAL;
    }

    colon = strrchr(input, ':');
    if (colon == NULL || colon == input || colon[1] == '\0')
    {
        return -EINVAL;
    }

    host_part_len = (size_t)(colon - input);
    if (host_part_len >= host_len)
    {
        return -EINVAL;
    }

    port_part_len = strlen(colon + 1);
    if (port_part_len == 0U || port_part_len >= sizeof(port_str))
    {
        return -EINVAL;
    }

    memcpy(host, input, host_part_len);
    host[host_part_len] = '\0';

    memcpy(port_str, colon + 1, port_part_len + 1);
    p = strtol(port_str, NULL, 10);
    if (p <= 0 || p > 65535)
    {
        return -EINVAL;
    }

    *port = (uint16_t)p;
    return 0;
}

static int read_hosts(char* buf, size_t buf_len)
{
    struct fs_file_t file;
    ssize_t read_bytes;
    int ret;

    if (buf_len == 0U)
    {
        return -EINVAL;
    }

    buf[0] = '\0';
    fs_file_t_init(&file);

    ret = fs_open(&file, TARGET_FILE_PATH, FS_O_READ);
    if (ret < 0)
    {
        return ret;
    }

    read_bytes = fs_read(&file, buf, buf_len - 1);
    (void)fs_close(&file);
    if (read_bytes < 0)
    {
        return (int)read_bytes;
    }

    buf[read_bytes] = '\0';
    trim_line_end(buf);
    return 0;
}

static int ensure_default_hosts_file(void)
{
    struct fs_file_t file;
    int ret;

    fs_file_t_init(&file);
    ret = fs_open(&file, TARGET_FILE_PATH, FS_O_READ);
    if (ret == 0)
    {
        (void)fs_close(&file);
        return 0;
    }

    ret = fs_open(&file, TARGET_FILE_PATH, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0)
    {
        return ret;
    }

    ret = fs_write(&file, default_host, strlen(default_host));
    if (ret >= 0)
    {
        (void)fs_sync(&file);
    }
    (void)fs_close(&file);

    return ret < 0 ? ret : 0;
}

static bool hosts_addr_is_valid(const char* addr)
{
    char host[80];
    uint16_t port;

    if (addr == NULL || addr[0] == '\0')
    {
        return false;
    }

    return parse_host_port(addr, host, sizeof(host), &port) == 0;
}

static void usb_fix_code_triple(const enum usbd_speed speed)
{
    if (IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS))
    {
        (void)usbd_device_set_code_triple(&my_flash_usbd, speed,
                                          USB_BCC_MISCELLANEOUS, 0x02, 0x01);
    }
    else
    {
        (void)usbd_device_set_code_triple(&my_flash_usbd, speed, 0, 0, 0);
    }
}

static int setup_usb_msc(void)
{
    int ret;

    ret = usbd_add_descriptor(&my_flash_usbd, &usb_lang);
    if (ret < 0)
    {
        return ret;
    }
    ret = usbd_add_descriptor(&my_flash_usbd, &usb_mfr);
    if (ret < 0)
    {
        return ret;
    }
    ret = usbd_add_descriptor(&my_flash_usbd, &usb_product);
    if (ret < 0)
    {
        return ret;
    }

    if (USBD_SUPPORTS_HIGH_SPEED&& usbd_caps_speed(&my_flash_usbd)
    ==
    USBD_SPEED_HS
    )
    {
        ret = usbd_add_configuration(&my_flash_usbd, USBD_SPEED_HS, &usb_hs_config);
        if (ret < 0)
        {
            return ret;
        }
        ret = usbd_register_all_classes(&my_flash_usbd, USBD_SPEED_HS, 1,
                                        usb_class_blocklist);
        if (ret < 0)
        {
            return ret;
        }
        usb_fix_code_triple(USBD_SPEED_HS);
    }

    ret = usbd_add_configuration(&my_flash_usbd, USBD_SPEED_FS, &usb_fs_config);
    if (ret < 0)
    {
        return ret;
    }
    ret = usbd_register_all_classes(&my_flash_usbd, USBD_SPEED_FS, 1, usb_class_blocklist);
    if (ret < 0)
    {
        return ret;
    }
    usb_fix_code_triple(USBD_SPEED_FS);

    ret = usbd_init(&my_flash_usbd);
    if (ret < 0)
    {
        return ret;
    }

    ret = usbd_msg_register_cb(&my_flash_usbd, usb_msg_cb);
    if (ret < 0)
    {
        return ret;
    }

    return usbd_enable(&my_flash_usbd);
}

static int setup_flash_disk(void)
{
    int ret;

    ret = disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);
    if (ret < 0)
    {
        return ret;
    }

    ret = fs_mount(&mp);
    if (ret < 0)
    {
        ret = fs_mkfs(FS_FATFS, (uintptr_t)DISK_NAME, NULL, 0);
        if (ret < 0)
        {
            return ret;
        }
        ret = fs_mount(&mp);
        if (ret < 0)
        {
            return ret;
        }
    }

    ret = ensure_default_hosts_file();
    if (ret < 0)
    {
        return ret;
    }

    ret = read_hosts(last_host_value, sizeof(last_host_value));
    if (ret < 0)
    {
        return ret;
    }

    host_value_valid = true;
    ret = serv_addr_init(last_host_value);
    if (ret < 0)
    {
        return ret;
    }

    hosts_refresh_pending = false;

    printk("[HID] hosts.txt=%s\n", last_host_value);
    printk("[HID] active=%s\n", get_serv_addr());
    return 0;
}

static void refresh_hosts_if_needed(void)
{
    char host_value[HOSTS_BUF_SIZE];
    int ret;

    if (!hosts_refresh_pending)
    {
        return;
    }
    hosts_refresh_pending = false;

    ret = disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL);
    if (ret < 0)
    {
        LOG_DBG("disk sync failed (%d)", ret);
    }

    ret = fs_unmount(&mp);
    if (ret < 0)
    {
        return;
    }

    ret = fs_mount(&mp);
    if (ret < 0)
    {
        LOG_WRN("fs remount failed (%d)", ret);
        return;
    }

    if (read_hosts(host_value, sizeof(host_value)) == -ENOENT)
    {
        return;
    }

    ret = read_hosts(host_value, sizeof(host_value));
    if (ret < 0)
    {
        return;
    }

    if (!host_value_valid || strcmp(host_value, last_host_value) != 0)
    {
        if (host_value[0] == '\0')
        {
            printk("[HID] hosts.txt is empty, keep active=%s\n", get_serv_addr());
        }
        else if (!hosts_addr_is_valid(host_value))
        {
            printk("[HID] hosts.txt invalid '%s', keep active=%s\n",
                   host_value, get_serv_addr());
        }
        else
        {
            ret = serv_addr_try_promote(host_value);
            if (ret == 0)
            {
                printk("[HID] promote success active=%s\n", get_serv_addr());
            }
            else
            {
                printk("[HID] promote keep old active=%s\n", get_serv_addr());
            }
        }

        strncpy(last_host_value, host_value, sizeof(last_host_value) - 1);
        last_host_value[sizeof(last_host_value) - 1] = '\0';
        host_value_valid = true;
    }
}

static void fill_drone(struct Drone* drone, uint32_t seq)
{
    memset(drone, 0, sizeof(*drone));

    drone->status = DRONE_STATUS_RISING;
    drone->position.longitude = 2000U + seq;
    drone->position.latitude = 3000U + seq;
    drone->position.altitude = 1080U + seq;
    drone->flight.pose.yaw = 4321 + (int32_t)seq;
    drone->flight.pose.pitch = 1234 + (int32_t)seq;
    drone->flight.pose.roll = 5678 + (int32_t)seq;
    drone->flight.acceleration[0] = -1001 - (int32_t)seq;
    drone->flight.acceleration[1] = 1002 + (int32_t)seq;
    drone->flight.acceleration[2] = 1003 + (int32_t)seq;
    drone->power.is_charging = ((seq % 2U) == 0U);
    drone->power.battery = 98;
    drone->power.status = POWER_STATUS_ON;
    drone->propellers[0].id = 1;
    drone->propellers[0].direction = ROTATING_DIRECTION_CLOCK_WISE;
    drone->propellers[0].status = PROPELLER_STATUS_ROTATING;
    drone->network.signal = 15;
    drone->network.heartbeat_at = 1611280511628LL + (int64_t)seq;
    drone->landing_gear.status = LANDING_GEAR_STATUS_FOLDED;
}

static int send_all(int sock, const uint8_t* buf, size_t len)
{
    int64_t deadline = k_uptime_get() + GPSEC_IO_TIMEOUT_MS;

    while (len > 0U)
    {
        ssize_t n = zsock_send(sock, buf, len, 0);
        if (n < 0)
        {
            if (errno == EAGAIN && k_uptime_get() < deadline)
            {
                k_sleep(K_MSEC(100));
                continue;
            }
            return -EIO;
        }
        if (n == 0)
        {
            return -EIO;
        }
        buf += n;
        len -= (size_t)n;
    }

    return 0;
}

static int recv_all(int sock, uint8_t* buf, size_t len)
{
    int64_t deadline = k_uptime_get() + GPSEC_IO_TIMEOUT_MS;

    while (len > 0U)
    {
        ssize_t n = zsock_recv(sock, buf, len, 0);
        if (n < 0)
        {
            if (errno == EAGAIN && k_uptime_get() < deadline)
            {
                k_sleep(K_MSEC(100));
                continue;
            }
            return -EIO;
        }
        if (n == 0)
        {
            return -EIO;
        }
        buf += n;
        len -= (size_t)n;
    }

    return 0;
}

static int connect_active_socket(void)
{
    const char* serv_addr = get_serv_addr();
    char host[80];
    uint16_t port;
    char port_str[8];
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };
    struct zsock_addrinfo* res = NULL;
    struct zsock_timeval timeout = {
        .tv_sec = GPSEC_IO_TIMEOUT_MS / 1000,
        .tv_usec = 0,
    };
    int sock = -1;
    int ret;

    ret = parse_host_port(serv_addr, host, sizeof(host), &port);
    if (ret < 0)
    {
        printk("invalid active address: %s\n", serv_addr);
        return ret;
    }

    snprintk(port_str, sizeof(port_str), "%u", (unsigned int)port);
    ret = zsock_getaddrinfo(host, port_str, &hints, &res);
    if (ret != 0 || res == NULL)
    {
        printk("resolve failed for %s:%s (%d)\n", host, port_str, ret);
        return -EHOSTUNREACH;
    }

    sock = zsock_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0)
    {
        ret = -errno;
        zsock_freeaddrinfo(res);
        return ret;
    }

    (void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    if (ret < 0)
    {
        ret = -errno;
        (void)zsock_close(sock);
        return ret;
    }

    return sock;
}

static int write_frame(int sock, const uint8_t* payload, size_t payload_len)
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

static int read_frame(int sock, uint8_t* payload, size_t payload_capacity, size_t* out_len)
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

static int run_roundtrip_once(void)
{
    int sock;
    int ret;
    struct Drone drone = {0};
    struct Drone reply_drone = {0};
    uint8_t payload[BYTES_LENGTH_DRONE] = {0};
    uint8_t reply[BYTES_LENGTH_DRONE] = {0};
    size_t payload_len;
    size_t reply_len = 0;

    sock = connect_active_socket();
    if (sock < 0)
    {
        return sock;
    }

    fill_drone(&drone, g_seq);
    payload_len = EncodeDrone(&drone, (unsigned char*)payload);
    if (payload_len != BYTES_LENGTH_DRONE)
    {
        (void)zsock_close(sock);
        return -EINVAL;
    }

    ret = write_frame(sock, payload, payload_len);
    if (ret < 0)
    {
        (void)zsock_close(sock);
        return ret;
    }

    ret = read_frame(sock, reply, sizeof(reply), &reply_len);
    if (ret < 0)
    {
        (void)zsock_close(sock);
        return ret;
    }

    if (reply_len != payload_len || memcmp(reply, payload, payload_len) != 0)
    {
        (void)zsock_close(sock);
        return -EIO;
    }

    if (DecodeDrone(&reply_drone, (unsigned char*)reply) != BYTES_LENGTH_DRONE)
    {
        (void)zsock_close(sock);
        return -EBADMSG;
    }

    printk("[TCP] seq=%u ok active=%s lat=%u lon=%u alt=%u\n",
           (unsigned int)g_seq,
           get_serv_addr(),
           (unsigned int)reply_drone.position.latitude,
           (unsigned int)reply_drone.position.longitude,
           (unsigned int)reply_drone.position.altitude);
    g_seq++;
    (void)zsock_close(sock);
    return 0;
}

static void print_ec801e_diag(void)
{
    struct ec801e_module_info mod = {0};
    struct ec801e_signal_info sig = {0};
    int en_level = -1;
    int qret;

    qret = ec801e_get_module_info(&mod);
    if (qret == 0)
    {
        printk("EC801E info: imei=%s cereg=%d ip=%s\n",
               mod.imei, mod.cereg_stat, mod.ip_valid ? mod.ip : "N/A");
    }

    qret = ec801e_get_signal_info(&sig);
    if (qret == 0)
    {
        printk("EC801E signal: csq=%d ber=%d rssi_dbm=%d\n",
               sig.rssi_raw, sig.ber, sig.rssi_dbm);
    }

    qret = ec801e_get_en_pin_state(&en_level);
    if (qret == 0)
    {
        printk("EC801E EN pin level: %d\n", en_level);
    }
}

int main(void)
{
    int ret;
    int64_t next_report_at;

    k_sleep(K_MSEC(800));
    printk("\n=== EC801E + HID serv_addr TCP Demo ===\n");

    ret = setup_flash_disk();
    if (ret < 0)
    {
        printk("setup_flash_disk failed: %d\n", ret);
        return ret;
    }

    ret = setup_usb_msc();
    if (ret < 0)
    {
        printk("setup_usb_msc failed: %d\n", ret);
        return ret;
    }

    printk("USB MSC + CDC ready, active=%s\n", get_serv_addr());
    print_ec801e_diag();

    next_report_at = k_uptime_get();

    while (true)
    {
        int64_t now = k_uptime_get();

        if (usb_host_connected || hosts_refresh_pending)
        {
            refresh_hosts_if_needed();
        }

        if (now >= next_report_at)
        {
            ret = -EIO;
            for (int attempt = 1; attempt <= GPSEC_RETRIES; attempt++)
            {
                ret = run_roundtrip_once();
                if (ret == 0)
                {
                    break;
                }
                printk("[TCP] attempt %d/%d failed: %d (active=%s)\n",
                       attempt, GPSEC_RETRIES, ret, get_serv_addr());
                if (attempt < GPSEC_RETRIES)
                {
                    k_sleep(K_MSEC(GPSEC_RETRY_BACKOFF_MS));
                }
            }

            next_report_at = k_uptime_get() + DRONE_REPORT_INTERVAL_MS;
        }

        k_sleep(K_MSEC(MAIN_LOOP_SLEEP_MS));
    }

    return 0;
}
