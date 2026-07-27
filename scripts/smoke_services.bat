@echo off
rem Smoke-check the two camera services (run inside an activated shell, e.g.
rem via scripts\ros_shell.bat scripts\smoke_services.bat).
ros2 service call /ainstec_camera/discover ainstec_interfaces/srv/Discover "{timeout_ms: 500}"
echo ===
ros2 service call /ainstec_camera/capture std_srvs/srv/Trigger "{}"
