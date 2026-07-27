"""ROS 2 node publishing an Ainstec 3D camera through the pyainstec bindings.

Threading model: a single receiver thread is the ONLY consumer of
Pipeline.wait_for_frames() and publishes every topic from the frameset it
popped (plus a decimated preview cloud). Services never touch the frames
themselves - ~/capture only enqueues a trigger and ~/discover runs a fresh
device query - so there is no frameset race.
"""

import json
import threading
import time

import numpy as np

import rclpy
from rcl_interfaces.msg import (
    FloatingPointRange,
    IntegerRange,
    ParameterDescriptor,
    SetParametersResult,
)
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CameraInfo, Image, PointCloud2, PointField
from std_msgs.msg import Header
from std_srvs.srv import Trigger

from ainstec_interfaces.srv import Discover

import pyainstec as ac


class AinstecCameraNode(Node):
    def __init__(self):
        super().__init__('ainstec_camera')

        self.declare_parameter('camera_ip', '')          # "" = first discovered
        self.declare_parameter('frame_id', 'ainstec_camera')
        self.declare_parameter('continuous_hz', 0.0)     # 0 = manual triggering
        self.declare_parameter('preview_decimation', 8)

        self._frame_id = self.get_parameter('frame_id').value
        camera_ip = self.get_parameter('camera_ip').value
        continuous_hz = float(self.get_parameter('continuous_hz').value)
        self._preview_decimation = int(self.get_parameter('preview_decimation').value)

        # --- camera bring-up ---
        self._context = ac.Context()
        self._device = self._open_device(camera_ip)

        mask_3d = ac.OUTPUT_POINT3D | ac.OUTPUT_DEPTH | ac.OUTPUT_IR
        for sensor in self._device.sensors:
            sensor.outputs = ac.OUTPUT_RGB if 'rgb' in sensor.name.lower() else mask_3d

        self._pipeline = ac.Pipeline(self._context)
        self._pipeline.open(self._device)
        if continuous_hz > 0.0:
            self._pipeline.auto_trigger = continuous_hz
        self._pipeline.start()

        self._intrinsics = self._device.intrinsics()  # device must be open

        # --- publishers (SensorDataQoS) ---
        self._pub_points = self.create_publisher(PointCloud2, '~/points', qos_profile_sensor_data)
        self._pub_preview = self.create_publisher(PointCloud2, '~/points_preview', qos_profile_sensor_data)
        self._pub_depth = self.create_publisher(Image, '~/depth/image_raw', qos_profile_sensor_data)
        self._pub_depth_info = self.create_publisher(CameraInfo, '~/depth/camera_info', qos_profile_sensor_data)
        self._pub_ir = self.create_publisher(Image, '~/ir/image_raw', qos_profile_sensor_data)
        self._pub_rgb = self.create_publisher(Image, '~/rgb/image_raw', qos_profile_sensor_data)

        # --- services ---
        self.create_service(Trigger, '~/capture', self._on_capture)
        self.create_service(Discover, '~/discover', self._on_discover)

        # --- dynamic attr.* parameters mirrored from the device attribute tree ---
        self._declare_attribute_parameters()
        self.add_on_set_parameters_callback(self._on_set_parameters)

        # --- receiver thread (single frameset consumer) ---
        self._stop_event = threading.Event()
        self._receiver = threading.Thread(target=self._receive_loop, name='ainstec_rx', daemon=True)
        self._receiver.start()

    # ---------------------------------------------------------------- startup

    def _open_device(self, camera_ip):
        devices = self._context.query_devices(1000)
        if not devices:
            raise RuntimeError('no Ainstec cameras discovered')
        device = None
        if camera_ip:
            for candidate in devices:
                if candidate.info.get('ip') == camera_ip:
                    device = candidate
                    break
            if device is None:
                raise RuntimeError(f'camera with ip {camera_ip} was not discovered')
        else:
            device = devices[0]
        device.open()
        info = device.info
        self.get_logger().info(
            f"opened camera '{info.get('name')}' ({info.get('model')}) "
            f"ip={info.get('ip')} serial={info.get('serial')}")
        return device

    def _declare_attribute_parameters(self):
        try:
            tree = self._device.params()
        except ac.Error as exc:
            self.get_logger().warning(f'attribute tree unavailable: {exc}')
            return
        for attr in tree.get('attributes', []):
            key = attr.get('key')
            atype = attr.get('type')
            if not key:
                continue
            try:
                value = self._device.get(key)
            except ac.Error:
                continue
            descriptor = ParameterDescriptor()
            lo, hi, step = attr.get('min'), attr.get('max'), attr.get('step')
            if lo is not None and hi is not None:
                descriptor.description = f'{key} range [{lo}..{hi}] step {step if step is not None else 0}'
                if atype == 'int':
                    descriptor.integer_range = [
                        IntegerRange(from_value=int(lo), to_value=int(hi), step=int(step or 0))]
                elif atype == 'float':
                    descriptor.floating_point_range = [
                        FloatingPointRange(from_value=float(lo), to_value=float(hi), step=float(step or 0.0))]
            else:
                descriptor.description = f'{key} ({atype or "unknown type"})'
            self.declare_parameter(f'attr.{key}', value, descriptor)

    # --------------------------------------------------------------- services

    def _on_capture(self, _request, response):
        try:
            self._pipeline.trigger()
        except ac.Error as exc:
            response.success = False
            response.message = str(exc)
            self.get_logger().error(f'capture failed: {exc}')
            return response
        response.success = True
        response.message = 'triggered'
        self.get_logger().info('capture triggered')
        return response

    def _on_discover(self, request, response):
        try:
            devices = self._context.query_devices(int(request.timeout_ms))
            response.devices_json = json.dumps([d.info for d in devices])
        except ac.Error as exc:
            response.devices_json = '[]'
            self.get_logger().error(f'discover failed: {exc}')
        return response

    # -------------------------------------------------------------- parameters

    def _on_set_parameters(self, params):
        result = SetParametersResult(successful=True)
        for param in params:
            if not param.name.startswith('attr.'):
                result.successful = False
                result.reason = f'parameter {param.name} is read-only'
                return result
            key = param.name[len('attr.'):]
            try:
                # pyainstec.Device.set dispatches on the Python value type
                # (bool/int/float), matching the declared parameter type.
                self._device.set(key, param.value)
            except (ac.Error, TypeError) as exc:
                result.successful = False
                result.reason = f'{param.name}: {exc}'
                return result
            self.get_logger().info(f'{key} <- {param.value}')
        return result

    # ------------------------------------------------------------ receive loop

    def _receive_loop(self):
        decimation = ac.Filter('decimation')
        decimation.set_config('factor', self._preview_decimation)
        while not self._stop_event.is_set():
            try:
                frameset = self._pipeline.wait_for_frames(200)
            except ac.Error as exc:
                if 'timeout' in str(exc):
                    continue
                if not self._stop_event.is_set():
                    self.get_logger().error(f'wait_for_frames failed: {exc}')
                    time.sleep(0.1)
                continue
            self._publish_frameset(frameset)
            try:
                preview = decimation.process(frameset)
            except ac.Error as exc:
                self.get_logger().warning(f'decimation failed: {exc}')
                continue
            points = preview.find(ac.FRAME_POINTS)
            if points is not None:
                self._pub_preview.publish(self._pointcloud2(points.points()))

    def _publish_frameset(self, frameset):
        depth = frameset.find(ac.FRAME_DEPTH)
        if depth is not None:
            self._pub_depth.publish(self._depth_image(depth.depth()))
            self._pub_depth_info.publish(self._camera_info())  # lockstep with depth
        ir = frameset.find(ac.FRAME_IR)
        if ir is not None:
            self._pub_ir.publish(self._image(ir.ir(), 'mono8'))
        rgb = frameset.find(ac.FRAME_RGB)
        if rgb is not None:
            self._pub_rgb.publish(self._image(rgb.rgb(), 'rgb8'))
        points = frameset.find(ac.FRAME_POINTS)
        if points is not None:
            self._pub_points.publish(self._pointcloud2(points.points()))

    # ----------------------------------------------------------- message build

    def _header(self):
        header = Header()
        header.stamp = self.get_clock().now().to_msg()
        header.frame_id = self._frame_id
        return header

    def _pointcloud2(self, points):
        arr = np.ascontiguousarray(points, dtype=np.float32).reshape(-1, 3)
        msg = PointCloud2()
        msg.header = self._header()
        msg.height = 1
        msg.width = int(arr.shape[0])
        msg.fields = [
            PointField(name='x', offset=0, datatype=PointField.FLOAT32, count=1),
            PointField(name='y', offset=4, datatype=PointField.FLOAT32, count=1),
            PointField(name='z', offset=8, datatype=PointField.FLOAT32, count=1),
        ]
        msg.is_bigendian = False
        msg.point_step = 12
        msg.row_step = msg.point_step * msg.width
        msg.is_dense = True
        msg.data = arr.tobytes()
        return msg

    def _image(self, array, encoding):
        arr = np.ascontiguousarray(array)
        channels = arr.shape[2] if arr.ndim == 3 else 1
        msg = Image()
        msg.header = self._header()
        msg.height = int(arr.shape[0])
        msg.width = int(arr.shape[1])
        msg.encoding = encoding
        msg.is_bigendian = 0
        msg.step = int(arr.shape[1] * channels * arr.dtype.itemsize)
        msg.data = arr.tobytes()
        return msg

    def _depth_image(self, depth_mm):
        meters = (depth_mm * 0.001).astype(np.float32, copy=False)  # REP-118: meters
        return self._image(meters, '32FC1')

    def _camera_info(self):
        intr = self._intrinsics
        fx, fy = float(intr['fx']), float(intr['fy'])
        cx, cy = float(intr['cx']), float(intr['cy'])
        msg = CameraInfo()
        msg.header = self._header()
        msg.width = int(intr['width'])
        msg.height = int(intr['height'])
        msg.distortion_model = 'plumb_bob'
        msg.d = [0.0] * 5
        msg.k = [fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0]
        msg.r = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
        msg.p = [fx, 0.0, cx, 0.0, 0.0, fy, cy, 0.0, 0.0, 0.0, 1.0, 0.0]
        return msg

    # --------------------------------------------------------------- shutdown

    def destroy_node(self):
        stop_event = getattr(self, '_stop_event', None)
        if stop_event is not None:
            stop_event.set()
        receiver = getattr(self, '_receiver', None)
        if receiver is not None:
            receiver.join(timeout=2.0)
        pipeline = getattr(self, '_pipeline', None)
        if pipeline is not None:
            try:
                pipeline.stop()
            except ac.Error:
                pass
        device = getattr(self, '_device', None)
        if device is not None and device.is_open:
            device.close()
        super().destroy_node()


def main(args=None):
    rclpy.init(args=args)
    node = None
    try:
        node = AinstecCameraNode()
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        if node is not None:
            node.destroy_node()
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
