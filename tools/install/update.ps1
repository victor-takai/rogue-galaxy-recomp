# Rogue Galaxy Recompiled: the updater behind Update.bat.
#
# Brings a folder that Install.bat built to the newest release and rebuilds
# what changed. It asks GitHub for the releases (one request, nothing sent
# but the request), downloads the newest RogueGalaxy-<version>.zip when it
# is newer than this folder's REVISION, writes over this folder only the
# files whose bytes differ (so the build that follows redoes just what
# depends on them), deletes from the code folders the files the new version
# no longer has, and runs install.ps1, which keeps the disc and the build
# tools it already has. The player's disc, saves, settings and textures are
# never touched.
#
#   Update.bat                          (or drop a downloaded RogueGalaxy-*.zip onto it)
#   Update.bat -Yes                     (no questions; the game is not started)
#
# Update.bat runs a copy of itself from the temp folder and passes the game's
# folder as -Root, since the update replaces the one in the folder.
# -NoBuild stops after the files are replaced (tests).
# Windows PowerShell 5.1, so no newer syntax; plain ASCII.

param(
    [string]$Root = "",
    [string]$Zip = "",
    [switch]$Yes,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Join-Path $PSScriptRoot "..\.." }
$Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd("\")
$System32 = Join-Path $env:SystemRoot "System32"
# Newest first, pre-releases included, as the game's own check does (0.1.0
# is one, which /releases/latest leaves out).
$ReleasesApi = "https://api.github.com/repos/victor-takai/rogue-galaxy-recomp/releases?per_page=10"
$ReleasePage = "https://github.com/victor-takai/rogue-galaxy-recomp/releases"
# The zip's files sit in this folder (tools/release/source_zip.py).
$ZipFolder = "RogueGalaxy/"
# Folders the build reads whole (src/*.cpp are globbed, every patch is
# applied): a file of an older version left in them would still be compiled
# or applied, so files the new version does not have are deleted there, and
# only there.
$CodeFolders = @("src", "patches", "config", "cmake", "tools", "docs", "debug", "thirdparty")
# What the zip never holds and the update never writes.
$NeverWrite = @("disc", "build", "toolchain", "mc0", "mc1", "textures")

function Write-Step([string]$Text) {
    Write-Host ""
    Write-Host "== $Text" -ForegroundColor Cyan
}

function Stop-Update([string]$Text) {
    Write-Host ""
    Write-Host $Text -ForegroundColor Red
    exit 1
}

function Confirm-Step([string]$Question) {
    if ($Yes) { return $true }
    $answer = Read-Host "$Question [Y/n]"
    return ($answer -eq "" -or $answer -match "^(y|yes|s|sim)$")
}

# "v0.2.0", "0.2.0" and "v0.1.0-12-gabcdef0" alike: the numbers in front.
function Get-VersionNumbers([string]$Version) {
    $m = [regex]::Match($Version, "^[vV]?(\d+(\.\d+)*)")
    if (-not $m.Success) { return @() }
    return @($m.Groups[1].Value.Split(".") | ForEach-Object { [int]$_ })
}

# 1 when A is newer than B, -1 when older, 0 when the same or unreadable.
function Compare-Versions([string]$Left, [string]$Right) {
    # (Variable names ignore case: a local $a would be the parameter $A.)
    $one = @(Get-VersionNumbers $Left)
    $two = @(Get-VersionNumbers $Right)
    if ($one.Count -eq 0 -or $two.Count -eq 0) { return 0 }
    $n = [math]::Max($one.Count, $two.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $x = 0
        $y = 0
        if ($i -lt $one.Count) { $x = $one[$i] }
        if ($i -lt $two.Count) { $y = $two[$i] }
        if ($x -gt $y) { return 1 }
        if ($x -lt $y) { return -1 }
    }
    return 0
}

function Get-RunningGame {
    return @(Get-Process -Name "roguegalaxy", "Rogue Galaxy" -ErrorAction SilentlyContinue)
}

# The newest release GitHub lists with a RogueGalaxy-<version>.zip: its tag
# and that asset (GitHub gives the asset's SHA-256 as "sha256:<hex>").
function Get-NewestRelease {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try {
        $releases = Invoke-RestMethod -Uri $ReleasesApi -Headers @{ Accept = "application/vnd.github+json" } `
            -UserAgent "RogueGalaxyRecompiled-Update" -UseBasicParsing
    } catch {
        Stop-Update ("Could not ask GitHub for the releases: $($_.Exception.Message) Check the internet connection, " +
            "or download the zip from $ReleasePage and drop it onto Update.bat.")
    }
    foreach ($release in @($releases)) {
        if ($release.draft) { continue }
        $asset = @($release.assets | Where-Object { $_.name -match "^RogueGalaxy-.*\.zip$" }) | Select-Object -First 1
        if (-not $asset) { continue }
        $sha = ""
        if ($asset.digest -and ($asset.digest -match "^sha256:([0-9a-fA-F]{64})$")) { $sha = $Matches[1].ToLowerInvariant() }
        return @{ Tag = [string]$release.tag_name; Name = [string]$asset.name; Url = [string]$asset.browser_download_url
                  Size = [long]$asset.size; Sha256 = $sha }
    }
    Stop-Update "GitHub lists no release with a RogueGalaxy-<version>.zip. See $ReleasePage."
}

# Downloads $Url to $File, checking the SHA-256 when one is known.
function Get-Download([string]$Url, [string]$File, [string]$Sha256, [string]$Label) {
    $part = "$File.part"
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
    $curl = Join-Path $System32 "curl.exe"
    if (Test-Path -LiteralPath $curl) {
        & $curl -L --fail --retry 3 --retry-delay 2 --progress-bar -o $part $Url
        if ($LASTEXITCODE -ne 0) {
            Stop-Update "Downloading $Label failed (curl code $LASTEXITCODE). Check the internet connection and run Update.bat again."
        }
    } else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $progress = $ProgressPreference
        $ProgressPreference = "SilentlyContinue"
        try { Invoke-WebRequest -Uri $Url -OutFile $part -UseBasicParsing }
        catch { Stop-Update "Downloading $Label failed: $($_.Exception.Message)" }
        finally { $ProgressPreference = $progress }
    }
    if ($Sha256) {
        $hash = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $Sha256) {
            Remove-Item -LiteralPath $part -Force
            Stop-Update ("The download of $Label is not the file GitHub lists (its SHA-256 differs). Run Update.bat again; " +
                "if this repeats, something on this network alters downloads.")
        }
    }
    Move-Item -LiteralPath $part -Destination $File -Force
}

# The first line of RogueGalaxy/REVISION inside the zip, or "".
function Get-ZipRevision([System.IO.Compression.ZipArchive]$Archive) {
    $entry = $Archive.GetEntry($ZipFolder + "REVISION")
    if (-not $entry) { return "" }
    $reader = New-Object System.IO.StreamReader($entry.Open())
    try { return ([string]$reader.ReadLine()).Trim() } finally { $reader.Dispose() }
}

# An entry's bytes (the comma keeps the array whole on the way out).
function Read-Entry([System.IO.Compression.ZipArchiveEntry]$Entry) {
    $stream = $Entry.Open()
    $memory = New-Object System.IO.MemoryStream
    try {
        $stream.CopyTo($memory)
        return ,$memory.ToArray()
    } finally {
        $memory.Dispose()
        $stream.Dispose()
    }
}

function Test-SameBytes([string]$Path, [byte[]]$Data) {
    if ((Get-Item -LiteralPath $Path).Length -ne $Data.Length) { return $false }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $have = [Convert]::ToBase64String($sha.ComputeHash([System.IO.File]::ReadAllBytes($Path)))
        $want = [Convert]::ToBase64String($sha.ComputeHash($Data))
        return $have -eq $want
    } finally {
        $sha.Dispose()
    }
}

# Writes the zip's files over $Root where their bytes differ. Returns the
# zip's revision, the relative paths it holds (lower-case keys) and how many
# files were written.
function Apply-Zip([string]$ZipPath) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $revision = Get-ZipRevision $archive
        if (-not $revision) { Stop-Update "$ZipPath is not a release of this project: it holds no $($ZipFolder)REVISION." }
        $delivered = @{}
        $written = 0
        foreach ($entry in $archive.Entries) {
            if (-not $entry.FullName.StartsWith($ZipFolder) -or $entry.FullName.EndsWith("/")) { continue }
            $relative = $entry.FullName.Substring($ZipFolder.Length).Replace("/", "\")
            $parts = $relative.Split("\")
            if ($relative -eq "" -or ($parts -contains "..") -or ($relative -match "^[A-Za-z]:|^\\")) {
                Stop-Update "$ZipPath holds a path outside the folder ($($entry.FullName)); not a release of this project."
            }
            if ($NeverWrite -contains $parts[0]) { continue }
            $delivered[$relative.ToLowerInvariant()] = $true
            $target = Join-Path $Root $relative
            $data = Read-Entry $entry
            if ((Test-Path -LiteralPath $target -PathType Leaf) -and (Test-SameBytes $target $data)) { continue }
            $dir = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
            [System.IO.File]::WriteAllBytes($target, $data)
            $written++
        }
        return @{ Revision = $revision; Delivered = $delivered; Written = $written }
    } finally {
        $archive.Dispose()
    }
}

# Deletes from the code folders the files the zip did not deliver; returns
# their relative paths. Python's caches and the patch stamp stay.
function Remove-Stale([hashtable]$Delivered) {
    $removed = @()
    foreach ($folder in $CodeFolders) {
        $dir = Join-Path $Root $folder
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        foreach ($file in @(Get-ChildItem -LiteralPath $dir -File -Recurse -Force)) {
            $relative = $file.FullName.Substring($Root.Length + 1)
            if ($Delivered.ContainsKey($relative.ToLowerInvariant())) { continue }
            if ($relative -match "\\__pycache__\\|\.pyc$|\\\.rg-patches-applied$") { continue }
            Remove-Item -LiteralPath $file.FullName -Force
            $removed += $relative
        }
    }
    return $removed
}

function Main {
    try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }
    try { Start-Transcript -LiteralPath (Join-Path $Root "update.log") -Force | Out-Null } catch { }
    Write-Host "Rogue Galaxy Recompiled: update" -ForegroundColor Green
    Write-Host "Brings this folder to the newest release and rebuilds what changed:"
    Write-Host "  $Root"

    if (-not (Test-Path -LiteralPath (Join-Path $Root "tools\install\install.ps1"))) {
        Stop-Update "$Root is not the game's folder: it has no tools\install\install.ps1."
    }
    if (Test-Path -LiteralPath (Join-Path $Root ".git")) {
        Stop-Update "This folder is a git checkout: update it with git pull, then Install.bat (or python setup.py install)."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Root "disc\manifest.json"))) {
        Stop-Update "The game is not installed here yet. Run Install.bat first."
    }
    $current = "unknown"
    $revisionFile = Join-Path $Root "REVISION"
    if (Test-Path -LiteralPath $revisionFile) {
        $current = ([string](Get-Content -LiteralPath $revisionFile -TotalCount 1)).Trim()
    }
    Write-Host "  this folder: $current"

    Write-Step "The newest release"
    if ($Zip) {
        if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { Stop-Update "There is no file $Zip." }
        $zipPath = (Resolve-Path -LiteralPath $Zip).Path
        Write-Host "  from $zipPath"
    } else {
        $release = Get-NewestRelease
        Write-Host ("  {0}: {1} ({2:N1} MB)" -f $release.Tag, $release.Name, ($release.Size / 1MB))
        if ((Compare-Versions $release.Tag $current) -le 0) {
            Write-Host ""
            Write-Host "This folder is already at $current; nothing to update." -ForegroundColor Green
            exit 0
        }
        $downloads = Join-Path $Root "build\update"
        New-Item -ItemType Directory -Force -Path $downloads | Out-Null
        $zipPath = Join-Path $downloads $release.Name
        if ((Test-Path -LiteralPath $zipPath) -and $release.Sha256 -and
            ((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant() -eq $release.Sha256)) {
            Write-Host "  already downloaded"
        } else {
            Write-Host "  downloading"
            Get-Download $release.Url $zipPath $release.Sha256 $release.Name
        }
    }

    while ((Get-RunningGame).Count -gt 0) {
        if ($Yes) { Stop-Update "The game is running; close it and run Update.bat again." }
        Read-Host "The game is running. Close it, then press Enter"
    }

    Write-Step "Replacing what changed"
    $result = Apply-Zip $zipPath
    Write-Host "  $($result.Revision): $($result.Written) file(s) written"
    $removed = @(Remove-Stale $result.Delivered)
    if ($removed.Count -gt 0) {
        Write-Host "  $($removed.Count) file(s) the new version no longer has, removed:"
        foreach ($name in $removed) { Write-Host "    $name" }
    }
    if ($NoBuild) {
        Write-Host "  (-NoBuild: not rebuilding)"
        exit 0
    }

    Write-Step "Rebuilding"
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $Root "tools\install\install.ps1"), "-Updating")
    if ($Yes) { $arguments += "-Yes" }
    & (Join-Path $System32 "WindowsPowerShell\v1.0\powershell.exe") @arguments
    if ($LASTEXITCODE -ne 0) {
        Stop-Update "The rebuild stopped (code $LASTEXITCODE); install.log has the details. Run Update.bat again to continue from where it stopped."
    }
    Write-Host ""
    Write-Host "Updated from $current to $($result.Revision)." -ForegroundColor Green
}

# Dot-sourced (tests), only the functions.
if ($MyInvocation.InvocationName -ne ".") { Main }
