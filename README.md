# ZZMiSTerRTG — ZZ9000-accelerated RTG for the MiSTer Minimig core

A hardware-accelerated Picasso96 driver for the Minimig core's Zorro III RTG
board. It is built from the **official ZZ9000 sources** (BlitterStudio
`zz9000-drivers` and `zz9000-firmware`) and needs **no FPGA changes**:

- **Amiga side:** `MiSTerZZ.card`, the official `ZZ9000.card` with its
  accelerated P96 hooks unchanged. The only change is the doorbell: where the
  ZZ9000 driver writes an op to the card's `blitter_dma_op` register, this one
  hands the op to the MiSTer through a mailbox in the RTG framebuffer.
  Display control goes to the core's existing `rtg.v` registers, as
  `MiSTer.card` does.
- **MiSTer side:** the official ZZ9000OS blitter (`gfx.c`, unmodified), run
  by a thread in the Minimig part of the MiSTer main binary. It draws
  straight into the RTG framebuffer in DDR3 (`0x27000000`).

**Status: working on hardware (2026-10-09).** 8, 16 and 32-bit screens are
pixel-exact against the native Amiga blitter. Fills are up to 6.5x faster,
scrolling 3–4x and text 1.5–1.8x compared with `MiSTer.card`.

---

## Contents

| Path | What |
|------|------|
| `release/amiga/` | Ready-to-use Amiga binaries: `MiSTerZZ.card`, `MiSTerZZInfo`, `MiSTerZZTest`, `SetToolType` |
| `release/mister/MiSTer_A2091` | The deployed MiSTer main binary: official Main_MiSTer `d5f3844` + A4091 + A2091 + this blitter |
| `release/core/minimig_20260906_A4091_RTG_A2065_v2.rbf` | The tested Minimig core (020 + Z3 RTG + A4091 + A2065), unchanged |
| `release/MD5SUMS` | Checksums of the above (match what is installed on the MiSTer) |
| `release/README.md` | Release note: file list, where each goes, install steps |
| `amiga/rtg/` | Driver source: official `mntgfx-gcc.c`, adapted for MiSTer |
| `amiga/include/` | Official ZZ9000 driver headers (unchanged) |
| `amiga/tools/` | Source of `MiSTerZZInfo`, `MiSTerZZTest`, `SetToolType` |
| `amiga/Makefile` | Amiga build (bebbo m68k-amigaos-gcc 6.5) |
| `mister/zz9000os/` | Official ZZ9000OS `gfx.c`, `gfx.h`, `zz_video_modes.h` (unchanged), `dma_rtg.c` (adapted), `video.h` (shim) |
| `mister/*.c, *.cpp, *.h` | MiSTer glue: service thread, mailbox, aligned-access memcpy, build wrapper |
| `mister/install.py` | Installs the MiSTer side into any Main_MiSTer tree (idempotent) |
| `common/` | Driver⇄ARM protocol (`mister_zz9000.h`), FPGA cache flush (`mister_zz9000_cache.h`) |
| `tests/host/` | Host test of the exact ARM code against reference renderers (242 checks) |
| `integration/main_mister_zz9000_hooks.patch` | The 3 hook lines + Makefile line that `install.py` applies |
| `tools/` | Bring-up helpers: `zz_amicmd.sh` (run Amiga CLI commands over serial from MiSTer), `zz_fbdump` (dump RTG framebuffer from DDR3) |
| `results/` | Hardware test and benchmark logs, framebuffer capture, backups of the original P96 settings/monitor icon |
| `history/` | The git commits as patches (import → port → hardware fixes) |
| `DESIGN.md` | Detailed design notes (memory map, protocol, cache coherency, strongly-ordered memory, polling) |
| `UPSTREAM.md` | Upstream commits imported and the list of local changes |
| `LICENSE` | GPL-3.0-or-later (as upstream) |

`history/` holds the development commits as patches: verbatim upstream
import → MiSTer port → hardware bring-up fixes.

---

## Requirements

- Minimig core with the **Z3 RTG board** (mfr `$139C`, product `$30`,
  "Rok Krajnc Minimig Z3 GraphicsCard"). Tested with
  `_Computer/minimig_20260906_A4091_RTG_A2065_v2.rbf` (68020 core).
- 68020 or 68030 CPU (Zorro III).
- Picasso96 (tested: rtg.library 43.538, Picasso96API.library 2.455).
- A MiSTer main binary that contains the blitter module, for example
  `release/mister/MiSTer_A2091`.

---

## Install

### MiSTer

The released binary is meant to be selected with `MiSTer.ini [minimig]
main=MiSTer_A2091`, so the Minimig core runs `/media/fat/MiSTer_A2091`
while other cores keep the stock `MiSTer`. Never overwrite a running binary:
load another core first.

```bash
ssh root@mister 'echo "load_core /media/fat/menu.rbf" > /dev/MiSTer_cmd'
scp release/mister/MiSTer_A2091 root@mister:/media/fat/MiSTer_A2091
ssh root@mister 'echo "load_core /media/fat/_Computer/minimig_20260906_A4091_RTG_A2065_v2.rbf" > /dev/MiSTer_cmd'
```

Keep a copy of any previous `MiSTer_A2091` so you can roll back.

### Amiga

Copy `release/amiga/*` to `SHARE:MiSTerZZ/`, then in an Amiga Shell:

```
copy SHARE:MiSTerZZ/MiSTerZZ.card LIBS:Picasso96/
copy DEVS:Monitors/MiSTer DEVS:Monitors/MiSTerZZ
copy DEVS:Monitors/MiSTer.info DEVS:Monitors/MiSTerZZ.info
SHARE:MiSTerZZ/SetToolType DEVS:Monitors/MiSTerZZ BOARDTYPE=MiSTerZZ
rename DEVS:Monitors/MiSTer SYS:Storage/Monitors/MiSTer
rename DEVS:Monitors/MiSTer.info SYS:Storage/Monitors/MiSTer.info
Reboot
```

Only one driver may own the board, so the old `MiSTer` monitor goes to
`SYS:Storage/Monitors`.

### Screen modes

Pick any **`MiSTer:`** mode in ScreenMode prefs (640x480, 800x600, 1280x1080,
1920x1080). The driver deliberately reports the board name `MiSTer`
(type 10), because `Picasso96Settings` stores the modes under that name, so
your existing modes keep working and are now accelerated. The mode *names*
can be changed in Picasso96Mode prefs if you want them to read `MiSTerZZ:`,
but the **board** name must stay `MiSTer`.

Accelerated formats are CLUT (8-bit), R5G6B5PC (16-bit) and R8G8B8A8
(32-bit). B8G8R8 (24-bit) works, but is drawn in software.

---

## Is the blitter working?

```
SHARE:MiSTerZZ/MiSTerZZInfo
```

Healthy output:

```
driver state     $7 (attached initcard BIF_BLITTER), 4878 hook calls, 0 to P96 default, last format 8
ARM protocol     v2 caps $000003FE, 4878 ops, 0 rejected
=> ARM blitter attached and running
```

- `ops` increases when you move windows or scroll → blits run on the ARM.
- `0 to P96 default` → nothing has fallen back to software.
- `state $7` → the ARM attached at boot and `BIF_BLITTER` is set.

From Linux you can read the same counter (big-endian word, ops at mailbox
+`0x3C`): `devmem 0x277C003C 32`.

Full self-test with benchmark (opens its own screen, compares against the
native blitter, about 30 s):

```
SHARE:MiSTerZZ/MiSTerZZTest DEPTH 8|16|32 [BENCH] [LOG SHARE:file.log]
```

If the MiSTer binary has no blitter, the driver waits about a second at boot
and then runs as an unaccelerated framebuffer (`state $3`). If the ARM side
disappears later, the driver falls back to P96 software routines on the next
blit.

---

## Results (hardware, 640x480, ops/s)

| primitive | 8-bit ZZ | 8-bit MiSTer.card | | 32-bit ZZ | 32-bit MiSTer.card | |
|---|---:|---:|---:|---:|---:|---:|
| RectFill 64x64 | 3300 | 2211 | 1.5x | 2255 | 714 | 3.2x |
| RectFill 320x200 | 801 | 185 | 4.3x | 306 | 47 | 6.5x |
| ScrollRaster 320x200 | 334 | 98 | 3.4x | 109 | 26 | 4.2x |
| Text 40 chars JAM2 | 970 | 671 | 1.45x | 978 | 553 | 1.8x |
| Line ~200px | 3555 | 3934 | 0.9x | 3293 | 3933 | 0.84x |

Each op has a fixed cost of about 0.28 ms: the synchronous mailbox round-trip
plus a flush of the FPGA CPU cache. That makes short lines slightly slower
than CPU drawing; everything with area is faster. Raw logs are in `results/`.
`results/rtg_framebuffer_testscene_8bit.png` is the test scene read straight
out of DDR3.

---

## Build from source

Amiga (any Linux host with [bebbo amiga-gcc](https://github.com/bebbo/amiga-gcc) in `/opt/amiga`):

```bash
cd amiga
export PATH=/opt/amiga/bin:$PATH
make p96-sdk        # once: fetch the Picasso96 developer headers into sdk/
make                # build/MiSTerZZ.card, MiSTerZZInfo, MiSTerZZTest, SetToolType
```

MiSTer main binary (ARMv7 cross compiler):

```bash
mister/install.py /path/to/Main_MiSTer
cd /path/to/Main_MiSTer
export PATH=/opt/armV7-linux-gcc/bin:$PATH
make
```

The released binary was built from Main_MiSTer `d5f3844`, with the
[A4091](https://github.com/georgemog/A4091) and
[A2091](https://github.com/georgemog/A2091) support applied and then
`install.py`. `integration/main_mister_zz9000_hooks.patch` shows the edits
`install.py` makes. It applies the same way to a stock Main_MiSTer tree.

Host tests (macOS/Linux):

```bash
make -C tests/host        # 242 checks, ASan + UBSan
```

---

## Things learned on hardware

1. **`CacheClearU()` does not flush the Minimig FPGA CPU cache.**
   `cpu_cache_new` clears on a 0→1 edge of CACR bit 3. The TG68 latches that
   bit and exec always writes it as 1, so only the first call clears. Use
   `mister_fpga_cache_clear()` from `common/mister_zz9000_cache.h`, which
   toggles CI 0→1 in supervisor mode. This matters for anything the HPS
   writes into Amiga-visible DDR3.
2. **Pin MiSTer worker threads to CPU0.** The main binary busy-loops on
   CPU1, and threads inherit that affinity. Sharing CPU1 cost about 0.5 ms
   per op.
3. **The RTG framebuffer is in Amiga byte order in DDR3**, because
   `cpu_wrapper.v` swaps RTG accesses. The ZZ9000 blitter therefore runs
   unmodified.
4. **The `/dev/mem` mapping of `0x27000000` is Strongly-ordered**, so no
   unaligned and no NEON accesses are allowed. The blitter is compiled with
   NEON, auto-vectorisation and store merging off, and uses its own aligned
   memcpy.
5. MiSTer's `screenshot` command does not capture the RTG output. Use
   `tools/zz_fbdump` to read the framebuffer instead (`/dev/mem` allows mmap
   only, not `read()`).

## Rollback

- Back to `MiSTer.card` while keeping the files:
  `SetToolType DEVS:Monitors/MiSTerZZ BOARDTYPE=MiSTer`, then reboot.
- Full rollback: move `SYS:Storage/Monitors/MiSTer(.info)` back to
  `DEVS:Monitors/`, delete `DEVS:Monitors/MiSTerZZ(.info)`, and on the
  MiSTer restore your previous `MiSTer_A2091` (with a non-Minimig core
  loaded). The original `Picasso96Settings` and `MiSTer.info` are in
  `results/`.

## Possible next steps

- RTL: inhibit the CPU cache for `sel_rtg`. That removes the per-op cache
  flush, the main remaining cost.
- Draw short lines and tiny fills in software, as upstream already does on
  Zorro II.
- Queue several ops per kick, for asynchronous blits.
- Use a cached ARM mapping of the framebuffer for faster pixel loops.
