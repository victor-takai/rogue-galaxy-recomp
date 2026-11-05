# Rogue Galaxy Recompiled: the installer behind Install-European.bat.
#
# Builds the texts of the player's own European disc image (SCES-54552:
# French, German, Spanish, Italian) into the US game's files, in disc\EU
# (about 80 MB a language). It uses the Python that Install.bat downloaded into
# toolchain\, so Install.bat has to have run first. Afterwards: Settings >
# Language > Text in the game's menu (F1).
#
#   Install-European.bat                   (or drop the European .iso onto it)
#   Install-European.bat -Iso "D:\Rogue Galaxy (Europe).iso"
#
# Windows PowerShell 5.1, so no newer syntax; plain ASCII.

param(
    [string]$Iso = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path

function Stop-European([string]$Text) {
    Write-Host ""
    Write-Host $Text -ForegroundColor Red
    exit 1
}

function Select-DiscImage {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your Rogue Galaxy European disc image"
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
Write-Host "Rogue Galaxy Recompiled: European texts (French, German, Spanish, Italian)" -ForegroundColor Green
Write-Host "Builds the texts of your own European disc into this folder:"
Write-Host "  $Root"

$python = Join-Path $Root "toolchain\python\python.exe"
if (-not (Test-Path -LiteralPath $python)) {
    # Developers who built with -UseToolsOnPath have no toolchain folder.
    $onPath = Get-Command python -ErrorAction SilentlyContinue
    if ($onPath -and (Test-Path -LiteralPath (Join-Path $Root "build\game"))) {
        $python = $onPath.Source
    } else {
        Stop-European "The game is not installed here yet. Run Install.bat first, then this."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $Root "disc\manifest.json"))) {
    Stop-European "disc\ does not hold the extracted US disc. Run Install.bat first, then this."
}

if (-not $Iso) {
    Write-Host ""
    Write-Host "Choose the image of your Rogue Galaxy European disc (SCES-54552) in the window that opens."
    $Iso = Select-DiscImage
    if (-not $Iso) { Stop-European "No disc image chosen. Run Install-European.bat again when you have it." }
}
if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) { Stop-European "There is no file $Iso." }
Write-Host "  $Iso"

# Only this program's environment, the way Install.bat keeps the build's.
$env:PYTHONUTF8 = "1"
$env:PYTHONUNBUFFERED = "1"
foreach ($name in @("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP")) {
    Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Building the four languages' texts (about 320 MB); it takes about a minute."
$ErrorActionPreference = "Continue"
Push-Location -LiteralPath $Root
try {
    & $python -u (Join-Path $Root "setup.py") extract-european --iso $Iso 2>&1 | ForEach-Object { Write-Host "$_" }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
$ErrorActionPreference = "Stop"
if ($code -ne 0) {
    Stop-European "It stopped (code $code). If the message above says it is not the European Rogue Galaxy, that disc's texts are not known to line up with the US game."
}

Write-Host ""
Write-Host "Done. In the game, press F1 > Settings > Language and pick the text's language." -ForegroundColor Green
