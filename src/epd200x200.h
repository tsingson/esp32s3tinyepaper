#ifndef CHIPINFO_EPD200X200_H
#define CHIPINFO_EPD200X200_H

#include <stdbool.h>

int epd200x200_init(void);
int epd200x200_clear(bool black);
int epd200x200_show_demo(void);
int epd200x200_show_grayscale_transition(void);

#endif
