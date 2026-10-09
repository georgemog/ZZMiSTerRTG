/*
 * MiSTerZZTest - hardware test for MiSTerZZ.card.
 *
 * Draws one scene with graphics.library twice: on a Picasso96 screen of the
 * MiSTer RTG board (so every primitive goes through the accelerated hooks
 * and the ARM blitter), and on a planar chip-RAM bitmap rendered by the
 * native Amiga blitter as the reference. Then compares the two pixel for
 * pixel, and reports how many ops the ARM executed (mailbox OPS counter).
 * Optionally benchmarks the primitives.
 *
 *   MiSTerZZTest [DEPTH 8|16|32] [HOLD secs] [BENCH] [LOG file]
 *
 * 16/32-bit screens are compared through the pen palette (quantised for
 * 16-bit); the scene steps whose meaning differs between pens and direct
 * colour (COMPLEMENT, XOR minterm) only run on 8-bit screens.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/expansion.h>
#include <proto/timer.h>
#include <proto/Picasso96.h>
#include <graphics/rastport.h>
#include <graphics/gfxmacros.h>
#include <intuition/screens.h>
#include <libraries/configvars.h>
#include <devices/timer.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "../../common/mister_zz9000.h"
#include "../../common/mister_zz9000_cache.h"

/* NDK 3.2: ULONG is unsigned int - printf with %u/%X. */

struct Library *P96Base;
struct ExpansionBase *ExpansionBase;
struct Device *TimerBase;

#define AW 320
#define AH 200

static BPTR logfh;
static UBYTE *board;
static ULONG pal[256];

static void out(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	fflush(stdout);
	if (logfh)
		FPuts(logfh, (STRPTR)buf);
}

static ULONG mb(ULONG field)
{
	if (!board) return 0;
	mister_fpga_cache_clear();
	return *(volatile ULONG *)(board + MZZ_MBOX_OFFSET + field);
}

static UWORD area_pat[4] = { 0xF0F0, 0x0F0F, 0xAAAA, 0x5555 };

/* The scene. full = also the steps that only make sense with pens. */
static void scene(struct RastPort *rp, struct BitMap *planar, int full)
{
	int i;

	SetDrMd(rp, JAM1);
	SetAPen(rp, 0);
	RectFill(rp, 0, 0, AW - 1, AH - 1);

	/* FillRect: assorted sizes and odd edges */
	for (i = 0; i < 24; i++) {
		SetAPen(rp, (i * 17 + 3) & 255);
		RectFill(rp, (i * 13) % 250, (i * 7) % 150, (i * 13) % 250 + 3 + i * 3, (i * 7) % 150 + 1 + i);
	}

	/* InvertRect (COMPLEMENT fill) */
	if (full) {
		SetDrMd(rp, COMPLEMENT);
		RectFill(rp, 20, 30, 140, 90);
		RectFill(rp, 101, 5, 102, 190);
		SetDrMd(rp, JAM1);
	}

	/* BlitTemplate: text in JAM1, JAM2, INVERSVID (+COMPLEMENT) */
	SetAPen(rp, 201); SetBPen(rp, 77);
	Move(rp, 5, 120); Text(rp, "MiSTer ZZ9000 JAM1 text 0123", 28);
	SetDrMd(rp, JAM2);
	Move(rp, 7, 132); Text(rp, "JAM2 text with background!", 26);
	SetDrMd(rp, JAM2 | INVERSVID);
	Move(rp, 3, 144); Text(rp, "inverse video", 13);
	if (full) {
		SetDrMd(rp, COMPLEMENT);
		Move(rp, 60, 156); Text(rp, "complement", 10);
	}
	SetDrMd(rp, JAM1);

	/* DrawLine: solid fans, then patterned JAM2 */
	for (i = 0; i < 16; i++) {
		SetAPen(rp, 128 + i);
		Move(rp, 250, 100);
		Draw(rp, 180 + i * 9, (i & 1) ? 10 : 190);
		Move(rp, 250, 100);
		Draw(rp, (i & 2) ? 319 : 190, 20 + i * 10);
	}
	SetDrPt(rp, 0xF0F0);
	SetDrMd(rp, JAM2);
	SetAPen(rp, 33); SetBPen(rp, 66);
	Move(rp, 200, 195); Draw(rp, 315, 160);
	Move(rp, 200, 160); Draw(rp, 315, 160);
	SetDrPt(rp, 0xFFFF);
	SetDrMd(rp, JAM1);

	/* BlitPattern: area pattern, JAM2 and JAM1 */
	SetAfPt(rp, area_pat, 2);
	SetAPen(rp, 99); SetBPen(rp, 150);
	SetDrMd(rp, JAM2);
	RectFill(rp, 150, 110, 230, 150);
	SetDrMd(rp, JAM1);
	SetAPen(rp, 250);
	RectFill(rp, 165, 100, 205, 175);
	SetAfPt(rp, NULL, 0);

	/* BlitRect: overlapping scrolls in all four directions */
	ScrollRaster(rp, 3, 2, 0, 0, 160, 100);
	ScrollRaster(rp, -5, -1, 10, 10, 150, 90);
	ScrollRaster(rp, 0, 7, 160, 0, 319, 99);
	ScrollRaster(rp, -1, 0, 0, 100, 140, 199);

	/* BlitRectNoMaskComplete: copy and XOR within the screen */
	ClipBlit(rp, 0, 0, rp, 260, 140, 40, 30, 0xC0);
	if (full)
		ClipBlit(rp, 20, 20, rp, 270, 20, 40, 30, 0x60);

	/* BlitPlanar2Chunky / 2Direct: planar chip bitmap onto the screen.
	 * Targets are cleared first: with a shallower source, a planar
	 * destination keeps its upper planes, a chunky/direct one cannot. */
	SetAPen(rp, 0);
	RectFill(rp, 290, 60, 318, 129);
	RectFill(rp, 5, 170, 124, 194);
	BltBitMapRastPort(planar, 3, 0, rp, 290, 60, 29, 70, 0xC0);
	BltBitMapRastPort(planar, 0, 5, rp, 5, 170, 120, 25, 0xC0);

	WaitBlit();
}

static void make_planar(struct BitMap *bm)
{
	for (int p = 0; p < bm->Depth; p++)
		for (int y = 0; y < bm->Rows; y++)
			for (int x = 0; x < bm->BytesPerRow; x++)
				bm->Planes[p][y * bm->BytesPerRow + x] =
					(UBYTE)((x * 37 + y * 11) ^ (p * 0x5B) ^ (y << p));
}

static ULONG expect_rgb(int depth, ULONG rgb)
{
	if (depth == 16)
		return rgb & 0xF8FCF8;
	return rgb & 0xFFFFFF;
}

static int compare(struct Screen *scr, struct RastPort *ref, int depth)
{
	static UBYTE refline[AW + 16], scrline[AW + 16];
	struct RastPort tmp;
	int bad = 0;

	InitRastPort(&tmp);
	tmp.BitMap = AllocBitMap(AW + 16, 1, 8, BMF_CLEAR, NULL);
	for (int y = 0; y < AH; y++) {
		ReadPixelLine8(ref, 0, y, AW, refline, &tmp);
		if (depth == 8)
			ReadPixelLine8(&scr->RastPort, 0, y, AW, scrline, &tmp);
		for (int x = 0; x < AW; x++) {
			int ok;
			ULONG got, want;
			if (depth == 8) {
				got = scrline[x];
				want = refline[x];
				ok = got == want;
			} else {
				got = expect_rgb(depth, p96ReadPixel(&scr->RastPort, x, y));
				want = expect_rgb(depth, pal[refline[x]]);
				ok = got == want;
			}
			if (!ok) {
				if (bad < 12)
					out("  mismatch at %3d,%3d: got %06X want %06X (ref pen %u)\n",
						x, y, got, want, refline[x]);
				bad++;
			}
		}
	}
	FreeBitMap(tmp.BitMap);
	return bad;
}

static double secs(struct EClockVal *a, struct EClockVal *b, ULONG freq)
{
	double ta = a->ev_hi * 4294967296.0 + a->ev_lo;
	double tb = b->ev_hi * 4294967296.0 + b->ev_lo;
	return (tb - ta) / freq;
}

static void bench(struct RastPort *rp)
{
	struct EClockVal t0, t1;
	ULONG freq, ops0;
	int i;
	char line[41];

	memset(line, 'W', 40);
	line[40] = 0;

#define BENCH(label, n, body) do { \
	ops0 = mb(MZZ_MB_OPS); freq = ReadEClock(&t0); \
	for (i = 0; i < (n); i++) { body; } \
	WaitBlit(); ReadEClock(&t1); \
	double s = secs(&t0, &t1, freq); \
	out("  %-28s %6d in %6.3fs = %8.1f/s  (ARM ops %u)\n", label, (n), s, (n) / s, mb(MZZ_MB_OPS) - ops0); \
	} while (0)

	SetDrMd(rp, JAM1);
	BENCH("RectFill 64x64", 2000, { SetAPen(rp, i & 255); RectFill(rp, (i * 7) % 250, (i * 3) % 130, (i * 7) % 250 + 63, (i * 3) % 130 + 63); });
	BENCH("RectFill 320x200", 300, { SetAPen(rp, i & 255); RectFill(rp, 0, 0, AW - 1, AH - 1); });
	BENCH("ScrollRaster 320x200 by 1", 300, ScrollRaster(rp, 0, 1, 0, 0, AW - 1, AH - 1));
	SetAPen(rp, 1); SetBPen(rp, 2); SetDrMd(rp, JAM2);
	BENCH("Text 40 chars JAM2", 1000, { Move(rp, 0, 8 + (i % 24) * 8); Text(rp, line, 40); });
	SetDrMd(rp, JAM1);
	BENCH("Draw line ~200px", 3000, { SetAPen(rp, i & 255); Move(rp, i % 320, 0); Draw(rp, 319 - i % 320, 199); });
#undef BENCH
}

int main(void)
{
	LONG args[4] = { 0 };
	struct RDArgs *rda = ReadArgs((STRPTR)"DEPTH/N,HOLD/N,BENCH/S,LOG/K", args, NULL);
	int depth = args[0] ? *(LONG *)args[0] : 8;
	int hold = args[1] ? *(LONG *)args[1] : 0;
	int rc = 10;
	struct timerequest tr;
	struct Screen *scr = NULL;
	struct BitMap *refbm = NULL, *planar = NULL;
	struct ConfigDev *cd;

	if (args[3])
		logfh = Open((STRPTR)args[3], MODE_NEWFILE);

	if (!(P96Base = OpenLibrary((STRPTR)"Picasso96API.library", 2))) {
		out("needs Picasso96API.library\n");
		goto done;
	}
	if ((ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library", 0))) {
		if ((cd = FindConfigDev(NULL, MZZ_RTG_MANUFACTURER, MZZ_RTG_PRODUCT)))
			board = (UBYTE *)cd->cd_BoardAddr;
		CloseLibrary((struct Library *)ExpansionBase);
	}
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0)) {
		out("no timer.device\n");
		goto done;
	}
	TimerBase = tr.tr_node.io_Device;

	ULONG fmts = depth == 8 ? RGBFF_CLUT : depth == 16 ? RGBFF_R5G6B5PC : RGBFF_R8G8B8A8;
	ULONG id = p96BestModeIDTags(P96BIDTAG_NominalWidth, 640, P96BIDTAG_NominalHeight, 480,
		P96BIDTAG_FormatsAllowed, fmts, TAG_DONE);
	if (id == INVALID_ID) {
		out("no %d-bit P96 mode available\n", depth);
		goto done_timer;
	}
	out("MiSTerZZTest: mode $%08X %ux%u, %s, depth %d, ARM %s\n", id,
		p96GetModeIDAttr(id, P96IDA_WIDTH), p96GetModeIDAttr(id, P96IDA_HEIGHT),
		(char *)p96GetModeIDAttr(id, P96IDA_BOARDNAME), depth,
		mb(MZZ_MB_ACK_MAGIC) == MZZ_ACK_MAGIC ? "attached" : "NOT attached");

	scr = p96OpenScreenTags(P96SA_DisplayID, id, P96SA_RGBFormat,
		depth == 8 ? RGBFB_CLUT : depth == 16 ? RGBFB_R5G6B5PC : RGBFB_R8G8B8A8,
		P96SA_Depth, depth == 32 ? 24 : depth, P96SA_Quiet, TRUE, P96SA_ShowTitle, FALSE, TAG_DONE);
	if (!scr) {
		out("cannot open screen\n");
		goto done_timer;
	}

	for (int i = 0; i < 256; i++) {
		ULONG r = i, g = (i * 7) & 255, b = (i * 13 + 64) & 255;
		pal[i] = (r << 16) | (g << 8) | b;
		SetRGB32(&scr->ViewPort, i, r * 0x01010101u, g * 0x01010101u, b * 0x01010101u);
	}

	refbm = AllocBitMap(AW, AH, 8, BMF_CLEAR, NULL);
	planar = AllocBitMap(160, 80, depth == 8 ? 8 : 5, BMF_CLEAR, NULL);
	if (!refbm || !planar) {
		out("out of chip memory\n");
		goto done_screen;
	}
	make_planar(planar);

	struct RastPort ref;
	InitRastPort(&ref);
	ref.BitMap = refbm;
	SetFont(&ref, scr->RastPort.Font);

	ULONG ops0 = mb(MZZ_MB_OPS), rej0 = mb(MZZ_MB_REJECTED);
	scene(&scr->RastPort, planar, depth == 8);
	ULONG ops = mb(MZZ_MB_OPS) - ops0, rej = mb(MZZ_MB_REJECTED) - rej0;
	scene(&ref, planar, depth == 8);

	int bad = compare(scr, &ref, depth);
	out("scene: %u ARM ops, %u rejected, %d of %d pixels differ -> %s\n",
		ops, rej, bad, AW * AH, (bad == 0 && ops > 0) ? "PASS" : "FAIL");
	rc = (bad == 0 && ops > 0) ? 0 : 5;

	if (hold)
		Delay(hold * 50);

	if (args[2])
		bench(&scr->RastPort);

done_screen:
	if (planar) FreeBitMap(planar);
	if (refbm) FreeBitMap(refbm);
	CloseScreen(scr);
done_timer:
	CloseDevice((struct IORequest *)&tr);
done:
	if (P96Base) CloseLibrary(P96Base);
	if (logfh) Close(logfh);
	if (rda) FreeArgs(rda);
	return rc;
}
