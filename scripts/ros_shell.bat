@echo off
rem Run one command inside the fully activated ROS 2 environment (cmd):
rem   scripts\ros_shell.bat ros2 node list
rem Activates the conda env, the colcon overlay (ros2\install), and the
rem pyainstec staging dir. Set ACAM_MOCK=1 before calling to use the mock
rem camera backend (e.g. set ACAM_MOCK=1 && scripts\ros_shell.bat ros2 launch ...)
call F:\ProgramData\miniconda3\condabin\conda.bat activate ainstec_ros
if errorlevel 1 exit /b 1
call %~dp0..\ros2\install\local_setup.bat
if errorlevel 1 exit /b 1
set "PYTHONPATH=%~dp0..\install\python;%PYTHONPATH%"
set "PATH=%~dp0..\install\python;%PATH%"
%*
exit /b %errorlevel%
