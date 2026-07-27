import os

from ament_index_python.packages import PackageNotFoundError, get_package_share_directory
from launch import LaunchDescription
from launch.actions import IncludeLaunchDescription
from launch.launch_description_sources import AnyLaunchDescriptionSource


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
            os.path.join(rosbridge_share, 'launch', 'rosbridge_websocket_launch.xml')))

    return LaunchDescription([camera, rosbridge])
