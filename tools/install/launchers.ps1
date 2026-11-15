# Rogue Galaxy Recompiled: what the install scripts do for a launcher that
# installs the game into a folder of its own and starts it with Play
# (QuiverLauncher, 2026-11-13). Dot-sourced by the scripts behind
# Install.bat, Update.bat, Install-European.bat and Install-Undub.bat.
#
# Such a launcher starts a batch file with no input, no output and no
# arguments: its window stays empty, and every question reads as no. So a
# script started that way runs its batch file again in a new window, where
# it can be read and answered, and waits for that window. Started with
# arguments (-Yes, -Iso, from a script or a test), or with RG_OWN_WINDOW
# set, nothing changes.
#
# Until a program is in the folder, Play starts the first batch file by
# name, Install-European.bat; so it, and Install-Undub.bat, install the game
# first while it is not built. Once the compiler is downloaded, install.ps1
# builds "Rogue Galaxy.exe" (Build-Launcher), which Play starts from then
# on, and which offers Install.bat while the game is not built.
#
# Windows PowerShell 5.1, so no newer syntax; plain ASCII.

# Runs $Bat in a new window and returns its exit code once that window
# closes, when this script has no input or output; $null otherwise, and in
# that new window (RG_OWN_WINDOW says it is one).
function Invoke-OwnWindow([string]$Bat) {
    if ($env:RG_OWN_WINDOW) { return $null }
    if (-not ([Console]::IsInputRedirected -and [Console]::IsOutputRedirected)) { return $null }
    $env:RG_OWN_WINDOW = "1"
    # cmd.exe by name: a batch file opened through its file type opens in an
    # editor on a PC set up that way.
    $cmd = Join-Path $env:SystemRoot "System32\cmd.exe"
    $window = Start-Process -FilePath $cmd -ArgumentList "/d /c `"`"$Bat`"`"" -WorkingDirectory (Split-Path -Parent $Bat) -PassThru
    # Only the window's own process: Start-Process -Wait would also wait for
    # what it starts, the game among them.
    $null = $window.Handle
    $window.WaitForExit()
    return $window.ExitCode
}

# While the game is not built in $Root (the file "Rogue Galaxy.exe" checks
# for), says so and runs install.ps1 instead of the calling script, then
# exits with its code; $Bat is the caller's batch file, which adds $What
# when run again afterwards. The disc image the caller was given is not the
# US one, so it is not passed on.
function Invoke-InstallFirst([string]$Root, [string]$Bat, [string]$What) {
    if (Test-Path -LiteralPath (Join-Path $Root "build\game\src\roguegalaxy.exe")) { return }
    Write-Host "The game is not installed in this folder yet: this installs it."
    Write-Host "For $What, run $Bat again afterwards."
    Write-Host ""
    # This window is the install's: it has no arguments, but needs no other.
    $env:RG_OWN_WINDOW = "1"
    & (Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe") -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot "install.ps1")
    exit $LASTEXITCODE
}

# One install or update at a time in a folder: a second one (Install.bat
# started again, or by "Rogue Galaxy.exe" while one runs) would build over
# it, and the game would start half built. $Root\build\install.lock is held,
# unshared, while the caller runs, and Windows deletes it when the caller
# ends, however it ends; "Rogue Galaxy.exe" looks at it too. Returns the
# open file, to be kept and disposed of to let go; "busy" when another
# install or update holds it; $null when it cannot be made (the build then
# says why).
function Lock-Folder([string]$Root) {
    $lock = Join-Path $Root "build\install.lock"
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $lock) | Out-Null
        return New-Object System.IO.FileStream($lock, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None, 1, [System.IO.FileOptions]::DeleteOnClose)
    } catch {
        # ERROR_SHARING_VIOLATION
        if ($_.Exception.GetBaseException().HResult -eq -2147024864) { return "busy" }
        return $null
    }
}
