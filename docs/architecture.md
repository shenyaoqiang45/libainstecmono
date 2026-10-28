# 架构

libainstecmono 是 Ainstec 3D 相机的分层软件栈，分层方式对标 librealsense / OrbbecSDK：厂商闭源内核之上叠一层自研、ABI 稳定的 C 接口，再向上是 Python 绑定、ROS 2 主通道与 UI。

通用模型不经过 ROS。一次采集在 `pyainstec.Sample` 里交出深度、RGB、设备时间戳和标定；契约见 [model.md](model.md)。ROS 2 仍是 UI 和现有生态的入口。UI 里另有两个不经过 ROS 的仿真：运动学人形用头部相机探索房间，协作臂用腕部相机抓取桌面物体。虚拟相机内参与 mock 相同，见 [sim.md](sim.md)。

## 四层与对标映射

| 层 | 本仓库 | librealsense | OrbbecSDK |
| --- | --- | --- | --- |
| L4 应用/UI | `ui/`（React，经 rosbridge） | realsense-viewer | OrbbecViewer |
| L3 框架通道 | `ros2/`（ainstec_camera 节点） | realsense-ros | OrbbecSDK_ROS2 |
| L2 SDK 接口 | `camsdk_c/` C ABI + `bindings/python/` pyainstec | librealsense2（`rs2.h` C API） | OrbbecSDK（`ob::` C++ API） |
| L1 设备内核 | 厂商 CamSDK（`CameraPro.dll`，GigE） | librealsense 内核/固件协议 | Orbbec 固件协议 |

概念对应（L2 对象模型 → 对标物）：

| 本仓库 | librealsense | 说明 |
| --- | --- | --- |
| `ac_context` | `rs2_context` | 全局入口：后端选择 + 设备发现 |
| `ac_device` / `ac_device_list` | `rs2_device` / `rs2_device_list` | 设备引用计数（内部 `std::shared_ptr`） |
| `ac_sensor` | `rs2_sensor` | 采集单元，借用句柄（borrowed），不可释放 |
| `ac_frame` + `ac_frame_add_ref/release` | `rs2_frame` + `rs2_frame_add_ref/release` | 帧引用计数，原子操作 |
| `ac_frameset` | `rs2_frame`（frameset 角色） | 一次采集的全部 typed frames |
| `ac_filter`（`"decimation"`） | `rs2::decimation_filter` / processing block | frameset → frameset 处理块，按名创建 |
| `ac_pipeline` | `rs2_pipeline` | 触发 + worker 线程 + 有界帧队列 |
| 字符串键参数 + `GetAttributeTreeJson` | `rs2_options` | 选项枚举/读写 |
| `ac_device_record_to_file` / `stop_record` | `rs2::recorder` | 把活设备的 capture 追加写入自研 `.acam`（v1：无 pause） |
| `ac_context_load_device(path)` | `rs2::playback` / `ctx.load_device` | 从 `.acam` 加载回放设备；顺序 `capture`，EOF→无数据；无 seek/倍速 |

## 对象模型

定义见 `camsdk_c/include/ainstec_cam/ainstec_cam.h`（纯 C99，`ACAM_API` 导出）。

| 概念 | 定义 | 本仓库落地 |
| --- | --- | --- |
| context | 全局状态与后端选择 | `ac_context_create()` 读一次 `ACAM_MOCK`：`"1"` → mock，其余 → 真实厂商后端。`ac_context_query_devices(timeout_ms)` 经 UDP 广播发现设备；`ac_context_load_device(path)` 加载 `.acam` 回放设备（不经 L1） |
| device | 一台物理相机 | `open`/`close` 幂等；字符串键参数（int/float/bool set/get）+ `ac_device_get_attribute_tree_json()` 枚举键/类型/范围（**始终扁平** `{"attributes":[{"key","type",...}]}`；真机 GenICam 树在 L2 规范化）；`ac_device_get_intrinsics_json()` 返回 3D 相机针孔内参（须先 open）；`ac_device_get_calibration_json()` 一次返回 `{"depth","rgb","points_to_rgb"}`——深度内参 + RGB 内参 + 点云→RGB 相机坐标系的 4×4 外参（mm，须先 open；无 RGB 模块时后两个字段为 null）；`ac_device_get_info_json()` 无需 open |
| sensor | 设备内的采集单元 | 双 sensor：index 0 = `"3D Module"`（点云/深度/IR/法线），index 1 = `"RGB Camera"`（彩色纹理）。借用句柄，生命周期随 device。每个 sensor 一个输出掩码（`AC_OUTPUT_*` 位） |
| frame | 一个 typed 数据平面 | 类型：`AC_FRAME_DEPTH`（float32 毫米）、`AC_FRAME_IR`（uint8 mono）、`AC_FRAME_RGB`（uint8 packed RGB）、`AC_FRAME_POINTS`（float32 xyz，`w`=点数、`h`=1）、`AC_FRAME_NORMALS`。引用计数，`ac_frame_get_data()` 零拷贝 |
| frameset | 一次采集产出的全部帧 | 一次 `Capture()` 拆成 typed frames，全部 frame 经 `std::shared_ptr` 共享同一份 `CaptureData` 所有权；`ac_frameset_find(type)` 按类型取借用帧 |
| processing 框架 | frameset → frameset 的处理管线约定 | `ac_filter_process(in, out)`；filter 不处理的帧 `add_ref` 后直通进输出集，处理的帧共享一个新的 `BufferCapture` |
| filter | 具体算法块 | v1 仅 `"decimation"`：配置键 `"factor"`（整数 ≥1，默认 1 = 直通）；POINTS 每 factor 取 1（`count/factor`），DEPTH/IR/RGB 步进抽稀到 `(w/factor, h/factor)`（下限 1 像素） |
| pipeline | 采集总调度 | 单 worker 线程执行全部 `capture()`；手动触发（`ac_pipeline_trigger()` 入队一个信号，立即返回）或定频自触发（`ac_pipeline_set_auto_trigger(hz)`，`0`=手动）；帧集进 capacity-2 drop-oldest 队列，`ac_pipeline_wait_for_frames(timeout_ms)` 弹出 |
| record / playback | 文件设备（对标 librealsense recorder/playback） | 自研单文件 `.acam` v1（与 L1 无关）。`ac_device_record_to_file(path)` 在已 open 的设备上套 `RecordingDevice` 装饰器，后续 capture 追加 frameset；`ac_device_stop_record` 写 EOF 并卸包装。`ac_context_load_device(path)` 返回 `PlaybackDevice`（只读参数、顺序吐帧；EOF 为 `kErrNoData`，pipeline 静默丢弃失败 capture）。格式：magic/`version`/header JSON（info、扁平 attributes、values 快照、intrinsics、calibration；旧文件无 calibration 字段时回放由 intrinsics 合成 depth-only 标定）+ `FRM1` frameset 流 + `EOF1` |

真实后端的一次 capture：取所有 sensor 输出掩码的并集 → 映射为厂商 `OutputSettings`（`sendPoint3D/sendDepthmap/sendRemapTexture/sendTexture/sendNormals`）→ `Camera::Capture()` 填充 `FrameData` → 按掩码拆 typed planes（`camsdk_c/src/ainstec_backend.cpp`）。

深度图与内参的坐标系约定：厂商 `depthType` 为 `DEPTH_MAP_ALIGN_CAMERA` 时深度图对齐 IR 相机（尺寸取 `irWidth×irHeight`，内参取 `CAM_LEFT`），否则对齐 RGB 纹理（尺寸取纹理分辨率，内参取 `CAM_RGB`）；`get_intrinsics_json` 与深度帧严格遵循同一规则。

## 线程模型

- **pipeline worker（1 条）**：`ac_pipeline_start()` 创建，是唯一调用 `device->capture()` 的线程。等待手动信号（`pending` 计数）或 auto-trigger 截止时间；落后时跳过 missed periods（`next_fire` 追上当前时间）。capture 期间释放 pipeline 互斥锁（device 有自己的锁）。capture 失败静默丢弃。
- **消费方（任意线程）**：`ac_pipeline_wait_for_frames()` 阻塞在 `cv_frames` 上；队列满（2）时新帧集顶掉最旧帧集并释放之。ROS 2 节点内是单一接收线程作为唯一消费者（见 docs/ros2.md）。
- **device 互斥锁**：真实后端所有厂商 SDK 调用（open/close/capture/SetValue/GetValue/...）经设备级 `std::mutex` 串行化；mock 后端同构。sensor 掩码存在 device 上（slot 0/1），同锁保护。
- **帧引用计数**：`std::atomic<uint32_t>`，`add_ref`/`release` 线程安全；payload 存活到最后一次 release。
- **GIL**：pyainstec 在阻塞调用（query_devices/open/capture 路径的 `wait_for_frames`/filter process）外释放 GIL。

## 错误模型

- 除注明外，函数返回 `int`：`AC_OK`(0) 成功，非零为错误码；失败后 `ac_last_error()` 给出线程局部消息（永不为 NULL）。
- 错误码值镜像厂商 `GenericError.h`（见 `camsdk_backend.h`）：`kErrUnknown=1`、`kErrTimeout=2`、`kErrNotFound=13`、`kErrNotOpened=17`、`kErrInvalidParam=31`、`kErrNullPtr=33`、`kErrNoData=40`、`kErrNoMemory=72`。真实后端透传厂商原始返回码。
- C ABI 不让任何 C++ 异常逃逸：每个 public 函数有 catch-all（`ACAM_CATCH`），映射为 `kErrUnknown` + 消息。
- `ac_pipeline_wait_for_frames()` 超时返回 `kErrTimeout` 且 `ac_last_error()` 含 `"timeout"`——消费者以此区分超时与真错误。
- 所有 destroy/release/close 类函数 NULL-safe 且（除 filter create 外标注的）幂等。
- pyainstec 把任何非零返回码转成 `pyainstec.Error`（`RuntimeError` 子类），消息即 `ac_last_error()`。

## mock 后端

`ACAM_MOCK=1` 在 `ac_context_create()` 时读取一次选择 mock（`camsdk_c/src/mock_backend.cpp`），全程无硬件：

- 发现固定返回 1 台相机：`AinstecMock` / `MOCK-3D` / `10.0.0.1` / `MOCK0001`。
- 恒为 2 个 sensor；默认掩码：3D = `POINT3D|DEPTH|IR`，RGB = `RGB`。
- 合成帧（确定性伪噪声）：全部 sensor 同分辨率 640×512（DEPTH/POINTS/IR/RGB；RGB 为 8px 棋盘格）；点云按 fx=fy=500、cx=320、cy=256 反投影，内参 JSON 同值。**不产生 NORMALS 帧**。
- 5 个参数：`Exposure`(int 1..100, 默认 10)、`Gain`(int 0..15, 默认 3)、`HDR`(bool, 默认 false)、`FillHole`(bool, 默认 true)、`Smooth`(int 0..10, 默认 2)；无 float 参数，未知键返回 `kErrNotFound`。
- open 永不失败；capture 要求已 open（与真机一致）。

C 测试（`camsdk_c/tests/test_camsdk_c.cpp`）与 pytest（`bindings/python/tests/`）都跑在 mock 上。

## 厂商集成坑（CamSDK_CPP）

1. **GBK 编码头文件**：厂商头（`Camera.h`/`Common.h` 等）是 GBK 注释、CRLF；本仓库源码统一 UTF-8。根 `CMakeLists.txt` 对 MSVC 全局加 `/utf-8 /wd4819`——前者保证两套编码共存时的字面量行为，后者压制厂商头触发的 C4819 警告。
2. **`FrameData` 析构统一释放所有平面**：厂商 `FrameData` 析构时一次性释放 point3D/depthmap/texture 等全部平面，无法按平面单独释放。因此一次 Capture 拆出的所有 frame 通过 `std::shared_ptr<CaptureData>`（`AinstecCapture` 内嵌 `FrameData`）共享所有权，最后一个 frame 句柄释放时才真正释放底层内存。
3. **`rgbStatus` 只有 Open 后才知道**：`CameraInfo.rgbStatus`（`Common.h:199`，`mutable int`，0=正常）在 `Open()` 后才被 SDK 更新。因此 sensor 数量是动态的：未 open 或无 RGB 模块时为 1（仅 3D Module），open 且 `rgbStatus==0` 后为 2。capture 里对 `sendTexture` 也有 `rgbStatus==0` 守卫。
4. **`CameraInfo` 是事实句柄**：厂商 API 无独立设备句柄，每次调用（Open/Close/Capture/SetValue...）都携带同一个 `CameraInfo`。后端把它按值保存在 device 内并贯穿所有调用。
5. **防火墙**：GigE 发现（UDP 广播应答）与流数据走 unsolicited 入站 UDP，控制通道走入站 TCP；无入站规则时相机不可见/不推流。厂商自带 `Tools/NetworkRule.exe` 为其进程开规则；本仓库 `scripts/setup_firewall.ps1` 为 conda 环境的 `python.exe` 加等价规则。
6. **软触发工作模式**：`Open()` 后立即 `SetValue("TriggerMode", Camera_SoftTrigger)`（同厂商 Capture 示例），之后的每次 `Capture()` 都是一次软触发采集。
7. **标定头不封装**：厂商 `include/` 里还有标定/手眼标定头与 `camera_calibration.dll`、`hand_eye_calibration.dll`。标定是出厂/离线一次性流程（用厂商工具完成），不属于运行时采集路径；运行时需要的内参已由 `Get3DCameraIntrinsic` 暴露并封装为 `ac_device_get_intrinsics_json()`。故 C ABI 不封装标定 API，避免扩大 ABI 表面。
8. **运行时 DLL 部署**：消费 `ainstec_cam` 的二进制旁边必须放 `CameraPro.dll`、`opencv_world410.dll`、`CameraProAuthority.dll`、`msvcp140.dll`、`vcruntime140.dll`（`FindAinstecCamSDK.cmake` 的 `ainstec_copy_runtime_dlls()` 构建后自动拷贝；`install/python` 暂存亦含全套）。Debug 构建另需 `CameraProd.dll` + `opencv_world410d.dll`。
9. **`GetAttributeTreeJson` 真机是嵌套 GenICam**：顶层为 `Description`/`Feature`/`Type`/`Version`。L2 在 `ac_device_get_attribute_tree_json` 出口经 `flatten_attribute_tree_json` 规范为扁平 `{"attributes":[{"key","type"(int|float|bool),...}]}`（`Enumeration`→`int`，缺省 `step` 对 int 补 1）；mock 本就扁平，直接透传。键名仍是叶节点名（如 `Exposure`），与 `GetValue`/`SetValue` 一致。
