/*
 * MiSTer side of the ZZ9000-compatible RTG accelerator: mailbox service.
 * Protocol: common/mister_zz9000.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <string.h>

#include "mister_zz9000.h"
#include "zz9000_fbmem.h"
#include "zz9000_service.h"
#include "zz9000os/gfx.h"
#include "zz9000os/video.h"

/* Mailbox words are 4-byte aligned, so a plain aligned load/store is safe
 * on the Strongly-ordered mapping. */
static inline uint32_t mb_rd(const struct zz_service *s, uint32_t field)
{
	return __builtin_bswap32(*(volatile uint32_t *)(s->fb + MZZ_MBOX_OFFSET + field));
}

static inline void mb_wr(struct zz_service *s, uint32_t field, uint32_t v)
{
	*(volatile uint32_t *)(s->fb + MZZ_MBOX_OFFSET + field) = __builtin_bswap32(v);
}

unsigned zz_service_gfxdata_size(void)
{
	return sizeof(struct GFXData);
}

void zz_service_init(struct zz_service *s, uint8_t *fb, uint32_t fb_size, void *gfxdata)
{
	memset(s, 0, sizeof(*s));
	s->fb = fb;
	s->fb_size = fb_size;
	s->gfxdata = gfxdata;
}

static int op_supported(uint32_t op)
{
	return op < 32 && (MZZ_CAPS_V1 & (1UL << op));
}

int zz_service_poll(struct zz_service *s)
{
	uint32_t magic = mb_rd(s, MZZ_MB_REQ_MAGIC);

	if (magic != MZZ_REQ_MAGIC) {
		/* 'ZM9Z': the 68k's words reached DDR3 byte-swapped, i.e. the core
		 * does not have the RTG byte swap this port depends on. */
		s->swapped = (magic == 0x5A4D395AUL);
		s->attached = 0;
		return ZZ_POLL_NO_DRIVER;
	}

	uint32_t session = mb_rd(s, MZZ_MB_SESSION);
	if (!s->attached || session != s->session) {
		s->session = session;
		s->done = mb_rd(s, MZZ_MB_KICK);
		s->attached = 1;
		s->ops = s->rejected = 0;
		mb_wr(s, MZZ_MB_VERSION, MZZ_PROTOCOL_VERSION);
		mb_wr(s, MZZ_MB_CAPS, MZZ_CAPS_V1);
		mb_wr(s, MZZ_MB_TAKEN, s->done);
		mb_wr(s, MZZ_MB_DONE, s->done);
		for (unsigned i = 0; i < MZZ_DONE_RING_SLOTS; i++)
			*(volatile uint32_t *)(s->fb + MZZ_DONE_RING_OFFSET + i * MZZ_DONE_RING_STRIDE) =
				__builtin_bswap32(s->done);
		mb_wr(s, MZZ_MB_OPS, 0);
		mb_wr(s, MZZ_MB_REJECTED, 0);
		__sync_synchronize();
		mb_wr(s, MZZ_MB_ACK_SESSION, session);
		mb_wr(s, MZZ_MB_ACK_MAGIC, MZZ_ACK_MAGIC);
		return ZZ_POLL_ATTACHED;
	}

	uint32_t kick = mb_rd(s, MZZ_MB_KICK);
	if (kick == s->done)
		return ZZ_POLL_IDLE;

	uint32_t op = mb_rd(s, MZZ_MB_OP);
	mb_wr(s, MZZ_MB_TAKEN, kick);

	if (op_supported(op)) {
		struct ZZ_VIDEO_STATE vs;

		/* Snapshot only the command header; the dispatcher byte-swaps its
		 * fields in place (upstream behaviour), which must never touch the
		 * Amiga's copy. */
		zz_fb_memcpy(s->gfxdata, s->fb + MZZ_GFXDATA_OFFSET, MZZ_GFXDATA_HEADER);
		vs.framebuffer = (uint32_t *)s->fb;
		vs.fb_size = s->fb_size;
		vs.gfxdata = (struct GFXData *)s->gfxdata;
		handle_blitter_dma_op(&vs, (uint16_t)op);
		s->ops++;
		mb_wr(s, MZZ_MB_OPS, s->ops);
	} else {
		s->rejected++;
		mb_wr(s, MZZ_MB_REJECTED, s->rejected);
	}
	s->last_op = (uint16_t)op;

	/* Every pixel store must be visible in DDR3 before the 68k sees DONE. */
	__sync_synchronize();
	mb_wr(s, MZZ_MB_DONE, kick);
	for (unsigned i = 0; i < MZZ_DONE_RING_SLOTS; i++)
		*(volatile uint32_t *)(s->fb + MZZ_DONE_RING_OFFSET + i * MZZ_DONE_RING_STRIDE) =
			__builtin_bswap32(kick);
	s->done = kick;
	return ZZ_POLL_OP;
}

void zz_service_heartbeat(struct zz_service *s)
{
	if (!s->attached)
		return;
	mb_wr(s, MZZ_MB_HEARTBEAT, ++s->heartbeat);
}
