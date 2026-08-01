#include "epd200x200.h"

#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "zpix12_font_data.h"

#define EPD_WIDTH 200
#define EPD_HEIGHT 200
#define EPD_FB_SIZE ((EPD_WIDTH * EPD_HEIGHT) / 8)
#define EPD_CHINESE_LINE_SPACING_DEFAULT 3
#define EPD_CHINESE_COLUMN_SPACING_DEFAULT 0

#define EPD_DC_PIN 3
#define EPD_CS_PIN 4
#define EPD_BUSY_PIN 5
#define EPD_RESET_PIN 6

static const struct device* const spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi3));
static const struct device* const gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

static const struct spi_config epd_spi_cfg = {
    .frequency = 2 * 1000 * 1000,
    .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
    .slave = 0,
};

static uint8_t epd_framebuf[EPD_FB_SIZE];
static int epd_chinese_line_spacing = EPD_CHINESE_LINE_SPACING_DEFAULT;
static int epd_chinese_column_spacing = EPD_CHINESE_COLUMN_SPACING_DEFAULT;

static void epd_fb_clear(bool black)
{
    memset(epd_framebuf, black ? 0x00 : 0xFF, sizeof(epd_framebuf));
}

static void epd_fb_set_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT)
    {
        return;
    }

    int pixel_index = y * EPD_WIDTH + x;
    int byte_index = pixel_index / 8;
    uint8_t bit = BIT(7 - (pixel_index % 8));

    if (black)
    {
        epd_framebuf[byte_index] &= ~bit;
    }
    else
    {
        epd_framebuf[byte_index] |= bit;
    }
}

static void epd_fb_draw_hline(int x0, int x1, int y, bool black)
{
    if (y < 0 || y >= EPD_HEIGHT)
    {
        return;
    }
    if (x0 > x1)
    {
        int tmp = x0;
        x0 = x1;
        x1 = tmp;
    }
    if (x0 < 0)
    {
        x0 = 0;
    }
    if (x1 >= EPD_WIDTH)
    {
        x1 = EPD_WIDTH - 1;
    }

    for (int x = x0; x <= x1; x++)
    {
        epd_fb_set_pixel(x, y, black);
    }
}

static void epd_fb_draw_vline(int x, int y0, int y1, bool black)
{
    if (x < 0 || x >= EPD_WIDTH)
    {
        return;
    }
    if (y0 > y1)
    {
        int tmp = y0;
        y0 = y1;
        y1 = tmp;
    }
    if (y0 < 0)
    {
        y0 = 0;
    }
    if (y1 >= EPD_HEIGHT)
    {
        y1 = EPD_HEIGHT - 1;
    }

    for (int y = y0; y <= y1; y++)
    {
        epd_fb_set_pixel(x, y, black);
    }
}

static void epd_fb_draw_rect(int x, int y, int w, int h, bool black)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }
    epd_fb_draw_hline(x, x + w - 1, y, black);
    epd_fb_draw_hline(x, x + w - 1, y + h - 1, black);
    epd_fb_draw_vline(x, y, y + h - 1, black);
    epd_fb_draw_vline(x + w - 1, y, y + h - 1, black);
}

static void epd_fb_fill_rect(int x, int y, int w, int h, bool black)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }
    for (int yy = y; yy < y + h; yy++)
    {
        epd_fb_draw_hline(x, x + w - 1, yy, black);
    }
}

static const uint8_t font5x7_E[7] = {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x1F};
static const uint8_t font5x7_P[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
static const uint8_t font5x7_S[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
static const uint8_t font5x7_2[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
static const uint8_t font5x7_3[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
static const uint8_t font5x7_dash[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
static const uint8_t bayer8x8[8][8] = {
    {0, 48, 12, 60, 3, 51, 15, 63},
    {32, 16, 44, 28, 35, 19, 47, 31},
    {8, 56, 4, 52, 11, 59, 7, 55},
    {40, 24, 36, 20, 43, 27, 39, 23},
    {2, 50, 14, 62, 1, 49, 13, 61},
    {34, 18, 46, 30, 33, 17, 45, 29},
    {10, 58, 6, 54, 9, 57, 5, 53},
    {42, 26, 38, 22, 41, 25, 37, 21},
};

static const uint8_t* epd_get_font5x7(char c)
{
    switch (c)
    {
    case 'E':
        return font5x7_E;
    case 'P':
        return font5x7_P;
    case 'S':
        return font5x7_S;
    case '2':
        return font5x7_2;
    case '3':
        return font5x7_3;
    case '-':
        return font5x7_dash;
    default:
        return NULL;
    }
}

static void epd_fb_draw_char5x7(int x, int y, char c, bool black)
{
    const uint8_t* glyph = epd_get_font5x7(c);

    if (glyph == NULL)
    {
        return;
    }

    for (int row = 0; row < 7; row++)
    {
        for (int col = 0; col < 5; col++)
        {
            if ((glyph[row] >> (4 - col)) & 0x1)
            {
                epd_fb_set_pixel(x + col, y + row, black);
            }
        }
    }
}

static void epd_fb_draw_text5x7(int x, int y, const char* text, bool black)
{
    for (size_t i = 0; text[i] != '\0'; i++)
    {
        epd_fb_draw_char5x7(x + (int)i * 6, y, text[i], black);
    }
}

/* Extracted from fonts/zpix.bdf (SIZE 12), glyphs U+4E2D and U+6587, BBX 11x11 */
static const zpix12_glyph_t* zpix12_find_glyph(uint32_t codepoint)
{
    size_t left = 0;
    size_t right = zpix12_font_glyphs_count;

    while (left < right)
    {
        size_t mid = left + (right - left) / 2;
        uint32_t cp = zpix12_font_glyphs[mid].codepoint;

        if (cp == codepoint)
        {
            return &zpix12_font_glyphs[mid];
        }
        if (cp < codepoint)
        {
            left = mid + 1;
        }
        else
        {
            right = mid;
        }
    }

    return NULL;
}

static uint32_t utf8_decode_one(const char** s)
{
    const uint8_t* p = (const uint8_t*)(*s);

    if (p[0] == '\0')
    {
        return 0;
    }

    if ((p[0] & 0x80) == 0)
    {
        *s += 1;
        return p[0];
    }

    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
    {
        uint32_t cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        *s += 2;
        return cp;
    }

    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) |
            ((uint32_t)(p[1] & 0x3F) << 6) |
            (uint32_t)(p[2] & 0x3F);
        *s += 3;
        return cp;
    }

    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
    {
        uint32_t cp = ((uint32_t)(p[0] & 0x07) << 18) |
            ((uint32_t)(p[1] & 0x3F) << 12) |
            ((uint32_t)(p[2] & 0x3F) << 6) |
            (uint32_t)(p[3] & 0x3F);
        *s += 4;
        return cp;
    }

    /* Invalid UTF-8 byte sequence; skip one byte and render fallback. */
    *s += 1;
    return '?';
}

static void epd_fb_draw_glyph12(int x, int y, const zpix12_glyph_t* glyph, bool black)
{
    if (glyph == NULL)
    {
        return;
    }

    for (int row = 0; row < 12; row++)
    {
        uint16_t bits = glyph->rows[row];
        for (int col = 0; col < 12; col++)
        {
            if ((bits & BIT(11 - col)) != 0U)
            {
                epd_fb_set_pixel(x + col, y + row, black);
            }
        }
    }
}

static void epd_fb_draw_utf8_text12(int x, int y, const char* text, int max_cols, int max_rows,
                                    int line_spacing, bool black)
{
    int col = 0;
    int row = 0;
    int line_pitch = 12 + line_spacing;
    int col_pitch = 12 + epd_chinese_column_spacing;
    const char* p = text;

    if (line_spacing < 0)
    {
        line_pitch = 12;
    }

    if (epd_chinese_column_spacing < 0)
    {
        col_pitch = 12;
    }

    while (*p != '\0' && row < max_rows)
    {
        if (*p == '\n')
        {
            p++;
            col = 0;
            row++;
            continue;
        }

        if (col >= max_cols)
        {
            col = 0;
            row++;
            if (row >= max_rows)
            {
                break;
            }
        }

        uint32_t cp = utf8_decode_one(&p);
        const zpix12_glyph_t* glyph = zpix12_find_glyph(cp);
        if (glyph == NULL)
        {
            glyph = zpix12_find_glyph('?');
        }

        epd_fb_draw_glyph12(x + col * col_pitch, y + row * line_pitch, glyph, black);
        col++;
    }
}

void epd200x200_set_chinese_line_spacing(int spacing)
{
    if (spacing < 0)
    {
        spacing = 0;
    }

    epd_chinese_line_spacing = spacing;
}

void epd200x200_set_chinese_column_spacing(int spacing)
{
    if (spacing < 0)
    {
        spacing = 0;
    }

    epd_chinese_column_spacing = spacing;
}

static int epd_write_bytes(bool is_data, const uint8_t* data, size_t len)
{
    int ret;
    struct spi_buf tx_buf = {
        .buf = (uint8_t*)data,
        .len = len,
    };
    struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };

    ret = gpio_pin_set(gpio_dev, EPD_DC_PIN, is_data ? 1 : 0);
    if (ret < 0)
    {
        return ret;
    }

    ret = gpio_pin_set(gpio_dev, EPD_CS_PIN, 0);
    if (ret < 0)
    {
        return ret;
    }

    ret = spi_write(spi_dev, &epd_spi_cfg, &tx);
    (void)gpio_pin_set(gpio_dev, EPD_CS_PIN, 1);
    return ret;
}

static int epd_write_cmd(uint8_t cmd)
{
    return epd_write_bytes(false, &cmd, 1);
}

static int epd_write_data(const uint8_t* data, size_t len)
{
    return epd_write_bytes(true, data, len);
}

static int epd_write_data_byte(uint8_t data)
{
    return epd_write_data(&data, 1);
}

static int epd_wait_idle(int timeout_ms)
{
    int64_t deadline = k_uptime_get() + timeout_ms;

    while (k_uptime_get() < deadline)
    {
        int busy = gpio_pin_get(gpio_dev, EPD_BUSY_PIN);
        if (busy < 0)
        {
            return busy;
        }
        if (busy == 0)
        {
            return 0;
        }
        k_sleep(K_MSEC(10));
    }

    return -ETIMEDOUT;
}

static int epd_reset_sequence(void)
{
    int ret = gpio_pin_set(gpio_dev, EPD_RESET_PIN, 0);
    if (ret < 0)
    {
        return ret;
    }
    k_sleep(K_MSEC(10));

    ret = gpio_pin_set(gpio_dev, EPD_RESET_PIN, 1);
    if (ret < 0)
    {
        return ret;
    }
    k_sleep(K_MSEC(10));
    return 0;
}

static int epd_set_ram_full_window(void)
{
    int ret;

    ret = epd_write_cmd(0x44);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x00);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte((EPD_WIDTH / 8) - 1);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x45);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x00);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x00);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte((EPD_HEIGHT - 1) & 0xFF);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    return ret;
}

static int epd_set_ram_cursor_origin(void)
{
    int ret;

    ret = epd_write_cmd(0x4E);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x00);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x4F);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x00);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x00);
    return ret;
}

static int epd_update_display(void)
{
    int ret;

    ret = epd_write_cmd(0x22);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0xF7);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x20);
    if (ret < 0)
    {
        return ret;
    }

    return epd_wait_idle(5000);
}

static int epd_present_framebuf(void)
{
    int ret;

    ret = epd_set_ram_full_window();
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_set_ram_cursor_origin();
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x24);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_data(epd_framebuf, sizeof(epd_framebuf));
    if (ret < 0)
    {
        return ret;
    }

    return epd_update_display();
}

int epd200x200_init(void)
{
    int ret;
    uint8_t gate_height_l = (EPD_HEIGHT - 1) & 0xFF;
    uint8_t gate_height_h = ((EPD_HEIGHT - 1) >> 8) & 0xFF;

    if (!device_is_ready(spi_dev) || !device_is_ready(gpio_dev))
    {
        printk("EPD: spi/gpio device not ready\n");
        return -ENODEV;
    }

    ret = gpio_pin_configure(gpio_dev, EPD_DC_PIN, GPIO_OUTPUT_INACTIVE);
    if (ret < 0)
    {
        return ret;
    }
    ret = gpio_pin_configure(gpio_dev, EPD_CS_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        return ret;
    }
    ret = gpio_pin_configure(gpio_dev, EPD_RESET_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        return ret;
    }
    ret = gpio_pin_configure(gpio_dev, EPD_BUSY_PIN, GPIO_INPUT);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_reset_sequence();
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x12);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_wait_idle(5000);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x01);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(gate_height_l);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(gate_height_h);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x00);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x11);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x03);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_set_ram_full_window();
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_set_ram_cursor_origin();
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x3C);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x05);
    if (ret < 0)
    {
        return ret;
    }

    ret = epd_write_cmd(0x18);
    if (ret < 0)
    {
        return ret;
    }
    ret = epd_write_data_byte(0x80);
    if (ret < 0)
    {
        return ret;
    }

    printk("EPD: init ok\n");
    return 0;
}

int epd200x200_clear(bool black)
{
    int ret;

    epd_fb_clear(black);

    ret = epd_present_framebuf();
    if (ret < 0)
    {
        return ret;
    }

    printk("EPD: clear %s done\n", black ? "black" : "white");
    return 0;
}

int epd200x200_show_demo(void)
{
    int ret;

    epd_fb_clear(false);

    epd_fb_draw_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, true);
    epd_fb_draw_rect(8, 8, EPD_WIDTH - 16, EPD_HEIGHT - 16, true);

    for (int i = 20; i < 180; i += 12)
    {
        epd_fb_set_pixel(i, i, true);
        epd_fb_set_pixel(199 - i, i, true);
    }

    epd_fb_fill_rect(20, 30, 36, 36, true);
    epd_fb_fill_rect(66, 30, 36, 36, false);
    epd_fb_draw_rect(66, 30, 36, 36, true);
    epd_fb_fill_rect(112, 30, 36, 36, true);
    epd_fb_fill_rect(158, 30, 22, 36, false);
    epd_fb_draw_rect(158, 30, 22, 36, true);

    epd_fb_draw_text5x7(70, 95, "ESP32-S3", true);
    epd_fb_draw_text5x7(52, 112, "EPD 200X200", true);

    epd_fb_draw_hline(20, 180, 135, true);
    for (int x = 20; x <= 180; x += 8)
    {
        epd_fb_draw_vline(x, 140, 170, true);
    }

    ret = epd_present_framebuf();
    if (ret < 0)
    {
        return ret;
    }

    printk("EPD: demo drawn\n");
    return 0;
}

int epd200x200_show_grayscale_transition(void)
{
    const int bands = 8;
    const int band_width = EPD_WIDTH / bands;
    int ret;

    epd_fb_clear(false);

    for (int y = 0; y < EPD_HEIGHT; y++)
    {
        for (int x = 0; x < EPD_WIDTH; x++)
        {
            int band = x / band_width;
            if (band >= bands)
            {
                band = bands - 1;
            }

            /* 0 -> white, 255 -> black */
            int black_level = (band * 255) / (bands - 1);
            int threshold = (bayer8x8[y & 0x7][x & 0x7] * 256) / 64;
            bool is_black = black_level > threshold;

            epd_fb_set_pixel(x, y, is_black);
        }
    }

    /* Add visual separators between gray levels */
    for (int i = 1; i < bands; i++)
    {
        int x = i * band_width;
        epd_fb_draw_vline(x, 0, EPD_HEIGHT - 1, true);
    }

    ret = epd_present_framebuf();
    if (ret < 0)
    {
        return ret;
    }

    printk("EPD: grayscale transition drawn\n");
    return 0;
}

int epd200x200_show_chinese_demo(void)
{
    int ret;
    static const char* const pages[] = {
        "原始:3.0M 396992行\n子集:1.0M 133584行\n体积约减少66%",
        "共保留7515字形\n缺失25字形\n可在extra文件补充",
        "zpix字库最小化完成保留:英文1 符号和常用简体中文2  zpix字库最小化完成3 保留:英文/符号\n和常用简体中文\nzpix字库最小化完成\n保留:英文/符号\n和常用简体中文",

    };
    const int page_count = sizeof(pages) / sizeof(pages[0]);

    for (int i = 0; i < page_count; i++)
    {
        epd_fb_clear(false);
        epd_fb_draw_utf8_text12(4, 30, pages[i], 16, 12, epd_chinese_line_spacing, true);

        ret = epd_present_framebuf();
        if (ret < 0)
        {
            return ret;
        }

        k_sleep(K_SECONDS(3));
    }

    printk("EPD: paged Chinese summary drawn\n");
    return 0;
}
