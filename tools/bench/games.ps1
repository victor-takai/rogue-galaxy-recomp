# The games running on this machine, for tools/bench/walk.sh: a line for
# each, its kind, process id and executable, tab-separated. The kinds:
#   player  the player's: "Rogue Galaxy.exe" itself; a game it or a debug\
#           batch file started (its parent is the launcher or cmd.exe, which
#           wait for it); a game whose starter has ended and that names the
#           disc as they do, disc\SCUS_974.90 (scripted runs pass
#           disc/SCUS_974.90); a roguegalaxy*.exe with no disc argument (it
#           finds disc\ by itself, as a shortcut to it starts it); or one
#           whose command line cannot be read (an elevated one)
#   here    a scripted run of this folder's (its executable under
#           FOLDER\build\, where EXE= copies live too): it would share
#           build\run with this one
#   other   a scripted run of another folder's (another session's)
# A game is a process whose first argument is the disc (...SCUS_974.90), so
# a tool that names the disc further on (mipsdis.py, a shell's -c line) is
# none. SKIP is a process id left out (the run's own game). The last line
# is "end", so a caller tells an empty list from a script that failed.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\games.ps1 -Folder FOLDER [-Skip ID]
param([Parameter(Mandatory = $true)][string]$Folder, [int]$Skip = 0)

$processes = @(Get-CimInstance Win32_Process)
$byId = @{}
foreach ($process in $processes) { $byId[[int]$process.ProcessId] = $process }
$builds = $Folder.TrimEnd('\') + '\build\'
$disc = '^\s*("[^"]*"|\S+)\s+("[^"]*SCUS_974\.90"|\S*SCUS_974\.90)(\s|$)'

foreach ($process in $processes) {
    if ($process.ProcessId -eq $Skip -or $process.ProcessId -eq $PID) { continue }
    $line = $process.CommandLine
    $game = $process.Name -like 'roguegalaxy*'
    $kind = $null
    if ($process.Name -like 'Rogue Galaxy*.exe') {
        $kind = 'player'
    } elseif ($null -ne $line -and $line -match $disc) {
        $argument = $Matches[2]
        # The parent by its id, if it is still the process that started this
        # one (an id is reused once its process has ended).
        $parent = $byId[[int]$process.ParentProcessId]
        if ($parent -and $parent.CreationDate -le $process.CreationDate) {
            $player = $parent.Name -like 'Rogue Galaxy*.exe' -or $parent.Name -eq 'cmd.exe'
        } else {
            $player = $argument.Contains('\')
        }
        $path = $process.ExecutablePath
        if ($player) {
            $kind = 'player'
        } elseif ($path -and $path.StartsWith($builds, [StringComparison]::OrdinalIgnoreCase)) {
            $kind = 'here'
        } else {
            $kind = 'other'
        }
    } elseif ($game) {
        # No disc argument, or no command line to read.
        $kind = 'player'
    }
    if ($kind) {
        $where = $process.ExecutablePath
        if (-not $where) { $where = $process.Name }
        "$kind`t$($process.ProcessId)`t$where"
    }
}
'end'
