# ROS 2 通道契约

ROS 2 层是相机数据的主通道。节点实现：`ros2/ainstec_camera/ainstec_camera/camera_node.py`（rclpy，经 pyainstec 走 L2 C ABI）；接口：`ros2/ainstec_interfaces/srv/Discover.srv`；launch：`ros2/ainstec_bringup/launch/`。

- 节点名：`ainstec_camera`，可执行入口 `ainstec_camera_node`（console_scripts）。
- 所有话题/服务用 `~` 私有命名，实际全名为 `/ainstec_camera/...`。
- 线程模型：节点内单一接收线程（`ainstec_rx`）是 `Pipeline.wait_for_frames()` 的唯一消费者，发布全部话题；服务回调不触碰帧，故无 frameset 竞态。
- 所有消息的 `header.frame_id` 取参数 `frame_id`，`header.stamp` 取节点时钟当前时间（非设备时间戳）。

## 话题

全部为 `qos_profile_sensor_data`（SensorDataQoS：best effort / volatile / keep last 5）。

| 话题 | 类型 | 编码/格式 | 说明 |
| --- | --- | --- | --- |
| `~/points` | `sensor_msgs/PointCloud2` | x/y/z 各 FLOAT32（offset 0/4/8，point_step 12，height=1，is_dense=true） | 完整点云，坐标为相机原始单位（毫米） |
| `~/points_preview` | `sensor_msgs/PointCloud2` | 同上 | 经 `decimation` filter（factor = `preview_decimation`）抽稀的预览云，供 UI/低带宽消费；仅当 frameset 含 POINTS 时发布 |
| `~/depth/image_raw` | `sensor_msgs/Image` | `32FC1`，**米制**（毫米 × 0.001，遵循 REP-118） | 深度图；尺寸随 `depthType`（对齐 IR 或 RGB，见 docs/architecture.md） |
| `~/depth/camera_info` | `sensor_msgs/CameraInfo` | `plumb_bob`，D 全 0，K 来自设备内参 | 与 depth 严格同步发布（同一帧一前一后）；**没有** `~/rgb/camera_info` |
| `~/ir/image_raw` | `sensor_msgs/Image` | `mono8` | 左 IR 明亮图像 |
| `~/rgb/image_raw` | `sensor_msgs/Image` | `rgb8` | RGB 纹理；仅当设备 RGB 模块可用（`rgbStatus==0`）且有帧时发布 |

## 服务

| 服务 | 类型 | 语义 |
| --- | --- | --- |
| `~/capture` | `std_srvs/Trigger` | **单消费者语义**：回调只向 pipeline 入队一个软触发信号并立即返回（成功 `success=True, message='triggered'`），响应不携带帧；帧由接收线程弹出后上话题。失败 `success=False, message=<ac_last_error>` |
| `~/discover` | `ainstec_interfaces/Discover` | 请求 `uint32 timeout_ms`；响应 `string devices_json` —— 一次全新 UDP 发现的结果，JSON 数组，元素为 `{name, model, ip, serial, mac}`；失败返回 `'[]'` 并记日志 |

调用示例（见 `scripts/smoke_services.bat`）：

```bat
ros2 service call /ainstec_camera/discover ainstec_interfaces/srv/Discover "{timeout_ms: 500}"
ros2 service call /ainstec_camera/capture std_srvs/srv/Trigger "{}"
```

## 参数

静态参数（构造时 declare）：

| 参数 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `camera_ip` | string | `''` | 要打开的相机 IP；空串 = 发现列表第一台。指定 IP 未发现则节点启动失败 |
| `frame_id` | string | `'ainstec_camera'` | 所有消息 header 的 TF frame id |
| `continuous_hz` | double | `0.0` | 定频自触发 Hz；`0` = 仅手动（`~/capture`）。>0 时映射到 `Pipeline.auto_trigger` |
| `preview_decimation` | int | `8` | `~/points_preview` 的抽稀因子（decimation filter 的 `factor`） |

动态参数 `attr.*`（镜像设备属性树）：

- 启动时经 `Device.params()`（L2 已把厂商 GenICam 树规范为扁平 `attributes[]`）枚举属性树，为每个键 declare `attr.<key>`；初值取自相机当前值，读不到的键跳过。
- 属性树提供 min/max 时写入 `IntegerRange`/`FloatingPointRange` descriptor（类型/范围/步长来自属性树）；否则仅写 description。
- set 回调（`add_on_set_parameters_callback`）：**非 `attr.` 前缀一律拒绝**（`read-only`）；`attr.*` 直发相机 `Device.set(key, value)`，按 Python 值类型分派 bool/int/float，相机拒绝则 `successful=False` 并带原因。

启动行为（核实自 `camera_node.py`）：`query_devices(1000)` → 按 `camera_ip` 过滤 → open → 3D sensor 掩码设为 `POINT3D|DEPTH|IR`、RGB sensor（名字含 `rgb`）设为 `RGB` → pipeline open/start → 读取内参（须 open）。设备打开后 sensor 数量可能从 1 变为 2（`rgbStatus` 见 docs/architecture.md）。

## Launch

`ainstec_bringup/launch/camera.launch.py`：启动单个 `ainstec_camera` 节点，参数与上表一一对应：

```bash
ros2 launch ainstec_bringup camera.launch.py \
  camera_ip:=192.168.1.100 continuous_hz:=1.0 frame_id:=ainstec_camera preview_decimation:=8
```

`ainstec_bringup/launch/viewer.launch.py`：include `camera.launch.py`（默认参数）+ `rosbridge_websocket_launch.xml`（rosbridge WebSocket 默认 `:9090`，同时带起 `rosapi`）。找不到 `rosbridge_server` 包时直接报错并提示安装 `ros-jazzy-rosbridge-suite`。UI（`ui/`）经 `:9090` 消费上述话题/服务——这是 UI 与本层的全部契约。

rosbridge 参数（viewer 覆盖）：`max_message_size:=67108864`（64 MiB；上游默认 10 MB，不够装 PRO-WR depth/RGB 的 JSON base64）、`use_compression:=true`。
rviz 配置：`ainstec_bringup/rviz/ainstec.rviz`（随包安装到 share）。

## Windows / RoboStack 实操要点

环境由 `scripts/setup_ros_env.ps1` 一次性创建（conda env `ainstec_ros`）：

- channel 组合 `conda-forge + robostack-staging + robostack-jazzy`——只有 `robostack-jazzy` 带 win-64 的 jazzy 包。
- **`python=3.11` 是硬约束**：必须与 `install/python` 里预置的 `pyainstec.cp311-win_amd64.pyd` 匹配，否则 import 失败。
- 安装包：`ros-jazzy-ros-base colcon-common-extensions numpy pybind11 pytest ros-jazzy-rosbridge-suite`；conda 渠道无 rosbridge 包时脚本降级为不带它创建并给出源码构建回退说明。
- **rosbridge `.bat` shim 的由来**：RoboStack 把 entry point 装成无扩展名的 python 脚本（`Library\lib\rosbridge_server\rosbridge_websocket`、`Library\lib\rosapi\rosapi_node`），launch_ros 经 PATHEXT 找不到可执行文件（报 `executable 'rosbridge_websocket' not found on the libexec directory`）。脚本在旁生成同名 `.bat` shim（`.bat` 在 PATHEXT 内）解决，幂等。

构建工作区用 `scripts/build_ros_workspace.bat`，三件事缺一不可（脚本注释有原文）：

- **vcvars64**：激活的 conda env 里没有编译器，而 `ainstec_interfaces` 的 rosidl typesupport 需要 `cl.exe`。
- **conda activate**：提供 ROS 2 + colcon。
- **NMake Makefiles**：单配置生成器，避免 colcon 下 Visual Studio 多配置的怪癖；构建加 `-DCMAKE_BUILD_TYPE=Release`。
- 包装成 .bat 的另一个原因是 git-bash 会弄坏长 `cmd /c` 引号串。git-bash 里调用 cmd 工具记得用 `cmd //c ...`（双斜杠防 MSYS 路径转换）。

激活与运行：

- PowerShell：`. scripts\activate_ros.ps1 [-Mock]`——conda-hook + `conda activate ainstec_ros` + RoboStack 环境钩子（`Library\local_setup.ps1`）+ `install\python` 入 `PYTHONPATH`/`PATH` + colcon overlay（`ros2\install\local_setup.ps1`，存在时）+ 默认 `FASTDDS_BUILTIN_TRANSPORTS=LARGE_DATA?...`（大图/点云）。
- **colcon 在 Windows 只生成 `.bat`/`.ps1` setup 脚本**（`ros2/install/` 下无 `.sh`），git-bash 用户走 `scripts\ros_shell.bat <命令>` 在完整激活的 cmd 环境里跑单条命令。
- **`ACAM_MOCK` 尾随空格坑**：cmd 里 `set ACAM_MOCK=1 && scripts\ros_shell.bat ...` 若 `1` 与 `&&` 之间有空格，变量值变成 `"1 "`，C 层 `strcmp(m, "1")` 失败，**静默走真机后端**。写 `set "ACAM_MOCK=1"` 或紧贴 `&&`。
- **大消息（Image / PointCloud2）**：默认 UDP 分片 + SensorDataQoS（best effort）下，PRO-WR 单幅 RGB/点云约 24 MB 时订阅方经常收不到。激活脚本与 `camera.launch.py` / `viewer.launch.py` 会设置 Fast DDS 2.14 **Large Data Mode**（`FASTDDS_BUILTIN_TRANSPORTS=LARGE_DATA?max_msg_size=48MB&sockets_size=64MB&non_blocking=true&tcp_negotiation_timeout=100`）：UDP 发现 + SHM/TCP 传样本。**发布端与订阅端进程都必须带同一变量**（另开 shell 跑 `ros2 topic echo` / UI 前也要 `. activate_ros.ps1`）。可选 XML：`share/ainstec_bringup/config/fastdds_large_data.xml`（`FASTRTPS_DEFAULT_PROFILES_FILE`，且不要与 `FASTDDS_BUILTIN_TRANSPORTS` 混用）。
- 防火墙：真机发现/推流需要入站规则，管理员运行 `scripts\setup_firewall.ps1`（规则名 `Ainstec ROS2 Camera Node`，针对 conda env 的 `python.exe`，UDP+TCP 入站允许）。
