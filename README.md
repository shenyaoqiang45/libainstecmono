# libainstecmono

Ainstec 3D 相机软件栈 monorepo。分层方式对标 librealsense / OrbbecSDK：自研 C ABI 封装厂商 CamSDK 内核，向上经 Python 绑定接入 ROS 2 主通道，再由 ROS 2 服务/React UI 消费。

## 四层架构

```
┌─────────────────────────────────────────────────────────┐
│ L4  ui/              React UI（浏览器，经 rosbridge :9090） │
├─────────────────────────────────────────────────────────┤
│ L3  ros2/            ROS 2 主通道（话题/服务/参数）         │
│     ainstec_camera · ainstec_interfaces · ainstec_bringup│
├─────────────────────────────────────────────────────────┤
│ L2  camsdk_c/        C ABI：ainstec_cam.dll              │
│     bindings/python/ pybind11 绑定：pyainstec (.pyd)      │
├─────────────────────────────────────────────────────────┤
│ L1  厂商 CamSDK      CameraPro.dll（GigE 内核，闭源）       │
│     C:/3DCameraViewer/CamSDK/CamSDK_CPP                  │
└─────────────────────────────────────────────────────────┘
```

- L2 的对象模型（context / device / sensor / frame / frameset / filter / pipeline）与线程、错误模型详见 [docs/architecture.md](docs/architecture.md)。
- L3 的话题、服务、参数契约详见 [docs/ros2.md](docs/ros2.md)。
- 无相机也可开发：L2 内置 mock 后端，环境变量 `ACAM_MOCK=1` 即切换。

## 目录布局

| 路径 | 内容 |
| --- | --- |
| `camsdk_c/` | C ABI 共享库（`include/ainstec_cam/ainstec_cam.h`），真实后端 + mock 后端，无框架冒烟测试 |
| `bindings/python/` | pybind11 绑定 `pyainstec` 及 pytest 测试 |
| `ros2/` | colcon 工作区：`ainstec_interfaces`（srv）、`ainstec_camera`（节点）、`ainstec_bringup`（launch/rviz） |
| `ui/` | React UI（经 rosbridge WebSocket 消费 ROS 2 数据） |
| `scripts/` | Windows 环境脚本（conda 环境、colcon 构建、激活、防火墙、冒烟） |
| `cmake/` | `FindAinstecCamSDK.cmake`（定位厂商 SDK，导入 `Ainstec::CamSDK`） |
| `install/python/` | `cmake --install` 暂存目录：`pyainstec.cp311-win_amd64.pyd` + `ainstec_cam.dll` + 厂商运行时 DLL，自包含 |

## 前置条件

- Windows 10/11 x64，Visual Studio 2022（C++ 工作负载），CMake ≥ 3.24；或 Linux x86_64 / aarch64（Jetson）：gcc + CMake ≥ 3.24。
- 厂商 CamSDK：默认定位 `C:/3DCameraViewer/CamSDK/CamSDK_CPP`（须含 `include/Camera.h`）。即使只用 mock 后端也必须存在——`find_package(AinstecCamSDK REQUIRED)` 无条件执行，`ainstec_cam` 始终链接厂商库。可用 `-DAINSTEC_CAMSDK_ROOT=<path>` 覆盖。SDK 自带三平台运行时：`bin/`（Windows）、`lib/linux_x86_64/`、`lib/linux_arm64/`。
- Python 3.11（与预置 `pyainstec.cp311` pyd 匹配）+ numpy + pytest；ROS 2 由 `scripts/setup_ros_env.ps1` 创建的 conda 环境同时提供。

## 快速开始

### 原生层（mock，无需相机）

```bash
cmake --preset msvc-x64
cmake --build --preset msvc-x64        # RelWithDebInfo，输出在 build/
ctest --preset msvc-x64                # camsdk_c_smoke，自动用 mock 后端

# Linux（x86_64/aarch64；SDK 路径按实际覆盖）
cmake --preset linux-x64 -DAINSTEC_CAMSDK_ROOT=/path/to/CamSDK_CPP
cmake --build --preset linux-x64
ctest --preset linux-x64
```

测试 `camsdk_c_smoke` 在进程内设置 `ACAM_MOCK=1`，覆盖 context/device/sensor/frame/frameset/filter/pipeline 全链路。

### Python 绑定

```bash
pytest bindings/python/tests           # conftest 自动定位 build/ 产物并默认 ACAM_MOCK=1
cmake --install build --config RelWithDebInfo   # 刷新 install/python 暂存目录
```

`install/python/` 是自包含目录（pyd + 全部 DLL），ROS 2 激活脚本把它加入 `PYTHONPATH`/`PATH`；也可手工加入后在任意 Python 3.11 中 `import pyainstec`。

### ROS 2（RoboStack on Windows）

```powershell
# 一次性：创建 ainstec_ros conda 环境（robostack-jazzy channel，python=3.11 匹配 cp311 pyd）
powershell -ExecutionPolicy Bypass -File scripts\setup_ros_env.ps1

# 构建 colcon 工作区（自动 vcvars64 + conda activate + NMake）
scripts\build_ros_workspace.bat

# 每次开新 shell：激活环境（-Mock 用 mock 相机）
. scripts\activate_ros.ps1 [-Mock]

ros2 launch ainstec_bringup camera.launch.py                    # 仅相机节点
ros2 launch ainstec_bringup camera.launch.py continuous_hz:=1.0 # 定频自动触发
ros2 launch ainstec_bringup viewer.launch.py                    # 相机 + rosbridge(:9090) + rosapi
```

cmd / git-bash 用户可用 `scripts\ros_shell.bat <命令>` 在激活环境中执行单条命令，或 `scripts\ros_shell.bat scripts\smoke_services.bat` 冒烟两个服务。

### UI

```bash
cd ui && npm install && npm run dev    # http://localhost:5173
```

UI 经 rosbridge WebSocket（`:9090`）取数，需先启动 `viewer.launch.py`。UI 内部结构见其自身文档，本仓库只约定上述契约。

### 真机注意

- **防火墙**：GigE 相机经入站 UDP 应答发现广播并推流、经 TCP 走控制通道，Windows 防火墙默认丢弃。以管理员运行：
  ```powershell
  powershell -ExecutionPolicy Bypass -File scripts\setup_firewall.ps1
  ```
  脚本为 conda 环境的 `python.exe` 加入站 UDP+TCP 允许规则（厂商自带工具 `Tools/NetworkRule.exe` 是其等价物）。
- **后端选择**：`ACAM_MOCK` 不设（或不为 `1`）即真机后端；`ACAM_MOCK=1` 为 mock。该变量在 `ac_context_create()` 时读取一次。cmd 中 `set ACAM_MOCK=1 && ...` 注意 `1` 与 `&&` 之间不能有空格，否则值带尾随空格导致比较失败、静默走真机。
- **网络**：相机与主机须同网段；发现经 UDP 广播（`DiscoverCameras`，超时参数默认 1000 ms）。

## 文档

- [docs/architecture.md](docs/architecture.md) — 架构分层、对象模型、线程/错误模型、mock 后端、厂商集成坑。
- [docs/ros2.md](docs/ros2.md) — ROS 2 通道契约（话题/服务/参数/QoS/launch）与 Windows/RoboStack 实操要点。
- [AGENTS.md](AGENTS.md) — 仓库约定（构建测试命令、ABI 边界、协作规则）。
