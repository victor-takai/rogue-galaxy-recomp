@echo off
rem Rogue Galaxy collecting its 2D art for a replacement pack: every glyph of
rem the font, menu label drawn as a picture, icon and piece of a window that
rem appears on the screen is written once into textures\collected, named by
rem its content (see src\texture_pack.h). Play as usual: the folder only
rem grows with what it has not seen yet. The pictures come from the game's
rem disc, so they stay on this machine (.gitignore keeps them out).
setlocal
rem The repository's folder, above this one.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
if not exist "%~dp0collected" mkdir "%~dp0collected"
set "RG_TEXTURE_DUMP=%~dp0collected"
call "%ROOT%\debug\RogueGalaxy.bat" %*
