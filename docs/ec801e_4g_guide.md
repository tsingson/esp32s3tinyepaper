# EC801E 4G 驱动与使用指南（Zephyr 4.4.1）

## 1. 文档目标

本文档用于说明本工程中 EC801E-CN 在 Zephyr 4.4.1 下的：

- 配置方法
- 使用方法
- 已知限制
- 调试注意事项

适用工程：

- 板卡：`esp32s3_devkitc/esp32s3/procpu`
- 入口：`src/main_ec801e.c`
- 驱动：`drivers/ec801e/`

## 2. 目录结构

与 EC801E 相关的关键文件如下：

- `drivers/ec801e/ec801e.c`：AT 命令、附网、PDP、TCP/SSL 打开与收发
- `drivers/ec801e/ec801e_socket_offload.c`：Zephyr socket offload 适配层
- `drivers/ec801e/Kconfig`：驱动配置项（如 APN）
- `drivers/ec801e/CMakeLists.txt`：驱动源码接入 app 目标
- `dts/bindings/modem/quectel,ec801e.yaml`：DTS binding
- `boards/esp32s3_devkitc_esp32s3_procpu.overlay`：串口、引脚、modem 节点
- `prj.conf`：网络与 offload 功能开关
- `src/main_ec801e.c`：bitproto TCP roundtrip 示例

## 3. 硬件连接与 DTS 配置

### 3.1 实际连线

当前工程约定：

- EC801E TX -> ESP32-S3 RX (`GPIO7`)
- EC801E RX -> ESP32-S3 TX (`GPIO8`)
- EC801E EN -> `GPIO9`
- EC801E 与 ESP32-S3 必须共地

### 3.2 Overlay 配置

在 `boards/esp32s3_devkitc_esp32s3_procpu.overlay`：

- `&uart1`：配置 `current-speed = <115200>`
- `uart1_modem_default`：配置 `UART1_TX_GPIO8` / `UART1_RX_GPIO7`
- `ec801e_mdm`：
  - `compatible = "quectel,ec801e"`
  - `mdm-uart = <&uart1>`
  - `mdm-power-gpios = <&gpio0 9 GPIO_ACTIVE_LOW>`

## 4. Kconfig 与 prj.conf 配置

### 4.1 驱动配置项

在 `drivers/ec801e/Kconfig`：

- `CONFIG_EC801E`：使能驱动
- `CONFIG_EC801E_APN`：默认 APN（例如 `CMNET`）

### 4.2 网络配置（本工程建议）

在 `prj.conf`：

- `CONFIG_NETWORKING=y`
- `CONFIG_NET_NATIVE=n`
- `CONFIG_NET_OFFLOAD=y`
- `CONFIG_NET_SOCKETS=y`
- `CONFIG_NET_SOCKETS_OFFLOAD=y`
- `CONFIG_NET_IPV4=y`
- `CONFIG_NET_TCP=y`
- `CONFIG_EC801E=y`
- `CONFIG_EC801E_APN="CMNET"`

说明：

- 当前工程为单一 offload provider 方案，`CONFIG_NET_NATIVE=n`。
- 若后续需要混合 native/offload，再评估开启 dispatcher。

## 5. CMake 常规接入方式

本工程采用“顶层最小化 + drivers 子目录完整声明”的常规方式：

1. 顶层 `CMakeLists.txt`：
- `add_subdirectory(drivers)`
- 仅保留业务入口和协议源码（如 `src/main_ec801e.c`、bitproto 文件）

2. `drivers/CMakeLists.txt`：
- `add_subdirectory_ifdef(CONFIG_EC801E ec801e)`

3. `drivers/ec801e/CMakeLists.txt`：
- 在该目录中通过 `target_sources(app PRIVATE ...)` 注入 `ec801e.c` 与 `ec801e_socket_offload.c`
- 通过 `target_include_directories(app PRIVATE ...)` 注入驱动头文件路径

不建议在顶层手工写 `drivers/ec801e/*.c`，否则可维护性差，且容易和 Kconfig 条件脱节。

## 6. 初始化与连接流程

### 6.1 驱动初始化流程（简化）

`ec801e_socket_stack_init()` 主要步骤：

1. UART 自检（AT 回环）
2. AT 基础握手（`AT` / `ATE0` / `CPIN`）
3. 附网流程（`CFUN`、`CEREG` 轮询）
4. PDP 配置与激活（`CGDCONT`、`QICSGP`、`QIACT`）
5. 读取本地 IP（`CGPADDR`）

### 6.2 建连流程（TCP）

`zsock_connect()` -> offload -> `AT+QIOPEN=...`：

- 先 `QICLOSE=0` 清理旧连接
- `QIOPEN=1,0,"TCP",...`
- 等待 `+QIOPEN: 0,0`

## 7. 应用层使用方法

应用通过标准 socket API 使用，无需业务层 AT 处理：

- `zsock_socket()`
- `zsock_connect()`
- `zsock_send()`
- `zsock_recv()`
- `zsock_close()`

`src/main_ec801e.c` 当前示例是 bitproto frame 方式：

- 先发 4 字节 header
- 再发 65 字节 payload
- 接收端按相同 framing 回读

## 8. 已知限制

当前实现的限制如下：

1. 单连接上下文
- `ec801e_socket_offload.c` 内使用单个全局 socket 上下文（`g_ctx`）
- 同时只支持一个活跃 TCP 连接

2. IPv4 only
- 当前仅支持 `AF_INET` / `NET_AF_INET`
- 不支持 IPv6

3. TCP 优先
- 主要验证路径为 TCP
- SSL/TLS 路径有基础实现，但证书校验策略较弱（更偏联调）

4. 轮询式接收
- `QIRD` 轮询读取，依赖超时窗口与 EAGAIN 重试

5. 功能覆盖范围
- 重点覆盖 client 主动建连与收发
- 未覆盖 server/listen/accept

## 9. 调试注意事项（重点）

### 9.1 先看是否进入 offload

若日志没有出现：

- `EC801E offload socket_create ...`
- `EC801E offload connect ...`

说明 socket 尚未进入驱动层，先检查：

- 驱动源码是否被正确接入（CMake）
- `CONFIG_NET_SOCKETS_OFFLOAD` 是否开启
- `CONFIG_EC801E` 是否开启

### 9.2 常见错误码对照（本工程）

- `-106`：`EAFNOSUPPORT`（地址族不支持）
  - 常见于 offload 未正确路由/未链接进最终镜像

- `-23`：`ENFILE`（系统文件描述符资源问题）
  - 需检查 socket 创建与关闭是否成对、上下文是否泄漏

- `-116`：`ETIMEDOUT`（连接/等待超时）
  - 常见于附网/PDP未就绪或网络质量差

### 9.3 关键 AT 日志判定

1. 模组可交互：
- `AT` -> `OK`
- `+CPIN: READY`

2. 附网成功：
- `+CEREG: ...,1` 或 `...,5`

3. PDP 成功：
- `AT+QIACT=1` -> `OK`
- `AT+CGPADDR=1` 返回 IP

4. 建连成功：
- `+QIOPEN: 0,0`

5. 发送成功：
- `SEND OK`

6. 接收行为：
- `+QIRD: 0` 表示当前无数据（可继续轮询）

### 9.4 串口与电源注意事项

- 串口波特率建议先固定 115200
- TX/RX 交叉连接必须正确
- EN 脚电平极性需与硬件一致
- 供电电流不足会导致随机重启、URC异常

## 10. 典型构建与烧录

在工程根目录执行：

```bash
/Users/qinshen/go/zephyrproject/.venv/bin/west build -p always -b esp32s3_devkitc/esp32s3/procpu
/Users/qinshen/go/zephyrproject/.venv/bin/west flash --erase
/Users/qinshen/go/zephyrproject/.venv/bin/west espressif monitor
```

## 11. 运行成功参考日志

成功链路通常包含：

- `EC801E offload socket_create ...`
- `EC801E offload connect ...`
- `+QIOPEN: 0,0`
- `round 0 ok`
- `round 1 ok`
- `round 2 ok`
- `GPSEC tcpip example done rounds=3 attempt=1`

## 12. 后续建议

1. 引入可配置日志级别
- 区分 INFO/DEBUG，减少生产日志噪声

2. 扩展并发连接能力
- 将单全局 socket 上下文改为多上下文数组

3. 完善 TLS 安全能力
- 增加证书配置与严格校验

4. 引入长稳压测模式
- 支持 100/1000 轮持续压测，记录失败率与恢复时延
