# Upstream sources

Everything under `amiga/rtg/`, `amiga/include/` and `mister/zz9000os/` was
imported verbatim from the official ZZ9000 sources, then adapted in later
commits (use `git log -p -- zz9000/` to see every change against upstream).

| Local path            | Upstream repo                                   | Upstream path                              | Commit |
|-----------------------|-------------------------------------------------|--------------------------------------------|--------|
| `amiga/rtg/`          | https://github.com/BlitterStudio/zz9000-drivers  | `rtg/`                                     | `ad9606b5d16a66426e00ae6f8cd08b0c0708af74` (2026-10-07) |
| `amiga/include/`      | https://github.com/BlitterStudio/zz9000-drivers  | `include/`                                 | same |
| `LICENSE`             | https://github.com/BlitterStudio/zz9000-drivers  | `LICENSE`                                  | same |
| `mister/zz9000os/gfx.c`, `gfx.h`, `zz_video_modes.h`, `dma_rtg.c` | https://github.com/BlitterStudio/zz9000-firmware | `ZZ9000_proto.sdk/ZZ9000OS/src/` | `8778ef3b21698cc78a28b857b587988a07294885` (2026-10-08) |

License: GPL-3.0-or-later (both repositories).

## Local changes

`amiga/rtg/mntgfx-gcc.c` (→ `MiSTerZZ.card`):
- `FindCard` finds the Minimig Z3 RTG board (`$139C/$30`) instead of a ZZ9000.
  It points `MemoryBase` at the framebuffer and the rtg.v regs at board +
  `$800100`, gives P96 the bottom `$7C0000`, and does the mailbox handshake
  with the MiSTer binary.
- `InitCard` uses the MiSTer.card identity/limits, sets `BIF_BLITTER` only
  when the ARM attached, and installs no ZZ9000-only hooks (sprite, capture,
  split, DPMS, off-screen bitmaps, PIP/YUV).
- Display hooks (`SetGC`, `SetPanning`, `SetColorArray`, `SetDAC`,
  `SetSwitch`, pixel clock, vsync) program rtg.v the way MiSTer.card does.
- Accelerated hooks: unchanged upstream Z3 paths. Only these differ:
  - doorbell `zzwrite16(&registers->blitter_dma_op, op)` → `mister_kick(b, op)`;
  - `mnt_colormode()` → `blit_colormode()`, which falls back to software when
    no ARM is attached;
  - staging-size checks for template/planar ops (MiSTer staging is 232KB).
- Pixel formats: CLUT, R5G6B5PC, R8G8B8A8 (+ B8G8R8 in software). Pitches
  are rounded to whole longwords.

`mister/zz9000os/dma_rtg.c`: reads GFXData from a private snapshot instead of
the Zynq aperture, range-checks every Amiga-supplied offset, uses `uintptr_t`
pointer math, and drops the display/sprite/palette/YUV/overlay ops.
`gfx.c`, `gfx.h` and `zz_video_modes.h` are unchanged. `video.h` is a MiSTer
shim.
