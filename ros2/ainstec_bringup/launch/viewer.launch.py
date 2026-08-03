import os

from ament_index_python.packages import PackageNotFoundError, get_package_share_directory
from launch import LaunchDescription
from launch.actions import IncludeLaunchDescription, SetEnvironmentVariable
from launch.launch_description_sources import AnyLaunchDescriptionSource

# Same Large Data Mode string as camera.launch.py — must cover rosbridge too.
_FASTDDS_LARGE_DATA = (
    'LARGE_DATA?max_msg_size=48MB&sockets_size=64MB'
    '&non_blocking=true&tcp_negotiation_timeout=100'
)

# Default rosbridge max_message_size is 10_000_000 (~10 MB). PRO-WR depth
# (~8 MB) and RGB (~24 MB) become ~11–32 MB after JSON base64; raise the
# limit so the browser receives complete Image messages.
_ROSBRIDGE_MAX_MESSAGE_SIZE = '67108864'  # 64 MiB


def generate_launch_description():
    ainstec_share = get_package_share_directory('ainstec_bringup')
    camera = IncludeLaunchDescription(
        AnyLaunchDescriptionSource(
            os.path.join(ainstec_share, 'launch', 'camera.launch.py')))

    try:
        rosbridge_share = get_package_share_directory('rosbridge_server')
    except PackageNotFoundError as exc:
        raise RuntimeError(
            "rosbridge_server was not found. Install 'ros-jazzy-rosbridge-suite' "
            '(RoboStack) or colcon-build ros2/thirdparty/rosbridge_suite '
            '(see scripts/setup_ros_env.ps1).') from exc

    # rosbridge_websocket_launch.xml also brings up rosapi.
    rosbridge = IncludeLaunchDescription(
        AnyLaunchDescriptionSource(
            os.path.join(rosbridge_share, 'launch', 'rosbridge_websocket_launch.xml')),
        launch_arguments={
            'max_message_size': _ROSBRIDGE_MAX_MESSAGE_SIZE,
            'use_compression': 'true',
        }.items(),
    )

    return LaunchDescription([
        SetEnvironmentVariable(
            name='FASTDDS_BUILTIN_TRANSPORTS', value=_FASTDDS_LARGE_DATA),
        camera,
        rosbridge,
    ])
