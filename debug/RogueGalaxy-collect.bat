@echo off
rem Rogue Galaxy collecting its 2D art for a replacement pack: every glyph of
rem the font, menu label drawn as a picture, icon and piece of a window that
rem appears on the screen is written once into textures\collected, named by
rem its content (see src\texture_pack.h). Play as usual: the folder only
rem grows with what it has not seen yet. The pictures come from the game's
rem disc, so they stay on this machine (.gitignore keeps them out). This file
rem is not in textures\: the release zip and Update.bat leave that folder to
rem the player.
setlocal
rem The repository's folder, above this one.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
if not exist "%ROOT%\textures\collected" mkdir "%ROOT%\textures\collected"
set "RG_TEXTURE_DUMP=%ROOT%\textures\collected"
call "%ROOT%\debug\RogueGalaxy.bat" %*
