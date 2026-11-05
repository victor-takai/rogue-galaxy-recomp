@echo off
rem Rogue Galaxy Recompiled: the texts in French, German, Spanish and Italian
rem from your own European disc image. Run Install.bat first. Double-click this,
rem or drop the European .iso onto it. The work is done by
rem tools\install\install-european.ps1.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install-european.ps1" %*
echo.
pause
