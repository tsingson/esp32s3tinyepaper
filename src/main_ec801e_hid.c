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
#include "usb.h"

#include <ff.h>

#include "bitproto/drone_bp.h"
#include "bitproto/frameheader_bp.h"
#include "serv_addr.h"
#include "ec801e.h"
#include "bitproto/drone_bp.h"
#include "bitproto/socket_utls.h"
#include "bitproto/protocol_utls.h"
#include "drone.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);


/* 改变磁盘名称为 FLASH */
#define DISK_NAME "FLASH"
#define MOUNT_POINT "/FLASH:"
#define TARGET_FILE_PATH "/FLASH:/hosts.txt"
#define HOSTS_BUF_SIZE 128
#define HOSTS_POLL_INTERVAL_MS 3000
//

#define HOSTS_BUF_SIZE 128
#define DRONE_REPORT_INTERVAL_MS (4 * 60 * 1000)
#define MAIN_LOOP_SLEEP_MS 1000
#define GPSEC_IO_TIMEOUT_MS 30000
#define GPSEC_RETRY_BACKOFF_MS 1000
#define GPSEC_ROUNDS 3
#define GPSEC_RETRIES 3

//
#include <zephyr/kernel.h>

static const char* const default_host = "192.168.1.100:8080\n";
static const char* const serv_ip = "192.168.0.1";
static const uint16_t serv_port = 8080;

static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = MOUNT_POINT,
};

static char last_host_value[HOSTS_BUF_SIZE];
static bool host_value_valid;
//
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

/*
 * Set the FAT volume label using FatFs API `f_setlabel` so that the
 * volume label root-directory entry is created/updated correctly. The
 * FatFs implementation handles both creating the root-dir label entry and
 * syncing metadata.
 *
 * pdrv: unused for FatFs API here (kept for compatibility)
 * label: ASCII label (up to 11 chars). Longer input will be truncated.
 */
static int set_fat_volume_label(const char* pdrv, const char* label)
{
    uint8_t sector[512];
    char lab11[11];
    int ret;
    size_t i;

    if (pdrv == NULL || label == NULL)
    {
        return -EINVAL;
    }

    /* Prepare 11-byte label (space-padded) */
    memset(lab11, ' ', sizeof(lab11));
    for (i = 0; i < sizeof(lab11) && label[i]; ++i) {
        lab11[i] = label[i];
    }

    /* Update VBR (sector 0) label fields first */
    ret = disk_access_read(pdrv, sector, 0, 1);
    if (ret) return ret;

    /* The mkfs implementation writes the 11-byte label starting at these offsets. */
    memcpy(&sector[43], lab11, 11);
    memcpy(&sector[71], lab11, 11);

    ret = disk_access_write(pdrv, sector, 0, 1);
    if (ret) return ret;

    /* Attempt to update the root-directory volume-label entry for FAT12/16.
     * For small on-device volumes (typical in this project) FAT12/16 is common
     * and the root-dir occupies a contiguous set of sectors immediately
     * following the FAT area, so we can update/create an AM_VOL entry there.
     * For FAT32 root-dir is cluster-based and not handled here.
     */
    {
        uint16_t bytes_per_sector = sector[11] | (sector[12] << 8);
        uint16_t root_ent_cnt = sector[17] | (sector[18] << 8);
        uint16_t rsvd_sec_cnt = sector[14] | (sector[15] << 8);
        uint8_t num_fats = sector[16];
        uint32_t fatsz16 = sector[22] | (sector[23] << 8);
        uint32_t fatsz32 = sector[36] | (sector[37] << 8) | (sector[38] << 16) | (sector[39] << 24);
        uint32_t fatsz = fatsz16 ? fatsz16 : fatsz32;

        if (fatsz == 0) {
            /* Unexpected; bail out */
            (void)disk_access_ioctl(pdrv, DISK_IOCTL_CTRL_SYNC, NULL);
            return 0;
        }

        if (fatsz32 != 0) {
            /* FAT32: root directory is cluster-based. We only updated VBR above.
             * Leaving further handling to FatFs APIs (not available here).
             */
            (void)disk_access_ioctl(pdrv, DISK_IOCTL_CTRL_SYNC, NULL);
            return 0;
        }

        /* FAT12/16: compute root dir sector and size */
        uint32_t root_dir_sectors = ((uint32_t)root_ent_cnt * 32 + bytes_per_sector - 1) / bytes_per_sector;
        uint32_t first_root_dir_sector = rsvd_sec_cnt + (uint32_t)num_fats * fatsz;

        if (root_dir_sectors == 0) {
            (void)disk_access_ioctl(pdrv, DISK_IOCTL_CTRL_SYNC, NULL);
            return 0;
        }

        /* Read root dir area */
        uint8_t *rootbuf = k_malloc(root_dir_sectors * bytes_per_sector);
        if (!rootbuf) return -ENOMEM;

        ret = disk_access_read(pdrv, rootbuf, first_root_dir_sector, root_dir_sectors);
        if (ret) {
            k_free(rootbuf);
            return ret;
        }

        /* Look for existing AM_VOL entry or a free slot */
        int found_idx = -1;
        int free_idx = -1;
        for (uint32_t off = 0; off < root_dir_sectors * bytes_per_sector; off += 32) {
            uint8_t first = rootbuf[off + 0];
            uint8_t attr = rootbuf[off + 11];
            if (first == 0x00) { /* no more entries */
                if (free_idx == -1) free_idx = (int)off;
                break;
            }
            if (first == 0xE5) { /* deleted */
                if (free_idx == -1) free_idx = (int)off;
                continue;
            }
            if ((attr & 0x08) == 0x08) { /* AM_VOL */
                found_idx = (int)off;
                break;
            }
        }

        int write_off = -1;
        if (found_idx >= 0) write_off = found_idx;
        else if (free_idx >= 0) write_off = free_idx;

        if (write_off >= 0) {
            memcpy(&rootbuf[write_off], lab11, 11);
            /* ensure attribute is volume label */
            rootbuf[write_off + 11] = 0x08;

            /* Write back modified root dir sectors */
            ret = disk_access_write(pdrv, rootbuf, first_root_dir_sector, root_dir_sectors);
        }

        k_free(rootbuf);
    }

    return disk_access_ioctl(pdrv, DISK_IOCTL_CTRL_SYNC, NULL);
}

/* USB setup and helpers moved to src/usb.c */

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
        /* Ensure the FAT volume has a readable label so host shows a name */
        (void)set_fat_volume_label(DISK_NAME, "FLASH");
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


void print_ec801e_diag(void)
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

//
//  消费者/接收方
void consumer_thread(void* p1, void* p2, void* p3)
{
    while (k_sem_take(&sync_sem, K_FOREVER) == 0)
    {
        printk("Consumer: Processed the event!\n");
        char host_value[HOSTS_BUF_SIZE];
        int ret = read_hosts(host_value, sizeof(host_value));
        if (ret < 0)
        {
            printk("Failed to read hosts.txt: %d\n", ret);
            continue;
        }
        else
        {
            printk("Consumer: Read hosts.txt: %s\n", host_value);

            char host[80];
            uint16_t port;

            int ret = parse_host_port(host_value, host, sizeof(host), &port);
            if (ret == 0)
            {
                printk("host:\t%s\tport:\t%d\n", host, port);
            }
        }
    }
}

#define STACK_SIZE 1024
#define PRIORITY 7

K_THREAD_DEFINE(cons_id, STACK_SIZE, consumer_thread, NULL, NULL, NULL, PRIORITY, 0, 0);


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
    // print_ec801e_diag();

    next_report_at = k_uptime_get();

    printk("Starting the worker thread manually...\n");

    for (int attempt = 1; attempt <= GPSEC_RETRIES; attempt++)
    {
        ret = run_roundtrip_session(serv_ip, serv_port);
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
                // ret = run_roundtrip_once();
                // if (ret == 0)
                // {
                //     break;
                // }
                // printk("[TCP] attempt %d/%d failed: %d (active=%s)\n",
                //        attempt, GPSEC_RETRIES, ret, get_serv_addr());
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
