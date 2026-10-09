/*
 * MiSTerZZInfo - show the state of the MiSTer ZZ9000 RTG blitter.
 *
 * Read-only: finds the Minimig Z3 RTG board, dumps the rtg.v scanout
 * registers and the driver<->ARM mailbox (common/mister_zz9000.h), and
 * watches the ARM heartbeat for half a second. Writes nothing, so it is
 * safe to run while MiSTerZZ.card is driving the screen.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <libraries/configvars.h>
#include <stdio.h>

/* NDK 3.2 ULONG is unsigned int (32-bit): printf with %u/%X. */

#include "../../common/mister_zz9000.h"
#include "../../common/mister_zz9000_cache.h"

struct ExpansionBase *ExpansionBase;

static ULONG mb(UBYTE *board, ULONG field)
{
	mister_fpga_cache_clear();   /* the FPGA CPU cache holds RTG window lines */
	return *(volatile ULONG *)(board + MZZ_MBOX_OFFSET + field);
}

static void fourcc(ULONG v, char *out)
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(v >> (24 - 8 * i));
		out[i] = (c >= 32 && c < 127) ? c : '.';
	}
	out[4] = 0;
}

int main(void)
{
	struct ConfigDev *cd;
	char f1[5], f2[5];

	if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library", 0)))
		return 20;
	cd = FindConfigDev(NULL, MZZ_RTG_MANUFACTURER, MZZ_RTG_PRODUCT);
	CloseLibrary((struct Library *)ExpansionBase);
	if (!cd) {
		printf("Minimig Z3 RTG board (%04X/%02X) not found - core without RTG?\n",
			MZZ_RTG_MANUFACTURER, MZZ_RTG_PRODUCT);
		return 10;
	}

	UBYTE *board = (UBYTE *)cd->cd_BoardAddr;
	volatile UWORD *regs = (volatile UWORD *)(board + MZZ_REGS_OFFSET + MZZ_REG_SUB_OFFSET);

	printf("RTG board        $%08X, %d KB, %s\n", (ULONG)board,
		(ULONG)cd->cd_BoardSize >> 10,
		(cd->cd_Flags & CDF_CONFIGME) ? "unclaimed" : "claimed by a driver");
	printf("rtg.v ID         $%04X (expect $5001)\n", regs[7]);
	printf("scanout          enable %u format $%02X %ux%u stride %u addr $%08X\n",
		regs[3], regs[2], regs[4], regs[5], regs[6],
		((ULONG)regs[0] << 16) | regs[1]);

	ULONG req = mb(board, MZZ_MB_REQ_MAGIC), ack = mb(board, MZZ_MB_ACK_MAGIC);
	fourcc(req, f1);
	fourcc(ack, f2);
	printf("driver request   %s session $%08X kick %u op %u\n", f1,
		mb(board, MZZ_MB_SESSION), mb(board, MZZ_MB_KICK), mb(board, MZZ_MB_OP));
	printf("ARM answer       %s session $%08X taken %u done %u\n", f2,
		mb(board, MZZ_MB_ACK_SESSION), mb(board, MZZ_MB_TAKEN), mb(board, MZZ_MB_DONE));
	printf("ARM protocol     v%u caps $%08X, %u ops, %u rejected\n",
		mb(board, MZZ_MB_VERSION), mb(board, MZZ_MB_CAPS),
		mb(board, MZZ_MB_OPS), mb(board, MZZ_MB_REJECTED));

	ULONG st = mb(board, MZZ_MB_DRV_STATE);
	printf("driver state     $%X (%s%s%s), %u hook calls, %u to P96 default, last format %u\n",
		st, (st & MZZ_DRV_ATTACHED) ? "attached " : "",
		(st & MZZ_DRV_INITCARD) ? "initcard " : "",
		(st & MZZ_DRV_BLITTER) ? "BIF_BLITTER" : "",
		mb(board, MZZ_MB_DRV_HOOKS), mb(board, MZZ_MB_DRV_FALLBACKS),
		mb(board, MZZ_MB_DRV_LAST_FMT));

	ULONG hb0 = mb(board, MZZ_MB_HEARTBEAT);
	Delay(25);
	ULONG hb1 = mb(board, MZZ_MB_HEARTBEAT);
	printf("ARM heartbeat    %u -> %u in 0.5s\n", hb0, hb1);

	if (req != MZZ_REQ_MAGIC)
		printf("=> MiSTerZZ.card has not initialised the mailbox (not loaded?)\n");
	else if (ack != MZZ_ACK_MAGIC || mb(board, MZZ_MB_ACK_SESSION) != mb(board, MZZ_MB_SESSION))
		printf("=> no MiSTer blitter attached: MiSTer binary without ZZ9000 support?\n");
	else if (hb0 == hb1)
		printf("=> ARM attached but heartbeat stopped: MiSTer binary stopped/restarted?\n");
	else
		printf("=> ARM blitter attached and running\n");
	return 0;
}
