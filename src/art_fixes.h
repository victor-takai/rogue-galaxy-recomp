#pragma once

class PS2Runtime;

// Slips in the game's own pictures, fixed in memory as the game registers
// them, before any reaches the GS.
//
// The main menu's Revelation page labels the members' portraits "Player"
// and "Party" from Pty_02 of menu_pt_b_EN.img (menu\rc\menu_static_img_en.pac,
// 128x128, 8-bit). 14 of its texels, at the outer corners of the words'
// black edges, take three opaque magenta colours of its palette (entries 74,
// 83 and 90, nothing else uses them): a pink dot at each, drawn nearest, on
// the console too, the US disc and the PAL disc's English alike. They take
// the picture's clear index (98), as the texels around them. The European
// text's pictures (other words) have none.
//
// RG_ART_FIXES=0 leaves the pictures as the disc has them, the console's
// look, to compare. Typeface letters then leaves Player and Party as the game
// draws them: its table has the fixed picture's words.
namespace rg::artfix
{

// After loadELF().
void install(PS2Runtime &runtime);

} // namespace rg::artfix
