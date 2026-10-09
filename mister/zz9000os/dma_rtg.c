/*
 * ZZ9000OS blitter command dispatcher, MiSTer Minimig port.
 *
 * Upstream: BlitterStudio/zz9000-firmware ZZ9000OS/src/dma_rtg.c. The RTG
 * blit cases are kept as upstream wrote them. MiSTer changes:
 *  - the GFXData block is a private snapshot taken by minimig_zz9000.cpp
 *    (vs->gfxdata), not the Zynq aperture, so the in-place SWAPs below never
 *    touch shared memory;
 *  - every Amiga-supplied offset is range checked against the mapped RTG
 *    window before any pointer is formed (a wild pointer would crash the
 *    MiSTer main binary, not just the blit);
 *  - display/sprite/palette/YUV/overlay ops are dropped: the Minimig RTG
 *    board has no ZZ video formatter. Panning and the CLUT are programmed
 *    by the Amiga driver directly through the core's rtg.v registers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include "zz_video_modes.h"
#include "gfx.h"
#include "video.h"


/* [off, off+len) inside the RTG window? 64-bit math: no wrap tricks. */
static int zz_range_ok(const struct ZZ_VIDEO_STATE *vs, uint64_t off, uint64_t len)
{
    return off <= vs->fb_size && len <= (uint64_t)vs->fb_size - off;
}

/* Source rectangle of a word-pitch (copy) op: rows 0..sy+h-1 of the source. */
static int zz_src_rows_ok(const struct ZZ_VIDEO_STATE *vs, uint32_t off,
        uint32_t pitch_bytes, uint32_t rows)
{
    return zz_range_ok(vs, off, (uint64_t)pitch_bytes * rows);
}

void handle_blitter_dma_op(struct ZZ_VIDEO_STATE* vs, uint16_t zdata)
{
    struct GFXData *data = vs->gfxdata;

    if (!data || !vs->framebuffer)
        return;
    /* Destination writes are clamped to the end of the RTG window by the
     * renderers' own fb_limit bound. */
    set_fb_limit((uint8_t *)vs->framebuffer + vs->fb_size);
    switch(zdata) {
        case OP_DRAWLINE:
            SWAP16(data->x[0]);		SWAP16(data->x[1]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);
            SWAP16(data->user[0]);	SWAP16(data->user[1]);
            SWAP16(data->user[3]);

            SWAP16(data->pitch[0]);
            SWAP32(data->offset[0]);
            if (!zz_range_ok(vs, data->offset[0], 0))
                break;

            set_fb_words((uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);

            if (line_uses_solid_path(data->user[1], data->mask,
                    data->u8_user[GFXDATA_U8_DRAWMODE]))
                draw_line_solid(data->x[0], data->y[0], data->x[1], data->y[1],
                        data->user[0], (int16_t)data->user[3], data->rgb[0],
                        data->u8_user[GFXDATA_U8_COLORMODE]);
            else
                /* The line pattern offset comes from the dedicated u8_user byte
                 * (a single byte, so no byte order to reconcile). The driver
                 * also packs it into user[2] for firmware that reads the word
                 * form, but that field is deliberately NOT SWAP16'd here and is
                 * unused on this path. */
                draw_line(data->x[0], data->y[0], data->x[1], data->y[1],
                        data->user[0], (int16_t)data->user[3], data->user[1],
                        data->u8_user[GFXDATA_U8_LINE_PATTERN_OFFSET], data->rgb[0], data->rgb[1],
                        data->u8_user[GFXDATA_U8_COLORMODE], data->mask, data->u8_user[GFXDATA_U8_DRAWMODE]);

            break;

        case OP_FILLRECT:
            SWAP16(data->x[0]);		SWAP16(data->x[1]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);

            SWAP16(data->pitch[0]);
            SWAP32(data->offset[0]);
            if (!zz_range_ok(vs, data->offset[0], 0))
                break;

            set_fb_words((uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);

            if (data->mask == 0xFF)
                fill_rect_solid(data->x[0], data->y[0], data->x[1], data->y[1],
                        data->rgb[0], data->u8_user[GFXDATA_U8_COLORMODE]);
            else
                fill_rect(data->x[0], data->y[0], data->x[1], data->y[1], data->rgb[0],
                        data->u8_user[GFXDATA_U8_COLORMODE], data->mask);
            break;

        case OP_COPYRECT:
        case OP_COPYRECT_NOMASK:
            SWAP16(data->x[0]);		SWAP16(data->x[1]);		SWAP16(data->x[2]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);		SWAP16(data->y[2]);

            SWAP16(data->pitch[0]);		SWAP16(data->pitch[1]);
            SWAP32(data->offset[0]);	SWAP32(data->offset[1]);
            if (!zz_range_ok(vs, data->offset[0], 0))
                break;
            /* Source rows sy..sy+h-1, word pitch (BlitRect reuses the dest
             * RenderInfo, NoMaskComplete brings its own). */
            if (zdata == OP_COPYRECT ?
                    !zz_src_rows_ok(vs, data->offset[0], data->pitch[0] * 4U,
                        (uint32_t)data->y[2] + data->y[1]) :
                    !zz_src_rows_ok(vs, data->offset[1], data->pitch[1] * 4U,
                        (uint32_t)data->y[2] + data->y[1]))
                break;

            set_fb_words((uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);

            switch (zdata) {
            case 3: // Regular BlitRect
                if (data->mask == 0xFF || (data->mask != 0xFF && data->u8_user[GFXDATA_U8_COLORMODE] != MNTVA_COLOR_8BIT))
                    copy_rect_nomask(data->x[0], data->y[0], data->x[1], data->y[1], data->x[2],
                                    data->y[2], data->u8_user[GFXDATA_U8_COLORMODE],
                                    (uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[0]),
                                    data->pitch[0], MINTERM_SRC);
                else
                    copy_rect(data->x[0], data->y[0], data->x[1], data->y[1], data->x[2],
                            data->y[2], data->u8_user[GFXDATA_U8_COLORMODE],
                            (uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[0]),
                            data->pitch[0], data->mask);
                break;
            case 4: // BlitRectNoMaskComplete
                copy_rect_nomask(data->x[0], data->y[0], data->x[1], data->y[1], data->x[2],
                                data->y[2], data->u8_user[GFXDATA_U8_COLORMODE],
                                (uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[1]),
                                data->pitch[1], data->minterm);
                break;
            }
            break;

        case OP_RECT_PATTERN:
        case OP_RECT_TEMPLATE: {
            SWAP16(data->x[0]);		SWAP16(data->x[1]);		SWAP16(data->x[2]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);		SWAP16(data->y[2]);

            SWAP16(data->pitch[0]);		SWAP16(data->pitch[1]);
            SWAP32(data->offset[0]);	SWAP32(data->offset[1]);
            if (!zz_range_ok(vs, data->offset[0], 0))
                break;
            if (zdata == OP_RECT_PATTERN) {
                /* 16 pixels wide, user[0] (= 1 << Pattern.Size) rows. */
                if (!zz_range_ok(vs, data->offset[1],
                        2U * __builtin_bswap16(data->user[0])))
                    break;
            } else if (!zz_range_ok(vs, data->offset[1],
                    (uint64_t)data->pitch[1] * data->y[1])) {
                break;
            }

            uint8_t* tmpl_data = (uint8_t*) ((uintptr_t)vs->framebuffer
                    + data->offset[1]);
            set_fb_bytes((uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);


            uint8_t bpp = 2 * data->u8_user[GFXDATA_U8_COLORMODE];
            if (bpp == 0)
                bpp = 1;
            uint16_t loop_rows = 0;

            if (zdata == OP_RECT_PATTERN) {
                SWAP16(data->user[0]);

                loop_rows = data->user[0];

                pattern_fill_rect(data->u8_user[GFXDATA_U8_COLORMODE],
                        data->x[0], data->y[0], data->x[1], data->y[1],
                        data->u8_user[GFXDATA_U8_DRAWMODE], data->mask,
                        data->rgb[0], data->rgb[1], data->x[2], data->y[2],
                        tmpl_data, 16, loop_rows);
            }
            else {
                template_fill_rect(data->u8_user[GFXDATA_U8_COLORMODE], data->x[0],
                        data->y[0], data->x[1], data->y[1], data->u8_user[GFXDATA_U8_DRAWMODE], data->mask,
                        data->rgb[0], data->rgb[1], data->x[2], data->y[2], tmpl_data,
                        data->pitch[1]);
            }

            break;
        }

        case OP_P2C:
        case OP_P2D: {
            SWAP16(data->x[0]);		SWAP16(data->x[1]);		SWAP16(data->x[2]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);		SWAP16(data->y[2]);

            SWAP16(data->pitch[0]);		SWAP16(data->pitch[1]);
            SWAP32(data->offset[0]);	SWAP32(data->offset[1]);

            SWAP16(data->user[0]);
            SWAP16(data->user[1]);

            if (!zz_range_ok(vs, data->offset[0], 0) || data->user[1] > 8)
                break;
            /* user[1] planes of pitch[1] * h bytes; P2D prefixes a 256-entry
             * 32-bit colour table. */
            if (!zz_range_ok(vs, data->offset[1],
                    (zdata == OP_P2D ? 1024U : 0U) +
                    (uint64_t)data->pitch[1] * data->y[2] * data->user[1]))
                break;

            uint8_t* bmp_data = (uint8_t*) ((uintptr_t)vs->framebuffer
                    + data->offset[1]);

            set_fb_words((uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);

            if (zdata == OP_P2C) {
                p2c_rect(data->x[0], 0, data->x[1], data->y[1], data->x[2],
                        data->y[2], data->minterm, data->user[1], data->mask,
                        data->user[0], data->pitch[1], bmp_data);
            }
            else {
                p2d_rect(data->x[0], 0, data->x[1], data->y[1], data->x[2],
                        data->y[2], data->minterm, data->user[1], data->mask, data->user[0],
                        data->rgb[0], data->pitch[1], bmp_data, data->u8_user[GFXDATA_U8_COLORMODE]);
            }
            break;
        }

        case OP_INVERTRECT:
            SWAP16(data->x[0]);		SWAP16(data->x[1]);
            SWAP16(data->y[0]);		SWAP16(data->y[1]);

            SWAP16(data->pitch[0]);
            SWAP32(data->offset[0]);
            if (!zz_range_ok(vs, data->offset[0], 0))
                break;

            set_fb_words((uint32_t*) ((uintptr_t)vs->framebuffer + data->offset[0]),
                    data->pitch[0]);
            invert_rect(data->x[0], data->y[0], data->x[1], data->y[1],
                    data->mask, data->u8_user[GFXDATA_U8_COLORMODE]);
            break;

        default:
            break;
    }
}
