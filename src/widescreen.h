#pragma once

class PS2Runtime;

// The game drawn for a wide screen, of the window's shape.
//
// The game keeps a wide mode of its own that nothing in this release turns
// on: an aspect mode at 0x481638, set to 1 by the system config's "TVMODE
// WIDE". In it the scene camera takes 9/16 for 3/4 (SceneCameraApplyAspect)
// and the HUD drawn over the scene (gauges, target slots, minimap entries,
// the attack chain, the sprite helpers) narrows its sprites by 3/4, so they
// keep their shape once the picture is widened. Its two readers,
// GetAspectMode and IsWidescreen, answer 1 here while the option is on.
//
// The rest is nemesis2000's PCSX2 patch for SCUS-97490: the compass and the
// insect menus' model previews, which set 3/4 themselves, take 9/16 too; a movie
// made for 16:9, drawn between bars on 4:3, fills the frame from top to
// bottom. Its letterbox fix is not taken: events keep their bars, by which
// the game places their text. Beyond the patch, a 4:3 movie (512x448)
// narrows to keep its shape, with bars at its sides.
//
// The presenter shows the picture at 16:10, the Steam Deck's screen, the
// one shape there is (a window of another shape gets bars), and the camera,
// the compass and the movies take it for the 9/16 of the mode and the patch.
// The HUD's 3/4 is the mode's own, right at 16:9, a little narrow at 16:10.
// 2D menus and text are drawn for 4:3 and come out stretched; so are the
// screens before the game (the title overlay's: opening, title, load menu,
// and the loading screen's recap), where the game draws for 4:3.
//
// Always on: there is no option and no environment variable.
namespace rg::widescreen
{

// After loadELF().
void install(PS2Runtime &runtime);

// The game is past the screens before it: the frame is drawn wide.
bool wide();

} // namespace rg::widescreen
