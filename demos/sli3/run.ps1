# PS1 SLI on two, three or more screens through the hub (run by run.bat).
#
#   run.bat                            every cartridge connected, in the order ps1transfer -list gives them
#   run.bat A10PVL53 A10PUCVT          these cartridges, from left to right
#   run.bat A10PVL53 A10PUCVT redux    and PCSX-Redux as the third screen (on the right)
#
# The hub (sli_hub.py) opens in its own window, then each screen: a cartridge gets sli.ps-exe ("-link usb"), its place
# (0x801BF000) and its bridge to the hub in a window of its own ("ps1transfer -net"); "redux" starts PCSX-Redux with
# its serial port bridged to the hub (redux.ps1). Close the windows to stop.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Screens)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$T = Join-Path $here '..\..\ps1transfer\ps1transfer.exe'
$game = Join-Path $here '..\sli\sli.ps-exe'      # (the PS1 SLI demo's program)

if (-not $Screens) {
    $Screens = @(& $T -list | Select-String '^-z \d+\s+-n (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
}
if ($Screens.Count -lt 2) {
    Write-Host 'Two screens at least: cartridges connected to this computer, or "redux" for PCSX-Redux.'
    exit 1
}

Start-Process -FilePath 'python' -ArgumentList @('sli_hub.py', '--screens', $Screens.Count) -WorkingDirectory $here
Start-Sleep -Seconds 1
for ($i = 0; $i -lt $Screens.Count; $i++) {
    $name = $Screens[$i]
    if ($name -eq 'redux') {
        Start-Process -FilePath 'powershell' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            "`"$(Join-Path $here 'redux.ps1')`"") -WorkingDirectory $here
        Write-Host "Screen $($i + 1): PCSX-Redux (the hub gives it the place left)"
        continue
    }
    & $T -n $name -link usb -f $game | Out-Null
    & $T -n $name -poke 0x801BF000 (0x534C4900 + $i) -s 4 | Out-Null
    Start-Process -FilePath $T -ArgumentList @('-n', $name, '-net', '127.0.0.1:7040') -WorkingDirectory $here
    Write-Host "Screen $($i + 1): $name"
}
Write-Host 'The screens start together once all of them are connected (see the hub window).'
