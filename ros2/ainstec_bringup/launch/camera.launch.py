from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    camera_ip = LaunchConfiguration('camera_ip')
    continuous_hz = LaunchConfiguration('continuous_hz')
    frame_id = LaunchConfiguration('frame_id')
    preview_decimation = LaunchConfiguration('preview_decimation')

    return LaunchDescription([
        DeclareLaunchArgument(
            'camera_ip', default_value='',
            description='Camera IP to open; empty = first discovered camera.'),
        DeclareLaunchArgument(
            'continuous_hz', default_value='0.0',
            description='Auto-trigger rate in Hz (0 = manual triggering via ~/capture).'),
        DeclareLaunchArgument(
            'frame_id', default_value='ainstec_camera',
            description='TF frame id stamped on every published message.'),
        DeclareLaunchArgument(
            'preview_decimation', default_value='8',
            description='Decimation factor for the ~/points_preview cloud.'),
        Node(
            package='ainstec_camera',
            executable='ainstec_camera_node',
            name='ainstec_camera',
            output='screen',
            parameters=[{
                'camera_ip': camera_ip,
                'continuous_hz': continuous_hz,
                'frame_id': frame_id,
                'preview_decimation': preview_decimation,
            }],
        ),
    ])
