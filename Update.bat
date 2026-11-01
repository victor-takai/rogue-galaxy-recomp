@echo off
rem Rogue Galaxy Recompiled: brings this folder to the newest release and
rem rebuilds what changed. Double-click it, or drop a downloaded
rem RogueGalaxy-<version>.zip onto it. The work is done by
rem tools\install\update.ps1, which says more.
rem
rem A copy of this file runs from the temp folder, with the game's folder as
rem its argument: the update replaces the one in the folder while it runs.
if /i not "%~1"=="--from-temp" (
    copy /y "%~f0" "%TEMP%\RogueGalaxyUpdate.bat" >nul
    "%TEMP%\RogueGalaxyUpdate.bat" --from-temp "%~dp0" %*
    exit /b
)
set "RG_ROOT=%~2"
if "%RG_ROOT:~-1%"=="\" set "RG_ROOT=%RG_ROOT:~0,-1%"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%RG_ROOT%\tools\install\update.ps1" -Root "%RG_ROOT%" %3 %4 %5 %6
echo.
pause
