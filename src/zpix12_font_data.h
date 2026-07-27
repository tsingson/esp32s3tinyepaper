#ifndef ZPIX12_FONT_DATA_H
#define ZPIX12_FONT_DATA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t codepoint;
	uint16_t rows[12];
} zpix12_glyph_t;

extern const zpix12_glyph_t zpix12_font_glyphs[];
extern const size_t zpix12_font_glyphs_count;

#endif /* ZPIX12_FONT_DATA_H */
