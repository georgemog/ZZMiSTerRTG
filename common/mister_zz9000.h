/*
 * MiSTer Minimig ZZ9000-compatible RTG accelerator - shared protocol.
 *
 * Included by the Amiga driver (amiga/rtg/mntgfx-gcc.c, m68k, big-endian)
 * and by the MiSTer main binary (mister/minimig_zz9000.cpp, ARM,
 * little-endian). Everything here is a byte offset or a constant; there is
 * deliberately no shared struct, so the two compilers cannot disagree on
 * layout. Multi-byte mailbox fields are big-endian (written/read natively
 * by the 68k, byte-swapped by the ARM).
 *
 * Memory: the Minimig Z3 RTG board (mfr 0x139C, product 0x30) maps its
 * 8MB framebuffer at board offset 0. It is DDR3 at ARM physical
 * MZZ_FB_PHYS, byte-for-byte in Amiga order (rtl/cpu_wrapper.v swaps the
 * bytes of every RTG CPU access). The top 256KB of that window is taken
 * away from Picasso96 and shared between the driver and the ARM:
 *
 *   0x000000-0x7BFFFF  P96 video memory (b->MemorySize)
 *   0x7C0000-0x7C00FF  mailbox (below)
 *   0x7C1000-0x7C1C5F  struct GFXData, official ZZ9000 layout (zz9000.h)
 *   0x7C2000-0x7FBFFF  template/pattern/planar staging (ZZ "template" area)
 *   0x7FC000-0x7FFFFF  P96 mouse save buffer (same place as MiSTer.card)
 *
 * Handshake: the driver zeroes the mailbox, writes SESSION and then
 * REQ_MAGIC. The ARM thread, once it sees REQ_MAGIC with a SESSION it has
 * not attached to, writes VERSION/CAPS/DONE and then ACK_SESSION and
 * ACK_MAGIC. The ARM never writes anywhere in the RTG window before it has
 * seen REQ_MAGIC.
 *
 * Commands are synchronous, like the ZZ9000's register-triggered DMA ops:
 * the driver fills GFXData (and the staging area), writes OP, then
 * increments KICK. The ARM writes TAKEN=KICK when it starts, runs the op
 * with the official ZZ9000OS blitter, then writes DONE=KICK (and into the
 * DONE ring). The driver spins until it reads DONE==KICK. The FPGA CPU
 * cache (cpu_cache_new) caches the RTG window, so the driver reads the
 * ring (fresh lines) and flushes that cache after the op, or it would see
 * stale mailbox words and stale pixels. See mister_zz9000_cache.h for why
 * CacheClearU() cannot do the flush.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MISTER_ZZ9000_H
#define MISTER_ZZ9000_H

/* Board / window geometry */
#define MZZ_RTG_MANUFACTURER   0x139C
#define MZZ_RTG_PRODUCT        0x30
#define MZZ_FB_SIZE            0x00800000UL   /* framebuffer window at board offset 0 */
#define MZZ_REGS_OFFSET        0x00800000UL   /* rtg.v regs+CLUT 64KB block */
#define MZZ_REG_SUB_OFFSET     0x00000100UL   /* control regs within that block */
#define MZZ_FB_PHYS            0x27000000UL   /* ARM physical address of board offset 0 */

/* Reserved top of the framebuffer window */
#define MZZ_P96_MEMORY_SIZE    0x007C0000UL
#define MZZ_MBOX_OFFSET        0x007C0000UL
#define MZZ_GFXDATA_OFFSET     0x007C1000UL
#define MZZ_TEMPLATE_OFFSET    0x007C2000UL
#define MZZ_TEMPLATE_SIZE      0x0003A000UL
#define MZZ_MOUSESAVE_OFFSET   0x007FC000UL

/* Mailbox fields: byte offsets from MZZ_MBOX_OFFSET, 32-bit big-endian */
#define MZZ_MB_REQ_MAGIC       0x00   /* Amiga: MZZ_REQ_MAGIC once initialised */
#define MZZ_MB_SESSION         0x04   /* Amiga: changes on every driver init */
#define MZZ_MB_KICK            0x08   /* Amiga: sequence number of the submitted op */
#define MZZ_MB_OP              0x0C   /* Amiga: enum gfx_dma_op of that op */
#define MZZ_MB_ACK_MAGIC       0x20   /* ARM: MZZ_ACK_MAGIC once attached */
#define MZZ_MB_ACK_SESSION     0x24   /* ARM: SESSION it attached to */
#define MZZ_MB_TAKEN           0x28   /* ARM: KICK it is working on */
#define MZZ_MB_DONE            0x2C   /* ARM: last KICK completed */
#define MZZ_MB_VERSION         0x30   /* ARM: MZZ_PROTOCOL_VERSION */
#define MZZ_MB_CAPS            0x34   /* ARM: MZZ_CAP_* */
#define MZZ_MB_HEARTBEAT       0x38   /* ARM: increments while the thread runs */
#define MZZ_MB_OPS             0x3C   /* ARM: ops executed (statistics) */
#define MZZ_MB_REJECTED        0x40   /* ARM: ops refused (unknown / out of range) */
/* Driver diagnostics (Amiga writes, nobody depends on them) */
#define MZZ_MB_DRV_STATE       0x80   /* MZZ_DRV_* bits */
#define MZZ_MB_DRV_HOOKS       0x84   /* accelerated P96 hook entries */
#define MZZ_MB_DRV_FALLBACKS   0x88   /* of those, handed to the P96 default */
#define MZZ_MB_DRV_LAST_FMT    0x8C   /* RGBFTYPE of the last hook call */
#define MZZ_DRV_ATTACHED       (1UL << 0)   /* FindCard: ARM answered */
#define MZZ_DRV_INITCARD       (1UL << 1)   /* InitCard ran */
#define MZZ_DRV_BLITTER        (1UL << 2)   /* InitCard set BIF_BLITTER */
#define MZZ_MB_SIZE            0x100

/* DONE ring (protocol 2): after every op the ARM also writes DONE into
 * MZZ_DONE_RING_SLOTS words, each on its own FPGA cache line. The driver
 * polls a different slot per read, so each read misses the CPU cache and
 * sees DDR3 without a cache flush; it only flushes when the ring wraps and
 * once after the op. (cpu_cache_new: 2-way, 256 sets of 8-byte lines.) */
#define MZZ_DONE_RING_OFFSET   (MZZ_MBOX_OFFSET + 0x400)
#define MZZ_DONE_RING_SLOTS    64
#define MZZ_DONE_RING_STRIDE   32

#define MZZ_REQ_MAGIC          0x4D5A5A39UL   /* 'MZZ9' */
#define MZZ_ACK_MAGIC          0x6D7A7A39UL   /* 'mzz9' */
#define MZZ_PROTOCOL_VERSION   2

/* Ops the ARM executes (enum gfx_dma_op values from the ZZ9000 sources) */
#define MZZ_CAP_FILLRECT       (1UL << 2)
#define MZZ_CAP_COPYRECT       (1UL << 3)
#define MZZ_CAP_COPYRECT_NOMASK (1UL << 4)
#define MZZ_CAP_TEMPLATE       (1UL << 5)
#define MZZ_CAP_PATTERN        (1UL << 6)
#define MZZ_CAP_P2C            (1UL << 7)
#define MZZ_CAP_P2D            (1UL << 8)
#define MZZ_CAP_INVERTRECT     (1UL << 9)
#define MZZ_CAP_DRAWLINE       (1UL << 1)
#define MZZ_CAPS_V1            (MZZ_CAP_DRAWLINE | MZZ_CAP_FILLRECT | \
                                MZZ_CAP_COPYRECT | MZZ_CAP_COPYRECT_NOMASK | \
                                MZZ_CAP_TEMPLATE | MZZ_CAP_PATTERN | \
                                MZZ_CAP_P2C | MZZ_CAP_P2D | MZZ_CAP_INVERTRECT)

/* GFXData bytes the ARM snapshots per op: everything before the CLUTs
 * (offset[2] .. u32_user[8] = 0x5C bytes, rounded up). */
#define MZZ_GFXDATA_HEADER     0x60

#endif
