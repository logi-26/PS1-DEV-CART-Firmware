# PS1 SLI: PCSX-Redux as one of the screens (started by run.bat with "redux" among the screens).
#
# PCSX-Redux's serial port (SIO1) listens on a local port, in raw mode (the program's bytes as they are); this script
# is the bridge between that port and the hub (sli_hub.py), as ps1transfer -net is for a console. PCSX-Redux runs
# with its own settings (.redux\ next to this script), made from your usual ones (BIOS, controller...) with the serial
# port turned on: your own settings aren't changed.
#
#   redux.ps1 [-Redux C:\path\to\pcsx-redux.exe] [-Server host:port] [-Gdb 3333]
param(
    [string]$Redux = $env:PCSX_REDUX,
    [string]$Server = '127.0.0.1:7040',
    [int]$Port = 6722,          # Local port of PCSX-Redux's serial port
    [int]$Gdb = 0               # PCSX-Redux's GDB server on this port (to debug the program), 0: off
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$exe = Join-Path $here '..\sli\sli.ps-exe'      # (the PS1 SLI demo's program)

# PCSX-Redux: -Redux, PCSX_REDUX, on the PATH, or the copy of the PSX.Dev extension for VS Code
if (-not $Redux) {
    $cmd = Get-Command pcsx-redux.exe -ErrorAction SilentlyContinue
    if ($cmd) { $Redux = $cmd.Source }
}
if (-not $Redux) {
    $vscode = Join-Path $env:APPDATA 'Code\User\globalStorage\grumpycoders.psx-dev\pcsx-redux\pcsx-redux.exe'
    if (Test-Path $vscode) { $Redux = $vscode }
}
if (-not $Redux -or -not (Test-Path $Redux)) {
    Write-Host 'PCSX-Redux not found. Install it (https://pcsx-redux.consoledev.net), or set PCSX_REDUX to its'
    Write-Host 'pcsx-redux.exe, then start run.bat again.'
    exit 1
}

# Its settings: your usual ones (next to the emulator when portable, else in AppData), with the serial port on
$base = Join-Path (Split-Path $Redux) 'pcsx.json'
if (-not (Test-Path $base)) { $base = Join-Path $env:APPDATA 'pcsx-redux\pcsx.json' }
$settings = if (Test-Path $base) { Get-Content $base -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }

function Set-Field($object, [string]$name, $value) {
    if ($object.PSObject.Properties[$name]) { $object.$name = $value }
    else { $object | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}
function Get-Section($object, [string]$name) {
    if (-not $object.PSObject.Properties[$name]) { Set-Field $object $name ([pscustomobject]@{}) }
    return $object.$name
}
$emu = Get-Section $settings 'emulator'
Set-Field $emu 'Dynarec' (-not $Gdb)       # Full speed (the interpreter: about 45 frames a second, the others wait)
$debug = Get-Section $emu 'Debug'
Set-Field $debug 'GdbServer' ($Gdb -ne 0)
Set-Field $debug 'SIO1Server' $true
Set-Field $debug 'SIO1ServerPort' $Port
Set-Field $debug 'SIO1Client' $false
Set-Field $debug 'SIO1Mode' 1                   # Raw: the bytes as they are
$dir = Join-Path $here '.redux'
New-Item -ItemType Directory -Force $dir | Out-Null
[IO.File]::WriteAllText((Join-Path $dir 'pcsx.json'), ($settings | ConvertTo-Json -Depth 32))

$arguments = @('-portable', "`"$dir`"", '-loadexe', "`"$exe`"", '-run')
if ($Gdb) { $arguments += @('-gdb', '-gdb-port', "$Gdb") }
$emulator = Start-Process -FilePath $Redux -ArgumentList $arguments -WorkingDirectory $dir -PassThru
Write-Host "PCSX-Redux started (close it to stop)."

# The bridge: the emulator's serial port <-> the hub
$hostName, $serverPort = $Server -split ':'
$game = $null
for ($i = 0; ($i -lt 40) -and -not $game; $i++) {     # (until PCSX-Redux listens)
    try { $game = New-Object Net.Sockets.TcpClient('127.0.0.1', $Port) } catch { Start-Sleep -Milliseconds 500 }
}
if (-not $game) { Write-Host 'The emulator''s serial port does not answer.'; exit 1 }
try { $net = New-Object Net.Sockets.TcpClient($hostName, [int]$serverPort) }
catch { Write-Host "Can't connect to the hub $Server (is it running?)."; $game.Close(); exit 1 }
$game.NoDelay = $true
$net.NoDelay = $true
Write-Host "Connected to the hub $Server."

$a = $game.GetStream()
$b = $net.GetStream()
$buffer = New-Object byte[] 16384
function Closed($client) {
    $s = $client.Client
    return $s.Poll(0, [Net.Sockets.SelectMode]::SelectRead) -and ($s.Available -eq 0)
}
while (-not $emulator.HasExited) {
    $moved = $false
    if ($game.Available -gt 0) { $n = $a.Read($buffer, 0, $buffer.Length); $b.Write($buffer, 0, $n); $moved = $true }
    if ($net.Available -gt 0) { $n = $b.Read($buffer, 0, $buffer.Length); $a.Write($buffer, 0, $n); $moved = $true }
    if (-not $moved) {
        if ((Closed $net) -or (Closed $game)) { break }
        Start-Sleep -Milliseconds 2
    }
}
$game.Close()
$net.Close()
Write-Host 'Disconnected.'
