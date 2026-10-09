# ZZMiSTerRTG release — 2026-10-09

The built binaries from `../`, the same files that are installed and tested
on the MiSTer (`MD5SUMS`).

| File | Goes to | What |
|------|---------|------|
| `amiga/MiSTerZZ.card` | Amiga `LIBS:Picasso96/` | P96 driver (ZZ9000.card 2.13 port), 68020+ |
| `amiga/MiSTerZZInfo` | anywhere, e.g. `SHARE:MiSTerZZ/` | status: board, scanout, ARM link, op counters |
| `amiga/MiSTerZZTest` | anywhere | self-test vs native blitter + benchmark (`DEPTH 8\|16\|32 [BENCH]`) |
| `amiga/SetToolType` | anywhere | `SetToolType <icon> NAME=VALUE`, used for the monitor's `BOARDTYPE` |
| `mister/MiSTer_A2091` | MiSTer `/media/fat/MiSTer_A2091` | MiSTer main binary: Main_MiSTer `d5f3844` + A4091 + A2091 + ZZ9000 blitter |
| `core/minimig_20260906_A4091_RTG_A2065_v2.rbf` | MiSTer `/media/fat/_Computer/` | The tested Minimig core: 68020, Z3 RTG board, A4091, A2065, Z3 FastRAM. Unchanged; this project needs no FPGA changes |

Any other Minimig build with the Z3 RTG board (`$139C/$30`) works too.

## Install

MiSTer (`MiSTer.ini [minimig] main=MiSTer_A2091`; never overwrite a running
binary):

```bash
scp core/minimig_20260906_A4091_RTG_A2065_v2.rbf root@mister:/media/fat/_Computer/
ssh root@mister 'echo "load_core /media/fat/menu.rbf" > /dev/MiSTer_cmd'
scp mister/MiSTer_A2091 root@mister:/media/fat/MiSTer_A2091
ssh root@mister 'echo "load_core /media/fat/_Computer/minimig_20260906_A4091_RTG_A2065_v2.rbf" > /dev/MiSTer_cmd'
```

Amiga (after copying `amiga/*` to `SHARE:MiSTerZZ/`):

```
copy SHARE:MiSTerZZ/MiSTerZZ.card LIBS:Picasso96/
copy DEVS:Monitors/MiSTer DEVS:Monitors/MiSTerZZ
copy DEVS:Monitors/MiSTer.info DEVS:Monitors/MiSTerZZ.info
SHARE:MiSTerZZ/SetToolType DEVS:Monitors/MiSTerZZ BOARDTYPE=MiSTerZZ
rename DEVS:Monitors/MiSTer SYS:Storage/Monitors/MiSTer
rename DEVS:Monitors/MiSTer.info SYS:Storage/Monitors/MiSTer.info
Reboot
```

Then pick a `MiSTer:` mode in ScreenMode and check with `MiSTerZZInfo`. You
should see `=> ARM blitter attached and running`, with `ops` rising as you
draw. See `../README.md` for details, results and rollback.
