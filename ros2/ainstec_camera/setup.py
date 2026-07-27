from setuptools import setup

package_name = 'ainstec_camera'

setup(
    name=package_name,
    version='0.1.0',
    packages=[package_name],
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='ainstec',
    maintainer_email='dev@ainstec.local',
    description='ROS 2 node for Ainstec 3D cameras, built on the pyainstec bindings.',
    license='MIT',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'ainstec_camera_node = ainstec_camera.camera_node:main',
        ],
    },
)
