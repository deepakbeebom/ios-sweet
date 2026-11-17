#pragma once
#include <stdint.h>

#define FB_BASE		0xb8000000UL	/* g0q: reserved-memory cont_splash_region (dtbo board-id 12) */
#define FB_W		1080
#define FB_H		2340	/* g0q: S6E3FAB AMB667XU01, 1080x2340 */
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_set(uintptr_t base, uint32_t stride_bytes);
void con_row(int r);
void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
