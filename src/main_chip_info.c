#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_heap.h>   // 必须包含此头文件以引入 sys_heap_runtime_stats_get
#include <esp_mac.h>
#include <string.h>

#include "epd200x200.h"

#define FLASH_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_flash_controller))
#define STRIP_NODE DT_ALIAS(led_strip)
#define BREATH_STEP_MS 20
#define BREATH_MAX 64

#if !DT_NODE_HAS_STATUS(STRIP_NODE, okay)
#error "led-strip alias is not defined in devicetree"
#endif

#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

// 声明 Zephyr 全局内核系统堆对象
extern struct sys_heap _system_heap;

int main(void) {
    const struct device *strip = DEVICE_DT_GET(STRIP_NODE);
    struct led_rgb pixels[STRIP_NUM_PIXELS];
    int breath = 0;
    int breath_dir = 1;
    int hue = 0;
    int hue_dir = 1;

    // 延迟 500ms 等待硬件供电与串口驱动完全稳定
    k_sleep(K_MSEC(500));
                                                                               
    
    printk("\n=== ESP32-S3-Tiny System Info ===\n");

    // 1. Chip ID (MAC Address)
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        printk("Chip ID (MAC)   : %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // 2. CPU Freq
    uint32_t cpu_hz = sys_clock_hw_cycles_per_sec();
    printk("CPU Frequency   : %u MHz\n", cpu_hz / 1000000);

    // 3. Flash & RAM
    printk("Flash Size      : %u MB\n", (uint32_t)(FLASH_SIZE / (1024 * 1024)));

    // 【修复核心】：使用 Zephyr 4.4.1 标准内核堆统计接口
    struct sys_memory_stats stats;
    int ret = sys_heap_runtime_stats_get(&_system_heap, &stats);
    if (ret == 0) {
        printk("Heap Total/Free : %zu / %zu bytes\n",
               (stats.free_bytes + stats.allocated_bytes), stats.free_bytes);
    } else {
        printk("Failed to get heap stats (Error: %d)\n", ret);
    }

    // 4. Interfaces (使用 #if 代替 #ifdef，防止预处理逻辑错误)
#if DT_HAS_COMPAT_STATUS_OKAY(espressif_esp32_uart)
    printk("Active UARTs    : %d\n", DT_NUM_INST_STATUS_OKAY(espressif_esp32_uart));
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(espressif_esp32_i2c)
    printk("Active I2Cs     : %d\n", DT_NUM_INST_STATUS_OKAY(espressif_esp32_i2c));
#endif

    printk("=================================\n");

    ret = epd200x200_init();
    if (ret == 0) {
        ret = epd200x200_show_grayscale_transition();
        if (ret < 0) {
            printk("EPD grayscale failed: %d\n", ret);
        }
    } else {
        printk("EPD init failed: %d\n", ret);
    }

    if (!device_is_ready(strip)) {
        printk("LED strip device is not ready\n");
        return 0;
    }

    while (1) {
        memset(pixels, 0, sizeof(pixels));

        pixels[0].r = 0;
        pixels[0].g = (uint8_t)((breath * (255 - hue)) / 255);
        pixels[0].b = (uint8_t)((breath * hue) / 255);

        if (led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS) != 0) {
            printk("Failed to update WS2812\n");
        }

        breath += breath_dir;
        if (breath >= BREATH_MAX) {
            breath = BREATH_MAX;
            breath_dir = -1;
        } else if (breath <= 0) {
            breath = 0;
            breath_dir = 1;
        }

        hue += hue_dir;
        if (hue >= 255) {
            hue = 255;
            hue_dir = -1;
        } else if (hue <= 0) {
            hue = 0;
            hue_dir = 1;
        }

        k_sleep(K_MSEC(BREATH_STEP_MS));
    }
}
