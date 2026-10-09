/*
 * Framebuffer-safe memset/memcpy/memmove for the ZZ9000 blitter on MiSTer.
 *
 * The RTG framebuffer is DDR3 above the Linux RAM map, so the /dev/mem
 * mapping the MiSTer binary gets is Strongly-ordered. ARMv7 faults every
 * unaligned access to Strongly-ordered/Device memory (the kernel then
 * emulates it, very slowly), and glibc's string routines freely use
 * unaligned word accesses. These replacements only ever issue naturally
 * aligned byte, halfword and word accesses, and use full 32-bit words for
 * the bulk of every transfer regardless of the source/destination
 * misalignment.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9000_FBMEM_H
#define ZZ9000_FBMEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *zz_fb_memset(void *dst, int c, size_t n);
void *zz_fb_memcpy(void *dst, const void *src, size_t n);
void *zz_fb_memmove(void *dst, const void *src, size_t n);

#ifdef __cplusplus
}
#endif

#endif
