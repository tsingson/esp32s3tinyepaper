# e-paper display in esp32 s3 tiny board

zephyr 4.4.1

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