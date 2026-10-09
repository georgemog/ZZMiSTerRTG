#ifndef __MINIMIG_ZZ9000_H__
#define __MINIMIG_ZZ9000_H__

// ZZ9000-compatible blitter for the Minimig Z3 RTG board.
//
// The Amiga side is ZZ9000.card (zz9000/amiga) built for MiSTer; it queues
// ZZ9000 "DMA ops" in a mailbox at the top of the RTG framebuffer. This
// module runs the official ZZ9000OS blitter on the ARM against that same
// framebuffer (DDR3 at 0x27000000), from a thread so blits do not wait for
// the main loop. Protocol: zz9000/common/mister_zz9000.h.

// Start the service thread. Called when the Minimig core is identified.
void zz9000_start(void);

// Stop the thread and unmap. Called before another core is loaded.
void zz9000_stop(void);

#endif
