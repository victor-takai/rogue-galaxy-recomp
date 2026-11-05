#pragma once

#include <cstdint>

// Scenes that only make the player wait, played faster: the whole game at up
// to twice the speed while one is on screen (the host's pace, as the
// menu's fast-forward; the sound driver keeps the audio device's, so music
// and sounds do not change).
//
// What the game is doing is its mode (u32 at 0x4B51A0, the index into the
// mode table GameModeDispatch runs), and where that is not enough the
// warp's script:
// - a transporter's warp: mode 11 (TransporterWarpModeUpdate, which runs the
//   area script a warp request names, s32 at 0x4821E0) on the transporter's
//   scripts, 0x320 leaving and 0x321 arriving; the other warps' scripts
//   (doors, lifts, story scenes) keep their pace.
// A chest opening (mode 13, TreasureBoxModeUpdate running the chest's
// script) was one too; even at twice it was too quick for the user, and
// worth little, so it was taken out. So were items going into a Revelation
// Flow cell (the board's placing step, +0x76E of the board
// RevelationBoardUpdate, 0x34C290, takes each frame; 4.1 s at the console's
// pace, 2.2 s hurried), on 2026-11-05.
// RG_SCENE_TRACE=1 prints each change of mode or of the mode's phase (s32
// at 0x4B51E0, which no scene goes by) and of the main menu's state (the id
// of the entry its machine at 0x48235C runs).
namespace rg::quickscenes
{

// After loadELF().
void install();
// The menu's "Fast transporters".
void setFastTransporter(bool on);

// The speed for what the game shows now (1 when nothing is to be hurried),
// from guest RAM; the main thread's.
double speed(const uint8_t *rdram);

} // namespace rg::quickscenes
