<#
.SYNOPSIS
    Creates the 'ainstec_ros' conda environment with ROS 2 Jazzy from RoboStack.

.DESCRIPTION
    - Locates conda (F:\ProgramData\miniconda3 first, then PATH).
    - Creates env 'ainstec_ros' from conda-forge + robostack-staging +
      robostack-jazzy (the channel that carries the win-64 jazzy packages) with:
        python=3.11 ros-jazzy-ros-base colcon-common-extensions numpy pybind11
        pytest ros-jazzy-rosbridge-suite
      (python 3.11 matches the prebuilt pyainstec.cp311 pyd in install/python.)
    - rosbridge: the binary conda package is the primary path; afterwards a
      tiny Windows fixup drops .bat launch shims into the env so
      rosbridge_websocket_launch.xml can resolve its entry points.
    - Idempotent: an existing env is left as-is (create is skipped).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\setup_ros_env.ps1
#>
[CmdletBinding()]
param(
    [string]$EnvName = 'ainstec_ros'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-Conda {
    $candidates = @('F:\ProgramData\miniconda3\Scripts\conda.exe')
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    $cmd = Get-Command conda.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $cmd = Get-Command conda.bat -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw 'conda not found (looked in F:\ProgramData\miniconda3 and on PATH).'
}

$conda     = Find-Conda
$condaBase = ((& $conda info --base) | Out-String).Trim()
$envDir    = Join-Path $condaBase "envs\$EnvName"
$envPython = Join-Path $envDir 'python.exe'
$repoRoot  = Split-Path $PSScriptRoot -Parent

Write-Host "conda      : $conda"
Write-Host "conda base : $condaBase"
Write-Host "env dir    : $envDir"

# robostack-jazzy: the per-distro channel that actually carries the win-64
# jazzy packages (robostack-staging no longer does); keep both.
$channels = @('-c', 'conda-forge', '-c', 'robostack-staging', '-c', 'robostack-jazzy')
# python=3.11: must match the prebuilt pyainstec.cp311 pyd in install/python.
$basePkgs = @('python=3.11', 'ros-jazzy-ros-base', 'colcon-common-extensions', 'numpy', 'pybind11', 'pytest')

if (Test-Path $envPython) {
    Write-Host "Env '$EnvName' already exists - skipping create."
}
else {
    Write-Host "Creating env '$EnvName' (downloads several hundred MB; be patient)..."
    # First attempt includes the rosbridge conda package; if the channels do
    # not carry it, retry without it and use the source-build fallback below.
    & $conda create -y -n $EnvName @channels @basePkgs 'ros-jazzy-rosbridge-suite'
    if ($LASTEXITCODE -ne 0) {
        Write-Warning 'ros-jazzy-rosbridge-suite not available on conda-forge/robostack-staging; creating env without it.'
        & $conda env remove -y -n $EnvName | Out-Null
        & $conda create -y -n $EnvName @channels @basePkgs
        if ($LASTEXITCODE -ne 0) { throw "conda create failed (exit $LASTEXITCODE)" }
    }
}

# --- rosbridge: the binary conda package is the primary path. ---------------
$pkgList = & $conda list -n $EnvName
if ($pkgList -match 'ros-jazzy-rosbridge') {
    Write-Host 'rosbridge: conda package ros-jazzy-rosbridge-* is installed.'
}
else {
    Write-Warning 'ros-jazzy-rosbridge-suite missing from the env; viewer.launch.py will not work.'
    # Source-build fallback (reference only, not executed by default):
    #   git clone --depth 1 --branch jazzy https://github.com/RobotWebTools/rosbridge_suite ros2\thirdparty\rosbridge_suite
    #   <env python> -m pip install tornado
    #   colcon build --merge-install   (the overlay then shadows the conda packages)
}

# Windows fixup for the binary rosbridge package: RoboStack ships the entry
# points as extensionless python scripts under Library\lib\<pkg>\, which
# launch_ros cannot resolve via PATHEXT ("executable 'rosbridge_websocket'
# not found on the libexec directory ..."). Drop tiny .bat shims next to them
# (.bat IS in PATHEXT). Idempotent.
$shims = @(
    (Join-Path $envDir 'Library\lib\rosbridge_server\rosbridge_websocket'),
    (Join-Path $envDir 'Library\lib\rosapi\rosapi_node')
)
foreach ($script in $shims) {
    if (Test-Path $script) {
        $bat = "$script.bat"
        if (-not (Test-Path $bat)) {
            Set-Content -Path $bat -Value "@echo off`r`npython `"%~dpn0`" %*`r`n"
            Write-Host "  wrote launch shim: $bat"
        }
    }
}

Write-Host ''
Write-Host 'Next steps:'
Write-Host '  1. Activate : . scripts\activate_ros.ps1            (PowerShell, add -Mock for the mock camera)'
Write-Host '     git-bash : source /f/ProgramData/miniconda3/etc/profile.d/conda.sh && conda activate ainstec_ros'
Write-Host "  2. Build    : cd ros2 && colcon build --merge-install"
Write-Host '  3. Launch   : ros2 launch ainstec_bringup camera.launch.py continuous_hz:=1.0'
Write-Host '  (GigE discovery needs inbound firewall rules: run scripts\setup_firewall.ps1 as Administrator.)'
