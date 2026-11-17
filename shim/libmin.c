/*
 * The few libc routines clang emits calls to in a freestanding build
 * (GCC inlines them for the original Linux build). Compiled with
 * -fno-builtin so these loops are not turned back into calls to themselves.
 * Byte-wise: the MMU may be off, so every access must stay aligned.
 */
#include <stddef.h>

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

void *memset(void *d, int c, size_t n)
{
	volatile unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	volatile unsigned char *p = d;
	const volatile unsigned char *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}
