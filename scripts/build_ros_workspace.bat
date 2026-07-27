@echo off
rem Build the ros2/ colcon workspace on Windows.
rem
rem Why this wrapper: git-bash mangles long "cmd /c ..." quoting, and the
rem plain activated conda env has no compiler - rosidl typesupport for
rem ainstec_interfaces needs cl.exe. So: vcvars64 (compiler) + conda activate
rem (ROS 2 + colcon) + NMake generator (single-config; avoids Visual Studio
rem multi-config quirks under colcon). This is the combination verified to
rem work on this machine.
setlocal

set "VCVARS="
for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
        goto :found_vcvars
    )
)
echo ERROR: vcvars64.bat not found (Visual Studio 2022 required). 1>&2
exit /b 1
:found_vcvars

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

call F:\ProgramData\miniconda3\condabin\conda.bat activate ainstec_ros
if errorlevel 1 exit /b 1

cd /d %~dp0..\ros2
set "CMAKE_GENERATOR=NMake Makefiles"
colcon build --merge-install --cmake-args -DCMAKE_BUILD_TYPE=Release
exit /b %errorlevel%
