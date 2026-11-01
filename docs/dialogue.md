# How the game picks and loads a line of dialogue

A study of the US executable (SCUS_974.90), checked against the European
(SCES-54552) and Japanese Director's Cut (SCPS-17013) discs. It is the base for
reading the PAL texts and the Japanese voices inside the US version. It records
how the game works only: no game text or audio is reproduced here.

Function addresses are for the US executable, named as in
[RGDecomp](https://github.com/Dorgengoa/RGDecomp). Data addresses are
guest addresses in the same executable.

## 1. Two language tokens in every path

Paths are resolved by `PathLocalize` (0x1C28C0), which `ArchiveGetFileByPath`
(0x1C2800) and the voice path builders call. It copies a path and replaces:

| character | replaced by | meaning |
| --------- | ----------- | ------- |
| `?` | the string at 0x481528 | text language |
| `!` | the string at 0x48152C | voice language |

`StaticInitLangCodeTables` (0x4698B0) sets the two from tables in the
executable. Table 0x44AF38 lists EN, ES, FR, IT, DE, KO, ZH (and JA after a
gap); table 0x44AF68 lists EN seven times. The US build takes the first entry
of each, so both tokens read `EN`. Nothing in the US executable changes them
later. The two tokens are independent, so a text language and a voice language
can differ without any new mechanism.

Path formats in the US executable that carry a token:

- `%s_?.stb`, `%s_?.evt`, `%s_?.txt` (0x45EE18): the three parts of a scene.
- `wav/!/EV%02d-%04d/%02d-%04d-%04d.mwv` (0x45EC50, 0x45C570) and
  `wav/!/LiveTalk/000%d-%d-%05d.mwv`: voices.
- `meswin/fontset_main_?.pak`, `meswin/fontset_launcher_?.pak` (0x45F640): fonts.
- Menu and message files such as `cmd_*_?.txt`, `menu_font_?.img`,
  `FontTex_*_?_N.tm2`, `fonttbl_*_?.bin`.

The PAL executable hard-codes the font to `fontset_main_EUR.pak` and
`fontset_launcher_EUR.pak` (one font for all five languages) and has more `?`
paths (for example `common_?.img`, `compass_?.chr`).

## 2. The archives

`DATA0_0`, `DATA0_1`, `DATA1_0`, `DATA1_1` (`.DAT` with a `.HD6` index) hold
every file. HD6 layout, as in the Dragon Quest VIII notes: a 0x34 byte header,
a pool of name fragments, a name table of LEB128 fragment ids, and a file table
of 64-bit entries (17-bit name offset, 21-bit data offset in 1024-byte units,
26-bit size in 4-byte units). The `_0` archives hold the game's resources; the
`_1` archives hold the voices (`wav`) and `snd2`.

Language dependent files, by name:

- **Scene dialogue:** inside `event\evNN\evNN-SSSS\evNN-SSSS.pac` (section 3).
- **Other text:** `event\ev_talk_<l>.txt`, `ev_thinking_<l>.txt`,
  `ev_ins_vs_<l>.txt`, `menu\cmd_*_<l>.txt`, `menu\inshelp_<l>.txt`,
  `rc\param\text\*_<l>.pac` (about 150 per language), `rc\param\zone\*_<l>.pac`
  (about 140), `menu\rc\*_<l>.pac`, `map\caption\*_<l>.pac`.
- **Pictures with words in them:** `menu\help_board\<l>\`,
  `event\boss_name\<l>\`, `event\area_master_name\<l>\`, `menu\fac_help\<l>\`.
- **Fonts:** `meswin\fontset_main_<l>.pak` and `fontset_launcher_<l>.pak`
  (section 5).
- **Voices:** `wav\<l>\evNN-SSSS\NN-SSSS-LLLL.mwv`, `wav\<l>\livetalk\...`.

Languages on each disc: US `en`; PAL `en fr de es it` (the font is `eur`);
Japan `ja`. The PAL `DATA0_1`/`DATA1_1` indexes are identical to the US ones.

## 3. A scene's lines

A scene event is a PAC container holding, per language, three entries:
`evNN-SSSS_<LANG>.evt` (script source), `.txt` (the lines) and `.stb` (compiled
script, magic `SB2`). Container entry layout: a 0x40-byte name, then
little-endian u32 header size (0x50), u32 data size and u32 stride to the next
entry, then the data at the header size; a size of 0xFFFFFFFF ends the pack.

- The US pack holds the EN trio, the Japanese pack the JA trio (about 2% larger
  than the US), the PAL pack the EN, IT, ES, FR, DE trios (about 5.2 times the
  US size). The PAL scripts (`.stb`) are not the same size as the US ones.
- `EvCmdEventPlay` (0x259750) builds the three names with the `%s_?.*` formats,
  localizes them, loads the pack through `FileLoadCached` and hands the `.txt`
  to the message window (`EvCmdMesWinLoadFile`) and the others to
  `EvCmdEventPlayerCreate`. The pack is searched by name like an archive.
- The `.txt` is plain text. A line starts with `@` and a ten-digit label
  (event 2 digits, scene 4, line 4), the body follows after a line break, `\n` breaks a line inside a window, and the next `@` starts the
  next label. Japanese text is Shift-JIS; the PAL European text is ASCII with
  markup (section 4).
- Menu and help tables use `MsgTableParseText` (0x28AAB0): `@N` entries, an
  `@I` base id, `//` comments, Shift-JIS lead bytes kept as pairs.
- Windows that look a message up by number use a binary table instead:
  `MesWinLookupMsg` (0x2745F0) scans `count` pairs of (id, offset) and returns
  an offset into a u16 text area.

## 4. Markup in text and accents

Text carries tags that the window turns into glyphs:

- Button icons such as `[l1]`, `(O)`: table at 0x44EA00 (`TextGaijiTagFind`
  0x26F060, `FontMatchGaijiTag`).
- Symbols such as `[ar-r]`: table at 0x44EB20 (`TextSymbolTagFind`
  0x26EFE0). Entry: name pointer, name length (s16), glyph code (u16).
- The US executable has no accent tags. The PAL executable adds a table of 64
  tags (grave `` ` ``, acute `'`, circumflex `^`, diaeresis `:`, cedilla `,`,
  tilde `~`, plus `<ss>`, `<oe>`, `<OE>`, digits and a few signs) at 0x45C420.
  Entry: name pointer, name length (u8), three advance widths (bytes, one per
  font). Their internal character codes run 0x423 to 0x462; the PAL layout code
  (around 0x26E440 to 0x26E500) indexes this table by `code - 0x423` for the
  width and the plain width table (0x45C270) below that.

Open: where the PAL draws those 64 glyphs from. The glyph tables (section 5)
are byte-identical between the US and PAL fonts, so the picture lives either in
the first texture page (which differs) or in PAL-only code.

## 5. Fonts

A `fontset_*.pak` is a PAC container with, per size class (`name`, `system`,
`normal`, `launcher`): a glyph table `fonttbl_<class>_<l>.bin` and one to five
TIM2 pages `FontTex_<class>_<l>_N.tm2`, plus `gaiji2.img` (the button icons).
`FontTablesLoad` (0x2702E0) reads the tables by name and `FontLoadGlyphTable`
(0x270270) registers them; `MesFontGetPakPath` (0x2704B0) builds the pack path.

Glyph table: u16 ASCII count, u16 total count, u32, then the code list as
byte pairs (an ASCII code is the character followed by 0x20; the rest are
Shift-JIS codes). `FontFindCode` (0x26EE00) binary-searches it. The US `normal`
table has 89 ASCII entries and 247 Shift-JIS entries. US EN and PAL EUR tables
are identical; of the texture pages, `normal` pages 1 to 3 are identical, pages
0 differ (the PAL one carries the extra glyphs), and the US has a fifth page.

## 6. Voices

Each spoken line is `wav/!/EV<ee>-<ssss>/<ee>-<ssss>-<llll>.mwv`, built from
the same ten-digit number as its text label: `EventVoiceMakePath` (0x24EDB0)
and `EventVoicePlayLine` (0x2071B0) split it as `ee = id / 1e8`,
`ssss = (id mod 1e8) / 1e4`, `llll = id mod 1e4`, format the path and localize
it (`!`). `EventVoiceWithMessage` (0x24EBF0) starts the voice together with the
window and finds the label in the message text. Live talks use
`wav/!/LiveTalk/...`.

A `.mwv` is a RIFF/WAVE file with format tag 0x0555 (a custom codec), mono,
plus an extra chunk. The US files are 48 kHz, the Japanese ones 44.1 kHz.

How the files line up: US `wav\en\...` and Japanese `wav\ja\...` share 6892 names
once the folder is swapped (6955 US files, 6930 Japanese). 63 exist only in the
US set and 38 only in the Japanese set (lines added or cut in the localization;
the US-only ones are spread over events 1 to 8).

## 7. Movies

Movies are not in the `DATA*` archives. They are loose files in `MOVIE\<nn>\*.MVI`
(89 per disc, the same names on the US and Japanese discs; the `ev00_movie*.pac`
entries in the archives only hold scripts). The voice token `!` does not touch
them: a movie carries its own sound.

- **Container.** An `.MVI` is an MPEG program stream (PSS) with a four-byte
  trailer. It is encrypted by XOR with a keystream that restarts every 16 KiB
  (4096 words): word `n` of a block is `k0 * 0x02E90EDD^n` (32-bit), and `k0` is
  recovered from the first word, since a decrypted file starts with the pack
  header `00 00 01 BA`. No secret key is needed. The game decodes the same
  stream (`MpegDemuxPss`); see also `docs/STATUS.md`, "Movie audio".
- **Streams.** Video is stream `E0`; sound is private stream 1 (`BD`) with the
  `SShd`/`SSbd` 16-bit PCM blocks.
- **US against Japan** (89 movies): 52 are byte-identical (no voice, or music
  only). 37 differ, in two groups:
  - **Same size** (about 15 movies, around 45% of the sectors differ). In the one
    sample taken apart (a story scene) the video stream is identical and only the
    sound differs, so the voices could be swapped in without touching the picture.
  - **Different size** (about 22 movies, almost every sector differs): the
    picture was encoded again (title screens, the credits, the character
    introductions, the opening). Words in the picture are likely baked in there.
    A sample with a small difference (0.1% in the video) exists too.
- **For the Japanese dub:** the same-size group needs only the Japanese file played
  in place of the US one (the picture is the same, and it has no baked-in
  subtitles to clash). For the other group, the Japanese file has the Japanese
  picture too (titles, credits); the choice is the player's.

## 8. What the two requests need

- **Voices from the Japanese disc:** make `!` read `JA`, and resolve the
  `wav/ja/...` files from the player's own Japanese `DATA*_1` archives (a
  second archive set, not merged into the US ones). The 63 names that exist
  only in the US set need a fall back (to English, or to silence). Tested: with the voice token set to `JA` and the Japanese `DATA*_1`
  archives in the disc folder, the Japanese voices play in a voiced scene, 44.1 kHz
  included (`src/voice_language.cpp`, `RG_VOICE_LANG=JA`).
  Movies are separate (section 7): the loose `MOVIE\*.MVI` of the Japanese disc
  play in place of the US ones, file by file.
- **Proposed way to mount the Japanese voices.** The runtime does not read an ISO: it
  serves `cdrom0:` from a folder and registers each file the game asks for under a
  pseudo sector number (`registerCdFile`, `sceCdSearchFile`). So the same model as
  the US disc works: setup extracts the player's Japanese `DATA0_1` and `DATA1_1`
  (`.DAT` and `.HD6`, about 1.9 GB) into a second folder, and when the voice
  language is Japanese the runtime resolves those four guest names to that folder.
  Movies work the same way (`MOVIE\*.MVI` resolved to the second folder). The 63
  US-only lines then have no file; the first step is to leave them silent and check
  that the game tolerates a missing voice, the second is a retry on `en` if both
  archive sets can be mounted.
  Done as an experiment without touching the runtime: `CdDataSetNamesInit`
  (0x1C31A0) fills the two archive names of a layer (`DATA%d_0`, `DATA%d_1`, 32
  byte slots); renaming the second to `JA/DATA%d_1` makes the game look for
  `JA/DATA<layer>_1.HD6` and `.DAT`, and the disc folder serves `disc/JA/`.
- **European text:** make `?` read the chosen language and resolve the
  `.txt`/`.evt`/`.stb` entries from PAL packs. The PAL `.stb` differs from the
  US one, so the PAL script has to travel with the PAL text (the whole
  per-language trio, from the PAL pack). Other `?` files (`menu`, `rc\param`)
  come the same way from the PAL archives. Needs the PAL font and the 64
  accent codes added to the US executable's tag lookup and layout code.
- Both need the player's own discs read at run time and a language setting; the
  chosen language and the disc paths have to reach the hook that fills the two
  tokens.

## 9. Not yet studied

- How PAL draws the 64 accent glyphs (texture cell source and layout).
- Text overflow: how window width and line count are fixed per style, and how
  the longer European lines fit (the one scene text sampled was 11% longer in
  French than in English).
- Whether the `.stb` scripts address texts by the same labels in every
  language (the labels in the sample did) and what else differs between the US
  and PAL scripts.
- Which sound a movie plays: whether the game picks the file by name only, so a
  Japanese `.MVI` can be handed over by the file layer (not tried).
- The `.mwv` codec itself (the stream player takes 44.1 kHz without trouble).
- Menu and help text: which of the `?` files the US build opens at which
  moments.
