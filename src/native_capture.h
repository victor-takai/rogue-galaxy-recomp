#pragma once

class PS2Runtime;

// The game functions the native pipeline's producer learns from
// (docs/NATIVE-PIPELINE.md, step 3): the render targets the game names
// (DrawEnvSetFrameFromDesc, and the frame's and the display's descriptor
// getters) and the textures and palettes it uploads (TexBuildUploadPkt),
// each read from the game's own data before the function runs, and handed
// to src/native_producer.h. Installed only with the native pipeline on.
namespace rg::nativecapture
{
void install(PS2Runtime &runtime);
}
