#ifndef CHIPINFO_EPD200X200_H
#define CHIPINFO_EPD200X200_H

#include <stdbool.h>

int epd200x200_init(void);
int epd200x200_clear(bool black);
int epd200x200_show_demo(void);
int epd200x200_show_grayscale_transition(void);
void epd200x200_set_chinese_line_spacing(int spacing);
void epd200x200_set_chinese_column_spacing(int spacing);
int epd200x200_show_chinese_demo(void);

#endif
