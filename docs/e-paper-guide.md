# E-Paper 中文显示实现指南

本文档说明本工程在 Zephyr 4.4.1 下，如何在 200x200 墨水屏实现中文显示，并支持分页自动翻页。

## 1. 实现目标

- 在 ESP32-S3 + e-paper(200x200) 上显示中文。
- 字体来源于 zpix BDF 字库。
- 将字库裁剪到最小常用集合，降低固件体积。
- 将 BDF 转成 C 数组，避免运行时解析 BDF。
- 支持 UTF-8 字符串绘制与分页显示（每页 3 秒）。

## 2. 关键文件

- 显示驱动主实现：src/epd200x200.c
- 主入口（调用中文演示）：src/main_chip_info.c
- 字库裁剪脚本：tools/zpix_subset.py
- BDF 转 C 数组脚本：tools/zpix_bdf_to_c.py
- 生成的字体头文件：src/zpix12_font_data.h
- 生成的字体数据：src/zpix12_font_data.c
- 原始字库：fonts/zpix.bdf
- 裁剪后字库：fonts/zpix_min_zh.bdf

## 3. 字库处理流程

### 3.1 从完整 zpix 裁剪最小字符集

执行：

python3 tools/zpix_subset.py

默认保留：

- ASCII 可打印字符（英文、数字、常见符号）
- GB2312 符号区（A1-A9）
- GB2312 一级汉字（B0-F7，常用简体）

输出：

- fonts/zpix_min_zh.bdf
- fonts/zpix_min_zh_codepoints.txt
- fonts/zpix_min_zh_missing.txt

可选：追加业务字符

python3 tools/zpix_subset.py --extra fonts/zpix_extra_chars.txt

说明：

- extra 文件支持两种格式：
  - 直接写中文字符（每行一个或多个都可）
  - 写 U+XXXX 码点

### 3.2 把 BDF 转成固件可编译的 C 数组

执行：

python3 tools/zpix_bdf_to_c.py

输出：

- src/zpix12_font_data.h
- src/zpix12_font_data.c

数据结构：

- 每个字形为 12x12 点阵
- 每行 12 bit，存入 uint16_t
- 每个字形包含：
  - codepoint（Unicode 码点）
  - rows[12]（12 行位图）

## 4. e-paper 中文渲染实现

### 4.1 帧缓冲与基础绘制

在 src/epd200x200.c 中：

- epd_framebuf：1bpp 帧缓冲（200x200/8）
- epd_fb_set_pixel：设置单像素黑白
- epd_fb_draw_hline / epd_fb_draw_vline / epd_fb_fill_rect：基础图元

这些函数负责把内容写入 RAM 缓冲区，最后一次性刷到屏幕。

### 4.2 UTF-8 解码

函数：utf8_decode_one

作用：

- 从 UTF-8 字符串读取一个 Unicode 码点。
- 支持 1/2/3/4 字节序列。
- 遇到非法序列时，回退到 '?'。

### 4.3 字形查找

函数：zpix12_find_glyph

作用：

- 在 zpix12_font_glyphs 数组中进行二分查找。
- 通过 codepoint 找到对应 12x12 字形。

要求：

- zpix12_font_data.c 中字形按 codepoint 升序存放（脚本已保证）。

### 4.4 字形与文本绘制

函数：

- epd_fb_draw_glyph12：绘制单个 12x12 字形
- epd_fb_draw_utf8_text12：绘制 UTF-8 多行文本
- epd200x200_set_chinese_line_spacing：设置行间距（像素）
- epd200x200_set_chinese_column_spacing：设置列间距/字间距（像素）

行为：

- 支持 '\n' 换行。
- 通过 max_cols/max_rows 限制绘制区域。
- 字形缺失时使用 '?' 作为回退。
- 行间距默认 3 像素（字高 12，所以行步进为 12 + 行间距）。
- 列间距默认 0 像素（字宽 12，所以列步进为 12 + 列间距）。
- 间距参数小于 0 时会被钳制为 0。

配置示例（在调用中文演示前设置）：

epd200x200_set_chinese_line_spacing(3);
epd200x200_set_chinese_column_spacing(1);
epd200x200_show_chinese_demo();

## 5. 分页显示（每页 3 秒）

函数：epd200x200_show_chinese_demo

实现方式：

- pages[] 中定义多页 UTF-8 中文字符串。
- 循环每页：
  1) 清屏
  2) 绘制文本
  3) 刷新到 e-paper
  4) k_sleep(K_SECONDS(3)) 等待 3 秒

说明：

- e-paper 刷新本身是阻塞操作，函数内部已等待 BUSY 引脚释放。
- 分页更适合展示较长中文说明，避免单页过密。

## 6. 构建与烧录

工程 CMake 已包含 src/zpix12_font_data.c。

构建：

/Users/qinshen/go/zephyrproject/.venv/bin/west build -p always -b esp32s3_devkitc/esp32s3/procpu

烧录：

/Users/qinshen/go/zephyrproject/.venv/bin/west flash --erase

串口监视：

/Users/qinshen/go/zephyrproject/.venv/bin/west espressif monitor

## 7. 后续维护建议

### 7.1 新增显示文本

- 直接修改 src/epd200x200.c 中 pages[] 内容。
- 若出现乱码或缺字，优先检查该字是否在子集字库中。
- 如需调整排版密度，可在调用演示函数前设置行间距和列间距。

### 7.2 新增缺失汉字

1) 在 fonts/zpix_extra_chars.txt 增加字符
2) 重新生成子集：

python3 tools/zpix_subset.py --extra fonts/zpix_extra_chars.txt

3) 重新生成 C 数组：

python3 tools/zpix_bdf_to_c.py

4) 重新编译烧录

### 7.3 体积优化

当前方案把整个最小集合都编入固件，换来最稳定的运行时体验。

如果要进一步缩小镜像：

- 可做“按页面文本提取字符”的二次裁剪，只保留实际用到的字。
- 或按业务拆分多个字库分支（调试版/量产版）。

## 8. 常见问题

### Q1: 显示为 '?' 或空白

- 该字符可能不在 zpix_min_zh.bdf。
- 查看 fonts/zpix_min_zh_missing.txt 是否包含该码点。
- 通过 --extra 补字后重新生成。

### Q2: 字符位置不整齐

- 本工程统一使用 12x12 字格，渲染时按固定栅格布局。
- 可通过 epd200x200_set_chinese_line_spacing 和 epd200x200_set_chinese_column_spacing 调整疏密。

### Q3: 翻页太快/太慢

- 修改 epd200x200_show_chinese_demo 中 k_sleep(K_SECONDS(3)) 的参数。

---

维护提示：

每次改动字体或页面文本后，建议执行完整构建并实机检查一次，重点关注：

- 字形完整性（是否缺字）
- 行列对齐
- 刷新耗时与视觉闪烁
