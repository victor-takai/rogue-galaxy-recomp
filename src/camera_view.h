#pragma once

class PS2Runtime;

// The field and battle camera's field of view, distance and turn by the
// stick.
//
// The scene's projection is set again every frame by ViewSetProjection
// (0x143000, f12 the vertical field of view in radians), from ViewSetAspect,
// ViewSetDepthRange and ViewReapplyCameraRec. The field camera's is fixed at
// 52 degrees across a 4:3 picture (CameraObjInit, 0x309AB0:
// FovHorizToVert(52), 40.2 degrees high), in the field, in battle and in the
// transporter's menu alike. With the menu's game number `field_of_view` at
// another value, that projection takes it instead while exploring (game
// mode 6) or fighting (7); an event's camera, which sets its own angle
// (CameraTrackUpdate), keeps it. A wide picture widens it further, as before.
//
// The camera keeps between a nearest and a farthest distance from the leader,
// pushed back or pulled in past them (CameraObjOrbitControl): 65 to 120 in
// the field, written by CameraSetDefaultParams (0x309260) into the camera's
// block at +0x15C, and in battle 150 to 190 up or 120 to 160 down, the two
// settings the camera button switches between, by CameraSetPresetParams
// (0x309100) into the same block. `camera_distance`, a percentage, scales
// both as they are written, and when it moves the cameras (the game keeps
// two, 0x4B5950 and 0x4B6220) take it at the next projection, each whose
// block still holds what was last written there.
//
// The field camera turns by the right stick in
// CameraObjOrbitControl (0x161260, from CameraObjApplyInput in mode 0's
// CameraFollowLeaderStep): its a1 points at the turn and the tilt this frame,
// the stick times the camera's speeds, which it adds whole, a frame's worth
// each frame, so at 60 frames a second the camera turned twice as fast as
// at 30; the battle cameras' turn by the stick (CameraModeLockTargetUpdate's
// and CameraTargetFollowUpdate's reads of it) alike, and the first-person
// view's (CameraFirstPersonUpdate, R3), by both sticks. All are multiplied
// by the frame's share of a step in the 60 fps mode (step fix "camera",
// the scene's time scale without the game's slow motion), the console's
// speed again, and by two with `faster_camera` ("Faster camera rotation"),
// at either frame rate. The following battle camera's tilt is not a turn:
// the stick sets how far it tilts, which it eases toward, so it stays as
// the game has it.
//
// With "Mouse camera" on (src/mouse_camera.h), the mouse's turn and tilt go
// in at the same places, after those factors, whatever the game's camera
// options: the field's turn and tilt as CameraObjOrbitControl gets them from
// the player's input (and its own read of the stick, which at the lowest
// tilt brings the camera in to look up), the battle cameras' reads of the
// stick, the first-person view's angles. The following battle camera's tilt
// is a push the mouse holds until the field camera takes the mouse again.
namespace rg::cameraview
{

// After loadELF().
void install(PS2Runtime &runtime);

// Degrees across a 4:3 picture; 52 is the game's.
void setFieldOfView(int degrees);
// Percent of the game's distances.
void setDistance(int percent);
// The stick turns the camera twice as fast (both in the first-person view).
void setFasterTurn(bool on);

} // namespace rg::cameraview
