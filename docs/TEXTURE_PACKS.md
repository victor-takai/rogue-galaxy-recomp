# Texture packs

The game can draw its 2D art (icons, window pieces, menu pictures, pictures
of text) from larger versions kept on the player's machine. This project
ships no art: the pictures come from the player's own disc, so a pack is
never part of the repository, and a pack made by someone else is theirs to
share. Faces, portraits and painted icons look best as the game draws them;
packs are for the pieces a pack's author wants to redraw.

Packs are read above 1x (an internal scale of 2x or more). The menu's
Textures page (F1 > Settings > Textures) has one switch for all of them
and one for each pack found, which take effect at once.

## Where packs go

- `textures\packs\NAME\`: one folder a pack, or `textures\packs\NAME.zip`, a
  zip of one (its files at the top, or all inside a single folder, as zipping
  a folder makes it). They are read by name, and a later one wins over an
  earlier one for the same picture.
- `textures\hd\`: what `textures\make-pack.bat` writes; read first, under
  every folder of `textures\packs`.
- `RG_TEXTURE_PACK=DIR` reads that one folder alone.

## Collecting what a pack can replace

`textures\RogueGalaxy-collect.bat` plays the game while writing each region
that appears on the screen once into `textures\collected`. A region is the
rectangle a sprite draws from a texture (a window piece, an icon, a label),
named by its content: `TEXELS_CLUT_WxH.png`, the texels' hash, the palette's
hash (both 16 hex digits) and the size in texels. The font's glyphs are
regions too, one a glyph. The PNG's alpha is doubled (the console's 0x80 is
opaque, written as 255).

## A pack's files

A picture is a PNG or a DDS named for the region it replaces, a whole 2 to 8
times larger in each direction (`TEXELS_CLUT_WxH.png` of 24x24 texels is 48x48
up to 192x192, at most 8192 a side). One file a region. A DDS is read as BC1,
BC2, BC3 or BC7 (DXT1, DXT3, DXT5, or the DX10 header's formats) or as 8-bit
RGBA, its first level only; it is decoded when read, so it saves disk, not
memory. In the file name CLUT may be `any`: the picture answers for the
content with every palette (a file for the exact palette wins over it).

An optional `pack.json` beside them:

```json
{
  "format": 1,
  "name": "My pack",
  "alpha": "png",
  "filter": "linear",
  "enabled": true,
  "aliases": { "0123456789ABCDEF_FEDCBA9876543210_24x24": "icons/sword.png" },
  "ignore": ["0123456789ABCDEF_any_24x24"]
}
```

- `format`: the version of this format, 1. A pack with a newer one is left
  out, with a line in the log.
- `name`: how the log names it (the folder's name otherwise).
- `alpha`: `"png"` (the default) for an ordinary PNG, 255 opaque; `"ps2"`
  when the files already hold the console's alpha, 0x80 opaque.
- `enabled`: `false` starts the pack switched off (its switch in the menu
  turns it on).
- `aliases`: a region's name for any PNG inside the pack's folder (no `..`,
  no absolute path), instead of naming the file after the region.
- `filter`: `"linear"` (the default) draws the pack's pictures filtered
  across their own texels; `"nearest"` draws them as they are, for pixel art
  made at a whole multiple of the original (a 4x nearest copy of a region then
  looks exactly as the game draws it, only finer).
- `ignore`: regions left as the game draws them, whatever the packs before
  this one gave.

A picture is read on a thread of its own the first time its region shows;
the game draws the original until it is ready.

## Packs made for PCSX2

A folder of PCSX2's replacement files, `TEX0HASH-CLUTHASH-BITS.dds` (or
`.png`, the hashes in hex as PCSX2 writes them, without leading zeros), works
as it is: point `RG_TEXTURE_PACK` at it, or put it in `textures\packs`. Such a
picture replaces a whole texture, not a region, and so reaches what the region
pictures do not: the scenery, the characters' skin and the 3D art in general.
The game names a texture as PCSX2 does, by an XXH3-64 hash: of the GS blocks it
covers for an 8-bit or 4-bit texture, of its indices a byte each, row by row,
for one smaller than a block or with its indices in a 32-bit word's high bits
(8H, 4HL, 4HH). It draws the pack's picture, which is 2 to 8 times larger, in
its place. Textures of direct colour (32, 24 and 16 bits, named
`TEX0HASH-BITS`) are not replaced: in the packs seen they are mostly captures
of the screen, which change every frame. The
palette's hash in the name is the hash PCSX2 makes of the palette's 16 or 256
entries as 32-bit words: the picture of the palette's own hash is drawn, else
any one of the texels' pictures. The alpha is read as the console's
(0x80 opaque), as a PCSX2 pack keeps it.

`RG_TEXTURE_DUMP_WHOLE=DIR` writes each such texture the game builds, whole
and in colour, as `TEX0HASH-CLUTHASH-BITS.pam` (RGBA, alpha as the GS's),
to make a pack for the textures the game really uses. The two textures that
differ only by palette get a file each. The disc does not give the list
beforehand: the game builds most of its 8-bit textures in memory from several
pieces, so only what the game draws names them.

xxHash (`xxhash.h`, in `thirdparty/dq8-gfx`) is Yann Collet's, BSD 2-Clause.

## Not yet

Changing the order of the packs from the menu; keeping a PCSX2 picture compressed on the GPU. Movies and render targets are never collected.

## Checking the readers

`tools\texture_pack\check_dds.py` decodes random BC7 blocks of every mode, and
BC1, BC2, BC3 and RGBA files, with the game's reader and with Pillow's, and
compares them; `check_zip.py` does the same for the zip reader against
Python's zipfile. Each wants its checker built first (the command is at the
top of `check_dds.cpp` and `check_zip.cpp`). BC7's partition tables
(`src/bc7_tables.h`) are read out of Pillow's decoder by `bc7_tables.py`.
