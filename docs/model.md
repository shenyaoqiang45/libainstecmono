# 面向通用模型的原生样本

相机侧交给通用模型的是**一次采集的传感器样本**，不是世界模型，也不是 ROS 消息。模型进程 `import pyainstec`，调用 `Pipeline.wait_sample()`。ROS 2 仍服务 UI 和已有生态（见 [ros2.md](ros2.md)），两条路都从同一次 `Capture()` 出来：

```
Camera Capture → frameset → pyainstec.Sample → 训练 / 推理
                         ↘ ROS 2 话题 → UI
```

C ABI 这次不新增类型。平面布局、单位和时间戳语义以本文为准。C 调用方用现有的 `ac_frameset_find`、`ac_frame_get_data`、`ac_device_get_calibration_json` 拼出同一份样本。

## P0：`Sample`

`Pipeline.wait_sample(timeout_ms)` 弹出队列里的下一帧集并打包。超时与 `wait_for_frames` 相同：抛 `pyainstec.Error`，消息含 `timeout`。某路输出没开时，对应数组是 `None`（没有 RGB 模块时 `rgb` 为 `None`）。

| 字段 | 含义 |
| --- | --- |
| `index` | 设备采集计数，这一次采集的所有平面相同 |
| `timestamp_s` | 设备时间，单位秒，所有平面相同。真机取 `frameInfo.frameTimestamp`，不是主机时钟 |
| `depth` | `float32`、`H×W`、**毫米**，与点云同一单位。零拷贝视图 |
| `depth_scale_m` | 恒为 `0.001`。米 = `depth * depth_scale_m`。原生路径不做米制拷贝 |
| `rgb` | `uint8`、`H×W×3`，或 `None` |
| `ir` | `uint8`、`H×W`，或 `None` |
| `points` | `float32`、`N×3`、毫米，或 `None` |
| `normals` | `float32`、`H×W×3`，或 `None` |
| `calibration` | `Device.calibration()` 的快照：`depth`、`rgb` 针孔内参，以及 `points_to_rgb`（4×4，行主序，平移毫米）。无 RGB 模块时后两个字段为 `null` |
| `depth_valid` | 深度里有限且大于 0 的像素数 |
| `depth_count` | 深度像素数 `H*W`；没有深度平面时为 0 |

视图的 base 是 `Sample`。持有数组即持有帧内存，`Sample` 这个名字丢掉之后视图仍然有效。`Frame.depth()` / `rgb()` 等仍返回独立拷贝，ROS 节点继续用拷贝，行为不变。

标定缓存在 pipeline 上，打开设备时读一次。深度图宽高与 `calibration["depth"]` 不一致时重读，避免切换深度对齐之后内参还是上一档分辨率。样本拿到的是当时缓存的深拷贝。

`.acam` 回放走同一条调用：文件头里已有标定，`Context.load_device()` 之后 `wait_sample()` 的形状与直播一致。训练读盘用这个容器，不另定义数据集格式。

```python
import pyainstec as ac

ctx = ac.Context()
device = ctx.query_devices()[0]
device.open()
pipe = ac.Pipeline(ctx)
pipe.open(device)
pipe.start()
pipe.trigger()
sample = pipe.wait_sample(5000)

depth_m = sample.depth * sample.depth_scale_m
k = sample.calibration["depth"]  # fx, fy, cx, cy 对应该深度图
```

可运行的 mock 走查：`python examples/model_sample.py`（脚本内强制 `ACAM_MOCK=1`）。

## 全景

下面各项都接在同一份 `Sample` 上，本次未实现。

1. **质量与异常。** 在 `depth_valid` 之外增加空洞比例、时间戳回退、分辨率变化。在宿主内存上算，不调用厂商标定库。
2. **几何模块。** 保存内参、`points_to_rgb`、深度尺度，并接受调用方写入的 `base_T_cam`（手眼结果，毫米）。提供投影、反投影和一个残差读数，供运行中核对。求解手眼和自动外参标定留在相机外（厂商离线工具或上层模型）。
3. **GPU 路径。** 样本保持 C 连续，导出 DLPack，示例里用 `torch.from_numpy` / `torch.utils.dlpack`。torch 和 CUDA 不进入 `ainstec_cam` 的链接依赖；厂商帧内存仍在 CPU。
4. **畸变。** 真机上核对 `Get3DCameraIntrinsic` 是否还有畸变系数。有则补进标定 JSON（ABI 变更，两个后端、测试和 ROS `CameraInfo.D` 一起改）；没有则契约维持针孔、畸变为空。这一步要打开真机，执行前单独确认。
5. **多机触发序号。** 属性树里若已有硬同步或触发计数，放进样本元数据。没有则维持一次 `Capture()` 一个时间戳。
6. **ROS 设备时间戳。** 可选地把 `header.stamp` 改成设备时间。这是 [ros2.md](ros2.md) 的契约变更，单独做。

本仓库不实现世界模型，不实现隐式坐标学习，也不撤掉 ROS。坐标变换继续是几何模块里的显式量：模型可以辅助标定，深度尺度、内参和外参仍然要能读出来、对得上。
