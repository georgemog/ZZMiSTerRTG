/*
 * MNT ZZ9000 Amiga Graphics Card Driver (ZZ9000.card)
 *
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *													MNT Research GmbH, Berlin
 *													https://mntre.com
 * Copyright (C) 2021,			Bjorn Astrom <beeanyew@gmail.com>
 * Copyright (C) 2026,			Dimitris Panokostas <midwan@gmail.com>
 *
 * More Info: https://mntre.com/zz9000
 *
 * MiSTer Minimig port (MiSTerZZ.card), 2026: drives the Minimig Z3 RTG
 * board (mfr 0x139C, product 0x30) instead of a ZZ9000. The accelerated
 * P96 hooks below are the official ZZ9000 Zorro III code paths, unchanged
 * except for the doorbell: where the ZZ9000 driver writes the op number to
 * the card's blitter_dma_op register, this one hands it to the ZZ9000OS
 * blitter running inside the MiSTer main binary via a mailbox at the top of
 * the RTG framebuffer (see ../../common/mister_zz9000.h). Display control
 * (mode, panning, CLUT, pixel format) goes to the core's own rtg.v
 * registers, as MiSTer.card does. ZZ9000-only hardware (video capture,
 * hardware sprite, screen split, PIP, DPMS, ARM SDK) is not offered.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/input.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/initializers.h>
#include <clib/debug_protos.h>
#include <devices/inputevent.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "mntgfx-gcc.h"
#include "zz9000.h"
#include "zzcfg_query.h"
#include "blitter_cache.h"
#include "memory_layout.h"
#include "custom_modeline.h"
#include "offscreen_bitmap.h"
#include "overlay_feature.h"
#include "zz9000_hw.h"
#include "videocap_mode.h"
#include "../../common/mister_zz9000.h"
#include "../../common/mister_zz9000_cache.h"
#include <clib/Picasso96_protos.h>
#include <inline/Picasso96.h>

#define STR(s) #s
#define XSTR(s) STR(s)

struct GFXBase {
	struct Library libNode;
	UBYTE Flags;
	UBYTE pad;
	struct ExecBase *ExecBase;
	struct ExpansionBase *ExpansionBase;
	BPTR segList;
	char *Name;
};

struct DOSBase;

#ifndef DEBUG
#define KPrintF(...)
#endif
#define __saveds__

#define DEVICE_VERSION 2
#define DEVICE_REVISION 13
#define DEVICE_PRIORITY 0
#define DEVICE_ID_STRING "$VER: MiSTerZZ.card " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE " ZZ9000.card port for MiSTer Minimig"
#define DEVICE_NAME "MiSTerZZ.card"
#define DEVICE_DATE "(09.10.2026)"

/* Firmware revision the MiSTer blitter answers as (major<<8|minor). 2.0 is
 * the oldest the ZZ9000 driver supports and, deliberately, older than every
 * optional feature gate below (off-screen bitmaps 2.4, PIP 2.6, DPMS 2.7):
 * the MiSTer side implements the plain 2.0 blitter op set only. */
#define MISTER_ZZ_FWREV 0x0200

int __attribute__((no_reorder)) _start()
{
		return -1;
}

#ifdef DEBUG
void exit(int hmm) {
	while(1);
}
#endif

asm("romtag:\n"
		"				dc.w		"XSTR(RTC_MATCHWORD)"		\n"
		"				dc.l		romtag									\n"
		"				dc.l		endcode									\n"
		"				dc.b		"XSTR(RTF_AUTOINIT)"		\n"
		"				dc.b		"XSTR(DEVICE_VERSION)"	\n"
		"				dc.b		"XSTR(NT_LIBRARY)"			\n"
		"				dc.b		"XSTR(DEVICE_PRIORITY)" \n"
		"				dc.l		_device_name						\n"
		"				dc.l		_device_id_string				\n"
		"				dc.l		_auto_init_tables				\n"
		"endcode:																\n");

char device_name[] = DEVICE_NAME;
char device_id_string[] = DEVICE_ID_STRING;

__saveds struct GFXBase* OpenLib(__REGA6(struct GFXBase *gfxbase));
BPTR __saveds CloseLib(__REGA6(struct GFXBase *gfxbase));
BPTR __saveds ExpungeLib(__REGA6(struct GFXBase *exb));
ULONG ExtFuncLib(void);
__saveds struct GFXBase* InitLib(__REGA6(struct ExecBase *sysbase),
																 __REGA0(BPTR seglist),
																 __REGD0(struct GFXBase *exb));

#define MEMORY_CLOCK_HZ 100000000

static struct GFXBase *_gfxbase;
char *gfxname = "MiSTerZZ";
char dummies[128];

#define CARDFLAG_ZORRO_3 1
/* GFXData and the template/planar staging area: the reserved top of the
 * Minimig RTG framebuffer, shared with the MiSTer side (MemoryBase-relative,
 * like the ZZ9000's own scratch area). */
#define Z3_GFXDATA_ADDR  MZZ_GFXDATA_OFFSET
#define Z3_TEMPLATE_ADDR MZZ_TEMPLATE_OFFSET
#define ZZ_CARD_DATA_GFXDATA 0
#define ZZ_CARD_DATA_VCAP_MODE 1
#define ZZ_CARD_DATA_NSVSYNC 2
#define ZZ_CARD_DATA_MONITOR_SWITCH 3
#define ZZ_CARD_DATA_DISPLAY_ENABLED 4
#define ZZ_CARD_DATA_SECONDARY_PALETTE 5
#define ZZ_CARD_DATA_OFFSCREEN_BITMAPS 6
#define ZZ_CARD_DATA_VIDEO_OVERLAY 7
#define ZZ_CARD_DATA_APERTURE_INFO 8
#define ZZ_CARD_DATA_TEMPLATE_OFFSET 9
#define ZZ_CARD_DATA_PIP_OFFSET 10
#define ZZ_CARD_DATA_PIP_SIZE 11
#define ZZ_CARD_DATA_FW_CAPABILITIES 12
/* Refused SetGC requests must not reach scanout through later panning. */
#define ZZ_CARD_DATA_GC_REJECTED 13
/* MiSTer: rtg.v control registers (board + $800100) */
#define ZZ_CARD_DATA_MISTER_REGS 14
/* MiSTer: 1 while the ARM blitter is attached and answering */
#define ZZ_CARD_DATA_MISTER_ACCEL 15
/* first firmware whose surface allocator really frees (major<<8|minor) */
#define OFFSCREEN_BITMAPS_MIN_FWREV 0x0204
/* first firmware with OP_VIDEO_OVERLAY + the shadow-scanout compositor */
#define VIDEO_OVERLAY_MIN_FWREV 0x0206
/* first firmware with CARD_FEATURE_DPMS + formatter sync gating */
#define DPMS_MIN_FWREV 0x0207
#define RGBFMT_BIT(format) (1UL << (format))
#define ZZ_RGBFMT_NONE     0
#define ZZ_RGBFMT_CLUT     1
#define ZZ_RGBFMT_B8G8R8   3
#define ZZ_RGBFMT_R5G6B5PC 4
#define ZZ_RGBFMT_R8G8B8A8 8

/*
 * MiSTer: advertise the formats MiSTer.card has proven on the Minimig RTG
 * scanout (rtg.v FORMAT codes in mister_format[] below). The ZZ9000 blitter
 * itself is byte-order agnostic within a pixel size, so R5G6B5PC runs on its
 * 16-bit paths and R8G8B8A8 on its 32-bit paths (alpha sits in the last
 * byte, exactly where the ZZ9000's BGRA keeps it). 24-bit B8G8R8 has no
 * ZZ9000 blitter mode and is drawn by Picasso96's software defaults.
 */
#define ZZ_SUPPORTED_RGB_FORMATS \
	(RGBFMT_BIT(ZZ_RGBFMT_NONE) | \
	 RGBFMT_BIT(ZZ_RGBFMT_CLUT) | \
	 RGBFMT_BIT(ZZ_RGBFMT_B8G8R8) | \
	 RGBFMT_BIT(ZZ_RGBFMT_R5G6B5PC) | \
	 RGBFMT_BIT(ZZ_RGBFMT_R8G8B8A8))

#ifndef CDF_CONFIGME
#define CDF_CONFIGME (1 << 1)
#endif

struct ExecBase *SysBase;
static struct ConfigDev *reserved_cd = NULL;
static struct BlitterRegisterCache blitter_register_cache;
enum GFXDataByteCacheSlot {
	GFXDATA_BYTE_U8_0,
	GFXDATA_BYTE_U8_1,
	GFXDATA_BYTE_MASK,
	GFXDATA_BYTE_MINTERM,
	GFXDATA_BYTE_CACHE_COUNT
};

struct GFXDataByteCache {
	volatile struct GFXData *owner;
	uint8 value[GFXDATA_BYTE_CACHE_COUNT];
	uint8 valid;
};

static struct GFXDataByteCache gfxdata_byte_cache;
static BOOL zz_overlay_hooks_enabled = FALSE;
struct ZZBitMap;
struct ZZZ2PIPAllocationRequest {
	BOOL pending;
	ULONG pitch;
	ULONG size;
	ULONG width;
	ULONG height;
	ULONG format;
};
static struct ZZZ2PIPAllocationRequest zz_z2_pip_request;
static struct ZZBitMap *zz_z2_pip_bitmap = NULL;
/* Card memory addressable from MemoryBase (autoconfig window minus the
 * 64 KB register space). b->MemorySize is SMALLER on Z3: it only caps
 * P96's own VRAM allocator, while the firmware surface heap for
 * off-screen bitmaps sits deliberately ABOVE it (board ~0x3310000+),
 * still inside the autoconfig window. Ownership/on-board checks must
 * bound Planes[0] against this, never against b->MemorySize. */
static ULONG zz_card_window_size = 0;

static inline volatile struct GFXData *zz_gfxdata(struct BoardInfo *b) {
	return (volatile struct GFXData *)b->CardData[ZZ_CARD_DATA_GFXDATA];
}

/* ---- MiSTer Minimig RTG board ------------------------------------------- */

/* rtg.v control registers, offsets from board + $800100 (see MiSTer.card) */
#define MISTER_REG_ADDRESS 0x00   /* 32-bit: scanout address, ARM physical */
#define MISTER_REG_FORMAT  0x04
#define MISTER_REG_ENABLE  0x06
#define MISTER_REG_HSIZE   0x08
#define MISTER_REG_VSIZE   0x0A
#define MISTER_REG_STRIDE  0x0C
#define MISTER_REG_ID      0x0E   /* reads $5001 */
#define MISTER_REG_PALETTE 0x300  /* 256 x 32-bit $00RRGGBB */
#define MISTER_RTG_ID      0x5001

/* rtg.v FORMAT per RGBFTYPE, MiSTer.card's SetDAC table:
 * [2:0] 011=8bpp CLUT 100=16bpp 101=24bpp 110=32bpp, [3] 1555,
 * [4] BGR, [5] byte swap. */
static const UWORD mister_format[] = {
	3, 3 + 32, 5, 5 + 16 + 32, 4 + 16 + 32, 4 + 8, 6, 6 + 16,
	6 + 32, 6 + 16, 4, 4 + 8, 4, 4 + 8 + 16,
};

/* ARM-side blitter timeouts, in DONE-ring polls (one Zorro III read each,
 * plus an FPGA cache flush every MZZ_DONE_RING_SLOTS polls). Attach waits for the MiSTer
 * thread's 20ms no-driver sleep; an op waits long only once the ARM has
 * taken it (a full-screen 32-bit copy through /dev/mem is tens of ms). */
#define MISTER_ATTACH_POLLS   400000UL
#define MISTER_UNTAKEN_POLLS  100000UL
#define MISTER_TAKEN_POLLS   2500000UL

static ULONG mister_seq;

static inline volatile UWORD *mister_regs(struct BoardInfo *b) {
	return (volatile UWORD *)b->CardData[ZZ_CARD_DATA_MISTER_REGS];
}

static inline volatile ULONG *mister_mbox_field(struct BoardInfo *b, ULONG field) {
	return (volatile ULONG *)((UBYTE *)b->MemoryBase + MZZ_MBOX_OFFSET + field);
}

static inline BOOL mister_accel(struct BoardInfo *b) {
	return b && b->CardData[ZZ_CARD_DATA_MISTER_ACCEL];
}

static inline ULONG mister_mbox_read(struct BoardInfo *b, ULONG field) {
	/* The FPGA CPU cache holds RTG window lines: drop them before every
	 * read of a word the ARM writes. */
	mister_fpga_cache_clear();
	return *mister_mbox_field(b, field);
}

/* The ZZ9000 doorbell. Upstream writes the op to blitter_dma_op and the
 * card's firmware runs it before the Zorro cycle completes; here the op
 * goes to the MiSTer-side ZZ9000OS blitter through the mailbox and this
 * waits for it, so callers keep the same synchronous semantics (and
 * WaitBlitter() has nothing left to wait for). */
static void mister_kick(struct BoardInfo *b, UWORD op) {
	ULONG seq, polls = 0;

	if (!mister_accel(b))
		return;

	volatile UBYTE *ring = (volatile UBYTE *)b->MemoryBase + MZZ_DONE_RING_OFFSET;
	ULONG slot = 0;

	seq = ++mister_seq;
	*mister_mbox_field(b, MZZ_MB_OP) = op;
	*mister_mbox_field(b, MZZ_MB_KICK) = seq;

	/* Every slot is a cache line not read since the last flush, so each
	 * read is fresh from DDR3; flush only when the ring wraps. */
	for (;;) {
		if (*(volatile ULONG *)(ring + slot * MZZ_DONE_RING_STRIDE) == seq)
			break;
		if (++slot == MZZ_DONE_RING_SLOTS) {
			slot = 0;
			mister_fpga_cache_clear();
		}
		polls++;
		if (polls > MISTER_TAKEN_POLLS ||
		    (polls > MISTER_UNTAKEN_POLLS && slot == 0 &&
		     *mister_mbox_field(b, MZZ_MB_TAKEN) != seq)) {
			/* The MiSTer side is gone (binary replaced/killed): stop
			 * offloading. Every hook falls back to the P96 software
			 * defaults from here on; this one op is lost. */
			KPrintF("MiSTerZZ.card: ARM blitter timed out on op %ld, acceleration off\n", (LONG)op);
			b->CardData[ZZ_CARD_DATA_MISTER_ACCEL] = 0;
			break;
		}
	}
	/* An interrupt between the last clear and DONE could have cached
	 * pre-blit pixels; make sure the CPU sees the ARM's result. */
	mister_fpga_cache_clear();
}

/* Handshake with the MiSTer binary (protocol in mister_zz9000.h).
 * Returns TRUE if the ARM blitter attached. */
static BOOL mister_attach(struct BoardInfo *b) {
	volatile ULONG *mb = mister_mbox_field(b, 0);
	ULONG session, polls, i;

	for (i = 0; i < MZZ_MB_SIZE / 4; i++)
		mb[i] = 0;
	memset((void *)zz_gfxdata(b), 0, sizeof(struct GFXData));

	/* Any value that differs from the previous boot's will do. */
	session = (ULONG)SysBase->VBlankFrequency << 24 ^ (ULONG)SysBase->IdleCount ^
		(ULONG)SysBase->DispCount ^ ((ULONG)b << 4) ^ 0x5A5A0000UL;
	mister_seq = 0;
	mb[MZZ_MB_SESSION / 4] = session;
	mb[MZZ_MB_KICK / 4] = 0;
	mb[MZZ_MB_REQ_MAGIC / 4] = MZZ_REQ_MAGIC;

	for (polls = 0; polls < MISTER_ATTACH_POLLS; polls++) {
		if (mister_mbox_read(b, MZZ_MB_ACK_MAGIC) == MZZ_ACK_MAGIC &&
		    mister_mbox_read(b, MZZ_MB_ACK_SESSION) == session) {
			ULONG version = mister_mbox_read(b, MZZ_MB_VERSION);
			ULONG caps = mister_mbox_read(b, MZZ_MB_CAPS);
			if (version != MZZ_PROTOCOL_VERSION ||
			    (caps & MZZ_CAPS_V1) != MZZ_CAPS_V1) {
				KPrintF("MiSTerZZ.card: MiSTer blitter protocol %ld caps %lx unsupported\n",
					(LONG)version, (ULONG)caps);
				return FALSE;
			}
			return TRUE;
		}
	}
	KPrintF("MiSTerZZ.card: no MiSTer blitter answered (old MiSTer binary?), software rendering\n");
	return FALSE;
}

/* Like mnt_colormode(), but NO_USE once the ARM blitter is unavailable, so
 * every accelerated hook takes its existing "unsupported format" exit into
 * the P96 software default. */
static inline uint16_t blit_colormode(struct BoardInfo *b, UWORD format);

static inline uint32_t mister_bytes_per_pixel(RGBFTYPE format);

/* Bytes per row of a card bitmap and of the scanout. Rounded up to whole
 * longwords: the ZZ9000 blitter addresses rows in 32-bit words and ignores
 * template/pattern ops on any other pitch. */
static inline UWORD mister_pitch(UWORD width, RGBFTYPE format) {
	return (UWORD)(((ULONG)width * mister_bytes_per_pixel(format) + 3) & ~3UL);
}

static inline uint32_t mister_bytes_per_pixel(RGBFTYPE format) {
	switch (format) {
	case ZZ_RGBFMT_NONE:
	case ZZ_RGBFMT_CLUT:     return 1;
	case ZZ_RGBFMT_B8G8R8:   return 3;
	case ZZ_RGBFMT_R5G6B5PC: return 2;
	case ZZ_RGBFMT_R8G8B8A8: return 4;
	default:                 return 0;
	}
}

static inline BOOL zz_z2_layout_active(struct BoardInfo *b) {
	return b && !(b->CardFlags & CARDFLAG_ZORRO_3) &&
		b->CardData[ZZ_CARD_DATA_APERTURE_INFO] != 0 &&
		b->CardData[ZZ_CARD_DATA_TEMPLATE_OFFSET] != 0;
}

static inline uint32_t zz_z2_template_offset(struct BoardInfo *b) {
	return zz_z2_layout_active(b) ?
		(uint32_t)b->CardData[ZZ_CARD_DATA_TEMPLATE_OFFSET] :
		(uint32_t)b->MemorySize;
}

static inline BOOL zz_z2_template_fits(uint32_t bytes) {
	return bytes <= ZZ_Z2_TEMPLATE_SIZE;
}

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const uint16_t rtg_to_mnt[] = {
	MNTVA_COLOR_8BIT,		// 0x00 -- None
	MNTVA_COLOR_8BIT,		// 0x01 -- 8BPP CLUT
	MNTVA_COLOR_NO_USE,		// 0x02 -- 24BPP RGB
	MNTVA_COLOR_NO_USE,		// 0x03 -- 24BPP BGR
	MNTVA_COLOR_16BIT565,	// 0x04 -- 16BPP R5G6B5PC (MiSTer)
	MNTVA_COLOR_NO_USE,		// 0x05 -- 15BPP R5G5B5PC
	MNTVA_COLOR_NO_USE,		// 0x06 -- 32BPP ARGB
	MNTVA_COLOR_NO_USE,		// 0x07 -- 32BPP ABGR
	MNTVA_COLOR_32BIT,		// 0x08 -- 32BPP RGBA (MiSTer)
	MNTVA_COLOR_32BIT,		// 0x09 -- 32BPP BGRA
	MNTVA_COLOR_16BIT565,	// 0x0A -- 16BPP R5G6B5
	MNTVA_COLOR_15BIT,		// 0x0B -- 15BPP R5G5B5
	MNTVA_COLOR_NO_USE,		// 0x0C -- 16BPP B5G6R5PC
	MNTVA_COLOR_NO_USE,		// 0x0D -- 15BPP B5G5R5PC
	MNTVA_COLOR_NO_USE,		// 0x0E -- YUV 4:2:2
	MNTVA_COLOR_NO_USE,		// 0x0F -- YUV 4:1:1
	MNTVA_COLOR_NO_USE,		// 0x10 -- YUV 4:1:1PC
	MNTVA_COLOR_NO_USE,		// 0x11 -- YUV 4:2:2 (Duplicate for some reason)
	MNTVA_COLOR_NO_USE,		// 0x12 -- YUV 4:2:2PC
	MNTVA_COLOR_NO_USE,		// 0x13 -- YUV 4:2:2 Planar
	MNTVA_COLOR_NO_USE,		// 0x14 -- YUV 4:2:2PC Planar
};

static inline uint16_t mnt_colormode(UWORD format) {
	if (format < ARRAY_SIZE(rtg_to_mnt))
		return rtg_to_mnt[format];

	return MNTVA_COLOR_NO_USE;
}

static inline void mister_diag_hook(struct BoardInfo *b, ULONG format, BOOL fallback) {
	(*mister_mbox_field(b, MZZ_MB_DRV_HOOKS))++;
	if (fallback)
		(*mister_mbox_field(b, MZZ_MB_DRV_FALLBACKS))++;
	*mister_mbox_field(b, MZZ_MB_DRV_LAST_FMT) = format;
}

static inline uint16_t blit_colormode(struct BoardInfo *b, UWORD format) {
	uint16_t cm = mister_accel(b) ? mnt_colormode(format) : MNTVA_COLOR_NO_USE;
	mister_diag_hook(b, format, cm == MNTVA_COLOR_NO_USE);
	return cm;
}

static inline uint16_t panning_colormode(uint16_t colormode) {
	return (colormode == MNTVA_COLOR_15BIT) ? MNTVA_COLOR_16BIT565 : colormode;
}

static inline UBYTE direct_color_mask(uint16_t colormode, UBYTE mask) {
	return (colormode == MNTVA_COLOR_32BIT) ? 0xFF : mask;
}

static inline BOOL supported_rgb_format(RGBFTYPE format) {
	if (format >= 32)
		return FALSE;

	return (ZZ_SUPPORTED_RGB_FORMATS & (1UL << format)) != 0;
}

static BOOL chars_equal_ci(char a, char b) {
	if (a >= 'a' && a <= 'z')
		a -= ('a' - 'A');
	if (b >= 'a' && b <= 'z')
		b -= ('a' - 'A');

	return a == b;
}

static BOOL string_equal_ci(const char *a, const char *b) {
	if (!a || !b)
		return FALSE;

	while (*a && *b) {
		if (!chars_equal_ci(*a, *b))
			return FALSE;
		a++;
		b++;
	}

	return *a == *b;
}

static BOOL tooltype_name_matches(const char *entry, const char *name) {
	const char *p = entry;

	if (!entry || !name)
		return FALSE;

	while (*name && *p && *p != '=') {
		if (!chars_equal_ci(*p, *name))
			return FALSE;
		p++;
		name++;
	}

	return *name == 0 && (*p == 0 || *p == '=');
}

static const char *tooltype_value(char **tool_types, const char *name) {
	if (!tool_types)
		return NULL;

	while (*tool_types) {
		const char *entry = *tool_types;
		if (tooltype_name_matches(entry, name)) {
			while (*entry && *entry != '=')
				entry++;
			return (*entry == '=') ? entry + 1 : "";
		}
		tool_types++;
	}

	return NULL;
}

static BOOL value_is_false(const char *value) {
	return value &&
		(string_equal_ci(value, "NO") ||
		 string_equal_ci(value, "FALSE") ||
		 string_equal_ci(value, "OFF") ||
		 string_equal_ci(value, "0"));
}

static BOOL value_is_true(const char *value) {
	return !value || *value == 0 ||
		string_equal_ci(value, "YES") ||
		string_equal_ci(value, "TRUE") ||
		string_equal_ci(value, "ON") ||
		string_equal_ci(value, "1");
}

static BOOL env_flag_exists(struct DOSBase *DOSBase, const char *name) {
	BPTR f = Open((APTR)name, MODE_OLDFILE);
	if (f) {
		Close(f);
		return TRUE;
	}

	return FALSE;
}

static LONG tooltype_vcap_mode(char **tool_types, LONG current) {
	const char *value = tooltype_value(tool_types, "ZZ9000-VCAP-800x600");
	if (value)
		current = value_is_false(value) ? ZZ_VMODE_720x576 : ZZ_VMODE_800x600;

	value = tooltype_value(tool_types, "VCAP");
	if (!value)
		return current;

	if (string_equal_ci(value, "800x600") ||
		string_equal_ci(value, "60") ||
		string_equal_ci(value, "60HZ") ||
		value_is_true(value)) {
		return ZZ_VMODE_800x600;
	}
	if (string_equal_ci(value, "CENTERED") ||
		string_equal_ci(value, "1080P") ||
		string_equal_ci(value, "CENTERED_1080P_60"))
		return ZZ_VMODE_CENTERED_1080P_60;
	if (string_equal_ci(value, "CENTERED_1080P_50"))
		return ZZ_VMODE_CENTERED_1080P_50;
	if (string_equal_ci(value, "CENTERED_1080P_MATCH"))
		return ZZ_VMODE_CENTERED_1080P_MATCH;

	if (string_equal_ci(value, "720x576") ||
		string_equal_ci(value, "50") ||
		string_equal_ci(value, "50HZ") ||
		value_is_false(value)) {
		return ZZ_VMODE_720x576;
	}

	return current;
}

static LONG tooltype_nonstandard_vsync(char **tool_types, LONG current) {
	const char *value = tooltype_value(tool_types, "ZZ9000-NS-VSYNC");
	if (value)
		current = value_is_false(value) ? 0 : 1;

	value = tooltype_value(tool_types, "ZZ9000-NS-VSYNC-NTSC");
	if (value)
		current = value_is_false(value) ? 0 : 2;

	value = tooltype_value(tool_types, "NSVSYNC");
	if (!value)
		return current;

	if (string_equal_ci(value, "NTSC"))
		return 2;
	if (string_equal_ci(value, "PAL") || value_is_true(value))
		return 1;
	if (value_is_false(value))
		return 0;

	return current;
}

static inline void zzwrite16(volatile uint16_t* reg, uint16_t value);
static inline void selectCardFeature(MNTZZ9KRegs* registers, UWORD feature);

static struct ConfigDev *find_unconfigured_configdev(struct ExpansionBase *ExpansionBase, UWORD manufacturer, UBYTE product) {
	struct ConfigDev *cd = NULL;

	while ((cd = (struct ConfigDev *)FindConfigDev(cd, manufacturer, product))) {
		if ((cd->cd_Flags & CDF_CONFIGME) && cd != reserved_cd)
			return cd;
	}

	return NULL;
}

static void apply_vcap_settings(MNTZZ9KRegs *regs, LONG mode) {
	if (mode == ZZ_VMODE_CENTERED_1080P_MATCH) {
		KPrintF("ZZ9000.card: centered 1280x1024, source-synced to the Amiga.\n");
		regs->videocap_vmode = ZZ_VMODE_CENTERED_1080P_MATCH;
	} else if (mode == ZZ_VMODE_CENTERED_1080P_50) {
		KPrintF("ZZ9000.card: centered 1280x1024 in 1080p50 mode.\n");
		regs->videocap_vmode = ZZ_VMODE_CENTERED_1080P_50;
	} else if (mode == ZZ_VMODE_CENTERED_1080P_60) {
		KPrintF("ZZ9000.card: centered 1280x1024 in 1080p60 mode.\n");
		regs->videocap_vmode = ZZ_VMODE_CENTERED_1080P_60;
	} else if (mode == ZZ_VMODE_800x600) {
		KPrintF("ZZ9000.card: 800x600 60hz scandoubler mode.\n");
		regs->videocap_vmode = ZZ_VMODE_800x600;
	} else {
		KPrintF("ZZ9000.card: 720x576 50hz scandoubler mode.\n");
		regs->videocap_vmode = ZZ_VMODE_720x576;
	}
}

static void apply_nonstandard_vsync_settings(MNTZZ9KRegs *regs, LONG nonstandard_vsync_mode) {
	selectCardFeature(regs, CARD_FEATURE_NONSTANDARD_VSYNC);
	zzwrite16(&regs->set_feature_status, (UWORD)nonstandard_vsync_mode);
}

static void apply_card_settings(struct BoardInfo *b, char **tool_types) {
	if (!b || !b->RegisterBase)
		return;

	MNTZZ9KRegs *regs = (MNTZZ9KRegs *)b->RegisterBase;

	b->CardData[ZZ_CARD_DATA_VCAP_MODE] = zz_vcap_mode_sanitize(
		(UWORD)tooltype_vcap_mode(tool_types,
			(LONG)b->CardData[ZZ_CARD_DATA_VCAP_MODE]),
		(UWORD)b->CardData[ZZ_CARD_DATA_FW_CAPABILITIES]);
	/* Any explicit legacy sync tooltype selects the legacy full/native
	 * identity; the firmware's runtime sync setter intentionally clears
	 * centered mode. */
	if (zz_vcap_mode_is_centered(
			(UWORD)b->CardData[ZZ_CARD_DATA_VCAP_MODE]) &&
		(tooltype_value(tool_types, "ZZ9000-NS-VSYNC") ||
		 tooltype_value(tool_types, "ZZ9000-NS-VSYNC-NTSC") ||
		 tooltype_value(tool_types, "NSVSYNC")))
		b->CardData[ZZ_CARD_DATA_VCAP_MODE] = ZZ_VMODE_800x600;
	b->CardData[ZZ_CARD_DATA_NSVSYNC] = tooltype_nonstandard_vsync(
		tool_types, (LONG)b->CardData[ZZ_CARD_DATA_NSVSYNC]);

	apply_vcap_settings(regs, (LONG)b->CardData[ZZ_CARD_DATA_VCAP_MODE]);
	if (!zz_vcap_mode_is_centered(
			(UWORD)b->CardData[ZZ_CARD_DATA_VCAP_MODE]))
		apply_nonstandard_vsync_settings(regs,
			(LONG)b->CardData[ZZ_CARD_DATA_NSVSYNC]);
}

static inline UWORD abs_word(WORD value) {
	return (value < 0) ? (UWORD)(-((LONG)value)) : (UWORD)value;
}

static inline UWORD line_length(const struct Line *line) {
	if (line->Length)
		return line->Length;

	UWORD dx = abs_word(line->dX);
	UWORD dy = abs_word(line->dY);
	return (dx >= dy) ? dx : dy;
}

static inline uint16_t planar_line_bytes(SHORT x, SHORT w) {
	return ((((UWORD)x) & 0x07) + (UWORD)w + 7) >> 3;
}

static inline uint16_t planar_line_bytes_padded(SHORT w) {
	// P2C keeps the historical two-byte padding; the firmware's
	// source byte wrap check otherwise fires at the end of most scanlines.
	return (((UWORD)w) >> 3) + 2;
}

static inline void zzwrite16(volatile uint16_t* reg, uint16_t value) {
	*reg = value;
}

static inline void zzwrite32(volatile uint16_t* reg, uint32_t value) {
	uint16_t *v = (uint16_t *)&value;
	reg[0] = v[0];
	reg[1] = v[1];
}
static inline void gfxdata_byte_cache_reset(void) {
	gfxdata_byte_cache.owner = NULL;
	gfxdata_byte_cache.valid = 0;
}

static inline void writeGfxDataByte(volatile struct GFXData *gfxdata,
	volatile uint8 *field, uint8 slot, uint8 value) {
	uint8 bit = (uint8)(1U << slot);

	if (gfxdata_byte_cache.owner != gfxdata) {
		gfxdata_byte_cache.owner = gfxdata;
		gfxdata_byte_cache.valid = 0;
	}

	if (!(gfxdata_byte_cache.valid & bit) ||
		gfxdata_byte_cache.value[slot] != value) {
		*field = value;
		gfxdata_byte_cache.value[slot] = value;
		gfxdata_byte_cache.valid |= bit;
	}
}

/* u8_user[2] is firmware-owned by codec operations and u8_user[3] doubles
 * as DrawLine padding and the ALLOC clear flag. Keep those writes direct. */
static inline void writeGfxDataU8(volatile struct GFXData *gfxdata,
	uint8 index, uint8 value) {
	writeGfxDataByte(gfxdata, &gfxdata->u8_user[index], index, value);
}

static inline void writeGfxDataMask(volatile struct GFXData *gfxdata,
	uint8 value) {
	writeGfxDataByte(gfxdata, &gfxdata->mask, GFXDATA_BYTE_MASK, value);
}

static inline void writeGfxDataMinterm(volatile struct GFXData *gfxdata,
	uint8 value) {
	writeGfxDataByte(gfxdata, &gfxdata->minterm, GFXDATA_BYTE_MINTERM, value);
}

// Assuming that it takes longer to write the same value through slow ZorroII register access again
// than comparing it with a cached value in FAST RAM, these routines should speed up things a lot.
static inline void writeBlitterSrcOffset(MNTZZ9KRegs* registers, ULONG offset) {
	static ULONG old = 0;
	if (offset != old) {
		zzwrite32(&registers->blitter_src_hi, offset);
		old = offset;
	}
}

static inline void writeBlitterDstOffset(MNTZZ9KRegs* registers, ULONG offset) {
	static ULONG old = 0;
	if (offset != old) {
		zzwrite32(&registers->blitter_dst_hi, offset);
		old = offset;
	}
}

static inline void writeBlitterRGB(MNTZZ9KRegs* registers, ULONG color) {
	static ULONG old = 0;
	if (color != old) {
		zzwrite32(&registers->blitter_rgb_hi, color);
		old = color;
	}
}

static inline void writeBlitterSrcPitch(MNTZZ9KRegs* registers, UWORD srcpitch) {
	if (blitter_cache_src_pitch_write_needed(&blitter_register_cache,
			registers, srcpitch)) {
		zzwrite16(&registers->blitter_src_pitch, srcpitch);
	}
}

static inline void writeBlitterRGB2(MNTZZ9KRegs* registers, ULONG color) {
	if (blitter_cache_write32_needed(&blitter_register_cache, registers,
			BLITTER_CACHE_RGB2, &blitter_register_cache.rgb2, color)) {
		zzwrite32(&registers->blitter_rgb2_hi, color);
	}
}

static inline void writeBlitterUser1(MNTZZ9KRegs* registers, UWORD value) {
	if (blitter_cache_write16_needed(&blitter_register_cache, registers,
			BLITTER_CACHE_USER1, &blitter_register_cache.user1, value)) {
		zzwrite16(&registers->blitter_user1, value);
	}
}

/* USER1 doubles as the selector for set_feature_status. Feature commands must
 * always reach the card, then leave USER1 invalid in the blitter cache because
 * the selector is not a reusable blitter operand. */
static inline void selectCardFeature(MNTZZ9KRegs* registers, UWORD feature) {
	zzwrite16(&registers->blitter_user1, feature);
	blitter_cache_invalidate(&blitter_register_cache, registers,
		BLITTER_CACHE_USER1);
}

static inline void writeBlitterUser2(MNTZZ9KRegs* registers, UWORD value) {
	if (blitter_cache_write16_needed(&blitter_register_cache, registers,
			BLITTER_CACHE_USER2, &blitter_register_cache.user2, value)) {
		zzwrite16(&registers->blitter_user2, value);
	}
}

/* The line work-variable seed changes almost every line, so caching it would
 * never hit; write it through directly. */
static inline void writeBlitterUser3(MNTZZ9KRegs* registers, UWORD value) {
	zzwrite16(&registers->blitter_user3, value);
}

static inline void writeBlitterDstPitch(MNTZZ9KRegs* registers, UWORD dstpitch) {
	static UWORD old = 0;
	if (dstpitch != old) {
		zzwrite16(&registers->blitter_row_pitch, dstpitch);
		old = dstpitch;
	}
}

static inline void writeBlitterColorMode(MNTZZ9KRegs* registers, UWORD colormode) {
	static UWORD old = MNTVA_COLOR_32BIT;
	if (colormode != old) {
		zzwrite16(&registers->blitter_colormode, colormode);
		old = colormode;
	}
}

static inline void writeBlitterX1(MNTZZ9KRegs* registers, UWORD x) {
	static UWORD old = 0;
	if (x != old) {
		zzwrite16(&registers->blitter_x1, x);
		old = x;
	}
}

static inline void writeBlitterY1(MNTZZ9KRegs* registers, UWORD y) {
	static UWORD old = 0;
	if (y != old) {
		zzwrite16(&registers->blitter_y1, y);
		old = y;
	}
}

static inline void writeBlitterX2(MNTZZ9KRegs* registers, UWORD x) {
	static UWORD old = 0;
	if (x != old) {
		zzwrite16(&registers->blitter_x2, x);
		old = x;
	}
}

static inline void writeBlitterY2(MNTZZ9KRegs* registers, UWORD y) {
	static UWORD old = 0;
	if (y != old) {
		zzwrite16(&registers->blitter_y2, y);
		old = y;
	}
}

static inline void writeBlitterX3(MNTZZ9KRegs* registers, UWORD x) {
	static UWORD old = 0;
	if (x != old) {
		zzwrite16(&registers->blitter_x3, x);
		old = x;
	}
}

static inline void writeBlitterY3(MNTZZ9KRegs* registers, UWORD y) {
	static UWORD old = 0;
	if (y != old) {
		zzwrite16(&registers->blitter_y3, y);
		old = y;
	}
}

#define ZZWRITE16(a, b) *a = b;

#define ZZWRITE32(b, c) \
	zzwrite16(b##_hi, ((uint16_t *)&c)[0]); \
	zzwrite16(b##_lo, ((uint16_t *)&c)[1]);

// useful for debugging
void waitclick() {
#define CIAAPRA ((volatile uint8_t*)0xbfe001)
	// bfe001 http://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node012E.html
	while (!(*CIAAPRA & (1<<6))) {
		// wait for left mouse button pressed
	}
	while ((*CIAAPRA & (1<<6))) {
		// wait for left mouse button released
	}
}

__saveds struct GFXBase* __attribute__((used)) InitLib(__REGA6(struct ExecBase *sysbase),
														 __REGA0(BPTR seglist),
														 __REGD0(struct GFXBase *exb))
{
	_gfxbase = exb;
	_gfxbase->libNode.lib_Node.ln_Name = device_name;
	_gfxbase->libNode.lib_Version = DEVICE_VERSION;
	_gfxbase->libNode.lib_Revision = DEVICE_REVISION;
	_gfxbase->libNode.lib_IdString = device_id_string;
	_gfxbase->Name = gfxname;
	SysBase = *(struct ExecBase **)4L;
	return _gfxbase;
}

__saveds struct GFXBase* __attribute__((used)) OpenLib(__REGA6(struct GFXBase *gfxbase))
{
	gfxbase->libNode.lib_OpenCnt++;
	gfxbase->libNode.lib_Flags &= ~LIBF_DELEXP;

	return gfxbase;
}

BPTR __saveds __attribute__((used)) CloseLib(__REGA6(struct GFXBase *gfxbase))
{
	gfxbase->libNode.lib_OpenCnt--;

	if (!gfxbase->libNode.lib_OpenCnt) {
		if (gfxbase->libNode.lib_Flags & LIBF_DELEXP) {
			return (ExpungeLib(gfxbase));
		}
	}
	return 0;
}

BPTR __saveds __attribute__((used)) ExpungeLib(__REGA6(struct GFXBase *exb))
{
	BPTR seglist;
	struct ExecBase *SysBase = *(struct ExecBase **)4L;

	if(!exb->libNode.lib_OpenCnt) {
		ULONG negsize, possize, fullsize;
		UBYTE *negptr = (UBYTE *)exb;

		seglist = exb->segList;

		Remove((struct Node *)exb);

		negsize	 = exb->libNode.lib_NegSize;
		possize	 = exb->libNode.lib_PosSize;
		fullsize = negsize + possize;
		negptr	-= negsize;

		FreeMem(negptr, fullsize);
		return(seglist);
	}

	exb->libNode.lib_Flags |= LIBF_DELEXP;
	return 0;
}

ULONG ExtFuncLib(void)
{
	return 0;
}

#define LOADLIB(a, b) if ((a = (struct a*)OpenLibrary((STRPTR)b,0L))==NULL) { \
		KPrintF((STRPTR)"ZZ9000.card: Failed to load %s.\n", b); \
		goto cleanup; \
	} \


int __attribute__((used)) FindCard(__REGA0(struct BoardInfo* b)) {
	struct ConfigDev* cd = NULL;
	struct ExpansionBase *ExpansionBase = NULL;
	struct ExecBase *SysBase = *(struct ExecBase **)4L;
	int result = 0;

	LOADLIB(ExpansionBase, "expansion.library");

	b->CardFlags = 0;
	b->CardData[ZZ_CARD_DATA_GFXDATA] = 0;
	b->CardData[ZZ_CARD_DATA_VCAP_MODE] = ZZ_VMODE_800x600;
	b->CardData[ZZ_CARD_DATA_NSVSYNC] = 0;
	b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH] = 0;
	b->CardData[ZZ_CARD_DATA_DISPLAY_ENABLED] = 1;
	b->CardData[ZZ_CARD_DATA_SECONDARY_PALETTE] = 0;
	b->CardData[ZZ_CARD_DATA_OFFSCREEN_BITMAPS] = 0;
	b->CardData[ZZ_CARD_DATA_VIDEO_OVERLAY] = 0;
	b->CardData[ZZ_CARD_DATA_APERTURE_INFO] = 0;
	b->CardData[ZZ_CARD_DATA_TEMPLATE_OFFSET] = 0;
	b->CardData[ZZ_CARD_DATA_PIP_OFFSET] = 0;
	b->CardData[ZZ_CARD_DATA_PIP_SIZE] = 0;
	b->CardData[ZZ_CARD_DATA_FW_CAPABILITIES] = 0;
	b->CardData[ZZ_CARD_DATA_GC_REJECTED] = 0;
	b->CardData[ZZ_CARD_DATA_MISTER_REGS] = 0;
	b->CardData[ZZ_CARD_DATA_MISTER_ACCEL] = 0;
	zz_overlay_hooks_enabled = FALSE;
	memset(&zz_z2_pip_request, 0, sizeof(zz_z2_pip_request));
	zz_z2_pip_bitmap = NULL;

	/* Minimig Z3 RTG board: framebuffer at board offset 0, rtg.v regs+CLUT
	 * at +$800000 (control block +$100). */
	cd = find_unconfigured_configdev(ExpansionBase, MZZ_RTG_MANUFACTURER, MZZ_RTG_PRODUCT);
	if (!cd) {
		KPrintF("MiSTerZZ.card: Minimig Z3 RTG board not found!\n");
		goto cleanup;
	}

	b->MemoryBase = (uint8_t *)cd->cd_BoardAddr;
	b->RegisterBase = (void *)((uint8_t *)cd->cd_BoardAddr + MZZ_REGS_OFFSET);
	b->CardData[ZZ_CARD_DATA_MISTER_REGS] =
		(ULONG)((uint8_t *)cd->cd_BoardAddr + MZZ_REGS_OFFSET + MZZ_REG_SUB_OFFSET);
	if (mister_regs(b)[MISTER_REG_ID / 2] != MISTER_RTG_ID) {
		KPrintF("MiSTerZZ.card: RTG register block not answering\n");
		goto cleanup;
	}

	/* P96 owns the window below the shared mailbox/staging area. */
	zz_card_window_size = MZZ_P96_MEMORY_SIZE;
	b->MemorySize = MZZ_P96_MEMORY_SIZE;
	b->MemorySpaceBase = b->MemoryBase;
	b->MemorySpaceSize = MZZ_P96_MEMORY_SIZE;
	b->CardFlags |= CARDFLAG_ZORRO_3;
	b->CardData[ZZ_CARD_DATA_GFXDATA] = (ULONG)((uint8_t *)b->MemoryBase + Z3_GFXDATA_ADDR);

	b->CardData[ZZ_CARD_DATA_MISTER_ACCEL] = mister_attach(b);
	*mister_mbox_field(b, MZZ_MB_DRV_STATE) =
		b->CardData[ZZ_CARD_DATA_MISTER_ACCEL] ? MZZ_DRV_ATTACHED : 0;

	KPrintF(device_id_string);
	KPrintF("MiSTerZZ.card: RTG board at %lx, ARM blitter %s\n",
		(ULONG)cd->cd_BoardAddr, b->CardData[ZZ_CARD_DATA_MISTER_ACCEL] ? "on" : "off");

	cd->cd_Flags &= ~CDF_CONFIGME;
	reserved_cd = cd;
	result = 1;

cleanup:
	if (ExpansionBase) CloseLibrary((struct Library *)ExpansionBase);

	return result;
}

#define gfxdata zz_gfxdata(b)

int __attribute__((used)) InitCard(__REGA0(struct BoardInfo* b), __REGA1(char **tool_types)) {
	int i;

	b->CardBase = (struct CardBase *)_gfxbase;
	b->ExecBase = SysBase;
	/* Same identity MiSTer.card reports (board name "MiSTer", type 10):
	 * Picasso96Settings keys the screen modes on these, so an existing
	 * MiSTer.card setup keeps its modes. */
	b->BoardName = "MiSTer";
	b->BoardType = 10;
	b->PaletteChipType = 0;
	b->GraphicsControllerType = 0;

	b->Flags |= BIF_GRANTDIRECTACCESS | BIF_INDISPLAYCHAIN | BIF_CACHEMODECHANGE;
	if (mister_accel(b))
		b->Flags |= BIF_BLITTER;

	b->MoniSwitch = (UWORD)b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH];
	b->RGBFormats = ZZ_SUPPORTED_RGB_FORMATS;
	b->SoftSpriteFlags = 0;
	b->BitsPerCannon = 8;
	b->MouseSaveBuffer = (UBYTE *)b->MemoryBase + MZZ_MOUSESAVE_OFFSET;

	/* rtg.v: 12-bit HSIZE/VSIZE; limits as in MiSTer.card. */
	for(i = 0; i < MAXMODES; i++) {
		b->MaxHorValue[i] = 4095;
		b->MaxVerValue[i] = 4095;
		b->MaxHorResolution[i] = 2048;
		b->MaxVerResolution[i] = 2048;
		b->PixelClockCount[i] = 1;
	}

	b->MemoryClock = 113440000;

	b->SetSwitch = (void *)SetSwitch;
	b->SetColorArray = (void *)SetColorArray;
	b->SetDAC = (void *)SetDAC;
	b->SetGC = (void *)SetGC;
	b->SetPanning = (void *)SetPanning;
	b->CalculateBytesPerRow = (void *)CalculateBytesPerRow;
	b->CalculateMemory = (void *)CalculateMemory;
	b->GetCompatibleFormats = (void *)GetCompatibleFormats;
	b->SetDisplay = (void *)SetDisplay;

	b->ResolvePixelClock = (void *)ResolvePixelClock;
	b->GetPixelClock = (void *)GetPixelClock;
	b->SetClock = (void *)SetClock;

	b->SetMemoryMode = (void *)SetMemoryMode;
	b->SetWriteMask = (void *)SetWriteMask;
	b->SetClearMask = (void *)SetClearMask;
	b->SetReadPlane = (void *)SetReadPlane;

	b->WaitVerticalSync = (void *)WaitVerticalSync;
	b->GetVSyncState = (void *)GetVSyncState;

	/* Accelerated hooks: the official ZZ9000 Z3 paths. Installed even when
	 * the ARM blitter did not attach - each one drops to the P96 software
	 * default by itself in that case (blit_colormode / mister_accel). */
	b->WaitBlitter = (void *)WaitBlitter;
	b->BlitPlanar2Chunky = (void *)BlitPlanar2Chunky;
	b->BlitPlanar2Direct = (void *)BlitPlanar2Direct;
	b->FillRect = (void *)FillRect;
	b->InvertRect = (void *)InvertRect;
	b->BlitRect = (void *)BlitRect;
	b->BlitTemplate = (void *)BlitTemplate;
	b->BlitPattern = (void *)BlitPattern;
	b->DrawLine = (void *)DrawLine;
	b->BlitRectNoMaskComplete = (void *)BlitRectNoMaskComplete;

	/* Not offered on MiSTer (no ZZ9000 video formatter / ARM SDK):
	 * hardware sprite, video capture switch, screen split, secondary
	 * palette, DPMS, off-screen bitmap allocator, YUV/PIP overlay. */

	blitter_cache_reset(&blitter_register_cache);
	gfxdata_byte_cache_reset();

	*mister_mbox_field(b, MZZ_MB_DRV_STATE) |= MZZ_DRV_INITCARD |
		((b->Flags & BIF_BLITTER) ? MZZ_DRV_BLITTER : 0);

	return 1;
}

// None of these five really have to do anything.
void SetDAC (__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) {
	/* MiSTer: the scanout pixel format lives in rtg.v's FORMAT register. */
	if (!b || !mister_regs(b) || format >= ARRAY_SIZE(mister_format))
		return;
	mister_regs(b)[MISTER_REG_FORMAT / 2] = mister_format[format];
}
void WaitBlitter (__REGA0(struct BoardInfo *b)) {
	/* MiSTer: mister_kick() only returns once the ARM has finished the op
	 * and the FPGA CPU cache has been dropped, so there is never a blit in
	 * flight here. */
}
void SetClock (__REGA0(struct BoardInfo *b)) { }
void SetMemoryMode (__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) { }
void SetWriteMask (__REGA0(struct BoardInfo *b), __REGD0(UBYTE mask)) { }
void SetClearMask (__REGA0(struct BoardInfo *b), __REGD0(UBYTE mask)) { }
void SetReadPlane (__REGA0(struct BoardInfo *b), __REGD0(UBYTE plane)) { }

// Optional dummy read for tricking the 68k cache on processors with occasional garbage output on screen
#ifdef DUMMY_CACHE_READ
	#define dmy_cache memcpy(dummies, (uint32_t *)(uint32_t)0x7F00000, 4);
#else
	#define dmy_cache
#endif

static void zz_p96_mode_of(const struct ModeInfo *mode_info,
	uint32_t pixel_clock, struct zz_p96_mode *p96) {
	p96->width = mode_info->Width;
	p96->height = mode_info->Height;
	p96->hor_total = mode_info->HorTotal;
	p96->hor_sync_start = mode_info->HorSyncStart;
	p96->hor_sync_size = mode_info->HorSyncSize;
	p96->ver_total = mode_info->VerTotal;
	p96->ver_sync_start = mode_info->VerSyncStart;
	p96->ver_sync_size = mode_info->VerSyncSize;
	p96->flags = mode_info->Flags;
	p96->pixel_clock = pixel_clock;
}

static uint16_t zz_firmware_capabilities(struct BoardInfo *b) {
	return (uint16_t)b->CardData[ZZ_CARD_DATA_FW_CAPABILITIES];
}

static void zz_custom_reg_write(void *context, uint16_t offset, uint16_t value) {
	zzwrite16((volatile uint16_t *)((uint32_t)context + offset), value);
}

static uint16_t zz_custom_reg_status(void *context) {
	return zz9000_read_reg16((ULONG)context, ZZ_CUSTOM_REG_COMMIT);
}

static inline void sanitize_mode_flags(struct ModeInfo *mode_info) {
	mode_info->Flags &= (UBYTE)~(GMF_INTERLACE | GMF_DOUBLECLOCK);
}

void SetGC(__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(BOOL border)) {
	if (!b)
		return;
	b->CardData[ZZ_CARD_DATA_GC_REJECTED] = 1;
	if (!mode_info || !mister_regs(b))
		return;

	/* MiSTer: the scaler generates the output timing; the core only needs
	 * the source geometry (as MiSTer.card). STRIDE is rewritten by
	 * SetPanning from the real bitmap pitch. */
	volatile UWORD *regs = mister_regs(b);
	regs[MISTER_REG_STRIDE / 2] = mister_pitch(mode_info->Width, b->RGBFormat);
	regs[MISTER_REG_HSIZE / 2] = mode_info->Width;
	regs[MISTER_REG_VSIZE / 2] = mode_info->Height;

	b->ModeInfo = mode_info;
	b->Border = border;
	b->CardData[ZZ_CARD_DATA_GC_REJECTED] = 0;
}

UWORD SetSwitch(__REGA0(struct BoardInfo *b), __REGD0(UWORD enabled)) {
	UWORD old = b ? (UWORD)b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH] : 1;

	if (!b || !mister_regs(b))
		return old;

	/* MiSTer: rtg.v ENABLE switches the scaler between the RTG framebuffer
	 * (1) and the native Amiga display (0). */
	b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH] = enabled ? 1 : 0;
	b->MoniSwitch = (UWORD)b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH];
	mister_regs(b)[MISTER_REG_ENABLE / 2] = enabled ? 1 : 0;

	return old;
}

void SetPanning(__REGA0(struct BoardInfo *b), __REGA1(UBYTE *addr), __REGD0(UWORD width), __REGD1(WORD x_offset), __REGD2(WORD y_offset), __REGD4(UWORD height), __REGD7(RGBFTYPE format)) {
	if (!b || !mister_regs(b)) return;

	/* Width and format cannot identify which framebuffer belongs to a
	 * refused mode. Resume panning only after a successful SetGC. */
	if (b->CardData[ZZ_CARD_DATA_GC_REJECTED])
		return;

	b->XOffset = x_offset;
	b->YOffset = y_offset;

	/* MiSTer: rtg.v scans out from an ARM physical address with its own
	 * stride, so panning is just the start of the visible area. */
	volatile UWORD *regs = mister_regs(b);
	ULONG bpp = mister_bytes_per_pixel(format);
	ULONG pitch = mister_pitch(width, format);
	ULONG offset = ((ULONG)addr - (ULONG)b->MemoryBase) +
		(ULONG)y_offset * pitch + (ULONG)x_offset * bpp;
	ULONG phys = MZZ_FB_PHYS + offset;

	regs[MISTER_REG_STRIDE / 2] = (UWORD)pitch;
	*(volatile ULONG *)&regs[MISTER_REG_ADDRESS / 2] = phys;
}

void SetColorArray(__REGA0(struct BoardInfo *b), __REGD0(UWORD start), __REGD1(UWORD num)) {
	if (!b || !num || !mister_regs(b) || start >= 256)
		return;

	/* MiSTer: one CLUT in rtg.v, 256 x $00RRGGBB longwords. */
	volatile ULONG *pal = (volatile ULONG *)((UBYTE *)mister_regs(b) + MISTER_REG_PALETTE);
	UWORD end = (start + num > 256) ? 256 : start + num;

	for (UWORD i = start; i < end; i++)
		pal[i] = ((ULONG)b->CLUT[i].Red << 16) | ((ULONG)b->CLUT[i].Green << 8) |
			b->CLUT[i].Blue;
}

uint16_t calc_pitch_bytes(uint16_t w, uint16_t colormode) {
	switch (colormode) {
		case MNTVA_COLOR_8BIT:
			return w;
		case MNTVA_COLOR_16BIT565:
		case MNTVA_COLOR_15BIT:
			return w << 1;
		case MNTVA_COLOR_32BIT:
			return w << 2;
	}

	return 0;
}

uint16_t pitch_to_shift(uint16_t p) {
	if (p == 8192) return 13;
	if (p == 4096) return 12;
	if (p == 2048) return 11;
	if (p == 1024) return 10;
	if (p == 512) return 9;
	if (p == 256)	return 8;
	return 0;
}

UWORD CalculateBytesPerRow(__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(UWORD width), __REGD1(UWORD height), __REGD7(RGBFTYPE format)) {
	if (!b)
		return 0;
	/* packed YUV 4:2:2 PIP sources are 2 bytes/pixel, and the pitch
	 * MUST be exactly width*2: P96 sizes the managed source bitmap
	 * through this hook (returning 0 made it fall back to 4
	 * bytes/pixel), and RiVA hardcodes its write modulo as width*2
	 * because cgxvideo has no modulo attribute (RendererCGXInit.i,
	 * "modulo.... (cgx sux!)") - any padding breaks every VLayer
	 * writer with that assumption */
	if (zz_overlay_hooks_enabled && zz_overlay_variant(format) >= 0)
		return (UWORD)zz_offscreen_bytes_per_row(format, width);
	if (!supported_rgb_format(format))
		return 0;

	return mister_pitch(width, format);
}

APTR CalculateMemory(__REGA0(struct BoardInfo *b), __REGA1(APTR addr), __REGD0(struct RenderInfo *r), __REGD7(RGBFTYPE format)) {
	if (!b)
		return NULL;
	if (!supported_rgb_format(format) &&
			!(zz_overlay_hooks_enabled && zz_overlay_variant(format) >= 0))
		return NULL;

	return addr;
}

ULONG GetCompatibleFormats(__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) {
	ULONG mask = b ? b->RGBFormats : ZZ_SUPPORTED_RGB_FORMATS;

	/* the packed YUV formats are valid PIP overlay SOURCES (never
	 * framebuffer formats; b->RGBFormats stays untouched) */
	if (zz_overlay_hooks_enabled)
		mask |= ZZ_OVERLAY_SOURCE_FORMATS;

	if (format >= 32 || !(mask & (1UL << format)))
		return 0;

	return mask;
}

UWORD SetDisplay(__REGA0(struct BoardInfo *b), __REGD0(UWORD enabled)) {
	UWORD old = b ? (UWORD)b->CardData[ZZ_CARD_DATA_DISPLAY_ENABLED] : 1;

	if (b)
		b->CardData[ZZ_CARD_DATA_DISPLAY_ENABLED] = enabled ? 1 : 0;
	// No firmware display-blanking register is exposed; keep sync and report
	// the previous logical state as required by the P96 callback contract.
	return old;
}

void SetDPMSLevel(__REGA0(struct BoardInfo *b), __REGD0(ULONG level)) {
	if (!b || !b->RegisterBase || level > DPMS_OFF)
		return;

	MNTZZ9KRegs *registers = (MNTZZ9KRegs *)b->RegisterBase;
	selectCardFeature(registers, CARD_FEATURE_DPMS);
	zzwrite16(&registers->set_feature_status, (UWORD)level);
}

/* MiSTer: the scaler produces the output timing, so every mode gets the
 * same nominal pixel clock (as MiSTer.card). */
#define MISTER_PIXEL_CLOCK 100000000UL

LONG ResolvePixelClock(__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(ULONG pixel_clock), __REGD7(RGBFTYPE format)) {
	if (!mode_info || !supported_rgb_format(format))
		return -1;

	sanitize_mode_flags(mode_info);
	mode_info->PixelClock = MISTER_PIXEL_CLOCK;
	mode_info->pll1.Clock = 0;
	mode_info->pll2.ClockDivide = 1;

	return 0;
}

ULONG GetPixelClock(__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(ULONG index), __REGD7(RGBFTYPE format)) {
	if (index != 0 || !supported_rgb_format(format))
		return 0;

	return MISTER_PIXEL_CLOCK;
}

/* MiSTer: rtg.v has no vblank status; use the Amiga beam like MiSTer.card.
 * VPOSR/VHPOSR as one longword: V8 in bit 16, V7..V0 in bits 15..8. */
#define VBLANK_WAIT_LIMIT 200000UL
#define AMIGA_VPOS() ((*(volatile ULONG *)0xDFF004 >> 8) & 0x1FF)

static inline BOOL vblank_state(struct BoardInfo *b) {
	return AMIGA_VPOS() < 26;
}

void WaitVerticalSync(__REGA0(struct BoardInfo *b), __REGD0(BOOL end)) {
	ULONG guard = VBLANK_WAIT_LIMIT;

	if (!b)
		return;

	if (end) {
		while (vblank_state(b) && --guard) { }
	} else {
		while (!vblank_state(b) && --guard) { }
	}
}

BOOL GetVSyncState(__REGA0(struct BoardInfo *b), __REGD0(BOOL expected)) {
	if (!b)
		return expected;

	return vblank_state(b);
}


// Direct horizontal line draw for Z2: bypasses register setup overhead.
static inline void draw_hline_z2(uint8_t *fb, WORD x, WORD y, WORD w,
	ULONG color, UWORD bpr, uint16_t colormode)
{
	uint8_t *row = fb + (UWORD)y * bpr;
	switch (colormode) {
	case MNTVA_COLOR_8BIT:
		memset(row + x, (uint8_t)(color >> 24), w);
		break;
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT: {
		uint16_t *p = (uint16_t *)row + x;
		uint16_t c16 = (uint16_t)color;
		WORD n = w;
		while (n--) *p++ = c16;
		break;
	}
	case MNTVA_COLOR_32BIT: {
		uint32_t *p = (uint32_t *)row + x;
		WORD n = w;
		while (n--) *p++ = color;
		break;
	}
	}
}

// Direct vertical line draw for Z2: bypasses register setup overhead.
static inline void draw_vline_z2(uint8_t *fb, WORD x, WORD y, WORD h,
	ULONG color, UWORD bpr, uint16_t colormode)
{
	switch (colormode) {
	case MNTVA_COLOR_8BIT: {
		uint8_t *p = fb + (UWORD)y * bpr + x;
		uint8_t c8 = (uint8_t)(color >> 24);
		while (h >= 4) {
			p[0] = c8; p[bpr] = c8; p[bpr*2] = c8; p[bpr*3] = c8;
			p += bpr * 4; h -= 4;
		}
		while (h--) { *p = c8; p += bpr; }
		break;
	}
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT: {
		uint16_t *p = (uint16_t *)(fb + (UWORD)y * bpr) + x;
		uint16_t c16 = (uint16_t)color;
		UWORD step = bpr >> 1;
		while (h >= 4) {
			p[0] = c16; p[step] = c16; p[step*2] = c16; p[step*3] = c16;
			p += step * 4; h -= 4;
		}
		while (h--) { *p = c16; p += step; }
		break;
	}
	case MNTVA_COLOR_32BIT: {
		uint32_t *p = (uint32_t *)(fb + (UWORD)y * bpr) + x;
		UWORD step = bpr >> 2;
		while (h >= 4) {
			p[0] = color; p[step] = color; p[step*2] = color; p[step*3] = color;
			p += step * 4; h -= 4;
		}
		while (h--) { *p = color; p += step; }
		break;
	}
	}
}

static inline void fill_rect_accel(struct BoardInfo *b, struct RenderInfo *r,
	WORD x, WORD y, WORD w, WORD h, ULONG color, UBYTE mask,
	RGBFTYPE format, uint16_t colormode)
{
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

		writeGfxDataU8(gfxdata, GFXDATA_U8_COLORMODE, (uint8_t)colormode);
		writeGfxDataMask(gfxdata, mask);

		gfxdata->rgb[0] = color;
		gfxdata->x[0] = x;
		gfxdata->x[1] = w;
		gfxdata->y[0] = y;
		gfxdata->y[1] = h;

		mister_kick(b, OP_FILLRECT);
	} else {
		// Don't waste ~11 ZorroII write cycles to draw a very small rectangle.
		// Use min dimension so that thin lines (w=1 or h=1) always use the
		// blitter when their length exceeds the threshold.
		WORD min_dim = (w < h) ? w : h;
		WORD max_dim = (w > h) ? w : h;
		if (min_dim < 6 && max_dim < 32) {
			b->FillRectDefault(b, r, x, y, w, h, color, mask, format);
			return;
		}
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);

		writeBlitterDstOffset(registers, offset);
		writeBlitterRGB(registers, color);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, colormode);
		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y);
		writeBlitterX2(registers, w);
		writeBlitterY2(registers, h);
		zzwrite16(&registers->blitter_op_fillrect, mask);
	}
}

void FillRect(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(ULONG color), __REGD5(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r) return;
	if (w<1 || h<1) return;

	uint16_t colormode = blit_colormode(b, format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->FillRectDefault)
			b->FillRectDefault(b, r, x, y, w, h, color, mask, format);
		return;
	}
	mask = direct_color_mask(colormode, mask);

	fill_rect_accel(b, r, x, y, w, h, color, mask, format, colormode);
}

void InvertRect(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r)
		return;

	uint16_t colormode = blit_colormode(b, format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->InvertRectDefault)
			b->InvertRectDefault(b, r, x, y, w, h, mask, format);
		return;
	}
	mask = direct_color_mask(colormode, mask);
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

		writeGfxDataU8(gfxdata, GFXDATA_U8_COLORMODE, (uint8_t)colormode);
		writeGfxDataMask(gfxdata, mask);

		gfxdata->x[0] = x;
		gfxdata->x[1] = w;
		gfxdata->y[0] = y;
		gfxdata->y[1] = h;

		mister_kick(b, OP_INVERTRECT);
	} else {
		// Don't waste ~9 ZorroII write cycles to invert a very small rectangle.
		WORD min_dim = (w < h) ? w : h;
		WORD max_dim = (w > h) ? w : h;
		if (min_dim < 6 && max_dim < 32) {
			b->InvertRectDefault(b, r, x, y, w, h, mask, format);
			return;
		}
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);

		writeBlitterDstOffset(registers, offset);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, colormode);

		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y);
		writeBlitterX2(registers, w);
		writeBlitterY2(registers, h);

		zzwrite16(&registers->blitter_op_invertrect, mask);
	}
}

// This function shall copy a rectangular image region in the video RAM.
void BlitRect(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD dx), __REGD3(WORD dy), __REGD4(WORD w), __REGD5(WORD h), __REGD6(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (w<1 || h<1) return;
	if (!b || !r) return;

	uint16_t colormode = blit_colormode(b, format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->BlitRectDefault)
			b->BlitRectDefault(b, r, x, y, dx, dy, w, h, mask, format);
		return;
	}
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache

		// RenderInfo describes the video RAM containing the source and target rectangle,
		gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->offset[GFXDATA_SRC] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

		// x/y are the top-left edge of the source rectangle,
		gfxdata->x[2] = x;
		gfxdata->y[2] = y;

		// dx/dy the top-left edge of the destination rectangle,
		gfxdata->x[0] = dx;
		gfxdata->y[0] = dy;

		// and w/h the dimensions of the rectangle to copy.
		gfxdata->x[1] = w;
		gfxdata->y[1] = h;

		// RGBFormat is the format of the source (and destination); this format shall not be taken from the RenderInfo.
		writeGfxDataU8(gfxdata, GFXDATA_U8_COLORMODE, (uint8_t)colormode);

		// Mask is a bitmask that defines which (logical) planes are affected by the copy for planar or chunky bitmaps. It can be ignored for direct color modes.
		writeGfxDataMask(gfxdata, mask);

		// Source and destination rectangle may be overlapping, a proper copy operation shall be performed in either case.
		mister_kick(b, OP_COPYRECT);
	} else {
		writeBlitterY1(registers, dy);
		writeBlitterY2(registers, h);
		writeBlitterY3(registers, y);

		writeBlitterX1(registers, dx);
		writeBlitterX2(registers, w);
		writeBlitterX3(registers, x);

		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, colormode | (mask << 8));

		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		writeBlitterSrcOffset(registers, offset);
		writeBlitterDstOffset(registers, offset);

		zzwrite16(&registers->blitter_op_copyrect, 1);
	}
}

// This function shall copy one rectangle in video RAM to another rectangle in video RAM using a mode given in d6. A mask is not applied.
void BlitRectNoMaskComplete(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *rs), __REGA2(struct RenderInfo *rt), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD dx), __REGD3(WORD dy), __REGD4(WORD w), __REGD5(WORD h), __REGD6(UBYTE minterm), __REGD7(RGBFTYPE format)) {
	if (w<1 || h<1) return;
	if (!b || !rs || !rt) return;

	uint16_t colormode = blit_colormode(b, format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->BlitRectNoMaskCompleteDefault)
			b->BlitRectNoMaskCompleteDefault(b, rs, rt, x, y, dx, dy, w, h, minterm, format);
		return;
	}
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	// b->BlitRectNoMaskCompleteDefault(b, rs, rt, x, y, dx, dy, w, h, minterm, format);
	// return;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache

		// The source region in video RAM is given by the source RenderInfo in a1 and a position within it in x and y.
		gfxdata->x[2] = x;
		gfxdata->y[2] = y;
		gfxdata->offset[GFXDATA_SRC] = ((uint32_t)rs->Memory - (uint32_t)b->MemoryBase);
		gfxdata->pitch[GFXDATA_SRC] = (rs->BytesPerRow >> 2);

		// The destination region in video RAM is given by the destinaton RenderInfo in a2 and a position within it in dx and dy.
		gfxdata->x[0] = dx;
		gfxdata->y[0] = dy;
		gfxdata->offset[GFXDATA_DST] = ((uint32_t)rt->Memory - (uint32_t)b->MemoryBase);
		gfxdata->pitch[GFXDATA_DST] = (rt->BytesPerRow >> 2);

		// The dimension of the rectangle to copy is in w and h.
		gfxdata->x[1] = w;
		gfxdata->y[1] = h;

		// The mode is in register d6, it uses the Amiga Blitter MinTerms encoding of the graphics.library.
		writeGfxDataMinterm(gfxdata, minterm);

		// The common RGBFormat of source and destination is in register d7, it shall not be taken from the source or destination RenderInfo.
		writeGfxDataU8(gfxdata, GFXDATA_U8_COLORMODE, (uint8_t)colormode);

		mister_kick(b, OP_COPYRECT_NOMASK);
	} else {
		writeBlitterY1(registers, dy);
		writeBlitterY2(registers, h);
		writeBlitterY3(registers, y);

		writeBlitterX1(registers, dx);
		writeBlitterX2(registers, w);
		writeBlitterX3(registers, x);

		writeBlitterColorMode(registers, colormode | (minterm << 8));

		writeBlitterSrcPitch(registers, rs->BytesPerRow >> 2);
		uint32_t offset = ((uint32_t)rs->Memory - (uint32_t)b->MemoryBase);
		writeBlitterSrcOffset(registers, offset);

		writeBlitterDstPitch(registers, rt->BytesPerRow >> 2);
		offset = ((uint32_t)rt->Memory - (uint32_t)b->MemoryBase);
		writeBlitterDstOffset(registers, offset);

		zzwrite16(&registers->blitter_op_copyrect, 2);
	}
}

void BlitTemplate(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Template *t), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r) return;
	if (w<1 || h<1) return;
	if (!t) return;

	uint16_t colormode = blit_colormode(b, format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->BlitTemplateDefault)
			b->BlitTemplateDefault(b, r, t, x, y, w, h, mask, format);
		return;
	}
	mask = direct_color_mask(colormode, mask);
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		writeBlitterDstOffset(registers, offset);
	}

	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	/* MiSTer: the staging area is MZZ_TEMPLATE_SIZE, not the ZZ9000's
	 * open-ended Z3 scratch space. */
	if ((b->CardFlags & CARDFLAG_ZORRO_3) &&
	    (!t->BytesPerRow ||
	     (uint32_t)t->BytesPerRow * (uint32_t)h > MZZ_TEMPLATE_SIZE)) {
		if (b->BlitTemplateDefault)
			b->BlitTemplateDefault(b, r, t, x, y, w, h, mask, format);
		return;
	}
	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		uint32_t staged_size;

		zz_template_addr = zz_z2_template_offset(b);
		if (!t->BytesPerRow ||
		    (uint32_t)h > UINT32_MAX / (uint32_t)t->BytesPerRow) {
			if (b->BlitTemplateDefault)
				b->BlitTemplateDefault(b, r, t, x, y, w, h, mask, format);
			return;
		}
		staged_size = (uint32_t)t->BytesPerRow * (uint32_t)h;
		if (!zz_z2_template_fits(staged_size)) {
			if (b->BlitTemplateDefault)
				b->BlitTemplateDefault(b, r, t, x, y, w, h, mask, format);
			return;
		}
	}

	memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), t->Memory, t->BytesPerRow * h);

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->x[0] = x;
		gfxdata->x[1] = w;
		gfxdata->x[2] = t->XOffset;
		gfxdata->y[0] = y;
		gfxdata->y[1] = h;

		gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
		gfxdata->pitch[GFXDATA_DST] = r->BytesPerRow;
		gfxdata->pitch[GFXDATA_SRC] = t->BytesPerRow;

		gfxdata->rgb[0] = t->FgPen;
		gfxdata->rgb[1] = t->BgPen;

		writeGfxDataU8(gfxdata, GFXDATA_U8_COLORMODE, (uint8_t)colormode);
		writeGfxDataU8(gfxdata, GFXDATA_U8_DRAWMODE, t->DrawMode);
		writeGfxDataMask(gfxdata, mask);

		mister_kick(b, OP_RECT_TEMPLATE);
	} else {
		writeBlitterSrcOffset(registers, zz_template_addr);

		writeBlitterRGB(registers, t->FgPen);
		writeBlitterRGB2(registers, t->BgPen);

		writeBlitterSrcPitch(registers, t->BytesPerRow);
		writeBlitterDstPitch(registers, r->BytesPerRow);
		writeBlitterColorMode(registers, colormode | (t->DrawMode << 8));
		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y);
		writeBlitterX2(registers, w);
		writeBlitterY2(registers, h);
		writeBlitterX3(registers, t->XOffset);

		zzwrite16(&registers->blitter_op_filltemplate, mask);
	}
}

void BlitPattern(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Pattern *pat), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r) return;
	if (w<1 || h<1) return;
	if (!pat) return;

	uint16_t colormode = blit_colormode(b, format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->BlitPatternDefault)
			b->BlitPatternDefault(b, r, pat, x, y, w, h, mask, format);
		return;
	}
	mask = direct_color_mask(colormode, mask);
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		writeBlitterDstOffset(registers, offset);
	}

	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = zz_z2_template_offset(b);
	}

	if (pat->Size > 15) return;
	memcpy((uint8_t*)(((uint32_t)b->MemoryBase) + zz_template_addr), pat->Memory, 2 * (1 << pat->Size));

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->x[0] = x;
		gfxdata->x[1] = w;
		gfxdata->x[2] = pat->XOffset;
		gfxdata->y[0] = y;
		gfxdata->y[1] = h;
		gfxdata->y[2] = pat->YOffset;

		gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
		gfxdata->pitch[GFXDATA_DST] = r->BytesPerRow;

		gfxdata->rgb[0] = pat->FgPen;
		gfxdata->rgb[1] = pat->BgPen;

		writeGfxDataU8(gfxdata, GFXDATA_U8_COLORMODE, (uint8_t)colormode);
		writeGfxDataU8(gfxdata, GFXDATA_U8_DRAWMODE, pat->DrawMode);
		gfxdata->user[0] = (1 << pat->Size);
		writeGfxDataMask(gfxdata, mask);

		mister_kick(b, OP_RECT_PATTERN);
	} else {
		writeBlitterSrcOffset(registers, zz_template_addr);

		writeBlitterRGB(registers, pat->FgPen);
		writeBlitterRGB2(registers, pat->BgPen);

		writeBlitterUser1(registers, mask);
		writeBlitterDstPitch(registers, r->BytesPerRow);
		writeBlitterColorMode(registers, colormode | (pat->DrawMode << 8));
		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y);
		writeBlitterX2(registers, w);
		writeBlitterY2(registers, h);
		writeBlitterX3(registers, pat->XOffset);
		writeBlitterY3(registers, pat->YOffset);

		zzwrite16(&registers->blitter_op_filltemplate, (1 << pat->Size) | 0x8000);
	}
}

void DrawLine(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Line *l), __REGD0(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !l || !r)
		return;

	/* Line does not expose the RastPort's FRST_DOT state, so an accelerated
	 * COMPLEMENT line cannot tell a fresh Draw from a connected Draw. Processing
	 * a shared vertex twice is observable with XOR. Keep every other mode on the
	 * fast path and let Picasso96 preserve exact join ownership for COMPLEMENT. */
	if (l->DrawMode & COMPLEMENT) {
		if (b->DrawLineDefault)
			b->DrawLineDefault(b, r, l, mask, format);
		return;
	}

	uint16_t colormode = blit_colormode(b, format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->DrawLineDefault)
			b->DrawLineDefault(b, r, l, mask, format);
		return;
	}

	/* P96 may hand us a clipped *segment* of a larger line: X,Y is the segment
	 * start, Length its major-axis pixel extent, dX,dY the FULL line delta, and
	 * Xorigin,Yorigin the entire line's start (see the iComp P96 Driver
	 * Development wiki). Length == 0 means the line is fully clipped away. */
	if (l->Length == 0)
		return;

	mask = direct_color_mask(colormode, mask);
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	if (l->DrawMode == 0 && l->LinePtrn == 0xFFFF && mask == 0xFF && l->dY == 0) {
		UWORD len = line_length(l);
		WORD x = l->X;
		WORD y = l->Y;
		WORD w = (WORD)(len + 1);

		if (l->dX < 0)
			x -= len;

		if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
			draw_hline_z2(r->Memory, x, y, w, l->FgPen,
				r->BytesPerRow, colormode);
			return;
		}
		fill_rect_accel(b, r, x, y, w, 1, l->FgPen, mask, format, colormode);
		return;
	}

	if (l->DrawMode == 0 && l->LinePtrn == 0xFFFF && mask == 0xFF && l->dX == 0) {
		UWORD len = line_length(l);
		WORD x = l->X;
		WORD y = l->Y;
		WORD h = (WORD)(len + 1);

		if (l->dY < 0)
			y -= len;

		if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
			draw_vline_z2(r->Memory, x, y, h, l->FgPen,
				r->BytesPerRow, colormode);
			return;
		}
		fill_rect_accel(b, r, x, y, 1, h, l->FgPen, mask, format, colormode);
		return;
	}

	/* Round-half-up Bresenham accumulator at the segment start, computed from
	 * geometry in magnitudes so it is octant-independent. P96/DrawLineDefault
	 * places pixel i at minor offset floor((2*i*S + L) / (2*L)) == round(i*S/L)
	 * with ties up; the firmware reproduces it with e += S; if (e >= L)
	 * { e -= L; minor-step; } seeded at the half-pixel bias L/2. The accumulator
	 * at a segment that starts k major / m minor units from the origin is
	 * e = S*k - L*m + L/2 == (S*k + L/2) mod L (L/2 for an unclipped segment),
	 * so a clipped segment resumes P96's staircase exactly. In [0, L), fits
	 * 16 bits. (Verified on ZZ9000 hardware: truncation, seed 0, was 1px off;
	 * round-half-up matches DrawLineDefault pixel-for-pixel.) */
	int line_dxa = l->dX < 0 ? -l->dX : l->dX;
	int line_dya = l->dY < 0 ? -l->dY : l->dY;
	int line_major = line_dxa >= line_dya;
	int line_l = line_major ? line_dxa : line_dya;
	int line_s = line_major ? line_dya : line_dxa;
	/* Xorigin/Yorigin are UWORD, but P96 encodes a line clipped at the top/left
	 * (its start above/left of the RenderInfo) with a negative origin. Cast
	 * through WORD so it sign-extends to int (e.g. -8 stays -8, not 65528),
	 * keeping line_k/line_m and err_seed correct for such clipped lines. X, Y,
	 * dX and dY are already WORD, so they sign-extend on their own. */
	int line_dmaj = line_major ? ((int)l->X - (int)(WORD)l->Xorigin)
				   : ((int)l->Y - (int)(WORD)l->Yorigin);
	int line_dmin = line_major ? ((int)l->Y - (int)(WORD)l->Yorigin)
				   : ((int)l->X - (int)(WORD)l->Xorigin);
	int line_k = line_dmaj < 0 ? -line_dmaj : line_dmaj;
	int line_m = line_dmin < 0 ? -line_dmin : line_dmin;
	UWORD err_seed = (UWORD)(line_s * line_k - line_l * line_m + line_l / 2);

	/* P96's PatternShift is an LSB-based bit index: DrawLineDefault draws the
	 * segment's first pixel with pattern bit (0x0001 << PatternShift). The
	 * firmware uses an MSB-based index, (0x8000 >> offset), so translate here.
	 * 15 - PatternShift makes (0x8000 >> (15 - shift)) == (0x0001 << shift),
	 * matching DrawLineDefault pixel-for-pixel. (HW-confirmed via the PatDiff
	 * readback: the card pattern was 1px off because P96 sends PatternShift=15
	 * for a fresh line and the firmware mirrored it to bit 0. The wiki's "shifts
	 * the pattern PatternShift bits to the left" is misleading, like its line
	 * rounding text.) */
	UWORD line_pat_off = (UWORD)((15 - (l->PatternShift & 15)) & 15);

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		_Static_assert(offsetof(struct GFXData, x) % 4 == 0 &&
			offsetof(struct GFXData, y) % 4 == 0 &&
			offsetof(struct GFXData, user) % 4 == 0,
			"Z3 descriptor pairs must be longword-aligned");
		dmy_cache
		gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

		writeGfxDataU8(gfxdata, GFXDATA_U8_COLORMODE, (uint8_t)colormode);
		/* With a full pattern and mask, JAM1 and JAM2 both write only
		 * FgPen. Keep inversion and complement on the mode-aware path;
		 * high DrawMode bits are ignored by the current line renderer. */
		UBYTE submitted_mode = l->DrawMode;
		if (l->LinePtrn == 0xFFFF && mask == 0xFF &&
		    !(submitted_mode & (COMPLEMENT | INVERSVID)))
			submitted_mode = 0;
		writeGfxDataU8(gfxdata, GFXDATA_U8_DRAWMODE, submitted_mode);
		gfxdata->u8_user[GFXDATA_U8_LINE_PATTERN_OFFSET] = line_pat_off;
		gfxdata->u8_user[GFXDATA_U8_LINE_PADDING] = l->pad;

		gfxdata->rgb[0] = l->FgPen;
		gfxdata->rgb[1] = l->BgPen;

		blitter_write_descriptor_pair(gfxdata->x, l->X, l->dX);
		blitter_write_descriptor_pair(gfxdata->y, l->Y, l->dY);
		blitter_write_descriptor_pair(gfxdata->user, l->Length, l->LinePtrn);
		blitter_write_descriptor_pair(gfxdata->user + 2,
			(line_pat_off << 8) | l->pad, err_seed);

		writeGfxDataMask(gfxdata, mask);

		mister_kick(b, OP_DRAWLINE);
	} else {
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		/* Solid lines (LinePtrn 0xFFFF, full mask, JAM1) dispatch to the
		 * firmware's draw_line_solid, which ignores BgPen and the pattern
		 * shift. Skip those two slow Z2 register writes for solid lines.
		 * X3 (LinePtrn) is still written: the firmware selects the solid
		 * path on rect_x3 == 0xFFFF, and the cache collapses the repeats. */
		int solid = (l->DrawMode == 0 && l->LinePtrn == 0xFFFF && mask == 0xFF);

		writeBlitterDstOffset(registers, offset);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);

		writeBlitterColorMode(registers, colormode | (l->DrawMode << 8));

		writeBlitterRGB(registers, l->FgPen);

		if (!solid)
			writeBlitterRGB2(registers, l->BgPen);

		writeBlitterX1(registers, l->X);
		writeBlitterY1(registers, l->Y);
		writeBlitterX2(registers, l->dX);
		writeBlitterY2(registers, l->dY);
		writeBlitterUser1(registers, l->Length);
		writeBlitterUser3(registers, err_seed);
		writeBlitterX3(registers, l->LinePtrn);
		if (!solid)
			writeBlitterY3(registers, line_pat_off | (l->pad << 8));

		zzwrite16(&registers->blitter_op_draw_line, mask);
	}
}

/* Off-screen bitmaps we place in card VRAM. `bm` must stay the first
 * member so the struct BitMap* P96 hands back is also a ZZBitMap*.
 * All live wrappers sit on zz_bitmap_list; the pointer-identity walk
 * decides ours/foreign, with magic + the Planes[0] range check kept as
 * consistency guards. */
struct ZZBitMap {
	struct BitMap bm;
	struct ZZBitMap *next;
	ULONG magic;
	ULONG card_offset;
	ULONG allocation_size;
	ULONG rgbformat;
	UWORD width, height;
};

static struct ZZBitMap *zz_bitmap_list = NULL;

static struct ZZBitMap *zz_bitmap_ours(struct BoardInfo *b, struct BitMap *bm) {
	struct ZZBitMap *zbm;

	if (!bm)
		return NULL;

	/* membership first: never read wrapper fields (they sit past the
	 * end of a plain struct BitMap) before proving bm is ours */
	for (zbm = zz_bitmap_list; zbm; zbm = zbm->next)
		if (&zbm->bm == bm)
			break;
	if (!zbm)
		return NULL;

	if (!zz_offscreen_range_is_ours(zbm->magic,
			(uint32_t)bm->Planes[0], (uint32_t)zbm->allocation_size,
			(uint32_t)b->MemoryBase,
			zz_card_window_size ? (uint32_t)zz_card_window_size :
			(uint32_t)b->MemorySize))
		return NULL;

	return zbm;
}

struct BitMap * ZZ_AllocBitMap(__REGA0(struct BoardInfo *b), __REGD0(ULONG width), __REGD1(ULONG height), __REGA1(struct TagItem *tags)) {
	BOOL z3 = (b->CardFlags & CARDFLAG_ZORRO_3) != 0;
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	ULONG rgbformat = RGBFB_CLUT;
	ULONG mode_width = 0;
	ULONG alignment = 0;
	BOOL constant_pitch = FALSE;
	BOOL clear_requested = FALSE;

	struct TagItem *tag = tags;
	while (tag && tag->ti_Tag != TAG_DONE) {
		switch (tag->ti_Tag) {
			case TAG_IGNORE: break;
			case TAG_SKIP: tag += tag->ti_Data; break;
			case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
			case ABMA_RGBFormat: rgbformat = tag->ti_Data; break;
			case ABMA_NoMemory: return NULL;
			/* a flag, not a pitch: the stride must not depend on the
			 * bitmap width (ABMA_ModeWidth carries the mode's width) */
			case ABMA_ConstantBytesPerRow: constant_pitch = tag->ti_Data != 0; break;
			case ABMA_ModeWidth: mode_width = tag->ti_Data; break;
			case ABMA_Alignment: alignment = tag->ti_Data; break;
			/* requests we cannot honor in card VRAM: CPU-owned
			 * fast-mem bitmaps, explicit system-memory bitmaps,
			 * caller-supplied memory, byte-swapped views -> NULL so
			 * P96 uses system RAM */
			case ABMA_UserPrivate:
			case ABMA_System:
			case ABMA_Memory:
			case ABMA_ConstantByteSwapping:
				if (tag->ti_Data) return NULL;
				break;
			/* ABMA_Clear maps to the firmware's conditional surface
			 * clear, active only after the init-time
			 * REG_ZZ_ALLOC_CLEAR_PROTOCOL handshake: u8_user[3]==1
			 * tells the firmware NOT to zero-fill (this caller did
			 * not ask). Without the handshake the firmware always
			 * clears, so older drivers keep BMF_CLEAR semantics. */
			case ABMA_Clear: clear_requested = tag->ti_Data != 0; break;
			default: break;
		}
		tag++;
	}

	if (!zz_rgbformat_bytes_per_pixel(rgbformat))
		return NULL;

	/* the firmware allocator starts surfaces 256-byte aligned; refuse
	 * alignment requests we cannot promise */
	if (alignment && !zz_offscreen_align_valid(alignment))
		return NULL;
	uint32_t pitch_align = (alignment > ZZ_OFFSCREEN_PITCH_ALIGN) ?
		alignment : ZZ_OFFSCREEN_PITCH_ALIGN;

	/* constant pitch: derive the stride from the display mode's width
	 * so every bitmap of that mode/format shares it; the padding is
	 * deterministic, which keeps it constant */
	ULONG pitch_width = width;
	if (constant_pitch && mode_width > pitch_width)
		pitch_width = mode_width;

	/* pitch computed locally rather than via CalculateBytesPerRow: the
	 * P96-facing callback sizes display modes and stays YUV-blind,
	 * while this path also allocates YUV overlay source bitmaps */
	uint32_t row_bytes = zz_offscreen_pad_pitch_to(
		zz_offscreen_bytes_per_row(rgbformat, pitch_width), pitch_align);
	uint16_t bytesperrow;
	uint32_t size;
	uint32_t card_offset;
	ULONG bitmap_width = width;
	ULONG bitmap_height = height;
	ULONG bitmap_format = rgbformat;

	if (!row_bytes || !height || row_bytes > UINT16_MAX ||
	    height > UINT32_MAX / row_bytes)
		return NULL;
	bytesperrow = (uint16_t)row_bytes;
	size = row_bytes * height;

	if (!z3) {
		/* Z2 exposes no general off-screen heap.  Only CreateFeature's
		 * nested P96-managed source allocation may claim the one fixed
		 * aperture pool, and only once. */
		if (!zz_z2_layout_active(b) || !zz_z2_pip_request.pending ||
		    zz_z2_pip_bitmap != NULL ||
		    !b->CardData[ZZ_CARD_DATA_PIP_SIZE] ||
		    height != zz_z2_pip_request.height ||
		    zz_z2_pip_request.pitch > UINT16_MAX ||
		    zz_z2_pip_request.size > b->CardData[ZZ_CARD_DATA_PIP_SIZE])
			return NULL;
		bytesperrow = (uint16_t)zz_z2_pip_request.pitch;
		size = (uint32_t)zz_z2_pip_request.size;
		card_offset = (uint32_t)b->CardData[ZZ_CARD_DATA_PIP_OFFSET];
		bitmap_width = zz_z2_pip_request.width;
		bitmap_height = zz_z2_pip_request.height;
		bitmap_format = zz_z2_pip_request.format;
	} else {
		dmy_cache
		writeGfxDataU8(gfxdata, 1, 1);
		/* 1 = caller did not ask for ABMA_Clear: firmware skips the
		 * zero-fill (AllocBitMap does not promise cleared memory, and
		 * a re-allocated smart-refresh save buffer must not black out
		 * the window it backs). 0 keeps the legacy cleared surface.
		 * Direct write, NOT writeGfxDataU8: byte cache slot 3 is
		 * MINTERM's, and DrawLine writes this same byte (line
		 * padding) directly -- routing through the cache helper would
		 * alias the minterm slot and let stale paddings suppress the
		 * flag write. */
		gfxdata->u8_user[3] = clear_requested ? 0 : 1;
		gfxdata->offset[1] = size;
		zzwrite16(&registers->blitter_acc_op, ACC_OP_ALLOC_SURFACE);
		card_offset = gfxdata->offset[0];
		if (!card_offset)
			return NULL;
	}

	struct ZZBitMap *zbm = (struct ZZBitMap *)AllocMem(sizeof(struct ZZBitMap), MEMF_PUBLIC | MEMF_CLEAR);
	if (!zbm) {
		if (z3) {
			gfxdata->offset[0] = card_offset;
			writeGfxDataU8(gfxdata, 0, 0);
			zzwrite16(&registers->blitter_acc_op, ACC_OP_FREE_SURFACE);
		}
		return NULL;
	}
	if (!z3) {
		memset((void *)((uint32_t)b->MemoryBase + card_offset), 0,
			size);
	}

	zbm->bm.BytesPerRow = bytesperrow;
	zbm->bm.Rows = bitmap_height;
	zbm->bm.Depth = (UBYTE)zz_rgbformat_depth(bitmap_format);
	zbm->bm.Planes[0] = (PLANEPTR)((uint32_t)b->MemoryBase + card_offset);
	zbm->magic = ZZ_OFFSCREEN_MAGIC;
	zbm->card_offset = card_offset;
	zbm->allocation_size = size;
	zbm->rgbformat = bitmap_format;
	zbm->width = bitmap_width;
	zbm->height = bitmap_height;
	zbm->next = zz_bitmap_list;
	zz_bitmap_list = zbm;
	if (!z3)
		zz_z2_pip_bitmap = zbm;

	return &zbm->bm;
}

BOOL ZZ_FreeBitMap(__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGA2(struct TagItem *tags)) {
	struct ZZBitMap *zbm = zz_bitmap_ours(b, bm);
	if (!zbm) return FALSE;

	struct ZZBitMap **pp = &zz_bitmap_list;
	while (*pp && *pp != zbm)
		pp = &(*pp)->next;
	if (*pp)
		*pp = zbm->next;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
		dmy_cache
		gfxdata->offset[0] = zbm->card_offset;
		writeGfxDataU8(gfxdata, 0, 0);
		zzwrite16(&registers->blitter_acc_op, ACC_OP_FREE_SURFACE);
	} else if (zz_z2_layout_active(b) &&
		zbm->card_offset == b->CardData[ZZ_CARD_DATA_PIP_OFFSET]) {
		if (zz_z2_pip_bitmap == zbm)
			zz_z2_pip_bitmap = NULL;
	}

	FreeMem(zbm, sizeof(struct ZZBitMap));
	return TRUE;
}

ULONG ZZ_GetBitMapAttr(__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGD0(ULONG attr)) {
	struct ZZBitMapAttrs attrs;
	struct ZZBitMap *zbm;

	if (!bm) return 0;

	if ((zbm = zz_bitmap_ours(b, bm))) {
		attrs.memory = (uint32_t)bm->Planes[0];
		attrs.basememory = (uint32_t)b->MemoryBase;
		attrs.bytesperrow = bm->BytesPerRow;
		attrs.bytesperpixel = zz_rgbformat_bytes_per_pixel(zbm->rgbformat);
		attrs.bitsperpixel = zz_rgbformat_storage_bits(zbm->rgbformat);
		attrs.rgbformat = zbm->rgbformat;
		attrs.width = zbm->width;
		attrs.height = zbm->height;
		/* ModeInfo convention: color depth, 15 for RGB555 */
		attrs.depth = zz_rgbformat_depth(zbm->rgbformat);
	} else {
		/* foreign bitmap (system RAM, planar): answer from the plain
		 * struct BitMap fields */
		attrs.memory = (uint32_t)bm->Planes[0];
		attrs.basememory = (uint32_t)b->MemoryBase;
		attrs.bytesperrow = bm->BytesPerRow;
		attrs.bytesperpixel = 1;
		attrs.bitsperpixel = bm->Depth;
		attrs.rgbformat = RGBFB_PLANAR;
		attrs.width = (uint32_t)bm->BytesPerRow * 8;
		attrs.height = bm->Rows;
		attrs.depth = bm->Depth;
	}

	return zz_offscreen_attr(&attrs, attr);
}

/* WriteYUVRect contract probe (Stage A). P96 calls this hook from its
 * software picture-in-picture path (p96PIP with a YUV source), but the
 * TagItem contract - how the source stride and format arrive - is
 * undocumented and P96's source is closed. This probe logs every
 * argument, the full tag list and the head of the source data, then
 * delegates to P96's own converter, so a Sashimi capture from a real
 * PIP session defines the contract the accelerated implementation will
 * code against. Build with -DDEBUG (and -ldebug) to enable the
 * logging; a release build is a pure pass-through. */
int ZZ_WriteYUVRect(__REGA0(struct BoardInfo *b), __REGA1(APTR src), __REGD0(SHORT srcx), __REGD1(SHORT srcy), __REGA2(struct RenderInfo *r), __REGD2(SHORT dstx), __REGD3(SHORT dsty), __REGD4(SHORT w), __REGD5(SHORT h), __REGA3(struct TagItem *tags)) {
	int result;
#ifdef DEBUG
	static ULONG yuv_calls = 0;
	ULONG n = yuv_calls++;
	int do_log = (n < 8) || ((n & 0xFF) == 0);

	if (do_log) {
		KPrintF("ZZ9000: WriteYUVRect #%ld src=%lx sx=%ld sy=%ld dst=%lx bpr=%ld fmt=%ld dx=%ld dy=%ld w=%ld h=%ld tags=%lx\n",
			n, (ULONG)src, (LONG)srcx, (LONG)srcy,
			(ULONG)(r ? (ULONG)r->Memory : 0),
			(LONG)(r ? r->BytesPerRow : 0),
			(LONG)(r ? (LONG)r->RGBFormat : -1),
			(LONG)dstx, (LONG)dsty, (LONG)w, (LONG)h, (ULONG)tags);

		struct TagItem *tag = tags;
		int guard = 32;
		while (tag && tag->ti_Tag != TAG_DONE && guard--) {
			switch (tag->ti_Tag) {
				case TAG_IGNORE: break;
				case TAG_SKIP: tag += tag->ti_Data; break;
				case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
				default:
					KPrintF("ZZ9000:   tag=%08lx data=%08lx\n",
						tag->ti_Tag, tag->ti_Data);
					break;
			}
			tag++;
		}

		if (src) {
			const UBYTE *p = (const UBYTE *)src;
			KPrintF("ZZ9000:   src[0..7]=%02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx\n",
				(ULONG)p[0], (ULONG)p[1], (ULONG)p[2], (ULONG)p[3],
				(ULONG)p[4], (ULONG)p[5], (ULONG)p[6], (ULONG)p[7]);
			KPrintF("ZZ9000:   src[8..15]=%02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx\n",
				(ULONG)p[8], (ULONG)p[9], (ULONG)p[10], (ULONG)p[11],
				(ULONG)p[12], (ULONG)p[13], (ULONG)p[14], (ULONG)p[15]);
		}
	}
#endif

	if (!b->WriteYUVRectDefault)
		return 0;
	result = b->WriteYUVRectDefault(b, src, srcx, srcy, r, dstx, dsty, w, h, tags);

#ifdef DEBUG
	if (do_log)
		KPrintF("ZZ9000:   WriteYUVRectDefault returned %ld\n", (LONG)result);
#endif

	return result;
}


/* --- P96 video window (PIP) Features API -----------------------------
 * Contract modeled on WinUAE's uaegfx overlay (the only public
 * reference): P96 creates the PIP as an SFT_MEMORYWINDOW feature, the
 * driver supplies a YUV source bitmap and the firmware composites it
 * into the display (OP_VIDEO_OVERLAY + shadow scanout). One overlay
 * at a time.
 *
 * The source bitmap MUST be P96-MANAGED (from p96AllocBitMap, in
 * rtg.library's own bookkeeping): every bench round that handed out a
 * bare driver-cooked bitmap froze the machine at RiVA's first
 * LockVLayer (P96's cgxvideo emulation walks its bitmap bookkeeping),
 * while the round with a p96AllocBitMap source played video through
 * the software path. On-board placement comes from a FRIEND bitmap
 * with board affinity: b->VisibleBitMap, the displayed screen's
 * bitmap, which P96 maintains in the BoardInfo - Intuition-free, so
 * no LockPubScreen deadlock (that variant froze CreateFeature). A
 * source P96 still places in system RAM fails the open cleanly into
 * P96's software PIP instead of risking the frozen hardware path. */

static struct ZZOverlayState zz_overlay;
static struct BitMap *zz_overlay_bitmap = NULL;
static struct Library *P96Base = NULL;
/* RastPort handed to the app for P96PIP_SourceRPort (the prefill=0
 * capture proved P96 does NOT construct the source objects itself):
 * wraps the managed source bitmap; InitRastPort defaults built by
 * hand so the driver needs no graphics.library base. Proven safe with
 * a managed bitmap - RiVA played video through exactly this pair. */
static struct RastPort zz_overlay_rport;
static void zz_overlay_free_source(struct BoardInfo *b) {
	zz_z2_pip_request.pending = FALSE;
	if (!zz_overlay_bitmap)
		return;
	p96FreeBitMap(zz_overlay_bitmap);
	zz_overlay_bitmap = NULL;
	/* P96 normally invokes our FreeBitMap hook synchronously.  If an
	 * allocation-failure path did not, reclaim the underlying fixed-pool
	 * wrapper explicitly so the single-owner state cannot leak. */
	if (zz_z2_pip_bitmap)
		ZZ_FreeBitMap(b, &zz_z2_pip_bitmap->bm, NULL);
}

/* Push the full overlay state (or OFF) to the firmware; returns the
 * firmware status word (0 = OK). */
static ULONG zz_overlay_push(struct BoardInfo *b, int off) {
	if (!b || !zz_gfxdata(b) || (!off && !zz_overlay_bitmap))
		return 1;
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	dmy_cache
	if (off) {
		gfxdata->u8_user[GFXDATA_U8_OVERLAY_SUBCMD] = OVERLAY_SUBCMD_OFF;
	} else {
		gfxdata->u8_user[GFXDATA_U8_OVERLAY_SUBCMD] = OVERLAY_SUBCMD_SET;
		gfxdata->u8_user[GFXDATA_U8_YUV_VARIANT] =
			(uint8_t)zz_overlay_variant(zz_overlay.rgbformat);
		gfxdata->offset[1] = (uint32_t)zz_overlay_bitmap->Planes[0] -
			(uint32_t)b->MemoryBase;
		gfxdata->pitch[1] = zz_overlay_bitmap->BytesPerRow;
		gfxdata->x[0] = (UWORD)zz_overlay.dst_x;
		gfxdata->y[0] = (UWORD)zz_overlay.dst_y;
		gfxdata->x[1] = (UWORD)zz_overlay.dst_w;
		gfxdata->y[1] = (UWORD)zz_overlay.dst_h;
		gfxdata->x[2] = zz_overlay.src_w;
		gfxdata->y[2] = zz_overlay.src_h;
		gfxdata->user[0] = (UWORD)((zz_overlay.occlusion ? 1 : 0) |
			(zz_overlay.active ? 2 : 0));
		/* pen convention: written as the raw 68k ULONG, the firmware
		 * reads it unswapped (see overlay_handle_op) */
		gfxdata->u32_user[1] = zz_overlay.color_key;
	}
	zzwrite16(&registers->blitter_dma_op, OP_VIDEO_OVERLAY);

	return gfxdata->u32_user[0];
}

APTR ZZ_CreateFeature(__REGA0(struct BoardInfo *b), __REGD0(ULONG type), __REGA1(struct TagItem *tags)) {
	ULONG friend_bpp;
	ULONG alloc_width;
	ULONG allocated_pitch;
#ifdef DEBUG
	/* build fingerprint: identifies the exact binary in every capture
	 * (three bench rounds in a row ran a stale card unnoticed) */
	KPrintF("ZZ9000: CreateFeature build " __DATE__ " " __TIME__ "\n");
#endif
	if (!b || type != SFT_MEMORYWINDOW)
		return NULL;
	if (zz_overlay_bitmap)
		return NULL; /* one overlay at a time (WinUAE parity) */
	if (!b->AllocBitMap ||
	    (!(b->CardFlags & CARDFLAG_ZORRO_3) &&
	     (!zz_z2_layout_active(b) ||
	      !b->CardData[ZZ_CARD_DATA_PIP_SIZE] || !zz_gfxdata(b))))
		return NULL;

	memset(&zz_overlay, 0, sizeof(zz_overlay));

	struct TagItem *tag = tags;
	int guard = 64;
	while (tag && tag->ti_Tag != TAG_DONE && guard--) {
		switch (tag->ti_Tag) {
			case TAG_IGNORE: break;
			case TAG_SKIP: tag += tag->ti_Data; break;
			case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
			default:
#ifdef DEBUG
				KPrintF("ZZ9000: CreateFeature tag=%08lx data=%08lx\n",
					tag->ti_Tag, tag->ti_Data);
#endif
				zz_overlay_apply_tag(&zz_overlay, tag->ti_Tag, tag->ti_Data);
				break;
		}
		tag++;
	}

	if (!zz_overlay_source_valid(zz_overlay.src_w, zz_overlay.src_h,
			zz_overlay.rgbformat)) {
		KPrintF("ZZ9000: CreateFeature REJECT src %ldx%ld fmt %ld\n",
			(LONG)zz_overlay.src_w, (LONG)zz_overlay.src_h,
			(LONG)zz_overlay.rgbformat);
		return NULL;
	}
	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		uint32_t surface_size;

		if (!zz_overlay_surface_size(zz_overlay.src_w, zz_overlay.src_h,
				&surface_size) ||
		    surface_size > b->CardData[ZZ_CARD_DATA_PIP_SIZE]) {
			KPrintF("ZZ9000: CreateFeature Z2 source exceeds PIP pool\n");
			return NULL;
		}
		zz_z2_pip_request.pitch = zz_overlay_line_bytes(zz_overlay.src_w);
		zz_z2_pip_request.size = surface_size;
		zz_z2_pip_request.width = zz_overlay.src_w;
		zz_z2_pip_request.height = zz_overlay.src_h;
		zz_z2_pip_request.format = zz_overlay.rgbformat;
	}

	/* P96-managed source (see the block comment above the statics):
	 * p96AllocBitMap with b->VisibleBitMap as the friend for board
	 * affinity. No BMF_DISPLAYABLE - a displayable YUV request has no
	 * display mode and lands in system RAM; the friend is what pins
	 * the board. */
	if (!P96Base)
		P96Base = OpenLibrary((APTR)"Picasso96API.library", 2);
	if (!P96Base) {
		KPrintF("ZZ9000: CreateFeature: no Picasso96API.library\n");
		return NULL;
	}
#ifdef DEBUG
	KPrintF("ZZ9000: CreateFeature friend VisibleBitMap %08lx\n",
		(ULONG)b->VisibleBitMap);
#endif
	/* P96 sizes the managed bitmap from the friend's storage format despite
	 * the requested YUV format. Choose a nominal width that yields the
	 * required packed-YUV row span in every screen depth: half width for a
	 * 32-bit friend, full width for 15/16-bit, double width for 8-bit. The
	 * feature retains the true source geometry. */
	/* BoardInfo.RGBFormat is authoritative for the active display friend.
	 * Do not query the foreign VisibleBitMap through our GetBitMapAttr
	 * fallback: its conservative planar answer would mis-size the source. */
	friend_bpp = zz_rgbformat_bytes_per_pixel(b->RGBFormat);
	alloc_width = zz_overlay_friend_width(zz_overlay.src_w, friend_bpp);
	if (alloc_width == 0) {
		KPrintF("ZZ9000: CreateFeature unsupported display fmt %ld\n",
			(LONG)b->RGBFormat);
		return NULL;
	}
#ifdef DEBUG
	KPrintF("ZZ9000: CreateFeature friend bpp %ld alloc width %ld\n",
		(LONG)friend_bpp, (LONG)alloc_width);
#endif
	if (!(b->CardFlags & CARDFLAG_ZORRO_3))
		zz_z2_pip_request.pending = TRUE;
	zz_overlay_bitmap = p96AllocBitMap(alloc_width,
		zz_overlay.src_h, 16, BMF_CLEAR, b->VisibleBitMap,
		(RGBFTYPE)zz_overlay.rgbformat);
	zz_z2_pip_request.pending = FALSE;
	if (!zz_overlay_bitmap) {
		KPrintF("ZZ9000: CreateFeature p96AllocBitMap FAILED\n");
		if (zz_z2_pip_bitmap)
			ZZ_FreeBitMap(b, &zz_z2_pip_bitmap->bm, NULL);
		return NULL;
	}
	allocated_pitch = zz_overlay_bitmap->BytesPerRow;
	if (!zz_overlay_tighten_pitch(zz_overlay.src_w,
			&zz_overlay_bitmap->BytesPerRow)) {
		KPrintF("ZZ9000: CreateFeature source pitch too small (%ld < %ld)\n",
			(LONG)allocated_pitch,
			(LONG)zz_overlay_line_bytes(zz_overlay.src_w));
		zz_overlay_free_source(b);
		return NULL;
	}
	/* The P96 allocation retains its full reserved capacity. Tighten the
	 * public BitMap stride so cgxvideo's fixed width*2 modulo, RastPort
	 * writers, and the firmware all advance by the same amount. */
	if (allocated_pitch != (ULONG)zz_overlay_bitmap->BytesPerRow) {
		KPrintF("ZZ9000: CreateFeature tighten source pitch %ld -> %ld\n",
			(LONG)allocated_pitch,
			(LONG)zz_overlay_bitmap->BytesPerRow);
	}
#ifdef DEBUG
	KPrintF("ZZ9000: CreateFeature bm fmt %ld w %ld bpr %ld bpp %ld p96 %ld onboard %ld\n",
		p96GetBitMapAttr(zz_overlay_bitmap, P96BMA_RGBFORMAT),
		p96GetBitMapAttr(zz_overlay_bitmap, P96BMA_WIDTH),
		p96GetBitMapAttr(zz_overlay_bitmap, P96BMA_BYTESPERROW),
		p96GetBitMapAttr(zz_overlay_bitmap, P96BMA_BYTESPERPIXEL),
		p96GetBitMapAttr(zz_overlay_bitmap, P96BMA_ISP96),
		p96GetBitMapAttr(zz_overlay_bitmap, P96BMA_ISONBOARD));
#endif

	/* the compositor reads the source from card VRAM: a source the
	 * friend affinity failed to place on board fails the open cleanly
	 * into P96's software PIP. The bound is the autoconfig window, NOT
	 * b->MemorySize: the firmware surface heap sits above the P96
	 * window (see zz_card_window_size), which a previous
	 * MemorySize-based check here misread as a system-RAM placement. */
	{
		uint32_t source_bytes;
		int source_size_ok = allocated_pitch != 0 &&
			zz_overlay.src_h <= UINT32_MAX / allocated_pitch;

		source_bytes = source_size_ok ?
			(uint32_t)allocated_pitch * zz_overlay.src_h : 0;
		if (!source_size_ok ||
		    !zz_offscreen_range_within(
			(uint32_t)zz_overlay_bitmap->Planes[0], source_bytes,
			(uint32_t)b->MemoryBase, (uint32_t)zz_card_window_size) ||
		    (!(b->CardFlags & CARDFLAG_ZORRO_3) &&
		     (uint32_t)zz_overlay_bitmap->Planes[0] !=
			(uint32_t)b->MemoryBase +
			(uint32_t)b->CardData[ZZ_CARD_DATA_PIP_OFFSET])) {
			KPrintF("ZZ9000: CreateFeature source NOT on board (%08lx)\n",
				(ULONG)zz_overlay_bitmap->Planes[0]);
			zz_overlay_free_source(b);
			return NULL;
		}
	}

	/* enable the firmware master gate (the vblank ISR checks it) */
	{
		MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
		selectCardFeature(registers, CARD_FEATURE_VIDEO_OVERLAY);
		zzwrite16(&registers->set_feature_status, 1);
	}

	memset(&zz_overlay_rport, 0, sizeof(zz_overlay_rport));
	zz_overlay_rport.BitMap = zz_overlay_bitmap;
	zz_overlay_rport.Mask = 0xFF;
	zz_overlay_rport.FgPen = -1;
	zz_overlay_rport.AOlPen = -1;
	zz_overlay_rport.LinePtrn = 0xFFFF;
	zz_overlay_rport.DrawMode = 1; /* JAM2 */

	zz_overlay.magic = ZZ_OVERLAY_MAGIC;

	{
		ULONG fw_status = zz_overlay_push(b, 0);
		if (fw_status != 0) {
			KPrintF("ZZ9000: CreateFeature fw SET status %ld\n",
				(LONG)fw_status);
			zz_overlay.magic = 0;
			zz_overlay_free_source(b);
			return NULL;
		}
	}

	KPrintF("ZZ9000: CreateFeature OK (%ldx%ld fmt %ld src %08lx)\n",
		(LONG)zz_overlay.src_w, (LONG)zz_overlay.src_h,
		(LONG)zz_overlay.rgbformat, (ULONG)zz_overlay_bitmap->Planes[0]);
	return (APTR)&zz_overlay;
}

static int zz_overlay_cookie_ok(APTR fd) {
	return fd == (APTR)&zz_overlay && zz_overlay.magic == ZZ_OVERLAY_MAGIC &&
		zz_overlay_bitmap != NULL;
}

ULONG ZZ_SetFeatureAttrs(__REGA0(struct BoardInfo *b), __REGA1(APTR fd), __REGD0(ULONG type), __REGA2(struct TagItem *tags)) {
	ULONG count = 0;
	int dirty = 0;

	if (!b || !zz_overlay_cookie_ok(fd))
		return 0;

	struct TagItem *tag = tags;
	int guard = 64;
	while (tag && tag->ti_Tag != TAG_DONE && guard--) {
		switch (tag->ti_Tag) {
			case TAG_IGNORE: break;
			case TAG_SKIP: tag += tag->ti_Data; break;
			case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
			default:
#ifdef DEBUG
				KPrintF("ZZ9000: SetFeatureAttrs tag=%08lx data=%08lx\n",
					tag->ti_Tag, tag->ti_Data);
#endif
				/* source geometry/format is fixed at CreateFeature:
				 * the backing bitmap cannot grow under a live
				 * feature */
				if (!zz_overlay_tag_create_only(tag->ti_Tag))
					dirty |= zz_overlay_apply_tag(&zz_overlay,
						tag->ti_Tag, tag->ti_Data);
				count++;
				break;
		}
		tag++;
	}

	if (dirty)
		zz_overlay_push(b, 0);

	return count;
}

ULONG ZZ_GetFeatureAttrs(__REGA0(struct BoardInfo *b), __REGA1(APTR fd), __REGD0(ULONG type), __REGA2(struct TagItem *tags)) {
	ULONG count = 0;
	int have = zz_overlay_cookie_ok(fd);

	if (!b)
		return 0;

	struct TagItem *tag = tags;
	int guard = 64;
	while (tag && tag->ti_Tag != TAG_DONE && guard--) {
		switch (tag->ti_Tag) {
			case TAG_IGNORE: break;
			case TAG_SKIP: tag += tag->ti_Data; break;
			case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
			default: {
				uint32_t out;
				/* the app's p96PIP_GetTags source-object queries:
				 * P96 does NOT construct these itself (a capture
				 * showed the stores arrive zeroed), so answer with
				 * the managed bitmap and the RastPort wrapping it -
				 * the pair RiVA demonstrably played video through. */
				if (have && tag->ti_Tag == ZZ_P96PIP_SOURCEBITMAP) {
					if (tag->ti_Data)
						*(ULONG *)tag->ti_Data = (ULONG)zz_overlay_bitmap;
					count++;
#ifdef DEBUG
					KPrintF("ZZ9000: GetFeatureAttrs PIP bitmap -> %08lx\n",
						(ULONG)zz_overlay_bitmap);
#endif
					break;
				}
				if (have && tag->ti_Tag == ZZ_P96PIP_SOURCERPORT) {
					if (tag->ti_Data)
						*(ULONG *)tag->ti_Data = (ULONG)&zz_overlay_rport;
					count++;
#ifdef DEBUG
					KPrintF("ZZ9000: GetFeatureAttrs PIP rport -> %08lx\n",
						(ULONG)&zz_overlay_rport);
#endif
					break;
				}
				if (zz_overlay_query_tag(&zz_overlay, have,
						(uint32_t)zz_overlay_bitmap,
						tag->ti_Tag, &out)) {
					/* ti_Data is a pointer to the result slot
					 * (WinUAE settag semantics) */
					if (tag->ti_Data)
						*(ULONG *)tag->ti_Data = out;
					count++;
				}
#ifdef DEBUG
				KPrintF("ZZ9000: GetFeatureAttrs tag=%08lx data=%08lx\n",
					tag->ti_Tag, tag->ti_Data);
#endif
				break;
			}
		}
		tag++;
	}

	return count;
}

BOOL ZZ_DeleteFeature(__REGA0(struct BoardInfo *b), __REGA1(APTR fd), __REGD0(ULONG type)) {
	if (!b || type != SFT_MEMORYWINDOW || !zz_overlay_cookie_ok(fd))
		return FALSE;

	zz_overlay_push(b, 1);
	zz_overlay.magic = 0;
	zz_overlay_free_source(b);

	return TRUE;
}

// This function shall blit a planar bitmap anywhere in the 68K address space into a chunky bitmap in video RAM.
// The source bitmap that contains the data to be blitted is in the bm argument.
// If one of its plane pointers is 0x0, the source data of that bitplane shall be considered to consist of all-zeros.
// If one of its plane pointers is 0xffffffff, the data in this bitplane shall be considered to be all ones.
void BlitPlanar2Chunky(__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGA2(struct RenderInfo *r), __REGD0(SHORT x), __REGD1(SHORT y), __REGD2(SHORT dx), __REGD3(SHORT dy), __REGD4(SHORT w), __REGD5(SHORT h), __REGD6(UBYTE minterm), __REGD7(UBYTE mask)) {
	if (!b || !r || !bm)
		return;
	if (w<1 || h<1)
		return;

	// b->BlitPlanar2ChunkyDefault(b, bm, r, x, y, dx, dy, w, h, minterm, mask);
	// return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	uint16_t zz_mask = mask;
	uint8_t cur_plane = 0x01;

	uint16_t line_size = planar_line_bytes_padded(w);
	uint32_t output_plane_size = line_size * h;

	if (!(b->CardFlags & CARDFLAG_ZORRO_3) &&
		!zz_z2_staging_fits(0, output_plane_size, bm->Depth)) {
		b->BlitPlanar2ChunkyDefault(b, bm, r, x, y, dx, dy, w, h, minterm, mask);
		return;
	}
	/* MiSTer: ARM blitter attached, at most 8 planes, staging area fits. */
	BOOL p2c_fallback = !mister_accel(b) || bm->Depth > 8 ||
	    (uint32_t)output_plane_size * bm->Depth > MZZ_TEMPLATE_SIZE;
	mister_diag_hook(b, r->RGBFormat, p2c_fallback);
	if (p2c_fallback) {
		b->BlitPlanar2ChunkyDefault(b, bm, r, x, y, dx, dy, w, h, minterm, mask);
		return;
	}

	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = zz_z2_template_offset(b);
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
		gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);
		gfxdata->pitch[GFXDATA_SRC] = line_size;

		writeGfxDataMask(gfxdata, mask);
		writeGfxDataMinterm(gfxdata, minterm);
	} else {
		writeBlitterDstOffset(registers, offset);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, mnt_colormode(r->RGBFormat) | (minterm << 8));
		writeBlitterSrcOffset(registers, zz_template_addr);
		writeBlitterSrcPitch(registers, line_size);
	}

	for (int16_t i = 0; i < bm->Depth; i++) {
		uint16_t x_offset = (x >> 3);
		if ((uint32_t)bm->Planes[i] == 0xFFFFFFFF) {
			memset((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), 0xFF, output_plane_size);
		}
		else if (bm->Planes[i] != NULL) {
			uint8_t* bmp_mem = (uint8_t*)bm->Planes[i] + (y * bm->BytesPerRow) + x_offset;
			uint8_t* zz_dest = (uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr);
			for (int16_t y_line = 0; y_line < h; y_line++) {
				memcpy(zz_dest, bmp_mem, line_size);
				zz_dest += line_size;
				bmp_mem += bm->BytesPerRow;
			}
		}
		else {
			zz_mask &= (cur_plane ^ 0xFF);
		}
		cur_plane <<= 1;
		zz_template_addr += output_plane_size;
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		gfxdata->x[0] = (x & 0x07);
		gfxdata->x[1] = dx;
		gfxdata->x[2] = w;
		gfxdata->y[1] = dy;
		gfxdata->y[2] = h;

		gfxdata->user[0] = zz_mask;
		gfxdata->user[1] = bm->Depth;

		mister_kick(b, OP_P2C);
	} else {
		writeBlitterX1(registers, x & 0x07);
		writeBlitterX2(registers, dx);
		writeBlitterY2(registers, dy);
		writeBlitterX3(registers, w);
		writeBlitterY3(registers, h);

		writeBlitterUser2(registers, zz_mask);

		zzwrite16(&registers->blitter_op_p2c, mask | bm->Depth << 8);
	}
}

void BlitPlanar2Direct(__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGA2(struct RenderInfo *r), __REGA3(struct ColorIndexMapping *clut), __REGD0(SHORT x), __REGD1(SHORT y), __REGD2(SHORT dx), __REGD3(SHORT dy), __REGD4(SHORT w), __REGD5(SHORT h), __REGD6(UBYTE minterm), __REGD7(UBYTE mask)) {
	if (!b || !r || !bm || !clut)
		return;
	if (w<1 || h<1)
		return;

	// b->BlitPlanar2DirectDefault(b, bm, r, clut, x, y, dx, dy, w, h, minterm, mask);
	// return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	uint16_t zz_mask = mask;
	uint8_t cur_plane = 0x01;

	uint16_t line_size = planar_line_bytes(x, w);
	uint32_t output_plane_size = line_size * h;

	if (!(b->CardFlags & CARDFLAG_ZORRO_3) &&
		!zz_z2_staging_fits((256 << 2), output_plane_size, bm->Depth)) {
		b->BlitPlanar2DirectDefault(b, bm, r, clut, x, y, dx, dy, w, h, minterm, mask);
		return;
	}
	/* MiSTer: ARM blitter attached, a 16/32-bit target (no ZZ9000 24-bit
	 * mode), at most 8 planes, colour table + planes fit the staging area. */
	{
		uint16_t cm = blit_colormode(b, r->RGBFormat);   /* counts the hook */
		if (cm == MNTVA_COLOR_NO_USE || cm == MNTVA_COLOR_8BIT || bm->Depth > 8 ||
		    (256 << 2) + (uint32_t)output_plane_size * bm->Depth > MZZ_TEMPLATE_SIZE) {
			b->BlitPlanar2DirectDefault(b, bm, r, clut, x, y, dx, dy, w, h, minterm, mask);
			return;
		}
	}

	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = zz_z2_template_offset(b);
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
		gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);
		gfxdata->pitch[GFXDATA_SRC] = line_size;
		gfxdata->rgb[0] = clut->ColorMask;

		writeGfxDataU8(gfxdata, GFXDATA_U8_COLORMODE,
			(uint8_t)mnt_colormode(r->RGBFormat));
		writeGfxDataMask(gfxdata, mask);
		writeGfxDataMinterm(gfxdata, minterm);
	} else {
		writeBlitterDstOffset(registers, offset);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, mnt_colormode(r->RGBFormat) | (minterm << 8));
		writeBlitterSrcOffset(registers, zz_template_addr);
		writeBlitterSrcPitch(registers, line_size);
		writeBlitterRGB(registers, clut->ColorMask);
	}

	memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), clut->Colors, (256 << 2));
	zz_template_addr += (256 << 2);

	for (int16_t i = 0; i < bm->Depth; i++) {
		uint16_t x_offset = (x >> 3);
		if ((uint32_t)bm->Planes[i] == 0xFFFFFFFF) {
			memset((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), 0xFF, output_plane_size);
		}
		else if (bm->Planes[i] != NULL) {
			uint8_t* bmp_mem = (uint8_t*)bm->Planes[i] + (y * bm->BytesPerRow) + x_offset;
			uint8_t* zz_dest = (uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr);
			for (int16_t y_line = 0; y_line < h; y_line++) {
				memcpy(zz_dest, bmp_mem, line_size);
				zz_dest += line_size;
				bmp_mem += bm->BytesPerRow;
			}
		}
		else {
			zz_mask &= (cur_plane ^ 0xFF);
		}
		cur_plane <<= 1;
		zz_template_addr += output_plane_size;
	}

	if(b->CardFlags & CARDFLAG_ZORRO_3) {
		gfxdata->x[0] = (x & 0x07);
		gfxdata->x[1] = dx;
		gfxdata->x[2] = w;
		gfxdata->y[1] = dy;
		gfxdata->y[2] = h;

		gfxdata->user[0] = zz_mask;
		gfxdata->user[1] = bm->Depth;

//		unsigned long mi=minterm;
//		unsigned long ma=mask;
//		unsigned long zm=zz_mask;
//		unsigned long cm=clut->ColorMask;
//		unsigned long cf=r->RGBFormat;
//		KPrintF("minterm = %ld, mask=%ld, zzmask=0x%08lX, colormask=0x%08lX, colorformat=%ld\n", mi, ma, zm, cm, cf);

		mister_kick(b, OP_P2D);
	} else {
		writeBlitterX1(registers, x & 0x07);
		writeBlitterX2(registers, dx);
		writeBlitterY2(registers, dy);
		writeBlitterX3(registers, w);
		writeBlitterY3(registers, h);

		writeBlitterUser2(registers, zz_mask);

		zzwrite16(&registers->blitter_op_p2d, mask | bm->Depth << 8);
	}
}

BOOL SetSprite(__REGA0(struct BoardInfo *b), __REGD0(BOOL what), __REGD7(RGBFTYPE format)) {
	if (!b || !b->RegisterBase)
		return FALSE;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	/*
	 * Keep the legacy ZZ9000 behavior here: the existing firmware/driver
	 * path expects SetSprite() to assert hardware-sprite visibility even
	 * when P96 passes FALSE during setup or screen transitions.
	 */
	zzwrite16(&registers->sprite_bitmap, 1);
	return TRUE;
}

void SetSpritePosition(__REGA0(struct BoardInfo *b), __REGD0(WORD x), __REGD1(WORD y), __REGD7(RGBFTYPE format)) {
	if (!b || !b->RegisterBase)
		return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	// The firmware stores b->XOffset/YOffset through SetPanning/SetSpriteImage
	// and applies the P96 sprite offset adjustment when positioning the sprite.
	b->MouseX = x;
	b->MouseY = y;

	if(b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->x[0] = x;
		gfxdata->y[0] = y + b->YSplit;

		zzwrite16(&registers->blitter_dma_op, OP_SPRITE_XY);
	} else {
		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y + b->YSplit);
		zzwrite16(&registers->sprite_y, 1);
	}
}

void SetSpriteImage(__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) {
	if (!b) return;
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = zz_z2_template_offset(b);
	}

	uint32_t flags = b->Flags;
	int hiressprite = 1;
	int doubledsprite = 0;
	if (flags & BIF_HIRESSPRITE)
		hiressprite = 2;
	if (flags & BIF_BIGSPRITE)
		doubledsprite = 1;

	uint32_t data_size = ((uint32_t)(b->MouseWidth >> 3) * 2U *
		(uint32_t)hiressprite) * (uint32_t)b->MouseHeight;
	if (!(b->CardFlags & CARDFLAG_ZORRO_3) &&
		!zz_z2_template_fits(data_size))
		return;
	memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), b->MouseImage + 2 * hiressprite, data_size);

	if(b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[1] = zz_template_addr;
		gfxdata->x[0] = b->XOffset;
		gfxdata->x[1] = b->MouseWidth | (doubledsprite << 8);
		gfxdata->y[0] = b->YOffset;
		gfxdata->y[1] = b->MouseHeight | ((hiressprite - 1) << 8);

		zzwrite16(&registers->blitter_dma_op, OP_SPRITE_BITMAP);
	} else {
		writeBlitterSrcOffset(registers, zz_template_addr);

		writeBlitterX1(registers, b->XOffset);
		writeBlitterX2(registers, b->MouseWidth | (doubledsprite << 8));
		writeBlitterY1(registers, b->YOffset);
		writeBlitterY2(registers, b->MouseHeight | ((hiressprite - 1) << 8));

		zzwrite16(&registers->sprite_bitmap, 0);
	}
}

void SetSpriteColor(__REGA0(struct BoardInfo *b), __REGD0(UBYTE idx), __REGD1(UBYTE R), __REGD2(UBYTE G), __REGD3(UBYTE B), __REGD7(RGBFTYPE format)) {
	if (!b) return;
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		((char *)&gfxdata->rgb[0])[0] = B;
		((char *)&gfxdata->rgb[0])[1] = G;
		((char *)&gfxdata->rgb[0])[2] = R;
		((char *)&gfxdata->rgb[0])[3] = 0x00;
		gfxdata->u8offset = idx + 1;

		zzwrite16(&registers->blitter_dma_op, OP_SPRITE_COLOR);
	} else {
		writeBlitterUser1(registers, R);
		writeBlitterUser2(registers, B | (G << 8));
		zzwrite16(&registers->sprite_colors, idx + 1);
	}
}

void SetSplitPosition(__REGA0(struct BoardInfo *b),__REGD0(SHORT pos)) {
	if (!b)
		return;

	b->YSplit = pos;
	if (!b->RegisterBase)
		return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = 0;

	if (pos != 0) {
		if (!b->VisibleBitMap || !b->VisibleBitMap->Planes[0]) {
			pos = 0;
			b->YSplit = 0;
		} else {
			offset = ((uint32_t)b->VisibleBitMap->Planes[0]) - ((uint32_t)b->MemoryBase);
		}
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[0] = offset;
		gfxdata->y[0] = pos;
		zzwrite16(&registers->blitter_dma_op, OP_SET_SPLIT_POS);
	} else {
		writeBlitterSrcOffset(registers, offset);
		zzwrite16(&registers->blitter_set_split_pos, pos);
	}
}

static uint32_t device_vectors[] = {
	(uint32_t)OpenLib,
	(uint32_t)CloseLib,
	(uint32_t)ExpungeLib,
	0,
	(uint32_t)FindCard,
	(uint32_t)InitCard,
	-1
};

struct InitTable
{
	ULONG LibBaseSize;
	APTR FunctionTable;
	APTR DataTable;
	APTR InitLibTable;
};

const uint32_t auto_init_tables[4] = {
	sizeof(struct GFXBase),
	(uint32_t)device_vectors,
	0,
	(uint32_t)InitLib,
};
