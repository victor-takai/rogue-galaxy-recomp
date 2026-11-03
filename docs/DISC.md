# Disc

## Supported dump

Only the NTSC-U release is supported for now. The Japanese Director's Cut
can add its voices and movies to it ([below](#the-japanese-disc-undub)).

| | |
|---|---|
| Title | Rogue Galaxy (USA) |
| Serial | SCUS-97490, disc version 1.01, EXE date 2006-11-14 |
| Media | 1 × DVD9 (dual layer), 6,967,721,984 bytes, 3,402,208 sectors |
| Redump | <http://redump.org/disc/6567/> |
| CRC32 / MD5 / SHA-1 | `044f1164` / `86170ff39c8e02c82bc043c9fe42baaf` / `87377e721be58e8b6a5a7501721563500d237df7` |
| Boot ELF | `SCUS_974.90`, SHA-1 `87e9eff1d795c894fc09047270baab70d6a8d2c8` (same build as [RGDecomp](https://github.com/Dorgengoa/RGDecomp)) |

Per-file sizes and SHA-256 for all 124 files are in
[`config/SCUS_974.90/hashes.json`](../config/SCUS_974.90/hashes.json).
`python setup.py verify --iso <image> --full` checks a dump against them and
against Redump without writing anything.

`Install.bat` takes the image as an uncompressed `.iso` of exactly that
size. A compressed one (`.chd`, `.cso`) must be turned back into an `.iso`
first.

## The Japanese disc (undub)

The undub takes the voices and movies of the Japanese Director's Cut
(SCPS-17013) from your own image of it. `Install-Undub.bat` (or
`python setup.py extract-japanese --iso <image>`) copies them, about 3 GB,
into `disc/JA`; a movie the US disc has the same bytes of is not copied. It
checks the image's two voice indexes against
[`config/SCUS_974.90/japanese.json`](../config/SCUS_974.90/japanese.json)
and refuses another Japanese release: only the Director's Cut has voices
that line up with the US game.

## Layers

The image holds two ISO9660 volumes:

| Layer | Volume starts at sector | Volume size (sectors) |
|---|---|---|
| 0 | 0 | 1,646,704 (= Redump layer break) |
| 1 | **1,646,688** | 1,755,520 |

Layer 1's descriptor sits *at* the layer break, so its volume starts 16
sectors before it (`1646688 + 1755520` = end of image). Directory records on
layer 1 are relative to that start. PCSX2 notes that this game "has arbitrary
layer sizes"; `tools/disc/iso9660.py` looks for the descriptor around the break
rather than assuming `break + 16`.

The game picks a layer per file (`sceCdLayerSearchFile`, `sceCdReadDvdDualInfo`).
35 files are on both layers and are byte-identical (BIN/, MOVIE/00/,
RGSYS.DAT, RGDMY.DAT), so one merged tree describes the disc.

## Contents

| What | Files | Size | Notes |
|---|---|---|---|
| `DATA{0,1}_{0,1}.DAT` + `.HD6` | 4 + 4 | 4.3 GiB | Level-5 archives; `DATA0_*` on layer 0, `DATA1_*` on layer 1. HD6 is the index (same format as DQ8). |
| `MOVIE/**.MVI` | 89 | 1.9 GiB | Sony PSS (libmpeg/IPU) with an XOR layer, as in DQ8 |
| `SCUS_974.90` | 1 | 3.5 MiB | Main ELF, stripped, MW MIPS C Compiler 2.4.1.01, SCE libraries 3.0.0 |
| `BIN/*.BIN` | 7 | 0.7 MiB | MWo3 code overlays, see [`overlays.toml`](../config/SCUS_974.90/overlays.toml) |
| `MODULES/` | 16 | 0.6 MiB | `IOPRP300.IMG` + 15 IRX: CDVDSTM, LIBSD, MCMAN, MCSERV, PADMAN, SIO2MAN, SDRDRV, PCMPLAY, MODHSYN, MODMIDI, MODMSIN, SKHSYNTH, SKMIDI, SKMSIN, SKSOUND |
| `RGSYS.DAT`, `RGDMY.DAT` | 2 | 16 MiB | 1 KiB system file; 16 MiB filler |

## Extracted tree

`python setup.py extract --iso <image>` writes every file once into `disc/`
(gitignored), hashing as it copies. The runtime boots `disc/SCUS_974.90` and
serves `cdrom0:` from that folder.

`disc/manifest.json` records, for every file, its SHA-256 and the absolute and
layer-relative start sector of each copy. Those are the values the real
`sceCdSearchFile` / `sceCdLayerSearchFile` would return, should the game ever
compute raw sector reads from them.

Nothing from the disc is committed. You need your own copy of the game.
