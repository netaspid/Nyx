# Basic rendezvous pen-test (run against local or VDS server)
param(
    [string]$Host = "127.0.0.1",
    [int]$Port = 3478,
    [int]$FloodCount = 500
)

$ErrorActionPreference = "Stop"
Write-Host "Pen-test rendezvous at ${Host}:${Port} (UDP flood $FloodCount)..."

# Requires nyx-tests or custom - use raw UDP via .NET
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Connect($Host, $Port)

# Minimal invalid Nyx frame (magic only)
$magic = [byte[]](0x4E,0x59,0x58,0x31, 1, 0x30, 0,0, 0,0,0,0, 0,0,0,0, 0,0)
for ($i = 0; $i -lt $FloodCount; $i++) {
    [void]$udp.Send($magic, $magic.Length)
}
$udp.Close()
Write-Host "Sent $FloodCount datagrams. Verify server still responds (GUI probe / nyx-node connect)."
Write-Host "See docs/SECURITY_AUDIT.md"
