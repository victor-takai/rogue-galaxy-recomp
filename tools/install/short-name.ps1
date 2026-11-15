# Rogue Galaxy Recompiled: a short name for the game's folder, which
# install.ps1 builds through and update.ps1 writes through. Dot-sourced by
# both.
#
# The deepest path the build writes is 185 characters below the game's folder
# (build\tools\_deps\toml11-src\tests\...), and Windows programs stop at 259
# characters unless long paths are turned on in the registry, which they are
# not by default. So a folder whose path is longer than $ShortNameLimit, or
# has spaces, accented letters or symbols, is reached through a directory
# junction, C:\RogueGalaxyBuild\<8 hex digits> (on the drive Windows is on,
# C: but for a few), while the install or update
# runs: the build's tools see only that short, plain name, and the files
# stay in the folder. Making a junction needs no administrator. The digits
# come from the folder's path the first time, and the name is written into
# the folder's build\short-name.txt: the build's caches record the name, so
# the folder keeps it from then on, however its path is spelled (an 8.3
# name, another drive letter) and wherever it is moved. The junction is
# removed afterwards.
#
# Windows PowerShell 5.1, so no newer syntax; plain ASCII.

$ShortNameLimit = 60
$ShortNameRecord = "build\short-name.txt"

# The short name recorded in the folder $Path, or "".
function Get-RecordedShortName([string]$Path) {
    $record = Join-Path $Path $ShortNameRecord
    if (-not (Test-Path -LiteralPath $record -PathType Leaf)) { return "" }
    $name = ([string](Get-Content -LiteralPath $record -TotalCount 1)).Trim()
    if ($name -match '^[A-Za-z]:\\RogueGalaxyBuild\\[0-9a-f]{8}$') { return $name }
    return ""
}

# True when the build should see $Path through a short name: it was built
# through one, or its path is long or not plain.
function Test-NeedsShortName([string]$Path) {
    if (Get-RecordedShortName $Path) { return $true }
    return ($Path.Length -gt $ShortNameLimit) -or ($Path -match "[^A-Za-z0-9_.:\\-]")
}

# The recorded name, or else C:\RogueGalaxyBuild\ and the first 8 hex digits
# of the SHA-256 of $Path in lower case (Windows ignores case in paths).
function Get-ShortName([string]$Path) {
    $recorded = Get-RecordedShortName $Path
    if ($recorded) { return $recorded }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Path.TrimEnd("\").ToLowerInvariant()))
    } finally {
        $sha.Dispose()
    }
    $hex = -join @($hash[0..3] | ForEach-Object { $_.ToString("x2") })
    return "$env:SystemDrive\RogueGalaxyBuild\$hex"
}

# $Path's short name, made as a junction to it or kept when it is one
# already, and recorded. "" when Windows does not allow it (no folder can be
# made at the drive's root, say), something other than a junction is in the
# way, or the junction leads nowhere (to a network drive).
function Use-ShortName([string]$Path) {
    $link = Get-ShortName $Path
    try {
        if (Test-Path -LiteralPath $link) {
            $item = Get-Item -LiteralPath $link -Force
            if ($item.LinkType -ne "Junction") { return "" }
            # A junction to another folder, or to this one spelled otherwise
            # (left by a run that was closed, or by the folder before a move):
            # the name is this folder's now.
            if (([string]@($item.Target)[0]).TrimEnd("\") -ne $Path.TrimEnd("\")) { [System.IO.Directory]::Delete($link) }
        }
        # Twice: another folder's run may remove the empty parent
        # (Remove-ShortName) between the two steps.
        for ($try = 1; $try -le 2 -and -not (Test-Path -LiteralPath $link); $try++) {
            try {
                $parent = Split-Path -Parent $link
                if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
                New-Item -ItemType Junction -Path $link -Target $Path | Out-Null
            } catch {
                if ($try -ge 2) { throw }
            }
        }
        if (-not (Test-Path -LiteralPath (Join-Path $link "tools\install\short-name.ps1"))) {
            Remove-ShortName $link
            return ""
        }
        $record = Join-Path $Path $ShortNameRecord
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $record) | Out-Null
        Set-Content -LiteralPath $record -Value $link -Encoding Ascii
        return $link
    } catch {
        return ""
    }
}

# Removes the short name: the junction only, never the files it leads to
# (Directory.Delete without recursion removes a junction itself), then
# C:\RogueGalaxyBuild when nothing else is in it.
function Remove-ShortName([string]$Link) {
    if (-not $Link) { return }
    try {
        $item = Get-Item -LiteralPath $Link -Force -ErrorAction Stop
        if ($item.LinkType -eq "Junction") { [System.IO.Directory]::Delete($Link) }
    } catch { }
    try { [System.IO.Directory]::Delete((Split-Path -Parent $Link)) } catch { }
}
