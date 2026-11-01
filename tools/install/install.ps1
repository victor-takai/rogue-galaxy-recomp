# Rogue Galaxy Recompiled: the installer behind Install.bat.
#
# Builds the game on this PC from the player's own disc image. It downloads
# pinned build tools into toolchain\ (nothing is installed in Windows, and
# each archive must match its SHA-256 below), asks for the disc image, and
# runs `python setup.py install`: verify the image, extract it, build the
# recompiler, recompile, build the game. Everything it writes stays in this
# folder; run it again after a failure and what is done is skipped.
#
#   Install.bat                         (or drop the .iso onto it)
#   Install.bat -Iso "D:\Rogue Galaxy (USA).iso"
#   Install.bat -UseToolsOnPath         (developers: the toolchain already on PATH)
#   Install.bat -Yes                    (no questions: a short path is used if needed,
#                                        no shortcut is made and the game is not started)
#   Install.bat -Updating               (from Update.bat: rebuilds what changed, no shortcut question)
#
# Windows PowerShell 5.1, so no newer syntax; plain ASCII, which 5.1 reads
# without a byte order mark.

param(
    [string]$Iso = "",
    [switch]$UseToolsOnPath,
    [switch]$Yes,
    [switch]$Updating
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$ToolDir = Join-Path $Root "toolchain"
$Downloads = Join-Path $ToolDir "downloads"
$System32 = Join-Path $env:SystemRoot "System32"

# The tools the build was tested with (docs/BUILDING.md). A new pin needs
# its archive's SHA-256 from the release page: GitHub shows each asset's.
$Tools = @(
    @{ Name = "git"; Label = "Git 2.56.0 (MinGit)"; Exe = "cmd\git.exe"; Ext = ".zip"; MB = 38
       Url = "https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.2/MinGit-2.56.0.2-64-bit.zip"
       Sha256 = "da35e72aa21c005a5a0d298cfbae110bc1609a815730ea0dde84b01a1b3cd3be" },
    @{ Name = "python"; Label = "Python 3.12.13"; Exe = "python.exe"; Ext = ".tar.gz"; MB = 44
       Url = "https://github.com/astral-sh/python-build-standalone/releases/download/20260807/cpython-3.12.13%2B20260807-x86_64-pc-windows-msvc-install_only.tar.gz"
       Sha256 = "6cf2be701aa7e9470454c9c86285c1bcc1832518d63e39c3e34e9d8ea1cbb99f" },
    @{ Name = "cmake"; Label = "CMake 3.31.12"; Exe = "bin\cmake.exe"; Ext = ".zip"; MB = 45
       Url = "https://github.com/Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-windows-x86_64.zip"
       Sha256 = "0c4baa40f28b3f8225eb3fdf6946c987b4fe901403b4eaf2fbbd9378100aaa0c" },
    @{ Name = "ninja"; Label = "Ninja 1.13.2"; Exe = "ninja.exe"; Ext = ".zip"; MB = 1
       Url = "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip"
       Sha256 = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65" },
    @{ Name = "llvm-mingw"; Label = "LLVM 22.1.8 (llvm-mingw 20260616)"; Exe = "bin\clang.exe"; Ext = ".zip"; MB = 179
       Url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-x86_64.zip"
       Sha256 = "b9b68a4d276e16fa25802aaba458e4638f64b3884c290aaccdc2d87083b6ca35" }
)

# The deepest path the build writes is 185 characters below this folder
# (in build\tools\_deps), and Windows tools stop at 259 in all.
$MaxRootLength = 60

function Write-Step([string]$Text) {
    Write-Host ""
    Write-Host "== $Text" -ForegroundColor Cyan
}

function Stop-Install([string]$Text) {
    Write-Host ""
    Write-Host $Text -ForegroundColor Red
    exit 1
}

function Confirm-Step([string]$Question) {
    if ($Yes) { return $true }
    $answer = Read-Host "$Question [Y/n]"
    return ($answer -eq "" -or $answer -match "^(y|yes|s|sim)$")
}

# Why this folder cannot hold the build, or "" when it can.
function Get-PathProblem([string]$Path) {
    if ($Path.StartsWith("\\")) { return "it is on a network share" }
    foreach ($cloud in @($env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial)) {
        if ($cloud -and ($Path + "\").StartsWith($cloud.TrimEnd("\") + "\", [StringComparison]::OrdinalIgnoreCase)) {
            return "it is inside OneDrive, which would upload the 10 GB the build writes"
        }
    }
    if ($Path.Length -gt $MaxRootLength) {
        return "its path is too long for the build tools ($($Path.Length) characters; at most $MaxRootLength)"
    }
    if ($Path -match "[^A-Za-z0-9_.:\\-]") {
        return "its path has spaces, accented letters or symbols, which the build tools mishandle"
    }
    return ""
}

function Get-Drive([string]$Path) {
    if ($Path -match "^[A-Za-z]:") { return $Path.Substring(0, 2) }
    return ""
}

# Offers a copy of this folder at a short path and continues the install
# there; returns only when the player says no.
function Move-ToShortPath([string]$Problem) {
    $drive = Get-Drive $Root
    if (-not $drive -or $Problem -match "OneDrive|network") { $drive = $env:SystemDrive }
    $target = "$drive\RogueGalaxy"
    Write-Host ""
    Write-Host "This folder cannot hold the game's build: $Problem." -ForegroundColor Yellow
    Write-Host "  $Root"
    $script = Join-Path $target "tools\install\install.ps1"
    if (Test-Path -LiteralPath $script) {
        if (-not (Confirm-Step "An earlier copy is in $target. Continue the install there?")) { return }
    } else {
        if ((Test-Path -LiteralPath $target) -and @(Get-ChildItem -LiteralPath $target -Force).Count -gt 0) {
            Stop-Install "Move this folder to a short path without spaces, such as $target (which already holds other files), and run Install.bat there."
        }
        if (-not (Confirm-Step "Copy it to $target and install there?")) { return }
        Write-Step "Copying to $target"
        & (Join-Path $System32 "robocopy.exe") $Root $target /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Host
        if ($LASTEXITCODE -ge 8) { Stop-Install "Copying to $target failed (robocopy code $LASTEXITCODE)." }
        Write-Host "Copied. The game will live in $target; this folder can be deleted afterwards."
    }
    $forward = @()
    if ($Iso) { $forward += @("-Iso", $Iso) }
    if ($UseToolsOnPath) { $forward += "-UseToolsOnPath" }
    if ($Yes) { $forward += "-Yes" }
    if ($Updating) { $forward += "-Updating" }
    & (Join-Path $System32 "WindowsPowerShell\v1.0\powershell.exe") -NoProfile -ExecutionPolicy Bypass -File $script @forward
    exit $LASTEXITCODE
}

# An antivirus looks at the files just written and holds them meanwhile:
# Windows then refuses to delete or rename them, or to rename the folder
# they are in, which stopped the first install from the internet (unpacking
# MinGit, 2026-10-27; tools are unpacked in place since). So each such step
# tries again for half a minute before it gives up.
function Invoke-Patiently([string]$What, [scriptblock]$Action, [switch]$Optional) {
    for ($try = 1; ; $try++) {
        try { & $Action; return }
        catch {
            if ($try -lt 60) { Start-Sleep -Milliseconds 500; continue }
            if ($Optional) { return }
            Stop-Install ("Windows did not let the installer $What ($($_.Exception.Message)) " +
                "An antivirus may still be looking at the files: wait a minute and run Install.bat again.")
        }
    }
}

function Get-Download([hashtable]$Tool, [string]$File) {
    $part = "$File.part"
    Invoke-Patiently "delete $part" { if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force } }
    $curl = Join-Path $System32 "curl.exe"
    if (Test-Path -LiteralPath $curl) {
        & $curl -L --fail --retry 3 --retry-delay 2 --progress-bar -o $part $Tool.Url
        if ($LASTEXITCODE -ne 0) { Stop-Install "Downloading $($Tool.Label) failed (curl code $LASTEXITCODE). Check the internet connection and run Install.bat again." }
    } else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $progress = $ProgressPreference
        $ProgressPreference = "SilentlyContinue"
        try { Invoke-WebRequest -Uri $Tool.Url -OutFile $part -UseBasicParsing }
        catch { Stop-Install "Downloading $($Tool.Label) failed: $($_.Exception.Message)" }
        finally { $ProgressPreference = $progress }
    }
    $hash = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $Tool.Sha256) {
        Invoke-Patiently "delete $part" { Remove-Item -LiteralPath $part -Force } -Optional
        Stop-Install "The download of $($Tool.Label) is not the expected file (its SHA-256 differs). Run Install.bat again; if this repeats, something on this network alters downloads."
    }
    Invoke-Patiently "rename $part" { Move-Item -LiteralPath $part -Destination $File -Force }
}

function Install-Tool([hashtable]$Tool) {
    $dir = Join-Path $ToolDir $Tool.Name
    $stamp = Join-Path $dir ".rg-sha256"
    if ((Test-Path -LiteralPath (Join-Path $dir $Tool.Exe)) -and (Test-Path -LiteralPath $stamp) -and
        ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $Tool.Sha256)) {
        Write-Host "  $($Tool.Label): ready"
        return
    }
    $file = Join-Path $Downloads ($Tool.Name + $Tool.Ext)
    $have = (Test-Path -LiteralPath $file) -and
        ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Tool.Sha256)
    if (-not $have) {
        Write-Host "  $($Tool.Label): downloading ($($Tool.MB) MB)"
        Get-Download $Tool $file
    }
    Write-Host "  $($Tool.Label): unpacking"
    # Python's archive is a .tar.gz, so tar.exe is needed in any case.
    $tar = Join-Path $System32 "tar.exe"
    if (-not (Test-Path -LiteralPath $tar)) {
        Stop-Install "This Windows has no tar.exe (Windows 10 version 1803 or later has it), needed to unpack $($Tool.Label)."
    }
    # Unpacked in place (see Invoke-Patiently). Archives that hold everything
    # in one folder give what is inside it.
    $names = @(& $tar -tf $file | ForEach-Object { $_ -replace '\\', '/' })
    if ($LASTEXITCODE -ne 0 -or $names.Count -eq 0) { Stop-Install "Unpacking $($Tool.Label) failed: its archive cannot be read." }
    $prefix = ""
    $strip = 0
    while ($true) {
        $deeper = @($names | Where-Object { $_.IndexOf("/", $prefix.Length) -ge 0 })
        if ($deeper.Count -eq 0) { break }
        $next = $deeper[0].Substring(0, $deeper[0].IndexOf("/", $prefix.Length) + 1)
        $outside = @($names | Where-Object { -not ($_ + "/").StartsWith($next) })
        if ($outside.Count -gt 0) { break }
        $prefix = $next
        $strip++
    }
    Invoke-Patiently "delete $dir" { if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force } }
    New-Item -ItemType Directory -Path $dir | Out-Null
    $options = @()
    if ($strip -gt 0) { $options = @("--strip-components=$strip") }
    & $tar -xf $file -C $dir @options
    if ($LASTEXITCODE -ne 0) { Stop-Install "Unpacking $($Tool.Label) failed. If an antivirus is running, it may be holding the files; run Install.bat again." }
    if (-not (Test-Path -LiteralPath (Join-Path $dir $Tool.Exe))) {
        Stop-Install ("$($Tool.Exe) is missing from $($Tool.Label) after unpacking. An antivirus may have removed it: " +
            "restore it from the antivirus's quarantine (or allow the folder $ToolDir) and run Install.bat again.")
    }
    Set-Content -LiteralPath $stamp -Value $Tool.Sha256 -Encoding Ascii
    Invoke-Patiently "delete $file" { Remove-Item -LiteralPath $file -Force } -Optional
}

# The build sees these tools and Windows only: compilers, Python or CMake
# settings installed for other projects change what it does.
function Set-BuildEnvironment {
    if (-not $UseToolsOnPath) {
        $env:PATH = (@(
            (Join-Path $ToolDir "llvm-mingw\bin"),
            (Join-Path $ToolDir "cmake\bin"),
            (Join-Path $ToolDir "ninja"),
            (Join-Path $ToolDir "python"),
            (Join-Path $ToolDir "git\cmd"),
            $System32,
            $env:SystemRoot,
            (Join-Path $System32 "Wbem"),
            (Join-Path $System32 "WindowsPowerShell\v1.0")
        ) -join ";")
    }
    foreach ($name in @("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CPATH", "C_INCLUDE_PATH",
                        "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "INCLUDE", "LIB", "CMAKE_GENERATOR",
                        "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET", "CMAKE_PREFIX_PATH",
                        "CMAKE_TOOLCHAIN_FILE", "PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "SDL3_DIR", "ZLIB_ROOT")) {
        Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    }
    $env:PYTHONUTF8 = "1"
    $env:PYTHONUNBUFFERED = "1"
    # One library's tests sit deeper than Windows' old path limit; the
    # dependency downloads check out at fixed tags.
    $env:GIT_CONFIG_COUNT = "3"
    $env:GIT_CONFIG_KEY_0 = "core.longpaths"
    $env:GIT_CONFIG_VALUE_0 = "true"
    $env:GIT_CONFIG_KEY_1 = "safe.directory"
    $env:GIT_CONFIG_VALUE_1 = "*"
    $env:GIT_CONFIG_KEY_2 = "advice.detachedHead"
    $env:GIT_CONFIG_VALUE_2 = "false"
}

function Select-DiscImage {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your Rogue Galaxy (USA) disc image"
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

function Main {
    try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }
    $version = "?"
    $project = Get-Content -LiteralPath (Join-Path $Root "CMakeLists.txt") -Raw -ErrorAction SilentlyContinue
    if ($project -match "VERSION\s+(\d+\.\d+\.\d+)") { $version = $Matches[1] }
    $Host.UI.RawUI.WindowTitle = "Rogue Galaxy Recompiled $version installer"
    Write-Host "Rogue Galaxy Recompiled $version installer" -ForegroundColor Green
    Write-Host "Builds the game on this PC from your own disc image. Everything goes into this folder:"
    Write-Host "  $Root"

    if (-not (Test-Path -LiteralPath (Join-Path $Root "setup.py"))) {
        Stop-Install "setup.py is not beside Install.bat. Extract the whole zip first, then run Install.bat from the extracted folder."
    }
    if (-not [Environment]::Is64BitOperatingSystem) { Stop-Install "The game needs 64-bit Windows 10 or later." }

    $problem = Get-PathProblem $Root
    if ($problem) {
        Move-ToShortPath $problem
        Stop-Install "Move this folder to a short path without spaces, such as C:\RogueGalaxy, and run Install.bat there."
    }

    $hashes = Get-Content -LiteralPath (Join-Path $Root "config\SCUS_974.90\hashes.json") -Raw | ConvertFrom-Json
    $extracted = (Test-Path -LiteralPath (Join-Path $Root "disc\manifest.json")) -and
        (Test-Path -LiteralPath (Join-Path $Root "disc\$($hashes.elf)"))
    $needGB = 9
    if (-not $extracted) { $needGB += 7 }
    $freeGB = [math]::Floor((New-Object System.IO.DriveInfo((Get-Drive $Root))).AvailableFreeSpace / 1GB)
    if ($freeGB -lt $needGB) {
        Stop-Install "The drive has $freeGB GB free; the install needs about $needGB GB. Free some space or move this folder to another drive."
    }

    if ($extracted -and $Iso) {
        Write-Host ""
        Write-Host "disc\ already holds the extracted disc, so $Iso is not used (delete disc\manifest.json to extract it instead)."
    }
    if (-not $extracted) {
        Write-Step "Your disc image"
        while ($true) {
            if (-not $Iso) {
                Write-Host "Choose the image of your Rogue Galaxy disc (USA, SCUS-97490) in the window that opens."
                $Iso = Select-DiscImage
                if (-not $Iso) { Stop-Install "No disc image chosen. Run Install.bat again when you have it." }
            }
            if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) { Stop-Install "There is no file $Iso." }
            $size = (Get-Item -LiteralPath $Iso).Length
            if ($size -eq $hashes.iso.size) { break }
            Write-Host ("$Iso is not the disc this project supports: the USA disc image is $($hashes.iso.size) bytes " +
                "(Redump's ""$($hashes.iso.name)""), this file is $size.") -ForegroundColor Yellow
            if ($Yes -or -not (Confirm-Step "Choose another file?")) { Stop-Install "No supported disc image chosen." }
            $Iso = ""
        }
        Write-Host "  $Iso"
    }

    Write-Step "Build tools"
    if ($UseToolsOnPath) {
        foreach ($name in @("git", "python", "cmake", "ninja", "clang", "clang++")) {
            if (-not (Get-Command $name -ErrorAction SilentlyContinue)) { Stop-Install "-UseToolsOnPath: $name is not on PATH." }
        }
        $python = (Get-Command python).Source
        # Windows' own python.exe may be the Store's stand-in.
        & $python -c "import sys; sys.exit(sys.version_info < (3, 11))"
        if ($LASTEXITCODE -ne 0) { Stop-Install "-UseToolsOnPath: $python is not Python 3.11 or later." }
        Write-Host "  using the tools on PATH"
    } else {
        Write-Host "Downloaded into $ToolDir; nothing is installed in Windows."
        New-Item -ItemType Directory -Force -Path $Downloads | Out-Null
        foreach ($tool in $Tools) { Install-Tool $tool }
        Invoke-Patiently "delete $Downloads" { if (Test-Path -LiteralPath $Downloads) { Remove-Item -LiteralPath $Downloads -Recurse -Force } } -Optional
        $python = Join-Path $ToolDir "python\python.exe"
    }
    Set-BuildEnvironment

    if (-not (Test-Path -LiteralPath (Join-Path $Root "thirdparty\PS2Recomp\CMakeLists.txt")) -or
        -not (Test-Path -LiteralPath (Join-Path $Root "thirdparty\imgui\imgui.h"))) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root ".git"))) {
            Stop-Install ("thirdparty\PS2Recomp or thirdparty\imgui is empty. GitHub's ""Source code"" download leaves them out: " +
                "download RogueGalaxy-$version.zip from the release page instead.")
        }
        Write-Step "The framework's source (git submodules)"
        & git -C $Root submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { Stop-Install "Fetching the git submodules failed." }
    }

    if ($Updating) {
        Write-Step "Rebuilding what changed"
        Write-Host "A few minutes when little changed; as long as the first install when the new version changed how the game's code is translated."
    } else {
        Write-Step "Building the game"
        Write-Host "This takes about 20 minutes on a fast PC, up to an hour on a slower one; it can be left alone."
    }
    Write-Host "Clicking inside this window pauses it (the title then starts with ""Select""); press Esc to go on."
    Write-Host "Everything shown here also goes to install.log."
    $log = Join-Path $Root "install.log"
    $arguments = @("-u", (Join-Path $Root "setup.py"), "install")
    if ($Iso -and -not $extracted) { $arguments += @("--iso", $Iso) }
    $writer = New-Object System.IO.StreamWriter($log, $false, (New-Object System.Text.UTF8Encoding($false)))
    $writer.AutoFlush = $true
    # Native programs' error output arrives as error records; with "Stop" the
    # first of them would end the script.
    $ErrorActionPreference = "Continue"
    Push-Location -LiteralPath $Root
    try {
        & $python @arguments 2>&1 | ForEach-Object {
            $line = "$_"
            $writer.WriteLine($line)
            Write-Host $line
        }
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
        $writer.Close()
    }
    $ErrorActionPreference = "Stop"
    $game = Join-Path $Root "Rogue Galaxy.exe"
    if ($code -ne 0 -or -not (Test-Path -LiteralPath $game)) {
        $hint = ""
        $text = Get-Content -LiteralPath $log -Raw
        if ($text -match "No such file or directory|is not recognized|Access is denied") {
            $hint = " A build tool went missing or was blocked: an antivirus may have quarantined it (restore it, or allow $ToolDir)."
        }
        Stop-Install "The install stopped (code $code).$hint The full output is in $log. Run Install.bat again to continue from where it stopped."
    }

    Write-Step "Done"
    Write-Host "The game is $game"
    if (-not $Yes -and -not $Updating -and (Confirm-Step "Put a shortcut to it on the desktop?")) {
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath("Desktop")) "Rogue Galaxy.lnk"))
        $link.TargetPath = $game
        $link.WorkingDirectory = $Root
        $link.IconLocation = "$game,0"
        $link.Save()
        Write-Host "  shortcut made"
    }
    if (-not $Yes -and (Confirm-Step "Start the game now?")) { Start-Process -FilePath $game -WorkingDirectory $Root }
}

# Dot-sourced (tests), only the functions.
if ($MyInvocation.InvocationName -ne ".") { Main }
