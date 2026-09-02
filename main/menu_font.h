#pragma once

#include <stdint.h>

/* ASCII 返回 8x16 行优先点阵（每行 1 字节，高位在左）。 */
const uint8_t *menu_font_ascii_glyph(uint32_t codepoint);

/* 中文返回 16x16 行优先点阵（每行 2 字节，高位在左）；没有时返回 NULL。 */
const uint8_t *menu_font_glyph(uint32_t codepoint);
