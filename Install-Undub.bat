@echo off
rem Rogue Galaxy Recompiled: the Japanese voices (undub) from your own Japanese
rem disc image. Run Install.bat first. Double-click this, or drop the Japanese
rem .iso onto it. The work is done by tools\install\install-undub.ps1.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install-undub.ps1" %*
echo.
pause
