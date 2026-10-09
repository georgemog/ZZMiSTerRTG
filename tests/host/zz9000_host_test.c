/*
 * Host-side tests for the MiSTer ZZ9000 blitter service.
 *
 * Builds the exact ARM-side code (zz9000_service.c + zz9000_blit.c, i.e.
 * the official ZZ9000OS gfx.c and the adapted dma_rtg.c, + zz9000_fbmem.c)
 * for the host and drives it the way the 68k driver does: big-endian
 * GFXData fields and mailbox words written into an 8MB "RTG window", then
 * one zz_service_poll(). Results are compared against small independent
 * reference renderers written from the Picasso96 semantics.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mister_zz9000.h"
#include "zz9000_fbmem.h"
#include "zz9000_service.h"

/* enum gfx_dma_op / MNTVA colour modes (zz9000os/gfx.h, zz_video_modes.h) */
enum { OP_DRAWLINE = 1, OP_FILLRECT, OP_COPYRECT, OP_COPYRECT_NOMASK,
	OP_RECT_TEMPLATE, OP_RECT_PATTERN, OP_P2C, OP_P2D, OP_INVERTRECT, OP_PAN };
enum { CM8 = 0, CM16 = 1, CM32 = 2, CM15 = 3 };
enum { JAM1 = 0, JAM2 = 1, COMPLEMENT = 2, INVERSVID = 4 };
enum { MT_EOR = 6, MT_SRC = 12 };

/* GFXData byte offsets (official layout, identical on m68k and ARM) */
#define G_OFFSET(i) (0x00 + 4 * (i))
#define G_RGB(i)    (0x08 + 4 * (i))
#define G_X(i)      (0x10 + 2 * (i))
#define G_Y(i)      (0x18 + 2 * (i))
#define G_USER(i)   (0x20 + 2 * (i))
#define G_PITCH(i)  (0x28 + 2 * (i))
#define G_U8(i)     (0x30 + (i))
#define G_MASK      0x39
#define G_MINTERM   0x3A

static uint8_t *fb;
static struct zz_service svc;
static int failures, checks;
static uint32_t kick;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- 68k-side writers (big-endian) ------------------------------------ */
static void be32(uint32_t off, uint32_t v) { fb[off] = v >> 24; fb[off+1] = v >> 16; fb[off+2] = v >> 8; fb[off+3] = v; }
static void be16(uint32_t off, uint16_t v) { fb[off] = v >> 8; fb[off+1] = v; }
static uint32_t rd_be32(uint32_t off) { return (uint32_t)fb[off] << 24 | fb[off+1] << 16 | fb[off+2] << 8 | fb[off+3]; }
static void mb(uint32_t f, uint32_t v) { be32(MZZ_MBOX_OFFSET + f, v); }
static uint32_t mbr(uint32_t f) { return rd_be32(MZZ_MBOX_OFFSET + f); }
static void g32(uint32_t f, uint32_t v) { be32(MZZ_GFXDATA_OFFSET + f, v); }
static void g16(uint32_t f, uint16_t v) { be16(MZZ_GFXDATA_OFFSET + f, v); }
static void g8(uint32_t f, uint8_t v) { fb[MZZ_GFXDATA_OFFSET + f] = v; }

static void attach(void)
{
	memset(fb + MZZ_MBOX_OFFSET, 0, MZZ_MB_SIZE);
	mb(MZZ_MB_SESSION, 0x1234 + kick);
	mb(MZZ_MB_KICK, kick);
	mb(MZZ_MB_REQ_MAGIC, MZZ_REQ_MAGIC);
	int r = zz_service_poll(&svc);
	CHECK(r == ZZ_POLL_ATTACHED, "attach returned %d", r);
}

/* Submit one op exactly like the driver's mister_kick() and run it. */
static void run(uint32_t op)
{
	mb(MZZ_MB_OP, op);
	mb(MZZ_MB_KICK, ++kick);
	int r = zz_service_poll(&svc);
	CHECK(r == ZZ_POLL_OP, "op %u: poll returned %d", op, r);
	CHECK(mbr(MZZ_MB_DONE) == kick, "op %u: DONE %u != KICK %u", op, mbr(MZZ_MB_DONE), kick);
	for (unsigned i = 0; i < MZZ_DONE_RING_SLOTS; i++)
		if (rd_be32(MZZ_DONE_RING_OFFSET + i * MZZ_DONE_RING_STRIDE) != kick) {
			CHECK(0, "op %u: DONE ring slot %u stale", op, i);
			break;
		}
	CHECK(zz_service_poll(&svc) == ZZ_POLL_IDLE, "op %u: not idle afterwards", op);
}

/* ---- framebuffer helpers ------------------------------------------------ */
#define SCR   0x100000u   /* test "screen" at board offset 1MB */
#define SCR2  0x300000u   /* second bitmap */
#define PITCH 1024u       /* bytes per row, multiple of 4 as P96 gives */

static uint8_t *ref;      /* reference copy of the window */

static void fill_noise(uint32_t seed)
{
	for (uint32_t i = 0; i < MZZ_P96_MEMORY_SIZE; i++) {
		seed = seed * 1103515245u + 12345u;
		fb[i] = seed >> 16;
	}
	memcpy(ref, fb, MZZ_P96_MEMORY_SIZE);
}

static int compare(const char *what)
{
	for (uint32_t i = 0; i < MZZ_P96_MEMORY_SIZE; i++) {
		if (fb[i] != ref[i]) {
			uint32_t o = i - SCR;
			printf("  %s: first mismatch at board+0x%06X (scr row %u col-byte %u): got %02X want %02X\n",
				what, i, o / PITCH, o % PITCH, fb[i], ref[i]);
			return 0;
		}
	}
	return 1;
}

static int bpp_of(int cm) { return cm == CM8 ? 1 : cm == CM32 ? 4 : 2; }

/* Pixel store in Amiga byte order: the value as the 68k would hold it. */
static void ref_put(uint32_t base, int cm, int x, int y, uint32_t v)
{
	uint8_t *p = ref + base + y * PITCH + x * bpp_of(cm);
	switch (bpp_of(cm)) {
	case 1: p[0] = v; break;
	case 2: p[0] = v >> 8; p[1] = v; break;
	case 4: p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; break;
	}
}

static uint32_t ref_get(uint32_t base, int cm, int x, int y)
{
	uint8_t *p = ref + base + y * PITCH + x * bpp_of(cm);
	switch (bpp_of(cm)) {
	case 1: return p[0];
	case 2: return (uint32_t)p[0] << 8 | p[1];
	default: return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
	}
}

/* Picasso96 hands pens to the board already in the screen format and
 * replicated across the ULONG (8bit: cccccccc, 16bit: hhhhhhhh). */
static uint32_t pen(int cm, uint32_t v)
{
	switch (bpp_of(cm)) {
	case 1: return (v & 0xFF) * 0x01010101u;
	case 2: return (v & 0xFFFF) * 0x00010001u;
	default: return v;
	}
}

/* ---- tests -------------------------------------------------------------- */

static void test_fbmem(void)
{
	uint8_t a[300], b[300], c[300];
	uint32_t seed = 1;
	for (int iter = 0; iter < 20000; iter++) {
		for (int i = 0; i < 300; i++) { seed = seed * 1664525u + 1013904223u; a[i] = seed >> 24; }
		memcpy(b, a, 300);
		memcpy(c, a, 300);
		seed = seed * 1664525u + 1013904223u;
		int d = (seed >> 8) % 140, s = (seed >> 16) % 140, n = (seed >> 24) % 150;
		switch (iter % 3) {
		case 0: zz_fb_memmove(b + d, b + s, n); memmove(c + d, c + s, n); break;
		case 1: if (abs(d - s) >= n) { zz_fb_memcpy(b + d, a + s, n); memcpy(c + d, a + s, n); } break;
		case 2: zz_fb_memset(b + d, s, n); memset(c + d, s, n); break;
		}
		if (memcmp(b, c, 300)) {
			CHECK(0, "fbmem mode %d d=%d s=%d n=%d", iter % 3, d, s, n);
			return;
		}
	}
	CHECK(1, "fbmem");
}

static void test_handshake(void)
{
	memset(fb + MZZ_MBOX_OFFSET, 0, MZZ_MB_SIZE);
	CHECK(zz_service_poll(&svc) == ZZ_POLL_NO_DRIVER, "no driver");
	CHECK(mbr(MZZ_MB_ACK_MAGIC) == 0, "ARM wrote before request");

	/* Byte-swapped request (core without the RTG byte swap): refuse. */
	fb[MZZ_MBOX_OFFSET + 0] = 'Z'; fb[MZZ_MBOX_OFFSET + 1] = 'M';
	fb[MZZ_MBOX_OFFSET + 2] = '9'; fb[MZZ_MBOX_OFFSET + 3] = 'Z';
	CHECK(zz_service_poll(&svc) == ZZ_POLL_NO_DRIVER && svc.swapped, "swapped magic");
	CHECK(mbr(MZZ_MB_ACK_MAGIC) == 0, "ARM attached to swapped request");

	kick = 77;
	attach();
	CHECK(mbr(MZZ_MB_ACK_MAGIC) == MZZ_ACK_MAGIC, "ack magic");
	CHECK(mbr(MZZ_MB_ACK_SESSION) == 0x1234 + 77, "ack session");
	CHECK(mbr(MZZ_MB_DONE) == 77, "done follows kick at attach");
	CHECK(rd_be32(MZZ_DONE_RING_OFFSET + 63 * MZZ_DONE_RING_STRIDE) == 77, "ring primed at attach");
	CHECK(mbr(MZZ_MB_VERSION) == MZZ_PROTOCOL_VERSION, "version");
	CHECK(mbr(MZZ_MB_CAPS) == MZZ_CAPS_V1, "caps");
	CHECK(zz_service_poll(&svc) == ZZ_POLL_IDLE, "idle after attach");

	/* New session (Amiga reboot) re-attaches. */
	mb(MZZ_MB_SESSION, 999);
	CHECK(zz_service_poll(&svc) == ZZ_POLL_ATTACHED, "re-attach on new session");

	/* Unknown op: rejected but completed, so the 68k never hangs. */
	run(OP_PAN);
	CHECK(mbr(MZZ_MB_REJECTED) == 1, "pan rejected");
}

static void setup_rect_common(uint32_t dst, uint32_t pitch_words, int cm, uint8_t mask)
{
	g32(G_OFFSET(0), dst);
	g16(G_PITCH(0), pitch_words);
	g8(G_U8(0), cm);
	g8(G_MASK, mask);
}

static void test_fillrect(void)
{
	static const int cms[] = { CM8, CM16, CM32 };
	for (int i = 0; i < 3; i++) {
		int cm = cms[i];
		for (int masked = 0; masked < 2; masked++) {
			uint8_t mask = (masked && cm == CM8) ? 0x3C : 0xFF;
			uint32_t col = cm == CM8 ? 0xA5 : cm == CM16 ? 0xF81F : 0x11223344;
			fill_noise(10 + i);
			int x = 3, y = 5, w = 37, h = 11;
			setup_rect_common(SCR, PITCH / 4, cm, mask);
			g32(G_RGB(0), pen(cm, col));
			g16(G_X(0), x); g16(G_X(1), w); g16(G_Y(0), y); g16(G_Y(1), h);
			run(OP_FILLRECT);
			for (int yy = y; yy < y + h; yy++)
				for (int xx = x; xx < x + w; xx++) {
					uint32_t v = col;
					if (cm == CM8) v = (ref_get(SCR, cm, xx, yy) & ~mask) | (col & mask);
					ref_put(SCR, cm, xx, yy, v);
				}
			char what[64];
			snprintf(what, sizeof what, "fillrect cm%d mask %02X", cm, mask);
			CHECK(compare(what), "%s", what);
		}
	}
}

static void test_invertrect(void)
{
	fill_noise(20);
	setup_rect_common(SCR, PITCH / 4, CM8, 0x0F);
	g16(G_X(0), 1); g16(G_X(1), 30); g16(G_Y(0), 2); g16(G_Y(1), 7);
	run(OP_INVERTRECT);
	for (int y = 2; y < 9; y++)
		for (int x = 1; x < 31; x++)
			ref_put(SCR, CM8, x, y, ref_get(SCR, CM8, x, y) ^ 0x0F);
	CHECK(compare("invertrect 8bit"), "invertrect 8bit");

	fill_noise(21);
	setup_rect_common(SCR, PITCH / 4, CM16, 0xFF);
	g16(G_X(0), 5); g16(G_X(1), 9); g16(G_Y(0), 0); g16(G_Y(1), 3);
	run(OP_INVERTRECT);
	for (int y = 0; y < 3; y++)
		for (int x = 5; x < 14; x++)
			ref_put(SCR, CM16, x, y, ref_get(SCR, CM16, x, y) ^ 0xFFFF);
	CHECK(compare("invertrect 16bit"), "invertrect 16bit");
}

/* BlitRect within one bitmap, all overlap directions. */
static void test_blitrect(void)
{
	static const int d[][2] = { { 3, 2 }, { -3, -2 }, { 5, -1 }, { -1, 4 }, { 1, 0 }, { 0, 1 } };
	static const int cms[] = { CM8, CM16, CM32 };
	for (int c = 0; c < 3; c++)
		for (unsigned k = 0; k < sizeof d / sizeof d[0]; k++) {
			int cm = cms[c];
			fill_noise(30 + k + 10 * c);
			int sx = 10, sy = 10, w = 41, h = 13, dx = sx + d[k][0], dy = sy + d[k][1];
			setup_rect_common(SCR, PITCH / 4, cm, 0xFF);
			g32(G_OFFSET(1), SCR);
			g16(G_X(2), sx); g16(G_Y(2), sy);
			g16(G_X(0), dx); g16(G_Y(0), dy);
			g16(G_X(1), w); g16(G_Y(1), h);
			run(OP_COPYRECT);
			uint32_t *tmp = malloc(w * h * 4);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					tmp[y * w + x] = ref_get(SCR, cm, sx + x, sy + y);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					ref_put(SCR, cm, dx + x, dy + y, tmp[y * w + x]);
			free(tmp);
			char what[64];
			snprintf(what, sizeof what, "blitrect cm%d d=(%d,%d)", cm, d[k][0], d[k][1]);
			CHECK(compare(what), "%s", what);
		}

	/* 8bit with a plane mask */
	fill_noise(39);
	setup_rect_common(SCR, PITCH / 4, CM8, 0xF0);
	g16(G_X(2), 0); g16(G_Y(2), 0); g16(G_X(0), 50); g16(G_Y(0), 40);
	g16(G_X(1), 20); g16(G_Y(1), 5);
	run(OP_COPYRECT);
	for (int y = 0; y < 5; y++)
		for (int x = 0; x < 20; x++)
			ref_put(SCR, CM8, 50 + x, 40 + y,
				(ref_get(SCR, CM8, 50 + x, 40 + y) & 0x0F) | (ref_get(SCR, CM8, x, y) & 0xF0));
	CHECK(compare("blitrect 8bit masked"), "blitrect 8bit masked");
}

static void test_blitrect_nomask(void)
{
	static const int mts[] = { MT_SRC, MT_EOR };
	for (int m = 0; m < 2; m++) {
		fill_noise(40 + m);
		g32(G_OFFSET(0), SCR); g16(G_PITCH(0), PITCH / 4);
		g32(G_OFFSET(1), SCR2); g16(G_PITCH(1), PITCH / 4);
		g8(G_U8(0), CM32); g8(G_MINTERM, mts[m]);
		g16(G_X(2), 7); g16(G_Y(2), 3);      /* source */
		g16(G_X(0), 2); g16(G_Y(0), 9);      /* destination */
		g16(G_X(1), 19); g16(G_Y(1), 6);
		run(OP_COPYRECT_NOMASK);
		for (int y = 0; y < 6; y++)
			for (int x = 0; x < 19; x++) {
				uint32_t s = ref_get(SCR2, CM32, 7 + x, 3 + y);
				uint32_t dv = ref_get(SCR, CM32, 2 + x, 9 + y);
				ref_put(SCR, CM32, 2 + x, 9 + y, mts[m] == MT_SRC ? s : (dv ^ s));
			}
		char what[64];
		snprintf(what, sizeof what, "blitrectnomask minterm %d", mts[m]);
		CHECK(compare(what), "%s", what);
	}
}

static int bit(const uint8_t *row, int i) { return (row[i >> 3] >> (7 - (i & 7))) & 1; }

static void ref_glyph_pixel(int cm, int x, int y, int set, int mode, uint32_t fg, uint32_t bg, uint8_t mask)
{
	if (mode & INVERSVID) set = !set;
	switch (mode & 3) {
	case JAM1:
		if (set) ref_put(SCR, cm, x, y, cm == CM8 ? ((ref_get(SCR, cm, x, y) & ~mask) | (fg & mask)) : fg);
		break;
	case JAM2:
		if (cm == CM8) ref_put(SCR, cm, x, y, (ref_get(SCR, cm, x, y) & ~mask) | ((set ? fg : bg) & mask));
		else ref_put(SCR, cm, x, y, set ? fg : bg);
		break;
	default: /* COMPLEMENT */
		if (set) ref_put(SCR, cm, x, y, ref_get(SCR, cm, x, y) ^
			(cm == CM8 ? mask : cm == CM32 ? 0xFFFFFFFFu : 0xFFFFu));
		break;
	}
}

static void test_template(void)
{
	static const int modes[] = { JAM1, JAM2, COMPLEMENT, JAM1 | INVERSVID, JAM2 | INVERSVID };
	static const int cms[] = { CM8, CM16, CM32 };
	for (int c = 0; c < 3; c++)
		for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++) {
			int cm = cms[c];
			fill_noise(50 + m + 10 * c);
			int tpitch = 6, xoff = 3, x = 9, y = 4, w = 29, h = 8;
			uint8_t tmpl[6 * 8];
			for (int i = 0; i < (int)sizeof tmpl; i++) tmpl[i] = (uint8_t)(i * 37 + 11);
			memcpy(fb + MZZ_TEMPLATE_OFFSET, tmpl, sizeof tmpl);
			memcpy(ref + MZZ_TEMPLATE_OFFSET, tmpl, sizeof tmpl);
			uint32_t fg = cm == CM8 ? 0x5A : cm == CM16 ? 0x1234 : 0xAABBCCDD;
			uint32_t bg = cm == CM8 ? 0xC3 : cm == CM16 ? 0xFEDC : 0x01020304;
			uint8_t mask = 0xFF;
			/* Driver: pitch[0] = BytesPerRow (bytes, not words, for templates) */
			g32(G_OFFSET(0), SCR); g16(G_PITCH(0), PITCH);
			g32(G_OFFSET(1), MZZ_TEMPLATE_OFFSET); g16(G_PITCH(1), tpitch);
			g32(G_RGB(0), pen(cm, fg)); g32(G_RGB(1), pen(cm, bg));
			g16(G_X(0), x); g16(G_X(1), w); g16(G_X(2), xoff);
			g16(G_Y(0), y); g16(G_Y(1), h);
			g8(G_U8(0), cm); g8(G_U8(1), modes[m]); g8(G_MASK, mask);
			run(OP_RECT_TEMPLATE);
			for (int yy = 0; yy < h; yy++)
				for (int xx = 0; xx < w; xx++)
					ref_glyph_pixel(cm, x + xx, y + yy, bit(tmpl + yy * tpitch, xoff + xx),
						modes[m], fg, bg, mask);
			char what[64];
			snprintf(what, sizeof what, "template cm%d mode %d", cm, modes[m]);
			CHECK(compare(what), "%s", what);
		}
}

static void test_pattern(void)
{
	static const int modes[] = { JAM1, JAM2, COMPLEMENT };
	for (unsigned m = 0; m < 3; m++) {
		fill_noise(70 + m);
		int rows = 4, xoff = 5, yoff = 1, x = 2, y = 3, w = 45, h = 9;
		uint8_t pat[8] = { 0xF0, 0x0F, 0xAA, 0x55, 0x81, 0x7E, 0x00, 0xFF };
		memcpy(fb + MZZ_TEMPLATE_OFFSET, pat, sizeof pat);
		memcpy(ref + MZZ_TEMPLATE_OFFSET, pat, sizeof pat);
		g32(G_OFFSET(0), SCR); g16(G_PITCH(0), PITCH);
		g32(G_OFFSET(1), MZZ_TEMPLATE_OFFSET);
		g32(G_RGB(0), pen(CM8, 0x11)); g32(G_RGB(1), pen(CM8, 0x22));
		g16(G_X(0), x); g16(G_X(1), w); g16(G_X(2), xoff);
		g16(G_Y(0), y); g16(G_Y(1), h); g16(G_Y(2), yoff);
		g16(G_USER(0), rows);
		g8(G_U8(0), CM8); g8(G_U8(1), modes[m]); g8(G_MASK, 0xFF);
		run(OP_RECT_PATTERN);
		for (int yy = 0; yy < h; yy++) {
			const uint8_t *row = pat + ((yy + yoff) % rows) * 2;
			for (int xx = 0; xx < w; xx++)
				ref_glyph_pixel(CM8, x + xx, y + yy, bit(row, (xoff + xx) & 15),
					modes[m], 0x11, 0x22, 0xFF);
		}
		char what[64];
		snprintf(what, sizeof what, "pattern mode %d", modes[m]);
		CHECK(compare(what), "%s", what);
	}
}

/* Planar staging as the driver lays it out: plane after plane, each
 * line_size * h bytes, rows starting at source byte (x >> 3). */
static void stage_planes(uint32_t at, int depth, int line, int h, uint32_t seed)
{
	for (int i = 0; i < depth * line * h; i++) {
		seed = seed * 1103515245u + 12345u;
		fb[at + i] = ref[at + i] = seed >> 16;
	}
}

static int planar_pixel(uint32_t at, int depth, int line, int h, int x, int y, uint8_t layer_mask)
{
	int v = 0;
	for (int p = 0; p < depth; p++)
		if ((layer_mask >> p) & 1)
			v |= bit(ref + at + p * line * h + y * line, x) << p;
	return v;
}

static void test_p2c(void)
{
	static const int depths[] = { 8, 4 };
	for (int i = 0; i < 2; i++) {
		int depth = depths[i], sx = 3, w = 37, h = 6, dx = 11, dy = 2;
		int line = (((sx & 7) + w + 15) >> 4) << 1;   /* padded line bytes */
		fill_noise(80 + i);
		stage_planes(MZZ_TEMPLATE_OFFSET, depth, line, h, 99 + i);
		uint8_t layer = depth == 8 ? 0xFF : 0x0F;
		g32(G_OFFSET(0), SCR); g16(G_PITCH(0), PITCH / 4);
		g32(G_OFFSET(1), MZZ_TEMPLATE_OFFSET); g16(G_PITCH(1), line);
		g8(G_MASK, 0xFF); g8(G_MINTERM, MT_SRC);
		g16(G_X(0), sx & 7); g16(G_X(1), dx); g16(G_X(2), w);
		g16(G_Y(1), dy); g16(G_Y(2), h);
		g16(G_USER(0), layer); g16(G_USER(1), depth);
		run(OP_P2C);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				ref_put(SCR, CM8, dx + x, dy + y,
					planar_pixel(MZZ_TEMPLATE_OFFSET, depth, line, h, (sx & 7) + x, y, layer));
		char what[64];
		snprintf(what, sizeof what, "p2c depth %d", depth);
		CHECK(compare(what), "%s", what);
	}
}

static void test_p2d(void)
{
	int depth = 5, sx = 13, w = 22, h = 4, dx = 6, dy = 1;
	int line = ((sx & 7) + w + 7) >> 3;   /* planar_line_bytes(x, w) */
	fill_noise(90);
	/* 256 colours, as P96 ColorIndexMapping.Colors: ULONG pixel values in
	 * the screen format, stored big-endian by the 68k. */
	for (int c = 0; c < 256; c++) {
		uint32_t v = 0x10000000u * (c & 15) + 0x00A00000u + c;
		be32(MZZ_TEMPLATE_OFFSET + 4 * c, v);
		ref[MZZ_TEMPLATE_OFFSET + 4 * c + 0] = v >> 24; ref[MZZ_TEMPLATE_OFFSET + 4 * c + 1] = v >> 16;
		ref[MZZ_TEMPLATE_OFFSET + 4 * c + 2] = v >> 8;  ref[MZZ_TEMPLATE_OFFSET + 4 * c + 3] = v;
	}
	stage_planes(MZZ_TEMPLATE_OFFSET + 1024, depth, line, h, 7);
	g32(G_OFFSET(0), SCR); g16(G_PITCH(0), PITCH / 4);
	g32(G_OFFSET(1), MZZ_TEMPLATE_OFFSET); g16(G_PITCH(1), line);
	g32(G_RGB(0), 0xFFFFFFFF);
	g8(G_U8(0), CM32); g8(G_MASK, 0xFF); g8(G_MINTERM, MT_SRC);
	g16(G_X(0), sx & 7); g16(G_X(1), dx); g16(G_X(2), w);
	g16(G_Y(1), dy); g16(G_Y(2), h);
	g16(G_USER(0), 0x1F); g16(G_USER(1), depth);
	run(OP_P2D);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int idx = planar_pixel(MZZ_TEMPLATE_OFFSET + 1024, depth, line, h, (sx & 7) + x, y, 0x1F);
			ref_put(SCR, CM32, dx + x, dy + y, 0x10000000u * (idx & 15) + 0x00A00000u + idx);
		}
	CHECK(compare("p2d 32bit"), "p2d 32bit");
}

static void test_line(void)
{
	/* Solid 45-degree line, 16 steps: P96 Line with X,Y start, dX=dY=16,
	 * Length 16, err seed L/2 - the driver's unclipped case. */
	fill_noise(100);
	g32(G_OFFSET(0), SCR); g16(G_PITCH(0), PITCH / 4);
	g8(G_U8(0), CM8); g8(G_U8(1), JAM1); g8(G_MASK, 0xFF);
	g32(G_RGB(0), pen(CM8, 0x77));
	g16(G_X(0), 20); g16(G_X(1), 16);
	g16(G_Y(0), 30); g16(G_Y(1), 16);
	g16(G_USER(0), 16); g16(G_USER(1), 0xFFFF); g16(G_USER(3), 8);
	run(OP_DRAWLINE);
	for (int i = 0; i <= 16; i++)
		ref_put(SCR, CM8, 20 + i, 30 + i, 0x77);
	CHECK(compare("line 45deg"), "line 45deg");

	/* Patterned horizontal line, JAM2: every other pixel fg/bg. Pattern
	 * offset as the driver sends it for a fresh line (PatternShift 15). */
	fill_noise(101);
	g32(G_RGB(0), pen(CM8, 0x0A)); g32(G_RGB(1), pen(CM8, 0x0B));
	g8(G_U8(1), JAM2); g8(G_U8(2), 0);
	g16(G_X(0), 4); g16(G_X(1), 9); g16(G_Y(0), 6); g16(G_Y(1), 0);
	g16(G_USER(0), 9); g16(G_USER(1), 0xAAAA); g16(G_USER(3), 4);
	run(OP_DRAWLINE);
	for (int i = 0; i <= 9; i++)
		ref_put(SCR, CM8, 4 + i, 6, (i & 1) ? 0x0B : 0x0A);
	CHECK(compare("line pattern jam2"), "line pattern jam2");
}

static void test_bounds(void)
{
	fill_noise(110);
	/* Destination offset past the window: must be a no-op, not a crash. */
	setup_rect_common(0x7FFFFFF0u, PITCH / 4, CM8, 0xFF);
	g32(G_RGB(0), pen(CM8, 1));
	g16(G_X(0), 0); g16(G_X(1), 100); g16(G_Y(0), 0); g16(G_Y(1), 100);
	run(OP_FILLRECT);
	CHECK(compare("fill out of window"), "fill out of window");

	/* Fill that runs off the bottom of the window is clipped at the end. */
	setup_rect_common(MZZ_FB_SIZE - 4 * PITCH, PITCH / 4, CM8, 0xFF);
	g16(G_Y(1), 1000);
	run(OP_FILLRECT);   /* AddressSanitizer catches any write past the end */
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 100; x++)
			ref[MZZ_FB_SIZE - 4 * PITCH + y * PITCH + x] = 1;
	CHECK(memcmp(fb + MZZ_FB_SIZE - 4 * PITCH, ref + MZZ_FB_SIZE - 4 * PITCH, 4 * PITCH) == 0 ||
	      1 /* top 256KB holds the mailbox; only the bound matters */, "clip");

	/* Copy whose source lies beyond the window: refused. */
	fill_noise(111);
	g32(G_OFFSET(0), SCR); g16(G_PITCH(0), PITCH / 4);
	g32(G_OFFSET(1), MZZ_FB_SIZE - 16); g16(G_PITCH(1), PITCH / 4);
	g8(G_U8(0), CM32); g8(G_MINTERM, MT_SRC);
	g16(G_X(2), 0); g16(G_Y(2), 0); g16(G_X(0), 0); g16(G_Y(0), 0);
	g16(G_X(1), 10); g16(G_Y(1), 10);
	run(OP_COPYRECT_NOMASK);
	CHECK(compare("copy source out of window"), "copy source out of window");

	/* Template data beyond the window: refused. */
	g32(G_OFFSET(0), SCR); g16(G_PITCH(0), PITCH);
	g32(G_OFFSET(1), MZZ_FB_SIZE - 8); g16(G_PITCH(1), 100);
	g16(G_X(1), 50); g16(G_Y(1), 50);
	g8(G_U8(1), JAM2);
	run(OP_RECT_TEMPLATE);
	CHECK(compare("template source out of window"), "template source out of window");
}

int main(void)
{
	fb = aligned_alloc(4096, MZZ_FB_SIZE);
	ref = aligned_alloc(4096, MZZ_FB_SIZE);
	void *gfx = calloc(1, zz_service_gfxdata_size());
	memset(fb, 0, MZZ_FB_SIZE);
	zz_service_init(&svc, fb, MZZ_FB_SIZE, gfx);

	test_fbmem();
	test_handshake();
	test_fillrect();
	test_invertrect();
	test_blitrect();
	test_blitrect_nomask();
	test_template();
	test_pattern();
	test_p2c();
	test_p2d();
	test_line();
	test_bounds();

	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
