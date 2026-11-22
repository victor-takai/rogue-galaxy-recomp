# The typeface letters' table

`src/typeface_letters.cpp` (the menu's "Typeface letters") draws the game's
Latin letters from a free typeface above 1x. Which glyph of the typeface
each character takes, at what size and where in its cell, comes from
`src/typeface_table.inc`, which `make_table.py` writes by fitting the
typeface to the console's glyphs:

```
python -I tools/typeface/make_table.py \
    normal=thirdparty/fonts/andika/Andika-Regular.ttf+alt \
    system=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    name=thirdparty/fonts/andika/Andika-Regular.ttf+alt
```

It needs the disc extracted (`disc/`), Pillow and fontTools
(`pip install pillow fonttools`), and takes about two minutes. The console's
glyphs are read from the disc into memory; the table holds only numbers and
glyph indices.

- **The fonts.** `normal` (dialogue and messages, 18x22 cells), `system`
  (menus, the same design narrowed, 16x20) and `name` (names over heads,
  16x18): the pages `FontTex_<font>_EN_0.tm2` of
  `meswin\fontset_main_EN.pak`, glyph code n in cell n, and the characters
  and widths of the executable's table at 0x44E850 (`pages.py`).
- **The fit** (`fit.py`). Each candidate glyph is drawn at 8 times the
  game's size, box-filtered into the game's cell and quantized to its 16
  alpha levels; the score is its difference with the console's glyph over
  the ink. The typeface's size, an extra horizontal scale, the baseline and
  a growth of the strokes in eighths of a pixel are fitted for the whole
  font; each glyph then gets its horizontal place, and a sign (`:` `*`
  `_`...) also its height, where the console's sits.
- **`+alt`** (`alternates.py`): a character may take one of the typeface's
  OpenType alternates (cvNN, ssNN, salt, aalt) where it is closer. Andika's
  give the console's straight-legged 9, tailed l, baseless 1 and straight
  y. The curly apostrophe and quotes are candidates too.

The choice of Andika, and the search over Google Fonts' sans-serif families
it came from, are in the research notes of the work (`build/research/font`,
outside the repository). Another typeface goes the same way: its file
(under a licence that allows it) in `thirdparty/fonts`, embedded by
`src/CMakeLists.txt`, named in `g_typefaces` in `src/typeface_letters.cpp`,
and this command with its path.

In the game, `RG_TYPEFACE_SHEET=DIR` writes each font's glyphs, the
console's beside the typeface's, as the pages are read (docs/DEVELOPMENT.md).
