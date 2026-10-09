# ZZ9000 accelerated RTG for the MiSTer Minimig core

The official ZZ9000 Picasso96 driver and the official ZZ9000OS blitter, ported
to the Minimig core's existing Zorro III RTG board. You get a hardware
accelerated P96 driver with no FPGA changes: the blitter runs on the MiSTer's
ARM, inside the Minimig part of the `MiSTer` binary, and draws straight into
the RTG framebuffer in DDR3.

```
 Amiga (TG68 020/030)                         MiSTer HPS (ARM, MiSTer binary)
 ─────────────────────                        ───────────────────────────────
 Picasso96
   └─ MiSTerZZ.card  (official ZZ9000.card)    minimig_zz9000.cpp  (thread)
        FillRect, BlitRect, BlitTemplate…        └─ zz9000_service.c (mailbox)
        fill GFXData + staging ─┐                    └─ dma_rtg.c  (official dispatcher)
        mailbox KICK ──────────┐│                        └─ gfx.c  (official ZZ9000OS blitter)
        wait DONE  ◄─────────┐ ││                                   │
                             │ ▼▼                                   ▼
            ┌──────────── Z3 RTG board, 8MB framebuffer = DDR3 @ 0x27000000 ──────────┐
            │ 0x000000 P96 video memory        │ 0x7C0000 mailbox   0x7C1000 GFXData │
            │          (scanout via rtg.v)     │ 0x7C2000 staging   0x7FC000 mouse   │
            └──────────────────────────────────┴─────────────────────────────────────┘
 Display control (mode size, panning, CLUT, pixel format, on/off) goes from
 the driver straight to the core's rtg.v registers at board + $800100.
```

## Layout

| Path | What |
|------|------|
| `amiga/rtg/` | Official ZZ9000 driver (`mntgfx-gcc.c` …), adapted → `MiSTerZZ.card` |
| `amiga/include/` | Official ZZ9000 driver headers, unchanged |
| `amiga/tools/MiSTerZZInfo.c` | Read-only status tool: board, scanout regs, mailbox, driver diagnostics |
| `amiga/tools/MiSTerZZTest.c` | Hardware test: accelerated scene vs native-blitter reference, plus benchmark |
| `amiga/tools/SetToolType.c` | CLI tooltype editor (to set `BOARDTYPE` on a monitor icon) |
| `common/mister_zz9000_cache.h` | FPGA CPU cache flush that actually works on the TG68 (see design notes) |
| `mister/zz9000os/` | Official ZZ9000OS `gfx.c`/`gfx.h`/`zz_video_modes.h` (unchanged), `dma_rtg.c` (adapted), `video.h` (shim) |
| `mister/zz9000_*.c` | MiSTer service: mailbox, build wrapper, framebuffer-safe memcpy |
| `mister/minimig_zz9000.cpp` | Thread + `/dev/mem` mapping, for `Main_MiSTer/support/minimig/` |
| `mister/install.py` | Copies the MiSTer side into a Main_MiSTer tree and hooks it up |
| `common/mister_zz9000.h` | The driver⇄ARM protocol, shared by both sides |
| `tests/host/` | Host test: drives the exact ARM code the way the 68k does |

`UPSTREAM.md` lists the upstream commits; the first `zz9000:` commit is the
verbatim import, so `git log -p -- zz9000/` shows every local change.

## Requirements

- A Minimig core with the **Zorro III RTG board** (mfr `$139C`, product `$30`,
  "Rok Krajnc Minimig Z3 GraphicsCard"): branch `rtg-z3-graphics-card` of
  `Minimig-AGA_MiSTer`, or the combined A4091+RTG+A2065 core.
- 68020 or 68030 CPU in the core (Zorro III).
- Picasso96 (3.x tested by upstream on ZZ9000).
- A `MiSTer` binary built with this module.

## Build

Amiga side (build server, bebbo GCC 6.5):
```bash
cd zz9000/amiga
export PATH=/opt/amiga/bin:$PATH
make p96-sdk        # once: Picasso96 developer headers into sdk/
make                # build/MiSTerZZ.card + MiSTerZZInfo, MiSTerZZTest, SetToolType
```

MiSTer side:
```bash
zz9000/mister/install.py /path/to/Main_MiSTer
cd /path/to/Main_MiSTer
export PATH=/opt/armV7-linux-gcc/bin:$PATH
make                # bin/MiSTer
```
`install.py` is idempotent. It adds `support/minimig/zz9000/*.c` to `C_SRC`
(upstream `gfx.c` is C and will not build as C++), and calls `zz9000_start()` /
`zz9000_stop()` next to `a2065_start()` / `a2065_stop()` in `user_io.cpp`.

Host tests (macOS/Linux, ASan + UBSan):
```bash
make -C zz9000/tests/host
```

## Install

1. MiSTer: install the new binary where the Minimig core will run it. Either
   replace `/media/fat/MiSTer` (`killall MiSTer`, copy, `reboot`; inittab
   starts it via sysinit and a running binary cannot be overwritten), or, if
   `MiSTer.ini [minimig]` has `main=<binary>`, replace that file while a
   non-Minimig core is loaded, then load the Minimig core. Its stdout goes to
   `/dev/console`; `MiSTerZZInfo` (step 3) is the practical way to see it.
2. Amiga: copy `MiSTerZZ.card` to `LIBS:Picasso96/`. Duplicate the existing
   MiSTer monitor in `DEVS:Monitors` (`MiSTer` → `MiSTerZZ`, with its
   `.info`), set `BOARDTYPE=MiSTerZZ` on the copy
   (`SetToolType DEVS:Monitors/MiSTerZZ BOARDTYPE=MiSTerZZ`), and move the
   old `MiSTer` monitor to `SYS:Storage/Monitors` (both drivers claim the same
   board; whichever runs first wins). In Picasso96Mode the board still
   appears as **"MiSTer"** (type 10), deliberately: `Picasso96Settings` keys
   screen modes on that name, so the existing MiSTer modes carry over.
3. Reboot the Amiga, then run `MiSTerZZInfo`. Expect
   `driver state $7 (attached initcard BIF_BLITTER)` and
   `=> ARM blitter attached and running`. `MiSTerZZTest DEPTH 8|16|32`
   should end in `PASS`.

If the MiSTer binary has no ZZ9000 support, the driver waits about a second at
boot, prints nothing on screen, and runs as a plain unaccelerated framebuffer
(like `MiSTer.card`). If the ARM side disappears later, the driver notices on
the next blit and drops to Picasso96's software routines.

## What is accelerated

The official ZZ9000 Zorro III code paths, run by the official ZZ9000OS
blitter: `FillRect`, `InvertRect`, `BlitRect`, `BlitRectNoMaskComplete`
(all minterms), `BlitTemplate` (text; JAM1/JAM2/COMPLEMENT/INVERSVID),
`BlitPattern`, `DrawLine` (patterned and solid), `BlitPlanar2Chunky`,
`BlitPlanar2Direct`.

Pixel formats: the ones `MiSTer.card` has proven on this scanout. CLUT (8-bit),
R5G6B5PC (16-bit) and R8G8B8A8 (32-bit) are accelerated. B8G8R8 (24-bit) is
offered too, but drawn in software (the ZZ9000 blitter has no 24-bit mode).
The ZZ9000 blitter does not care about byte order inside a pixel, only about
pixel size, so R5G6B5PC and RGBA use its 16-bit and 32-bit paths unchanged.

Not offered, because there is no ZZ9000 video formatter or ARM SDK behind
the Minimig board: hardware sprite (P96 uses its software sprite),
video-capture switch, screen split, secondary palette, DPMS, the firmware
off-screen bitmap allocator, YUV/PIP overlay. The driver reports firmware 2.0
so that the upstream feature gates switch these off by themselves.

## Design notes

**Synchronous ops, same as the ZZ9000.** On real hardware the op register write
does not complete until the firmware has run the op. Here `mister_kick()`
writes `OP`, increments `KICK`, and spins until the ARM writes `DONE`. One
`GFXData`, no queue: the upstream driver's byte caches and register
assumptions stay valid, and `WaitBlitter()` has nothing to do.

**FPGA cache coherency.** `cpu_cache_new` in `ddram_ctrl` caches CPU reads of
the RTG window. The ARM writes behind its back, so the driver flushes that
cache before every mailbox read and once more after `DONE`.
`CacheClearU()` does **not** work for this, which was found on hardware. The
cache clears on a 0→1 edge of CACR bit 3, the TG68 latches that bit, and exec
always writes it as 1, so only the first `CacheClearU()` after a
`CacheControl()` clears anything. The first kick then read a stale `DONE`,
timed out, and the driver dropped to software. `mister_fpga_cache_clear()`
(`common/mister_zz9000_cache.h`) writes CI=0 then CI=1 in supervisor mode,
with chip-register reads in between so the controller samples both levels.
The whole FPGA cache (instruction and data) is flushed every time, and this
is the main per-op cost. Inhibiting the cache for `sel_rtg` in
`cpu_wrapper.v` would remove the need for it (see below).

**Diagnostics.** The driver writes its init state and counts of hook calls,
fallbacks and the last format into spare mailbox words (`MZZ_MB_DRV_*`).
`MiSTerZZInfo` prints them, and Linux can read them with
`devmem 0x277C0080`.

**Strongly-ordered memory.** The framebuffer lies outside Linux RAM, so
`/dev/mem` maps it uncached and Strongly-ordered. ARMv7 faults every unaligned
access to such memory. The kernel can emulate scalar accesses slowly, but it
cannot emulate NEON. So `zz9000_blit.c` compiles the upstream `gfx.c` with
NEON off, auto-vectorisation off, store merging off and loop-to-libc
conversion off. It also routes `memcpy`/`memmove`/`memset` to `zz9000_fbmem.c`,
which uses aligned 32-bit accesses for the bulk of every copy, with
shift-merge when source and destination are misaligned. The build checks
this: `objdump -d` of `zz9000_blit.c.o` has 0 NEON loads/stores and no libc
string calls.

**Byte order.** The core byte-swaps every RTG CPU access, so DDR3 holds the
framebuffer in Amiga byte order, which is what the ZZ9000's ARM sees too. The
blitter therefore runs unmodified. Pens are read raw, as upstream does. The
ARM refuses to attach if the request magic arrives byte-swapped, and logs it.

**Safety.** Every offset that comes from the Amiga is range-checked against the
8MB window before a pointer is formed (`dma_rtg.c`). Destination rows are also
clamped by upstream's `fb_limit`. The ARM writes nothing in the window until
a driver has published `REQ_MAGIC`. It clears a stale `REQ_MAGIC` when the
core loads.

**Polling.** The thread runs on CPU0, away from MiSTer's main loop on CPU1.
It spins for 5ms after each op, sleeps 200µs when the driver is attached but
idle, and sleeps 20ms when no driver is attached. The driver's attach timeout
covers the 20ms sleep. On the 68k side, `DONE` is also written into a ring
of 64 words on separate FPGA cache lines (protocol v2). Each poll reads a
fresh line, so the driver flushes only once per op plus once per ring wrap.

## Status

**Working on hardware (2026-10-09).** Tested with
`minimig_20260906_A4091_RTG_A2065_v2.rbf` (68020, Z3 RTG board at
`$50000000`), Picasso96 rtg.library 43.538, and the MiSTer binary built from
the live A4091/A2091 tree plus this module.

- `MiSTerZZTest DEPTH 8/16/32`: **PASS** at every depth. The ~80 accelerated
  ops of the test scene (fills, invert, JAM1/JAM2/INVERSVID/COMPLEMENT text,
  solid and patterned lines, area patterns, overlapping scrolls in four
  directions, copy and XOR minterm blits, 8- and 5-plane planar blits) all
  ran on the ARM, and the result is 0 of 64000 pixels different from the
  same scene drawn on a planar bitmap by the native Amiga blitter.
- Host test: 242 checks pass under ASan/UBSan.

Benchmark (`MiSTerZZTest BENCH`, 640x480 screen, ops/s), MiSTerZZ.card vs the
previous software-only MiSTer.card:

| primitive | 8-bit ZZ | 8-bit sw | | 32-bit ZZ | 32-bit sw | |
|---|---:|---:|---:|---:|---:|---:|
| RectFill 64x64 | 3300 | 2211 | 1.5x | 2255 | 714 | 3.2x |
| RectFill 320x200 | 801 | 185 | 4.3x | 306 | 47 | 6.5x |
| ScrollRaster 320x200 | 334 | 98 | 3.4x | 109 | 26 | 4.2x |
| Text 40 chars JAM2 | 970 | 671 | 1.45x | 978 | 553 | 1.8x |
| Line ~200px | 3555 | 3934 | 0.9x | 3293 | 3933 | 0.84x |

Each op has a fixed cost of about 0.28ms: the synchronous mailbox
round-trip, the FPGA cache flush, and a cold 68k cache afterwards. Short lines
are therefore slightly slower than drawing them on the CPU, and everything
with area is faster.

Found during bring-up (all fixed): exec `CacheClearU()` does not flush the
FPGA cache (see design notes). The service thread must not share CPU1 with
MiSTer's busy main loop (it is pinned to CPU0, which nearly doubled op
throughput). The P96 board name must stay "MiSTer" to keep the existing
screen modes.

## Possible next steps

- RTL: drop `sel_rtg` from the CPU cache (feed `cache_inhibit`, but not
  `ddr_swap`, from it). Then the driver can lose its FPGA cache flushes,
  and ops become much cheaper for the 68k.
- Send short lines and tiny fills to the P96 software routine, as upstream
  already does for its Zorro II path.
- Queue several ops per kick, for asynchronous blits. This needs `WaitBlitter`
  to wait for real, and per-slot GFXData.
- Use a cached ARM mapping (kernel helper, or `u-dma-buf`-style reserved
  memory) for much faster pixel loops.
- Map the ZZ9000 hardware-sprite ops onto a MiSTer OSD/scaler sprite, if the
  core ever grows one.
