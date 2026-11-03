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
# The zip holds the framework as upstream has it, and the build applies
# patches/ps2recomp to it (cmake/framework_patches.cmake), so on disk its
# files are the patched ones. They are compared as the build will have them:
# compared as the zip has them, every update wrote the patched files back,
# the build applied the patches again, and the new dates rebuilt the
# recompiler, the runtime and all the generated code (15 minutes for one
# changed .cpp, 2026-11-03).
$FrameworkFolder = "thirdparty\PS2Recomp\"
$PatchFolder = "patches/ps2recomp/"
# The patches the framework holds, one name a line (framework_patches.cmake).
$PatchStamp = ".rg-patches-applied"

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

# This folder's game, if it runs. Another copy (a second install, a
# developer's worktree) may go on running: it uses none of these files. One
# whose path cannot be read counts, to be safe.
function Get-RunningGame {
    return @(Get-Process -Name "roguegalaxy", "Rogue Galaxy" -ErrorAction SilentlyContinue | Where-Object {
        $path = $null
        try { $path = $_.Path } catch { }
        (-not $path) -or $path.StartsWith($Root + "\", [StringComparison]::OrdinalIgnoreCase)
    })
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

# Writes $Data to $Relative under $Root unless the file there holds those
# bytes already; true when it wrote.
function Write-IfChanged([string]$Relative, [byte[]]$Data) {
    $target = Join-Path $Root $Relative
    if ((Test-Path -LiteralPath $target -PathType Leaf) -and (Test-SameBytes $target $Data)) { return $false }
    $dir = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllBytes($target, $Data)
    return $true
}

# The toolchain's git (Install.bat downloads it), or one on PATH.
function Find-Git {
    $own = Join-Path $Root "toolchain\git\cmd\git.exe"
    if (Test-Path -LiteralPath $own) { return $own }
    $onPath = @(Get-Command "git.exe" -ErrorAction SilentlyContinue) | Select-Object -First 1
    if ($onPath) { return $onPath.Path }
    return ""
}

# The zip's framework with the zip's patches applied, in build\update, as
# the build will have it: relative path (from $Root) -> bytes, the patch
# stamp among them. $null when it cannot be made (no git, a patch that does
# not apply); the zip's files are then written as they are, and the build
# applies the patches again.
function Get-PatchedFramework([System.IO.Compression.ZipArchive]$Archive) {
    $git = Find-Git
    if (-not $git) {
        Write-Host "  no git found: the framework is written as the zip has it, and the build patches it again"
        return $null
    }
    $prefix = $ZipFolder + $FrameworkFolder.Replace("\", "/")
    $patchPrefix = $ZipFolder + $PatchFolder
    # Inside the folder, whose path Install.bat keeps short, for the
    # framework's deepest paths.
    $stage = Join-Path $Root "build\update\framework"
    $tree = Join-Path $stage "PS2Recomp"
    $patchDir = Join-Path $stage "patches"
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    try {
        New-Item -ItemType Directory -Force -Path $tree, $patchDir | Out-Null
        $patches = @()
        foreach ($entry in $Archive.Entries) {
            if ($entry.FullName.EndsWith("/")) { continue }
            if ($entry.FullName.StartsWith($prefix)) {
                $target = Join-Path $tree $entry.FullName.Substring($prefix.Length).Replace("/", "\")
                $dir = Split-Path -Parent $target
                if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
                $data = Read-Entry $entry
                [System.IO.File]::WriteAllBytes($target, $data)
            } elseif ($entry.FullName.StartsWith($patchPrefix) -and $entry.Name.EndsWith(".patch") -and
                      ($entry.FullName.Substring($patchPrefix.Length) -eq $entry.Name)) {
                $data = Read-Entry $entry
                [System.IO.File]::WriteAllBytes((Join-Path $patchDir $entry.Name), $data)
                $patches += $entry.Name
            }
        }
        # In the build's order: list(SORT), by name, byte by byte.
        $names = [string[]]@($patches)
        [Array]::Sort($names, [StringComparer]::Ordinal)
        foreach ($name in $names) {
            # Its messages are no error of this script's (Windows PowerShell
            # turns a native program's stderr into errors when redirected).
            $previous = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                & $git -C $tree apply (Join-Path $patchDir $name) 2>$null | Out-Null
                $applied = ($LASTEXITCODE -eq 0)
            } catch {
                $applied = $false
            } finally {
                $ErrorActionPreference = $previous
            }
            if (-not $applied) {
                Write-Host "  $name does not apply to the zip's framework: it is written as the zip has it, and the build patches it again"
                return $null
            }
        }
        $files = @{}
        foreach ($file in @(Get-ChildItem -LiteralPath $tree -File -Recurse -Force)) {
            $files[$FrameworkFolder + $file.FullName.Substring($tree.Length + 1)] = [System.IO.File]::ReadAllBytes($file.FullName)
        }
        # As CMake's file(APPEND) writes it on Windows: CRLF.
        $stamp = ($names | ForEach-Object { "$_`r`n" }) -join ""
        $files[$FrameworkFolder + $PatchStamp] = [System.Text.Encoding]::ASCII.GetBytes($stamp)
        return $files
    } finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    }
}

# Writes the zip's files over $Root where their bytes differ (the framework's
# as patched). Returns the zip's revision, the relative paths it holds
# (lower-case keys) and how many files were written.
function Apply-Zip([string]$ZipPath) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $revision = Get-ZipRevision $archive
        if (-not $revision) { Stop-Update "$ZipPath is not a release of this project: it holds no $($ZipFolder)REVISION." }
        $framework = Get-PatchedFramework $archive
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
            # The framework's files come from the patched copy, below.
            if ($framework -and $relative.StartsWith($FrameworkFolder, [StringComparison]::OrdinalIgnoreCase)) { continue }
            $delivered[$relative.ToLowerInvariant()] = $true
            $data = Read-Entry $entry
            if (Write-IfChanged $relative $data) { $written++ }
        }
        if ($framework) {
            foreach ($relative in @($framework.Keys)) {
                $delivered[$relative.ToLowerInvariant()] = $true
                if (Write-IfChanged $relative $framework[$relative]) { $written++ }
            }
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
