#ifndef USB_H
#define USB_H

#include <zephyr/kernel.h>
#include <stdbool.h>

#define DISK_NAME "FLASH"
#define MOUNT_POINT "/FLASH:"
#define TARGET_FILE_PATH "/FLASH:/hosts.txt"

extern struct k_sem sync_sem;
extern bool usb_host_connected;
extern bool hosts_refresh_pending;

int setup_usb_msc(void);

#endif
