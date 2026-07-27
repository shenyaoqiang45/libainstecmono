<#
.SYNOPSIS
    Activates the ainstec_ros conda env plus this repo's ROS 2 paths.

.DESCRIPTION
    Dot-source (or run) in PowerShell:
        . .\scripts\activate_ros.ps1 [-Mock]

    - conda-hook + conda activate ainstec_ros
    - prepends <repo>\install\python to PYTHONPATH and PATH
      (pyainstec.pyd + ainstec_cam.dll + vendor DLLs, self-contained staging)
    - dot-sources <repo>\ros2\install\local_setup.ps1 when the colcon
      workspace has been built (colcon build --merge-install)
    - -Mock sets ACAM_MOCK=1 so pyainstec uses the mock camera backend
#>
[CmdletBinding()]
param(
    [switch]$Mock
)

$repoRoot = Split-Path $PSScriptRoot -Parent

# --- conda activation -------------------------------------------------------
$condaHook = 'F:\ProgramData\miniconda3\shell\condabin\conda-hook.ps1'
if (-not (Test-Path $condaHook)) {
    $condaCmd = Get-Command conda.exe -ErrorAction SilentlyContinue
    if ($condaCmd) {
        $condaBase = ((& $condaCmd.Source info --base) | Out-String).Trim()
        $condaHook = Join-Path $condaBase 'shell\condabin\conda-hook.ps1'
    }
}
if (-not (Test-Path $condaHook)) { throw "conda-hook.ps1 not found (checked F:\ProgramData\miniconda3 and PATH)." }
. $condaHook
conda activate ainstec_ros

# RoboStack on Windows: ROS environment hooks, in case the conda activation
# scripts did not already cover them.
$libSetup = Join-Path $env:CONDA_PREFIX 'Library\local_setup.ps1'
if (Test-Path $libSetup) { . $libSetup }

# --- pyainstec staging ------------------------------------------------------
$pyStage = Join-Path $repoRoot 'install\python'
$env:PYTHONPATH = "$pyStage;$env:PYTHONPATH"
$env:PATH = "$pyStage;$env:PATH"

# --- colcon workspace overlay (present after colcon build --merge-install) --
$wsSetup = Join-Path $repoRoot 'ros2\install\local_setup.ps1'
if (Test-Path $wsSetup) { . $wsSetup }

if ($Mock) { $env:ACAM_MOCK = '1' }

Write-Host "ainstec_ros activated  (repo: $repoRoot, ACAM_MOCK='$($env:ACAM_MOCK)')"
