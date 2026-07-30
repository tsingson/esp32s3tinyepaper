//
// Created by tsingson on 2026/7/27.
//

#include "chip_info.h"

#include <esp_mac.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_heap.h>

// 获取设备树中 Flash 节点的总大小
#define FLASH_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_flash_controller))

void get_memory_info(void)
{
    // 1. Flash 空间大小
    printk("Flash Size: %d MB\n", (uint32_t)(FLASH_SIZE / (1024 * 1024)));

    // 2. RAM 空间大小（获取系统内部堆内存状态）
    extern struct k_heap _system_heap;
    struct sys_memory_stats stats;

    sys_heap_runtime_stats_get(&_system_heap.heap, &stats);
    printk("RAM Heap Total: %zu bytes\n", stats.free_bytes + stats.allocated_bytes);
    printk("RAM Heap Free: %zu bytes\n", stats.free_bytes);
}


void get_chip_id(void)
{
    uint8_t mac[6];
    // 获取基准 MAC 地址作为唯一 Chip ID
    if (esp_efuse_mac_get_default(mac) == ESP_OK)
    {
        printk("Chip ID (MAC): %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}


void get_cpu_freq(void)
{
    // 方式 A：Zephyr 标准内核时钟频率接口
    uint32_t hz = sys_clock_hw_cycles_per_sec();
    printk("CPU Frequency (Zephyr): %u MHz\n", hz / 1000000);

    // 方式 B：底层 HAL 接口（需额外包含相关 esp_clk 头文件）
    // uint32_t esp_hz = esp_clk_cpu_freq();
}
