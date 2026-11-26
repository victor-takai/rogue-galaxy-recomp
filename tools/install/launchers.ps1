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

# Such a launcher updates by unpacking the new release's zip over the folder:
# the new files replace the old ones, and build\, the old "Rogue Galaxy.exe"
# and the files the new version dropped stay. The zip's FILES lists its
# files and REVISION its commit. REVISION's text and the two files' dates
# are kept in build\unpacked-archive once the archive's files are in place:
# the files the new version no longer has removed, and the build told what
# changed (install.ps1, Update-Unpacked; update.ps1 writes the files
# itself). FILES's date is new for each archive made; REVISION's, which
# Update-Unpacked sets to now, goes back to the zip's when the same archive
# is unpacked again (a build stopped halfway, then unpacked anew, would
# otherwise keep objects of the version before). "" when the folder has no
# FILES or REVISION (a git checkout, or a release before FILES).
function Get-UnpackStamp([string]$Root) {
    $files = Join-Path $Root "FILES"
    $revision = Join-Path $Root "REVISION"
    if (-not (Test-Path -LiteralPath $files -PathType Leaf) -or -not (Test-Path -LiteralPath $revision -PathType Leaf)) {
        return ""
    }
    $name = ([string](Get-Content -LiteralPath $revision -TotalCount 1)).Trim()
    $filesDate = (Get-Item -LiteralPath $files).LastWriteTimeUtc.Ticks
    $revisionDate = (Get-Item -LiteralPath $revision).LastWriteTimeUtc.Ticks
    return "$name $filesDate $revisionDate"
}

# True when an archive was unpacked over $Root and its files are not in
# place yet (Get-UnpackStamp).
function Test-UnpackPending([string]$Root) {
    $stamp = Get-UnpackStamp $Root
    if (-not $stamp) { return $false }
    $record = Join-Path $Root "build\unpacked-archive"
    if (-not (Test-Path -LiteralPath $record -PathType Leaf)) { return $true }
    return ([string](Get-Content -LiteralPath $record -TotalCount 1)).Trim() -ne $stamp
}

# Records that the archive unpacked over $Root has its files in place.
function Set-UnpackStamp([string]$Root) {
    $stamp = Get-UnpackStamp $Root
    if (-not $stamp) { return }
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "build") | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $Root "build\unpacked-archive"), "$stamp`r`n")
}

# The revision the game in $Root was built from (install.ps1 writes it when a
# build ends well), or "".
function Get-BuiltRevision([string]$Root) {
    $record = Join-Path $Root "build\installed-revision"
    if (-not (Test-Path -LiteralPath $record -PathType Leaf)) { return "" }
    return ([string](Get-Content -LiteralPath $record -TotalCount 1)).Trim()
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
