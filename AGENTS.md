# AGENTS.md

给 AI 代理的仓库约定。人类读者请先读 [README.md](README.md)；架构细节见 [docs/architecture.md](docs/architecture.md)、ROS 2 契约见 [docs/ros2.md](docs/ros2.md)。

## 目录所有权

| 目录 | 内容 | 改动时注意 |
| --- | --- | --- |
| `camsdk_c/` | L2 C ABI 库（公共头 + 真实/mock 两后端 + 冒烟测试） | **ABI 稳定边界**，见下节 |
| `bindings/python/` | pybind11 绑定 `pyainstec` + pytest | ABI 变更的下游 |
| `ros2/` | colcon 工作区（interfaces / camera 节点 / bringup） | 话题/服务/参数是对外契约，改动须同步 docs/ros2.md |
| `ui/` | React UI | 独立子项目，经 rosbridge `:9090` 消费 ROS 2 层；只依赖 docs/ros2.md 的契约 |
| `scripts/` | Windows 环境/构建/防火墙脚本 | 与 README、docs/ros2.md 的命令保持一致 |
| `cmake/` | `FindAinstecCamSDK.cmake` | 厂商 SDK 定位与运行时 DLL 清单 |
| `docs/` | 架构与契约文档 | 代码行为变更必须同步文档 |
| `install/`、`build/`、`ros2/{build,install,log}/` | 生成物（gitignore） | 不要手工编辑，不要提交 |

## 构建与测试命令

```bash
# 原生层（VS2022 + CMake ≥ 3.24；厂商 SDK 默认 C:/3DCameraViewer/CamSDK/CamSDK_CPP）
cmake --preset msvc-x64
cmake --build --preset msvc-x64
ctest --preset msvc-x64                                # camsdk_c_smoke，mock 后端

# 原生层 Linux（x86_64/aarch64，gcc；SDK 路径用 -DAINSTEC_CAMSDK_ROOT 覆盖）
cmake --preset linux-x64 && cmake --build --preset linux-x64 && ctest --preset linux-x64

# Python 绑定（需 Python 3.11 + numpy + pytest）
pytest bindings/python/tests                           # conftest 自动定位产物并设 ACAM_MOCK=1
cmake --install build --config RelWithDebInfo          # 刷新 install/python 暂存（pyd + 全部 DLL）

# ROS 2（Windows / RoboStack）
powershell -ExecutionPolicy Bypass -File scripts\setup_ros_env.ps1   # 一次性建 conda env
scripts\build_ros_workspace.bat                                      # vcvars64 + conda + NMake
. scripts\activate_ros.ps1 [-Mock]                                   # 每次新 shell
ros2 launch ainstec_bringup camera.launch.py [continuous_hz:=1.0]
ros2 launch ainstec_bringup viewer.launch.py                         # + rosbridge :9090
scripts\ros_shell.bat scripts\smoke_services.bat                     # 服务冒烟（cmd）
```

## 约定

1. **`camsdk_c` 是 ABI 稳定边界**。`include/ainstec_cam/ainstec_cam.h` 的任何改动（函数、枚举、语义）必须同步更新：两个后端（`src/ainstec_backend.cpp`、`src/mock_backend.cpp`）、C 冒烟测试（`tests/test_camsdk_c.cpp`）、pyainstec（`bindings/python/pyainstec.cpp` + pytest）、ROS 2 节点（`camera_node.py`）以及 `docs/architecture.md`。改完至少跑 `ctest --preset msvc-x64` 和 `pytest bindings/python/tests`。
2. **源码统一 UTF-8，注释用英文**。MSVC 的 `/utf-8 /wd4819` 已在根 CMakeLists 全局设置（厂商头是 GBK），不要为新文件改这套编译选项。
3. **mock 优先开发**：所有功能先在 `ACAM_MOCK=1` 下开发测试；该变量在 `ac_context_create()` 时读取一次。真机验证是显式步骤，不是默认动作。
4. **不要执行 git 变更操作**（commit/push/reset/rebase 等），除非用户明确要求。
5. **真机操作前需用户确认**：任何会 `ac_device_open`/`capture` 真实相机、或修改相机参数（`set_value*`/`attr.*`）的操作，先向用户说明再执行。mock 不受限。
6. **防火墙**：真机发现/推流需要入站 UDP+TCP 规则；引导用户以管理员运行 `scripts\setup_firewall.ps1`（厂商等价物是 `Tools/NetworkRule.exe`）。
7. **保持最小改动**：修 bug 不顺手重构；文档与代码行为同步更新；生成物目录（见上表）不进 git。
