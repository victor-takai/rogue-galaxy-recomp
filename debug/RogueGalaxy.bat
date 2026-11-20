@echo off
rem Starts Rogue Galaxy Recompiled on the extracted disc in disc\.
rem Double-click it, or run it from a terminal; extra arguments go to the game.
rem The game's log is written to build\last-run.log.
rem "Rogue Galaxy.exe" in the repository's folder, which the build makes, does
rem the same without this terminal (src\launcher\launcher.c): the one to
rem double-click for playing. This file stays for the other debug\ launchers
rem and terminal runs; a change to what it sets belongs in the launcher too.
setlocal
rem The repository's folder, above this one.
cd /d "%~dp0.."

set "GAME=build\game\src\roguegalaxy.exe"
if not exist "%GAME%" (
    echo The game is not built yet: run Install.bat, or see docs\BUILDING.md.
    pause
    exit /b 1
)
if not exist "disc\SCUS_974.90" (
    echo No extracted disc in disc\: run  python setup.py extract  first.
    pause
    exit /b 1
)

rem Options (see docs\DEVELOPMENT.md), for example:
rem   set RG_SHOW_FPS=1
rem   set RG_MUTE=1
rem 60 fps or the game's own 30: the menu's 60 frames per second (F1,
rem Interface > Game), from the next start. RG_FPS=30 or RG_FPS=60 set before
rem this file in a terminal decides for that run instead (see README.md).
rem One [gfx] line a second in build\last-run.log (the frame rate and the
rem backend's counters), so an F9 mark can be put next to that second.
if not defined DQ8_GFX_STATS_EVERY set DQ8_GFX_STATS_EVERY=60
rem Keeps other programs' Vulkan layers (OBS's capture hook, ReShade, the
rem Steam, GOG and Epic overlays) out of the game: they load into every
rem Vulkan process and sit on its presents. Remove the line to allow them.
set VK_LOADER_LAYERS_DISABLE=~implicit~

echo Rogue Galaxy is running. Close the game window to quit.
rem The previous run's log survives one more run as last-run.prev.log.
if exist "build\last-run.log" copy /y "build\last-run.log" "build\last-run.prev.log" >nul
"%GAME%" "disc\SCUS_974.90" %* > "build\last-run.log" 2>&1
rem Not "if errorlevel 1": a crash exits with a negative code (0xC0000005).
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo The game stopped with an error ^(exit code %RC%^). The end of build\last-run.log:
    powershell -NoProfile -Command "Get-Content -Encoding UTF8 -Tail 15 'build\last-run.log'"
    pause
)
exit /b %RC%
