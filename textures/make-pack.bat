@echo off
rem The larger 2D art made from what textures\RogueGalaxy-collect.bat
rem collected, from scratch each time: the text among the pictures in
rem textures\collected (the fonts' glyphs, the menus' labels; "all" as an
rem argument takes every picture) made ready for an upscaler
rem (textures\to-upscale), enlarged 4x (textures\upscaled) and put back
rem together into textures\hd, which the game reads when it starts
rem (tools\texture_pack\). The upscaler is Upscayl's, with its UltraSharp
rem model, where Upscayl is installed; otherwise Real-ESRGAN's Windows build
rem from github.com/xinntao/Real-ESRGAN/releases extracted into
rem textures\realesrgan, with its realesr-animevideov3 model. The other
rem pictures (icons, portraits, window pieces) are enlarged by xBR, which
rem keeps their pixels' shapes. "xbr" as the argument enlarges everything
rem by xBR, the text too, with no upscaler.
setlocal
set "UPSCAYL=%ProgramFiles%\Upscayl\resources"
set "PREPARE="
if /i "%~1"=="all" set "PREPARE=--all"
for %%D in (to-upscale upscaled hd) do if exist "%~dp0%%D" rmdir /s /q "%~dp0%%D"
if /i "%~1"=="xbr" (
    python "%~dp0..\tools\texture_pack\make_pack.py"
    goto done
)
python "%~dp0..\tools\texture_pack\prepare.py" %PREPARE% || goto done
mkdir "%~dp0upscaled"
if exist "%UPSCAYL%\bin\upscayl-bin.exe" (
    "%UPSCAYL%\bin\upscayl-bin.exe" -i "%~dp0to-upscale" -o "%~dp0upscaled" -m "%UPSCAYL%\models" -n ultrasharp-4x -s 4 -f png
) else if exist "%~dp0realesrgan\realesrgan-ncnn-vulkan.exe" (
    "%~dp0realesrgan\realesrgan-ncnn-vulkan.exe" -i "%~dp0to-upscale" -o "%~dp0upscaled" -n realesr-animevideov3 -s 4 -f png
) else (
    echo No upscaler: install Upscayl, or extract Real-ESRGAN into textures\realesrgan.
    goto done
)
python "%~dp0..\tools\texture_pack\make_pack.py"
:done
pause
