/*
 * Flush the Minimig FPGA CPU cache from the 68k (m68k only).
 *
 * The RTG window is cached by cpu_cache_new in ddram_ctrl, which clears
 * itself on a 0->1 edge of CACR bit 3 (CI) as seen by the TG68 - and the
 * TG68 keeps that bit latched. exec's CacheClearU() only ever writes CI=1,
 * so after the first call there is no edge and nothing is cleared (until
 * some CacheControl() happens to write CI=0). Toggle CI 0->1 here instead.
 *
 * The cache samples CACR only while the CPU is not using that RAM
 * controller (cpu_cache_new: cc_clr_r updates when !cpu_cs); the code runs
 * from fast RAM on the same controller, so a chip register read after each
 * CACR write guarantees it sees both levels. On a real 68020/030 CI is a
 * plain "clear instruction cache" command, so the extra write is harmless.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MISTER_ZZ9000_CACHE_H
#define MISTER_ZZ9000_CACHE_H

#include <proto/exec.h>

static inline void mister_fpga_cache_clear(void)
{
	APTR ssp = SuperState();   /* NULL if already in supervisor mode */

	__asm__ volatile(
		"	movec	%%cacr,%%d0\n"
		"	moveq	#-9,%%d1\n"
		"	and.l	%%d1,%%d0\n"
		"	movec	%%d0,%%cacr\n"     /* CI = 0 */
		"	tst.w	0xdff006\n"
		"	tst.w	0xdff006\n"
		"	moveq	#8,%%d1\n"
		"	or.l	%%d1,%%d0\n"
		"	movec	%%d0,%%cacr\n"     /* CI = 1: FPGA cache clears */
		"	tst.w	0xdff006\n"
		"	tst.w	0xdff006\n"
		: : : "d0", "d1", "cc", "memory");

	if (ssp)
		UserState(ssp);
}

#endif
