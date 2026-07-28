# e-paper display in esp32 s3 tiny board

zephyr 4.4.1

## EC801E 4G 使用指南

详见：

- [docs/ec801e_4g_guide.md](docs/ec801e_4g_guide.md)

## zpix 字库最小化（英文+符号+常用简体）

工程内提供了子集脚本，可把 [fonts/zpix.bdf](fonts/zpix.bdf) 裁剪为更小的集合，便于后续在 Zephyr 固件里使用。

默认保留：

- ASCII 可打印字符（英文+数字+常见符号）
- GB2312 符号区（A1-A9）
- GB2312 一级汉字（B0-F7，常用简体字）

运行：

```bash
python3 tools/zpix_subset.py
```

输出：

- [fonts/zpix_min_zh.bdf](fonts/zpix_min_zh.bdf)
- [fonts/zpix_min_zh_codepoints.txt](fonts/zpix_min_zh_codepoints.txt)
- [fonts/zpix_min_zh_missing.txt](fonts/zpix_min_zh_missing.txt)

可选追加字符（每行可写中文，或 U+XXXX）：

```bash
python3 tools/zpix_subset.py --extra fonts/zpix_extra_chars.txt
```

## 转 C 数组（用于固件内置显示）

将最小化 BDF 转为 Zephyr 可直接编译的 C 数组：

```bash
python3 tools/zpix_bdf_to_c.py
```

输出：

- [src/zpix12_font_data.h](src/zpix12_font_data.h)
- [src/zpix12_font_data.c](src/zpix12_font_data.c)

当前演示已使用该数组在 e-paper 分页显示中文总结（每页 3 秒）。

## Bitproto TCP Demo

工程内已集成一个基于 EC801E 的 bitproto TCP roundtrip 示例：

- ESP32 客户端入口： [src/main_ec801e.c](src/main_ec801e.c)
- Go 服务端入口： [cmd/gpsce/main.go](cmd/gpsce/main.go)

协议来源：

- `bitproto/tests/test_encoding/encoding-cases/tcpip`

当前默认行为：

- 直连服务端 `142.54.180.58:8061`
- `FrameHeader` 4 字节，`Drone` payload 65 字节
- ESP32 端默认连续验证 3 轮 roundtrip
- ESP32 端单次会话失败后最多重试 3 次
- Go 服务端持续监听，可反复接受新连接用于烧录调试
- Go 服务端对 accept/read/write 都加了超时与有限重试

运行 Go 服务端：

```bash
go run ./cmd/gpsce -listen :8061 -rounds 3 -io-timeout 15s
```

期望的 ESP32 成功日志关键字：

- `round 0 ok`
- `round 1 ok`
- `round 2 ok`
- `GPSEC tcpip example done rounds=3 attempt=1`

期望的 Go 服务端日志关键字：

- `accept remote=...`
- `rx header ... len=65`
- `tx payload ...`
- `frame done ...`

稳定性说明：

- EC801E 驱动已针对附网、PDP、`QIOPEN` 异步 URC、`QIRD: 0` 无数据轮询等行为做兼容处理。
- 若现场网络抖动，ESP32 端会自动进行 3 次会话级重试；Go 服务端保持持续监听，不需要每次重启。