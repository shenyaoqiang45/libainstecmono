# AGENTS.md

给 AI 编码代理的仓库说明。假设你对本项目一无所知——先读完本文件再动手。人类读者请先看 [README.md](README.md)；架构细节见 [docs/architecture.md](docs/architecture.md)，ROS 2 契约见 [docs/ros2.md](docs/ros2.md)。

## 项目概览

`libainstecmono` 是 **Ainstec 3D 相机（GigE）的软件栈 monorepo**，分层方式对标 librealsense / OrbbecSDK：在厂商闭源内核之上叠一层自研、ABI 稳定的 C 接口，再向上做 Python 绑定、ROS 2 主通道和 React UI。

```
┌─────────────────────────────────────────────────────────┐
│ L4  ui/            React UI（浏览器，经 rosbridge :9090）  │
├─────────────────────────────────────────────────────────┤
│ L3  ros2/          ROS 2 主通道（话题/服务/参数）          │
│     ainstec_camera · ainstec_interfaces · ainstec_bringup │
├─────────────────────────────────────────────────────────┤
│ L2  camsdk_c/      C ABI：ainstec_cam.dll                 │
│     bindings/python/ pybind11 绑定：pyainstec (.pyd)       │
├─────────────────────────────────────────────────────────┤
│ L1  厂商 CamSDK    CameraPro.dll（GigE 内核，闭源）        │
│     C:/3DCameraViewer/CamSDK/CamSDK_CPP                   │
└─────────────────────────────────────────────────────────┘
```

**无相机也能开发**：L2 内置 mock 后端，环境变量 `ACAM_MOCK=1` 即切换（在 `ac_context_create()` 时读取一次）。这是默认的开发/测试路径。

数据通路有两条，都从同一次 `Capture()` 出来：
- **通用模型**：不经过 ROS。模型进程 `import pyainstec`，调用 `Pipeline.wait_sample()` 拿到一次采集的 `Sample`（深度 float32 毫米 / RGB / 设备时间戳 / 标定，零拷贝视图）。契约见 [docs/model.md](docs/model.md)，可运行样本 `examples/model_sample.py`。
- **ROS 2 / UI**：服务 UI 和已有生态。见 [docs/ros2.md](docs/ros2.md)。

## 技术栈

| 层 | 技术 |
| --- | --- |
| L2 C ABI | C99 公共头 + C++17 实现；CMake ≥ 3.24；Windows 用 MSVC 2022，Linux 用 gcc（x86_64 / aarch64 Jetson） |
| Python 绑定 | pybind11 v2.13.6（CMake FetchContent 拉取）；**Python 3.11 是硬约束**（匹配预置 `pyainstec.cp311-win_amd64.pyd`）；numpy + pytest |
| ROS 2 | Jazzy，经 **RoboStack** 跑在 Windows 的 conda 环境 `ainstec_ros` 里；colcon + NMake 构建；rclpy 节点 |
| UI | React 18 + TypeScript + Vite 5 + Three.js（点云/深度可视化）+ roslib（rosbridge WebSocket 客户端） |
| DDS | Fast DDS 2.14+，大消息用 **Large Data Mode**（`FASTDDS_BUILTIN_TRANSPORTS=LARGE_DATA?...`） |

## 目录所有权

| 目录 | 内容 | 改动时注意 |
| --- | --- | --- |
| `camsdk_c/` | L2 C ABI 库：公共头 `include/ainstec_cam/ainstec_cam.h` + 派发层 `src/camsdk_c.cpp` + 两后端（`ainstec_backend.cpp` 真实 / `mock_backend.cpp` mock）+ 属性树规范化 `attribute_tree.cpp` + `.acam` 录制回放 `record_playback.cpp`/`acam_file.cpp` + 测试 | **ABI 稳定边界**，见「开发约定」 |
| `bindings/python/` | pybind11 绑定 `pyainstec`（`pyainstec.cpp`）+ pytest（`tests/`） | ABI 变更的下游 |
| `ros2/` | colcon 工作区：`ainstec_interfaces`（srv）/ `ainstec_camera`（rclpy 节点 `camera_node.py`）/ `ainstec_bringup`（launch、rviz、FastDDS 配置） | 话题/服务/参数是对外契约，改动须同步 docs/ros2.md |
| `ui/` | React UI：`src/components/`（Capture/Device/Params 面板 + Depth/Ir/Rgb/PointCloud 视图）+ `src/sim/`（不经 ROS 的仿真：人形探索、协作臂抓取） | 独立子项目，经 rosbridge `:9090` 消费 ROS 2 层；只依赖 docs/ros2.md 的契约 |
| `scripts/` | Windows 环境/构建/防火墙/冒烟脚本 | 与 README、docs/ros2.md 的命令保持一致 |
| `cmake/` | `FindAinstecCamSDK.cmake`（定位厂商 SDK、导入 `Ainstec::CamSDK`、`ainstec_copy_runtime_dlls()` 拷贝运行时 DLL） | 改运行时依赖清单在此 |
| `examples/` | `model_sample.py`（mock 下打印一份 `Sample`） | 给模型侧的原生样本 |
| `docs/` | 架构与契约文档 | 代码行为变更必须同步文档 |
| `install/`、`build/`、`build-linux/`、`ros2/{build,install,log}/`、`ui/{node_modules,dist}/`、`cam_sdk/` | 生成物 / 厂商投放物（gitignore） | 不要手工编辑，不要提交 |

## 构建命令

前置条件：Windows 10/11 x64 + Visual Studio 2022（C++ 工作负载）+ CMake ≥ 3.24；或 Linux x86_64/aarch64 + gcc + CMake ≥ 3.24。**厂商 CamSDK 必须存在**（默认 `C:/3DCameraViewer/CamSDK/CamSDK_CPP`，须含 `include/Camera.h`）——即使只用 mock 后端，`find_package(AinstecCamSDK REQUIRED)` 也无条件执行、`ainstec_cam` 始终链接厂商库。可用 `-DAINSTEC_CAMSDK_ROOT=<path>` 覆盖。

```bash
# 原生层（Windows / MSVC）
cmake --preset msvc-x64
cmake --build --preset msvc-x64        # RelWithDebInfo，输出在 build/

# 原生层（Linux；SDK 路径按实际覆盖）
cmake --preset linux-x64 -DAINSTEC_CAMSDK_ROOT=/path/to/CamSDK_CPP
cmake --build --preset linux-x64

# Python 绑定（随原生层一起构建；把 pyd + 全部 DLL 暂存到 install/python）
cmake --install build --config RelWithDebInfo

# ROS 2 工作区（Windows / RoboStack；先一次性建 conda env）
powershell -ExecutionPolicy Bypass -File scripts\setup_ros_env.ps1   # 一次性
scripts\build_ros_workspace.bat                                      # vcvars64 + conda activate + NMake
. scripts\activate_ros.ps1 [-Mock]                                   # 每次新开 shell

# UI
cd ui && npm install && npm run dev    # http://localhost:5173
```

## 测试说明

全部默认跑在 **mock 后端**上，无需相机。

```bash
# C 层冒烟（3 个用例，自动用 mock）
ctest --preset msvc-x64                # 或 linux-x64
#   camsdk_c_smoke        —— context/device/sensor/frame/frameset/filter/pipeline 全链路
#   acam_record_playback  —— .acam 录制 / 回放设备
#   attribute_tree_flatten—— GenICam 树 → 扁平 attributes[] 规范化（不链接厂商库）

# Python 绑定（34 个测试；conftest.py 自动定位 build/ 产物、注册 DLL 目录并设 ACAM_MOCK=1）
pytest bindings/python/tests

# 给模型侧的原生样本（脚本内强制 ACAM_MOCK=1，打印一份 Sample）
python examples/model_sample.py

# ROS 2 服务冒烟（需先 activate + 起节点；在激活的 cmd 里跑）
scripts\ros_shell.bat scripts\smoke_services.bat
```

真机全链路（C/Python → ROS `attr.*` → FastDDS 大消息 → UI+rosbridge）的联调步骤与验证状态见 [docs/real_camera_checklist.md](docs/real_camera_checklist.md)。**真机验证是显式步骤，不是默认动作**（见「安全」）。

## 运行时架构（对象模型）

定义见 `camsdk_c/include/ainstec_cam/ainstec_cam.h`（纯 C99，`ACAM_API` 导出），对标 librealsense。核心概念与落地：

- **context**：全局入口。`ac_context_create()` 读一次 `ACAM_MOCK` 选后端；`query_devices(timeout_ms)` 经 UDP 广播发现设备；`load_device(path)` 加载 `.acam` 回放设备（不经 L1）。
- **device**：一台相机。`open`/`close` 幂等；字符串键参数（int/float/bool set/get）+ `get_attribute_tree_json()` 枚举键/类型/范围（**始终扁平** `{"attributes":[...]}`，真机 GenICam 树在 L2 出口规范化）；`get_intrinsics_json()` / `get_calibration_json()`（须先 open）；`record_to_file`/`stop_record` 录 `.acam`。
- **sensor**：设备内的采集单元（index 0 = `"3D Module"`，index 1 = `"RGB Camera"`），借用句柄不可释放。每个 sensor 一个输出掩码（`AC_OUTPUT_POINT3D/DEPTH/IR/RGB/NORMALS` 位）。
- **frame**：一个 typed 数据平面（DEPTH=float32 毫米、IR=uint8 mono、RGB=uint8 packed、POINTS=float32 xyz、NORMALS=float32），引用计数（`add_ref`/`release` 线程安全），`ac_frame_get_data()` 零拷贝。
- **frameset**：一次采集产出的全部 typed 帧；同次 Capture 的所有帧经 `std::shared_ptr` 共享同一份底层内存。
- **filter**：frameset → frameset 处理块，按名创建。v1 仅 `"decimation"`（配置键 `"factor"`）。
- **pipeline**：单 worker 线程执行全部 `capture()`；手动触发（`trigger()`）或定频自触发（`set_auto_trigger(hz)`，`0`=手动）；帧集进 capacity-2 drop-oldest 队列，`wait_for_frames(timeout_ms)` 弹出。

错误模型：函数返回 `int`，`AC_OK`(0) 成功，非零为错误码（值镜像厂商 `GenericError.h`）；失败后 `ac_last_error()` 给线程局部消息（永不为 NULL）。C ABI 不让任何 C++ 异常逃逸。pyainstec 把非零返回码转成 `pyainstec.Error`。线程/GIL 细节见 docs/architecture.md。

## 代码风格

1. **源码统一 UTF-8，注释用英文**（项目文档用中文）。MSVC 的 `/utf-8 /wd4819` 已在根 `CMakeLists.txt` 全局设置（厂商头是 GBK/CRLF），不要为新文件改这套编译选项。
2. C++ 遵循 C++17、关闭编译器扩展（`CMAKE_CXX_EXTENSIONS OFF`）；C 公共头保持纯 C99。
3. 匹配周边代码的命名与结构惯例，不要引入自己的默认风格。
4. 默认不写解释性注释——代码和 git 历史已经说明；只保留说明「为什么」的必要注释。

## 开发约定

1. **`camsdk_c` 是 ABI 稳定边界**。`ainstec_cam.h` 的任何改动（函数、枚举、语义）必须同步更新：两个后端（`src/ainstec_backend.cpp`、`src/mock_backend.cpp`）、C 冒烟测试（`tests/test_camsdk_c.cpp`）、pyainstec（`bindings/python/pyainstec.cpp` + pytest）、ROS 2 节点（`camera_node.py`）以及 `docs/architecture.md`。改完至少跑 `ctest --preset msvc-x64` 和 `pytest bindings/python/tests`。
2. **mock 优先开发**：所有功能先在 `ACAM_MOCK=1` 下开发测试。真机验证是显式步骤。
3. **对外契约同步文档**：ROS 2 话题/服务/参数（docs/ros2.md）、模型 `Sample`（docs/model.md）、L2 对象模型（docs/architecture.md）是对外契约；代码行为变更必须同步对应文档。
4. **保持最小改动**：修 bug 不顺手重构；生成物目录（见目录表）不进 git。
5. **不要执行 git 变更操作**（commit/push/reset/rebase 等），除非用户明确要求。

## 安全与真机操作注意事项

1. **真机操作前需用户确认**：任何会 `ac_device_open` / `capture` 真实相机、或修改相机参数（`set_value*` / ROS `attr.*`）的操作，先向用户说明再执行——open 可能独占相机控制权，capture 会触发激光投影器，若相机多人共用需先协调。**mock 不受此限**。
2. **防火墙**：GigE 相机经入站 UDP 应答发现广播并推流、经 TCP 走控制通道，Windows 防火墙默认丢弃。引导用户以管理员运行 `scripts\setup_firewall.ps1`（为 conda 环境的 `python.exe` 加 UDP+TCP 入站允许规则；厂商等价物是 `Tools/NetworkRule.exe`）。
3. **`ACAM_MOCK` 尾随空格坑**：cmd 里 `set ACAM_MOCK=1 && ...` 若 `1` 与 `&&` 之间有空格，值变成 `"1 "`，C 层 `strcmp(m,"1")` 失败，**静默走真机后端**。写 `set "ACAM_MOCK=1"` 或紧贴 `&&`。后端在 `ac_context_create()` 时选定一次，之后改环境变量无效。
4. **大消息传输**：默认 UDP 分片 + SensorDataQoS 下，PRO-WR 单幅 RGB/点云（约 24 MB）订阅方经常收不到。发布端与订阅端进程都必须带 `FASTDDS_BUILTIN_TRANSPORTS=LARGE_DATA?...`（`activate_ros.ps1` 与 launch 文件已设；另开 shell 跑 `ros2 topic echo` / UI 前也要先 `. activate_ros.ps1`）。
5. **网络**：相机与主机须同网段；发现经 UDP 广播（默认超时 1000 ms）。

## 文档索引

- [docs/architecture.md](docs/architecture.md) — 架构分层、对象模型、线程/错误模型、mock 后端、厂商集成坑（9 条）。
- [docs/model.md](docs/model.md) — 面向通用模型的原生 `Sample` 契约（P0）与后续全景。
- [docs/ros2.md](docs/ros2.md) — ROS 2 通道契约（话题/服务/参数/QoS/launch/TF）与 Windows/RoboStack 实操要点。
- [docs/sim.md](docs/sim.md) — UI 内置仿真（人形探索 + 协作臂抓取，不经 ROS）。
- [docs/real_camera_checklist.md](docs/real_camera_checklist.md) — 真机联调清单与当前验证状态。
