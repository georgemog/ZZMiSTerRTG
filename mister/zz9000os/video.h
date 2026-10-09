/*
 * MiSTer shim for the ZZ9000OS video.h.
 *
 * The official gfx.h includes "video.h" only for struct ZZ_VIDEO_STATE,
 * which the blitter dispatcher (dma_rtg.c) uses for the framebuffer base.
 * On MiSTer there is no ZZ9000 video formatter: the framebuffer is the
 * Minimig Z3 RTG board's DDR3 window, mapped by minimig_zz9000.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ_VIDEO_H
#define ZZ_VIDEO_H

#include <stdint.h>
#include "zz_video_modes.h"

struct GFXData;

struct ZZ_VIDEO_STATE {
	uint32_t *framebuffer;   /* ARM view of RTG board offset 0 */
	uint32_t fb_size;        /* bytes mapped at framebuffer */
	struct GFXData *gfxdata; /* private snapshot of the Amiga's GFXData */
};

#endif
