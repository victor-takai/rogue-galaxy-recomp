@echo off
rem The larger 2D art made from what debug\RogueGalaxy-collect.bat
rem collected, from scratch each time: the text among the pictures in
rem textures\collected (the fonts' glyphs, the menus' labels; "all" as an
rem argument takes every picture) made ready for an upscaler
rem (textures\to-upscale), enlarged 4x (textures\upscaled) and put back
rem together into textures\hd, which the game reads when it starts
rem (this folder's scripts). The upscaler is Upscayl's, with its UltraSharp
rem model, where Upscayl is installed; otherwise Real-ESRGAN's Windows build
rem from github.com/xinntao/Real-ESRGAN/releases extracted into
rem textures\realesrgan, with its realesr-animevideov3 model. The other
rem pictures (icons, portraits, window pieces) are enlarged by xBR, which
rem keeps their pixels' shapes. "xbr" as the argument enlarges everything
rem by xBR, the text too, with no upscaler.
setlocal
rem The textures folder, two above this one.
for %%I in ("%~dp0..\..\textures") do set "TEX=%%~fI"
set "UPSCAYL=%ProgramFiles%\Upscayl\resources"
set "PREPARE="
if /i "%~1"=="all" set "PREPARE=--all"
for %%D in (to-upscale upscaled hd) do if exist "%TEX%\%%D" rmdir /s /q "%TEX%\%%D"
if /i "%~1"=="xbr" (
    python "%~dp0make_pack.py"
    goto done
)
python "%~dp0prepare.py" %PREPARE% || goto done
mkdir "%TEX%\upscaled"
if exist "%UPSCAYL%\bin\upscayl-bin.exe" (
    "%UPSCAYL%\bin\upscayl-bin.exe" -i "%TEX%\to-upscale" -o "%TEX%\upscaled" -m "%UPSCAYL%\models" -n ultrasharp-4x -s 4 -f png
) else if exist "%TEX%\realesrgan\realesrgan-ncnn-vulkan.exe" (
    "%TEX%\realesrgan\realesrgan-ncnn-vulkan.exe" -i "%TEX%\to-upscale" -o "%TEX%\upscaled" -n realesr-animevideov3 -s 4 -f png
) else (
    echo No upscaler: install Upscayl, or extract Real-ESRGAN into textures\realesrgan.
    goto done
)
python "%~dp0make_pack.py"
:done
pause
