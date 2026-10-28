# Connects standard input/output to a TCP port on this Windows machine.
#
# GDB runs in WSL, the emulator's GDB server on Windows. On Windows 10, WSL 2
# can't open connections to Windows (the firewall blocks them), but it can
# start Windows programs, so GDB uses this script as a pipe:
#   target remote | powershell.exe -NoProfile -ExecutionPolicy Bypass -File gdbbridge.ps1 3333
param([int]$Port = 3333, [string]$HostName = '127.0.0.1')

$client = New-Object System.Net.Sockets.TcpClient($HostName, $Port)
$client.NoDelay = $true
$net = $client.GetStream()
$stdin = [Console]::OpenStandardInput()
$stdout = [Console]::OpenStandardOutput()

# GDB to emulator in the background, emulator to GDB here
$null = $stdin.CopyToAsync($net)
$buffer = New-Object byte[] 65536
while ($true) {
    $n = $net.Read($buffer, 0, $buffer.Length)
    if ($n -le 0) { break }
    $stdout.Write($buffer, 0, $n)
    $stdout.Flush()
}
$client.Close()
