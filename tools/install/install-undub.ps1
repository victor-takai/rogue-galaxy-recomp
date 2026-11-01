# Rogue Galaxy Recompiled: the installer behind Install-Undub.bat.
#
# Takes the Japanese voices and movies (the "undub") from the player's own
# Japanese disc image, the Director's Cut (SCPS-17013), into disc\JA. It uses
# the Python that Install.bat downloaded into toolchain\, so Install.bat has
# to have run first. Afterwards: Settings > Language > Undub in the game's menu
# (F1).
#
#   Install-Undub.bat                   (or drop the Japanese .iso onto it)
#   Install-Undub.bat -Iso "D:\Rogue Galaxy (Japan).iso"
#
# Windows PowerShell 5.1, so no newer syntax; plain ASCII.

param(
    [string]$Iso = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path

function Stop-Undub([string]$Text) {
    Write-Host ""
    Write-Host $Text -ForegroundColor Red
    exit 1
}

function Select-DiscImage {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your Rogue Galaxy Japanese Director's Cut disc image"
    $dialog.Filter = "Disc images (*.iso)|*.iso|All files (*.*)|*.*"
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.FileName }
    } finally {
        $owner.Dispose()
    }
    return ""
}

try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }
Write-Host "Rogue Galaxy Recompiled: Japanese voices (undub)" -ForegroundColor Green
Write-Host "Takes the voices and movies from your own Japanese disc into this folder:"
Write-Host "  $Root"

$python = Join-Path $Root "toolchain\python\python.exe"
if (-not (Test-Path -LiteralPath $python)) {
    # Developers who built with -UseToolsOnPath have no toolchain folder.
    $onPath = Get-Command python -ErrorAction SilentlyContinue
    if ($onPath -and (Test-Path -LiteralPath (Join-Path $Root "build\game"))) {
        $python = $onPath.Source
    } else {
        Stop-Undub "The game is not installed here yet. Run Install.bat first, then this."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $Root "disc\manifest.json"))) {
    Stop-Undub "disc\ does not hold the extracted US disc. Run Install.bat first, then this."
}

if (-not $Iso) {
    Write-Host ""
    Write-Host "Choose the image of your Rogue Galaxy Japanese disc (the Director's Cut, SCPS-17013) in the window that opens."
    $Iso = Select-DiscImage
    if (-not $Iso) { Stop-Undub "No disc image chosen. Run Install-Undub.bat again when you have it." }
}
if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) { Stop-Undub "There is no file $Iso." }
Write-Host "  $Iso"

# Only this program's environment, the way Install.bat keeps the build's.
$env:PYTHONUTF8 = "1"
$env:PYTHONUNBUFFERED = "1"
foreach ($name in @("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP")) {
    Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Copying the voices and movies (about 3 GB); it takes a few minutes."
$ErrorActionPreference = "Continue"
Push-Location -LiteralPath $Root
try {
    & $python -u (Join-Path $Root "setup.py") extract-japanese --iso $Iso 2>&1 | ForEach-Object { Write-Host "$_" }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
$ErrorActionPreference = "Stop"
if ($code -ne 0) {
    Stop-Undub "It stopped (code $code). If the message above says it is not the Japanese Director's Cut, that disc has voices that do not line up with the US game."
}

Write-Host ""
Write-Host "Done. In the game, press F1 > Settings > Language and turn on the undub." -ForegroundColor Green
