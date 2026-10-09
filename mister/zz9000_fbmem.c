/*
 * Framebuffer-safe memset/memcpy/memmove - see zz9000_fbmem.h.
 *
 * Misaligned copies use the classic shift-merge: aligned word loads from
 * the source, two of them combined per aligned destination word. A load
 * may read up to 3 bytes outside the requested range, but never outside
 * the aligned word that holds a requested byte, so it cannot cross a page
 * or the end of the mapping. Little-endian only (ARM and the test host).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns") /* no memcpy() calls back into libc */
#pragma GCC optimize("no-store-merging")                 /* no merged, possibly unaligned stores */
#pragma GCC optimize("no-tree-vectorize")                /* no auto-NEON */

#include <stdint.h>
#include "zz9000_fbmem.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error zz9000_fbmem.c assumes a little-endian CPU
#endif

void *zz_fb_memset(void *dst, int c, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	uint8_t b = (uint8_t)c;

	while (n && ((uintptr_t)d & 3)) {
		*d++ = b;
		n--;
	}
	if (n >= 4) {
		uint32_t v = b * 0x01010101u;
		uint32_t *dw = (uint32_t *)d;
		while (n >= 32) {
			dw[0] = v; dw[1] = v; dw[2] = v; dw[3] = v;
			dw[4] = v; dw[5] = v; dw[6] = v; dw[7] = v;
			dw += 8;
			n -= 32;
		}
		while (n >= 4) {
			*dw++ = v;
			n -= 4;
		}
		d = (uint8_t *)dw;
	}
	while (n--)
		*d++ = b;
	return dst;
}

static void copy_fwd(uint8_t *d, const uint8_t *s, size_t n)
{
	while (n && ((uintptr_t)d & 3)) {
		*d++ = *s++;
		n--;
	}
	if (n >= 4) {
		uint32_t *dw = (uint32_t *)d;
		unsigned k = (uintptr_t)s & 3;

		if (!k) {
			const uint32_t *sw = (const uint32_t *)s;
			while (n >= 16) {
				uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
				dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
				dw += 4;
				sw += 4;
				n -= 16;
			}
			while (n >= 4) {
				*dw++ = *sw++;
				n -= 4;
			}
			s = (const uint8_t *)sw;
		} else {
			const uint32_t *sw = (const uint32_t *)(s - k);
			unsigned rs = 8 * k, ls = 32 - rs;
			uint32_t w0 = *sw++;
			while (n >= 4) {
				uint32_t w1 = *sw++;
				*dw++ = (w0 >> rs) | (w1 << ls);
				w0 = w1;
				s += 4;
				n -= 4;
			}
		}
		d = (uint8_t *)dw;
	}
	while (n--)
		*d++ = *s++;
}

static void copy_bwd(uint8_t *d, const uint8_t *s, size_t n)
{
	d += n;
	s += n;
	while (n && ((uintptr_t)d & 3)) {
		*--d = *--s;
		n--;
	}
	if (n >= 4) {
		uint32_t *dw = (uint32_t *)d;
		unsigned k = (uintptr_t)s & 3;

		if (!k) {
			const uint32_t *sw = (const uint32_t *)s;
			while (n >= 4) {
				*--dw = *--sw;
				n -= 4;
			}
			s = (const uint8_t *)sw;
		} else {
			const uint32_t *sw = (const uint32_t *)(s - k);
			unsigned rs = 8 * k, ls = 32 - rs;
			uint32_t hi = *sw;
			while (n >= 4) {
				uint32_t lo = *--sw;
				*--dw = (lo >> rs) | (hi << ls);
				hi = lo;
				s -= 4;
				n -= 4;
			}
		}
		d = (uint8_t *)dw;
	}
	while (n--)
		*--d = *--s;
}

void *zz_fb_memcpy(void *dst, const void *src, size_t n)
{
	copy_fwd((uint8_t *)dst, (const uint8_t *)src, n);
	return dst;
}

void *zz_fb_memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if (d == s || !n)
		return dst;
	if (d < s || d >= s + n)
		copy_fwd(d, s, n);
	else
		copy_bwd(d, s, n);
	return dst;
}
