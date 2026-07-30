#include "usb.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/usb/usbd_msg.h>
#include <zephyr/usb/usbd.h>
#include <string.h>

/*
 * USB module for providing a Mass Storage Class (MSC) LUN backed by the
 * on-device flash. This file centralizes USB device descriptors, callbacks,
 * and initialization so that `main` can remain focused on application logic.
 */
LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

/*
 * Semaphore used to notify consumer/worker threads that the host has
 * safely removed the MSC medium (USB unplug or logical unmount).
 * Main/other threads call `k_sem_take(&sync_sem, ...)` to wait for the event.
 */
K_SEM_DEFINE(sync_sem, 0, 1);

/* Indicates whether a USB host is currently connected and configured. This
 * flag is updated from the USBD message callback when VBUS/config events are
 * received. Application code may use this to decide whether to refresh
 * filesystem state or defer certain operations while a host is attached.
 */
bool usb_host_connected = false;

/* When true indicates the hosts file (on the MSC LUN) needs to be re-read.
 * This is set by the medium event handler when the host safely removes the
 * disk; the main loop can check this flag and remount/refresh the FS as
 * required.
 */
bool hosts_refresh_pending = false;

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

/*
 * USB device message callback.
 *
 * This callback is registered with the USBD core and receives asynchronous
 * notifications about USB bus and device state changes. Typical events we
 * care about:
 *  - USBD_MSG_VBUS_READY / USBD_MSG_CONFIGURATION / USBD_MSG_RESUME:
 *      Host powered and/or the device is configured -> mark host connected.
 *  - USBD_MSG_VBUS_REMOVED:
 *      Host removed VBUS -> mark host disconnected.
 *  - USBD_MSG_SUSPEND / USBD_MSG_RESET:
 *      We do not take special action for these in this app but they are
 *      provided for completeness.
 *
 * Parameters:
 *  - ctx: USBD context for the device (unused here but provided by API).
 *  - msg: pointer to the usbd_msg describing the event.
 */
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
        /* Host has powered / configured the device */
        usb_host_connected = true;
        break;
    case USBD_MSG_SUSPEND:
    case USBD_MSG_RESET:
        /* No-op for this application; could be used to pause activity */
        break;
    case USBD_MSG_VBUS_REMOVED:
        /* Host removed power; clear connected state */
        usb_host_connected = false;
        break;
    default:
        break;
    }
}

/*
 * Callback for MSC medium attach/remove events.
 *
 * Invoked by the USBD MSC class implementation when the logical LUN's
 * medium is loaded or removed by the host. We only handle events for the
 * configured `DISK_NAME`. On removal we mark `hosts_refresh_pending` and
 * give `sync_sem` so the application can react (remount, re-read files).
 */
void usbd_msc_medium_event(const char* disk, bool medium_loaded)
{
    if (disk == NULL || strcmp(disk, DISK_NAME) != 0)
    {
        return;
    }

    if (!medium_loaded)
    {
        hosts_refresh_pending = true;
        (void)k_sem_give(&sync_sem);
        printk("[HID] host safely removed disk, refresh pending\n");
    }
    else
    {
        printk("[HID] host loaded disk\n");
    }
}

/*
 * Adjust the bDeviceClass/bDeviceSubClass/bDeviceProtocol triple reported
 * in the device descriptor. Some host/os combinations expect particular
 * groupings when CDC ACM (serial) is present; this helper configures that
 * triple per-speed (FS/HS).
 */
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

/*
 * Configure and enable the USBD device as an MSC endpoint.
 *
 * Steps performed:
 *  - add string descriptors (lang, manufacturer, product)
 *  - add HS configuration and register classes if HS supported
 *  - add FS configuration and register classes
 *  - initialize the USBD context
 *  - register a message callback for VBUS/config events
 *  - enable the USBD device
 *
 * Returns 0 on success or a negative errno-style error code on failure.
 */
int setup_usb_msc(void)
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
 