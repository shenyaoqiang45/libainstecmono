<#
.SYNOPSIS
    Adds inbound firewall rules for the ainstec_ros python.exe.

.DESCRIPTION
    GigE Vision cameras answer discovery broadcasts and stream data over
    unsolicited inbound UDP (and the control channel over TCP) to the host
    process. Without an inbound allow rule, Windows Firewall silently drops
    that traffic and the camera never appears / never streams.

    This adds INBOUND UDP + TCP allow rules for the conda env's python.exe
    (the ROS 2 camera node process) under the name "Ainstec ROS2 Camera Node".

    Must be run as Administrator:
        powershell -ExecutionPolicy Bypass -File scripts\setup_firewall.ps1
#>
[CmdletBinding()]
param(
    [string]$EnvName = 'ainstec_ros'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- admin check ------------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This script must be run from an elevated (Administrator) PowerShell.'
}

# --- locate the env's python.exe --------------------------------------------
$python = "F:\ProgramData\miniconda3\envs\$EnvName\python.exe"
if (-not (Test-Path $python)) {
    $condaCmd = Get-Command conda.exe -ErrorAction SilentlyContinue
    if ($condaCmd) {
        $condaBase = ((& $condaCmd.Source info --base) | Out-String).Trim()
        $python = Join-Path $condaBase "envs\$EnvName\python.exe"
    }
}
if (-not (Test-Path $python)) {
    throw "env python.exe not found for '$EnvName' - run scripts\setup_ros_env.ps1 first."
}

# --- rules ------------------------------------------------------------------
# GigE discovery/streaming needs inbound UDP; the GigE control channel needs
# inbound TCP. Both are scoped to the node process only.
foreach ($proto in @('UDP', 'TCP')) {
    Write-Host "adding inbound $proto rule for $python ..."
    netsh advfirewall firewall add rule name="Ainstec ROS2 Camera Node" dir=in action=allow program="$python" protocol=$proto enable=yes | Out-Host
}

Write-Host 'Done. Verify with: netsh advfirewall firewall show rule name="Ainstec ROS2 Camera Node"'
