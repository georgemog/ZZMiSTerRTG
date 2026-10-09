/*
 * Build wrapper for the official ZZ9000OS blitter (zz9000os/gfx.c) and its
 * command dispatcher (zz9000os/dma_rtg.c) inside the MiSTer main binary.
 *
 * gfx.c is compiled unmodified; this translation unit only changes the
 * environment it is compiled in, because its destination memory is the
 * Strongly-ordered /dev/mem mapping of the RTG framebuffer:
 *  - memset/memcpy/memmove go to the aligned-access-only zz_fb_* versions;
 *  - NEON is switched off (the scalar paths only use naturally aligned
 *    accesses, and the kernel cannot emulate a faulting NEON access);
 *  - GCC may neither turn loops back into libc calls, merge stores into
 *    wider, possibly unaligned ones, nor auto-vectorise them into NEON.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#pragma GCC optimize("no-store-merging")
#pragma GCC optimize("no-tree-vectorize")    /* no auto-NEON on Strongly-ordered memory */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "zz9000_fbmem.h"

#undef __ARM_NEON__
#undef __ARM_NEON

#define memset  zz_fb_memset
#define memcpy  zz_fb_memcpy
#define memmove zz_fb_memmove

/* Upstream code, upstream style: keep Main_MiSTer's -Wextra quiet about
 * the intentional DECODE_PLANAR_PIXEL fall-throughs and unused arguments. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wtype-limits"
#include "zz9000os/gfx.c"
#pragma GCC diagnostic pop

#include "zz9000os/dma_rtg.c"
