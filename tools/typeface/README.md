# The typeface letters' table

`src/typeface_letters.cpp` (the menu's "Typeface letters") draws the game's
Latin letters from a free typeface above 1x. Which glyph of the typeface
each character takes, at what size and where in its cell, comes from
`src/typeface_table.inc`, which `make_table.py` writes by fitting the
typeface to the console's glyphs:

```
python -I tools/typeface/make_table.py \
    normal=thirdparty/fonts/rogue-letters/RogueLetters-Regular.ttf \
    system=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    name=thirdparty/fonts/andika/Andika-Regular.ttf+alt \
    normal_eur=thirdparty/fonts/andika/Andika-Regular.ttf+alt \
    system_eur=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    name_eur=thirdparty/fonts/andika/Andika-Regular.ttf+alt \
    menu=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    menu_eur=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    clock=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    status=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    statussmall=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    level=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    file=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    money=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    battle=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    battlesmall=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    damage=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    result=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    resultgold=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    registered=thirdparty/fonts/andika/Andika-Bold.ttf+alt \
    words=thirdparty/fonts/andika/Andika-Bold.ttf+alt
```

It needs the disc extracted (`disc/`), the European text built
(`setup.py extract-european`, `disc/EU/`) for the `_eur` fonts, Pillow and
fontTools (`pip install pillow fonttools`), and takes about half an hour. The
console's glyphs are read from the disc into memory; the table holds only
numbers, hashes and glyph indices.

- **The fonts.** `normal` (dialogue and messages, 18x22 cells), `system`
  (menus, the same design narrowed, 16x20) and `name` (names over heads,
  16x18; the game draws them a row short): the pages
  `FontTex_<font>_EN_0.tm2` of `meswin\fontset_main_EN.pak`, glyph code n
  in cell n, and the characters and widths of the executable's table at
  0x44E850 (`pages.py`). The table names the game's symbols 0; plus-minus,
  times, middle dot and reference mark (`pages.SYMBOLS`) are drawn too,
  found by their code, and placed without weighing in the font's fit (the
  times sign may take the typeface's x, raised: the console's is a large
  x); the arrows stay the game's (Andika's are hairlines).
- **The message font's own typeface** (`normal`): Rogue Letters
  (`thirdparty/fonts/rogue-letters`, the SIL Open Font License, made from
  Andika). Each of its 93 characters is drawn as a few pen strokes whose
  nodes and pen width were fitted to the console's glyph, and the
  apostrophe, quotes and commas are Andika's, placed and sized. It has no
  alternates (no `+alt`), and its glyphs are already as wide as the
  console's ink, so the game keeps their widths (`drawnAfterConsole` in
  `typeface_letters.cpp`) where it evens the gaps of Andika's. The tools
  that drew it are in the work's research notes
  (`build/research/custom-typeface`, outside the repository). The European
  message font is still Andika's: Rogue Letters has no accents yet.
- **The European fonts** (`normal_eur`, `system_eur`, `name_eur`): the European
  text's pages, `FontTex_<font>_EUR_0.tm2` of the language's
  `meswin\fontset_main_<lang>.pak` in `disc/EU` (one pack for the four
  languages, `--eur-lang` picks whose). Their glyphs are redrawn (other alpha
  levels), so they get fits of their own; the names' cells are 16x22. Codes
  0-95 are the US pages', 96-106 the symbols in another order, 107-170 the accents
  and marks of `src/text_language.cpp`'s kAccentTags (`pages.EUR_FONTS`). The
  table finds their glyphs by code, every row holding it. A font is fitted on
  its ASCII characters; the symbols and accents are placed. The menus'
  pictures have a copy in each language too (`pieces.pack_reader`): the digits
  in the same cells, in palettes of their own, so the table gets their hashes
  as more rows of the same glyphs; and a menu font of their own, the same page
  in the four languages with the accents after the ASCII (`menu_eur`, its
  setting's bytes from 0xA0 being `pages.EUR_ACCENTS`). Their words are the
  language's and stay the game's.
- **The fit** (`fit.py`). Each candidate glyph is drawn at 8 times the
  game's size, box-filtered into the game's cell and quantized to its 16
  alpha levels; the score is its difference with the console's glyph over
  the ink. The typeface's size, an extra horizontal scale, the baseline and
  a growth of the strokes in eighths of a pixel are fitted for the whole
  font; each glyph then gets its horizontal place, and a sign (`:` `*`
  `_`...) also its height, where the console's sits. The message fonts are
  then drawn two steps bolder than their fit (`EXTRA_GROW`): enlarged, the
  console's glyph shows its soft edge texels as whole blocks and reads
  fuller than its 1x weight.
- **`+alt`** (`alternates.py`): a character may take one of the typeface's
  OpenType alternates (cvNN, ssNN, salt, aalt) where it is closer. Andika's
  give the console's straight-legged 9, tailed l, baseless 1 and straight
  y. The curly apostrophe and quotes are candidates too.
- **The menus' digits** (`pieces.py`). The menus' words and numbers are
  pictures, not a font: the game draws a number a digit at a time, each
  from its cell of a row of digits in one of the menus' pictures (`clock`,
  the clock and play time; `status` and `statussmall`, the members' levels
  and attributes; `level`, the transporter's levels and the
  items' counts; `file`, the files' numbers; `money`; in battle `battle`
  and `battlesmall`, the members' HP and AP, and `damage`; after a battle
  `result` and `resultgold`), and the registered mark of the memory card's
  messages (`registered`, a special character of the text drawn from
  `gaiji2.img`, the buttons' picture, the European text's own copy too).
  The menus' own font (`menu`: item commands,
  Allies, shops) is drawn the same way, a letter at a time from
  `menu_font_EN.img`, each its width (from `menu_font_setting_EN.txt`)
  wide. Each set is fitted as a font, on the
  digits' light core (typeface_letters.cpp draws the dark edge around the
  typeface's glyph, in the colours read from the piece), with a slant for
  the slanted ones. The table also gets each
  digit's hash as the renderer hands it over (`hash_rgba`, at the cell's
  size, up to two texels narrower or one wider, and a row less or more) and
  the hash of each picture's palette as dq8-gfx makes it, so the game knows
  the digits when they are drawn (two characters with the same texels stop
  the tool); the
  colours come from the digit itself in the game. A sign among the digits
  (the clock's colon) is centred on the console's; a small capital among
  them (the clock row's P) may take the typeface's small capital.
- **The menus' words** (`pieces.WORDS`, `words=`): the words of the
  pictures (Items, Save, FILE, Equipment, the Battle Recorder's...), listed
  by hand as a window around each in its picture (places and words only).
  Each is fitted whole (size, width, baseline, growth) on its core, drawn
  with the menu font's glyph choices, a letter taking another drawing (an
  alternate, or the default one) where the word comes closer, and the table
  gets its box, the hash
  of the box's texels and each glyph's pen position. A word's style may
  be slanted, in small capitals (the members' names), with a letter's
  drawing named (their J with its bar; the story title's y with its tail,
  where the menu font takes the straight one), or shadowed: a soft dark shadow
  instead of an edge (Exp, Skills), whose offset, blur and
  strength the tool fits on the console word. Its stroke growth may be held
  where the fit alone draws it bolder than the words beside it (Acquired
  Items). The game redraws a word
  when the piece it draws holds that word on a clear background, other art
  two texels or more away being kept as the game draws it (the story
  screen's "Press" and its button). A word printed on a button's fill (the
  framed Player, marked "button" in the list) is redrawn with the fill put
  back under it, each row from the fill left of the word to the fill right
  of it. A word printed on a band of colour across its box (the items'
  MAX, marked "bar") is redrawn with the band put back: a row of the box
  opaque at both ends is the band's, all of it but the word's core. A word
  printed on art (the Revelation Flow's title on its frame's wood, marked
  "art") is told from the art by colour (the word's texels are as blue as
  red or more, the wood's red well above blue), and the art is put back
  under the new word, each run of the word's texels in a row filled from
  the art left of it to the art right of it (the wood's grain runs along
  the rows).
- **Pictures the game fixes** (`pieces.ART_FIXES`): a slip that
  `src/art_fixes.cpp` mends in a picture as the game registers it (the
  magenta at the corners of Pty_02's Player and Party) is mended here too
  as the picture is read, so the words' hashes and fits are of what the
  game draws.

The choice of Andika, and the search over Google Fonts' sans-serif families
it came from, are in the research notes of the work (`build/research/font`,
outside the repository). Another typeface goes the same way: its file
(under a licence that allows it) in `thirdparty/fonts`, embedded by
`src/CMakeLists.txt`, named in `g_typefaces` in `src/typeface_letters.cpp`,
and this command with its path.

In the game, `RG_TYPEFACE_SHEET=DIR` writes each font's glyphs, the
console's beside the typeface's, as the pages are read (docs/DEVELOPMENT.md).
