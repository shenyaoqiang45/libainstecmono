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
rem Fast DDS Large Data Mode — same default as activate_ros.ps1 / camera.launch.py
if not defined FASTDDS_BUILTIN_TRANSPORTS set "FASTDDS_BUILTIN_TRANSPORTS=LARGE_DATA?max_msg_size=48MB&sockets_size=64MB&non_blocking=true&tcp_negotiation_timeout=100"
%*
exit /b %errorlevel%
