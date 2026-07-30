#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <ff.h>
#include <string.h>
#include <stdbool.h>

#include <errno.h>

#include "serv_addr.h"

/* Zephyr 4.4.1 新版 USB 设备协议栈 */
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* 改变磁盘名称为 FLASH */
#define DISK_NAME "FLASH"
#define MOUNT_POINT "/FLASH:"
#define TARGET_FILE_PATH "/FLASH:/hosts.txt"
#define HOSTS_BUF_SIZE 128
#define HOSTS_POLL_INTERVAL_MS 3000

static const char* const default_host = "192.168.1.100:8080\n";
static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = MOUNT_POINT,
};

/* 声明新版 USB 设备实例 */
USBD_DEVICE_DEFINE(my_flash_usbd,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   0x16c0,
                   0x05e3);

USBD_DESC_LANG_DEFINE (usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(usb_mfr, "Qinshen");
USBD_DESC_PRODUCT_DEFINE(usb_product, "ESP32S3 Tiny MSC");

USBD_DESC_CONFIG_DEFINE(usb_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(usb_hs_cfg_desc, "HS Configuration");

USBD_CONFIGURATION_DEFINE(usb_fs_config, 0, 100, &usb_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(usb_hs_config, 0, 100, &usb_hs_cfg_desc);

USBD_DEFINE_MSC_LUN(flash, DISK_NAME, "Zephyr", "HostsDisk", "1.00");

static const char* const usb_class_blocklist[] = {
    "dfu_dfu",
    NULL,
};

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

static char last_host_value[HOSTS_BUF_SIZE];
static bool host_value_valid;

static void trim_line_end(char* s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n'))
    {
        s[--n] = '\0';
    }
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

/**
 * @brief 安全读取并打印 hosts.txt
 */
static void read_and_dump_hosts(void)
{
    char read_buf[HOSTS_BUF_SIZE];
    int ret;

    ret = read_hosts(read_buf, sizeof(read_buf));
    if (ret < 0)
    {
        LOG_WRN("无法打开 hosts.txt 进行读取 (%d)", ret);
        return;
    }

    printk("\r\n========================================\r\n");
    printk("[Flash 存储器] hosts.txt 当前地址:\r\n");
    printk("%s\r\n", read_buf[0] ? read_buf : "(empty)");
    printk("[激活地址] 当前生效地址:\r\n");
    printk("%s\r\n", get_serv_addr());
    printk("========================================\r\n");
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

/**
 * @brief 初始化物理 Flash 磁盘及文件系统
 */
static int setup_flash_disk(void)
{
    int ret;

    ret = disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);
    if (ret < 0)
    {
        LOG_ERR("Flash 磁盘初始化失败 (%d)", ret);
        return ret;
    }

    /* 1. 尝试挂载物理 Flash 文件系统 */
    ret = fs_mount(&mp);
    if (ret < 0)
    {
        LOG_WRN("首次上电或文件系统损坏 (错误码: %d)，正在尝试自动格式化物理 Flash...", ret);

        /* 如果未格式化，执行全新 FATFS 格式化 */
        ret = fs_mkfs(FS_FATFS, (uintptr_t)DISK_NAME, NULL, 0);
        if (ret < 0)
        {
            LOG_ERR("Flash 物理格式化失败！(%d)", ret);
            return ret;
        }

        /* 格式化后重新挂载 */
        ret = fs_mount(&mp);
        if (ret < 0)
        {
            LOG_ERR("格式化后挂载依旧失败 (%d)", ret);
            return ret;
        }
    }

    /* 2. 检查 hosts.txt 是否存在，若不存在则写入默认值 */
    ret = ensure_default_hosts_file();
    if (ret < 0)
    {
        LOG_ERR("创建 hosts.txt 失败 (%d)", ret);
        return ret;
    }

    ret = read_hosts(last_host_value, sizeof(last_host_value));
    if (ret < 0)
    {
        host_value_valid = false;
        return ret;
    }

    host_value_valid = true;

    ret = serv_addr_init(last_host_value);
    if (ret < 0)
    {
        LOG_ERR("激活地址初始化失败 (%d)", ret);
        return ret;
    }

    /* 3. 打印当前断电保存的有效地址 */
    read_and_dump_hosts();

    return 0;
}

static int setup_usb_msc(void)
{
    int ret;

    ret = usbd_add_descriptor(&my_flash_usbd, &usb_lang);
    if (ret < 0)
    {
        LOG_ERR("USB 语言描述符初始化失败 (%d)", ret);
        return ret;
    }

    ret = usbd_add_descriptor(&my_flash_usbd, &usb_mfr);
    if (ret < 0)
    {
        LOG_ERR("USB 厂商描述符初始化失败 (%d)", ret);
        return ret;
    }

    ret = usbd_add_descriptor(&my_flash_usbd, &usb_product);
    if (ret < 0)
    {
        LOG_ERR("USB 产品描述符初始化失败 (%d)", ret);
        return ret;
    }

    if (USBD_SUPPORTS_HIGH_SPEED&&
            usbd_caps_speed(&my_flash_usbd)

    ==
    USBD_SPEED_HS
    )
    {
        ret = usbd_add_configuration(&my_flash_usbd, USBD_SPEED_HS,
                                     &usb_hs_config);
        if (ret < 0)
        {
            LOG_ERR("USB HS 配置失败 (%d)", ret);
            return ret;
        }

        ret = usbd_register_all_classes(&my_flash_usbd, USBD_SPEED_HS, 1,
                                        usb_class_blocklist);
        if (ret < 0)
        {
            LOG_ERR("USB HS 类注册失败 (%d)", ret);
            return ret;
        }

        usb_fix_code_triple(USBD_SPEED_HS);
    }

    ret = usbd_add_configuration(&my_flash_usbd, USBD_SPEED_FS, &usb_fs_config);
    if (ret < 0)
    {
        LOG_ERR("USB FS 配置失败 (%d)", ret);
        return ret;
    }

    ret = usbd_register_all_classes(&my_flash_usbd, USBD_SPEED_FS, 1,
                                    usb_class_blocklist);
    if (ret < 0)
    {
        LOG_ERR("USB FS 类注册失败 (%d)", ret);
        return ret;
    }

    usb_fix_code_triple(USBD_SPEED_FS);

    ret = usbd_init(&my_flash_usbd);
    if (ret < 0)
    {
        LOG_ERR("USB 协议栈初始化失败 (%d)", ret);
        return ret;
    }

    ret = usbd_enable(&my_flash_usbd);
    if (ret < 0)
    {
        LOG_ERR("USB 启动失败 (%d)", ret);
        return ret;
    }

    return 0;
}

static void refresh_hosts_if_changed(void)
{
    char host_value[HOSTS_BUF_SIZE];
    int promote_ret;
    int ret;

    ret = disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL);
    if (ret < 0)
    {
        LOG_DBG("DISK sync 失败 (%d)", ret);
    }

    ret = fs_unmount(&mp);
    if (ret < 0)
    {
        LOG_DBG("文件系统卸载失败 (%d)", ret);
        return;
    }

    ret = fs_mount(&mp);
    if (ret < 0)
    {
        LOG_WRN("文件系统重挂载失败 (%d)", ret);
        return;
    }

    ret = read_hosts(host_value, sizeof(host_value));
    if (ret < 0)
    {
        LOG_WRN("读取 hosts.txt 失败 (%d)", ret);
        return;
    }

    if (!host_value_valid || strcmp(host_value, last_host_value) != 0)
    {
        promote_ret = serv_addr_try_promote(host_value);
        if (promote_ret == 0)
        {
            printk("\r\n[hosts.txt 已更新] 候选地址可达，激活地址切换为: %s\r\n",
                   get_serv_addr());
        }
        else
        {
            printk("\r\n[hosts.txt 已更新] 候选地址不可达，保持激活地址: %s\r\n",
                   get_serv_addr());
        }

        strncpy(last_host_value, host_value, sizeof(last_host_value) - 1);
        last_host_value[sizeof(last_host_value) - 1] = '\0';
        host_value_valid = true;
    }
}

int main(void)
{
    int ret;
    LOG_INF("系统正在启动 (Zephyr 4.4.1 物理持久化模式)...");

    /* 1. 初始化物理 Flash 磁盘 */
    ret = setup_flash_disk();
    if (ret < 0)
    {
        return ret;
    }

    /* 2. 绑定并启用新版 USB MSC */
    ret = setup_usb_msc();
    if (ret < 0)
    {
        return ret;
    }

    LOG_INF("USB 持久化 U 盘已完全就绪。");

    /* 3. 后台轮询：监控电脑是否对物理 Flash 盘进行了改动 */
    while (true)
    {
        k_sleep(K_MSEC(HOSTS_POLL_INTERVAL_MS));
        refresh_hosts_if_changed();
    }
    return 0;
}
