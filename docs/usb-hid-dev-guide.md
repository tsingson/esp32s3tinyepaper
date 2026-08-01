# USB HID Disk 开发与配置指南（Zephyr 4.4.1 / ESP32-S3）

## 1. 文档目标

本文档面向本工程的 USB 磁盘功能开发，重点说明：

- 如何把 ESP32-S3 枚举为可读写 U 盘（MSC）
- 如何在同一根 USB 线上同时保留调试串口（CDC ACM）
- 如何将 hosts.txt 作为“候选地址”输入
- 如何将“激活地址”持久化到私有 flash，并实现 fail-back 机制

> 说明：本文中“USB HID Disk”沿用你的项目叫法；在 USB 标准分类里，该功能本质是 USB Mass Storage Class（MSC）。

## 2. 当前实现能力（本工程）

当前工程已经实现以下行为：

1. 板子连接电脑后，出现一个 U 盘（盘名 FLASH），可见并可编辑 hosts.txt。
2. hosts.txt 内容保存到 flash（MSC 映射分区）中，断电后保留。
3. 同时枚举 CDC ACM 虚拟串口，串口调试和 U 盘可并行使用。
4. 引入 get_serv_addr() 接口返回“当前激活地址”。
5. 首次烧录后：若激活地址未初始化，使用 hosts.txt 初值写入私有分区。
6. hosts.txt 修改后：先 TCP 探活，成功才覆盖激活地址；失败则保留原值（fail-back）。
7. 激活地址存放在非 MSC 私有分区，主机侧无法直接通过 U 盘修改。

## 3. 关键文件

- 入口：src/main_hid.c
- 激活地址模块：src/serv_addr.c
- 激活地址头文件：src/serv_addr.h
- 构建入口：CMakeLists.txt
- 工程配置：prj.conf
- 板级设备树覆盖：boards/esp32s3_devkitc_esp32s3_procpu.overlay

## 4. 系统架构

### 4.1 数据与控制流

1. 启动阶段
- 挂载 FLASH 文件系统（FATFS）
- 确保 hosts.txt 存在（不存在则写入默认值）
- 初始化激活地址（从私有分区读；不存在则用 hosts.txt 初值写入）
- 启动 USB 复合设备（MSC + CDC ACM）

2. 运行阶段（轮询）
- 定期执行 DISK sync + fs unmount/mount
- 重新读取 hosts.txt
- 若检测到变化：
  - 对新地址执行 TCP connect 探活
  - 成功：更新私有分区中的激活地址
  - 失败：保留旧激活地址

3. 对外接口
- get_serv_addr()：返回当前激活地址（只读）

### 4.2 分区模型（重点）

工程使用 flash0 固定分区：

- boot_partition：0x00000000, 64KB
- slot0_partition：0x00010000, 3776KB
- storage_partition：0x003C0000, 252KB（MSC 映射，主机可见）
- activeaddr_partition：0x003FF000, 4KB（私有，不映射 MSC）

设计意义：

- hosts.txt 可编辑，便于业务侧动态输入候选地址。
- 激活地址与 hosts.txt 物理隔离，满足“激活地址不可直接修改”。

## 5. 设备树（overlay）配置细节

文件：boards/esp32s3_devkitc_esp32s3_procpu.overlay

### 5.1 chosen 节点

- zephyr,console = &cdc_acm0
- zephyr,shell-uart = &cdc_acm0
- zephyr,flash-disk = &msc_disk0

含义：

- 控制台日志走 USB CDC ACM 虚拟串口。
- 文件系统 flash-disk 绑定到 msc_disk0 设备节点。

### 5.2 flash-disk 设备节点

在根节点下定义：

- compatible = "zephyr,flash-disk"
- partition = <&storage_partition>
- disk-name = "FLASH"
- cache-size = <4096>
- sector-size = <512>

注意：

- 仅配置 zephyr,flash-disk 到 partition label 不够，必须有实际 compatible 节点，CONFIG_DISK_DRIVER_FLASH 才能满足依赖。

### 5.3 USB 控制器与 CDC ACM 节点

- &usb_otg 置为 okay
- 在 &zephyr_udc0 下创建 cdc_acm_uart0（compatible = "zephyr,cdc-acm-uart"）

这样可以让 device-next USB 栈自动注册 CDC ACM class 实例。

## 6. Kconfig / prj.conf 配置细节

文件：prj.conf

### 6.1 文件系统与磁盘

- CONFIG_FILE_SYSTEM=y
- CONFIG_FAT_FILESYSTEM_ELM=y
- CONFIG_FS_FATFS_MKFS=y
- CONFIG_FILE_SYSTEM_MKFS=y
- CONFIG_DISK_ACCESS=y
- CONFIG_DISK_DRIVER_FLASH=y
- CONFIG_FLASH=y
- CONFIG_FLASH_MAP=y
- CONFIG_FLASH_PAGE_LAYOUT=y

### 6.2 USB device-next 与 class

- CONFIG_USB_DEVICE_STACK_NEXT=y
- CONFIG_USBD_MSC_CLASS=y
- CONFIG_USBD_CDC_ACM_CLASS=y
- CONFIG_USBD_MSC_LUNS_PER_INSTANCE=1

说明：

- 必须启用 device-next 栈和对应 class。
- 复合设备场景建议保留 USBD 类注册默认路径（register_all_classes）。

## 7. main_hid.c 核心流程拆解

文件：src/main_hid.c

### 7.1 存储与文件

- 逻辑盘名：FLASH
- 挂载点：/FLASH:
- hosts 文件：/FLASH:/hosts.txt

流程：

1. disk_access ioctl init
2. fs_mount
3. mount 失败时 mkfs + 重挂载
4. 确保 hosts.txt 存在
5. 读取 hosts 作为初始候选

### 7.2 USB 复合设备

代码包含：

- USBD_DEVICE_DEFINE
- USBD_DESC_* 字符串描述符
- USBD_CONFIGURATION_DEFINE（FS/HS）
- USBD_DEFINE_MSC_LUN(..., "FLASH", ...)
- usbd_add_configuration + usbd_register_all_classes
- usbd_init + usbd_enable

复合设备类码：

- 启用 CDC ACM 时，设置 MISC/0x02/0x01（IAD 复合设备常用三元组）

### 7.3 hosts 变化检测

轮询周期默认 3 秒：

- DISK_IOCTL_CTRL_SYNC
- fs_unmount + fs_mount
- 读取 hosts
- 变化时调用 serv_addr_try_promote

## 8. serv_addr 模块设计（fail-back 核心）

文件：src/serv_addr.c, src/serv_addr.h

### 8.1 对外接口

- int serv_addr_init(const char *initial_addr)
- int serv_addr_try_promote(const char *candidate_addr)
- const char *get_serv_addr(void)

### 8.2 初始化策略

serv_addr_init 行为：

1. 优先从 activeaddr_partition 读取记录
2. 若存在且校验通过，作为当前激活地址
3. 若不存在，使用 initial_addr 写入 flash 并成为激活地址

### 8.3 记录结构

私有分区中保存结构：

- magic（用于识别有效记录）
- checksum（FNV-1a 校验）
- addr[128]（地址字符串）

### 8.4 升级策略（try_promote）

当 hosts 候选地址变化时：

1. 校验字符串格式 host:port
2. TCP 探活（socket + getaddrinfo + connect，带超时）
3. connect 成功：写入私有分区并更新内存激活地址
4. connect 失败：返回错误，不改激活地址

这就是 fail-back：

- 候选不可达时，系统自动回退到旧激活地址继续使用。

## 9. 开发中的常见坑

1. 只配置 chosen 的 zephyr,flash-disk，没有建 zephyr,flash-disk 节点
- 现象：CONFIG_DISK_DRIVER_FLASH 依赖不满足
- 解决：增加 msc_disk0 compatible 节点

2. 忘记启用 CONFIG_USBD_CDC_ACM_CLASS
- 现象：U 盘有了但看不到串口
- 解决：启用 CDC ACM class，并将 console/shell 绑定 cdc_acm0

3. 使用不存在的 Kconfig 符号
- 某些符号在当前 Zephyr 版本不存在，会触发 warning 并中止配置（工程策略）

4. 仅靠 disk status 位判断主机写入
- 版本/驱动行为可能不稳定
- 当前方案用 sync + unmount/mount + 内容比较，工程上更稳

5. 把激活地址放在 MSC 可见分区
- 会被主机直接改写，不符合“激活地址不可修改”约束

## 10. 调试建议

1. 首次烧录
- 查看串口应打印“首次写入激活地址”
- get_serv_addr() 返回初始值

2. 修改 hosts 为不可达地址
- 串口应提示候选不可达
- 激活地址保持不变

3. 修改 hosts 为可达地址
- 串口应提示激活地址已更新
- get_serv_addr() 返回新值

4. 断电重启
- 激活地址应从 private 分区恢复，不依赖 hosts 当前内容

## 11. 构建、烧录、监控

可参考工程脚本：

- 构建：/Users/qinshen/go/zephyrproject/.venv/bin/west build -p always -b esp32s3_devkitc/esp32s3/procpu
- 烧录：/Users/qinshen/go/zephyrproject/.venv/bin/west flash --erase
- 监控：/Users/qinshen/go/zephyrproject/.venv/bin/west espressif monitor

如同机存在多个串口，建议根据设备名固定 monitor 端口。

## 12. 后续扩展建议

1. 地址格式扩展
- 支持 IPv6（如 [addr]:port）
- 支持域名白名单策略

2. 健康检查增强
- 除 TCP connect 外，增加应用层握手（例如发送固定探活包）

3. 状态可观测性
- 增加 shell 命令：show_addr
- 输出 当前激活地址、hosts 候选、最近探活结果、最近更新时间

4. 写放大控制
- 对相同地址不重复写入
- 增加最小更新时间窗，减少 flash 擦写频率

## 13. 最小上手版（5 分钟）

本节只保留“能跑起来”的最小步骤。

### 13.1 目标

完成后你应看到两件事：

1. 电脑上出现 FLASH 磁盘，能编辑 hosts.txt。
2. 同一 USB 线同时出现调试串口（CDC ACM），可看日志。

### 13.2 必要配置（最小集合）

1. overlay 里必须有 flash-disk 节点（映射 storage_partition）。
2. overlay 的 chosen：
- zephyr,flash-disk 指向 flash-disk 节点
- zephyr,console / zephyr,shell-uart 指向 cdc_acm0
3. overlay 在 zephyr_udc0 下添加 cdc_acm_uart0 节点。
4. prj.conf 至少开启：
- CONFIG_USB_DEVICE_STACK_NEXT=y
- CONFIG_USBD_MSC_CLASS=y
- CONFIG_USBD_CDC_ACM_CLASS=y
- CONFIG_FILE_SYSTEM=y
- CONFIG_FAT_FILESYSTEM_ELM=y
- CONFIG_DISK_ACCESS=y
- CONFIG_DISK_DRIVER_FLASH=y

### 13.3 一次编译烧录

在工程根目录执行：

1. /Users/qinshen/go/zephyrproject/.venv/bin/west build -p always -b esp32s3_devkitc/esp32s3/procpu
2. /Users/qinshen/go/zephyrproject/.venv/bin/west flash --erase

### 13.4 快速验证

1. 插上 USB 后，确认出现 FLASH 磁盘。
2. 打开 FLASH/hosts.txt，写入一个地址，例如 192.168.1.100:8080。
3. 打开串口监控：
- /Users/qinshen/go/zephyrproject/.venv/bin/west espressif monitor
4. 串口应打印：
- 当前 hosts.txt 地址
- 当前激活地址

### 13.5 fail-back 验证（最短路径）

1. 把 hosts.txt 改成明显不可达地址（例如 10.255.255.1:65000）。
2. 等待轮询周期后观察日志：
- 候选地址不可达
- 激活地址保持不变
3. 再改成可达地址。
4. 日志应显示激活地址更新成功。

### 13.6 常见失败与一键排查

1. 只有磁盘没有串口：
- 检查是否启用 CONFIG_USBD_CDC_ACM_CLASS
- 检查 chosen 是否绑定到 cdc_acm0

2. 编译时报 DISK_DRIVER_FLASH 依赖不满足：
- 检查是否真正定义了 compatible = "zephyr,flash-disk" 的节点

3. hosts.txt 改了但设备没反应：
- 在电脑侧“安全弹出”后再观察
- 确认轮询周期已到（默认约 3 秒）

### 13.7 业务侧最小调用

业务模块只需要调用：

- get_serv_addr()

它返回“当前可用的激活地址”，可直接用于后续 TCP 建连。
