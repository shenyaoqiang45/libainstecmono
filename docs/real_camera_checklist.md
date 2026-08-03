# 真机联调清单（P4）

当前状态：**真机全链路（C/Python / ROS attr.\* / FastDDS 大消息 / UI+rosbridge）已在 `.229` 复验通过**（2026-08-03）。

执行前注意：open 可能独占相机控制权，capture 会触发激光投影器；若相机为多人共用，先协调。

## 0. 前置

- [x] 相机与主机同一网段，能 ping 通相机 IP
- [ ] 管理员运行 `scripts/setup_firewall.ps1`（本次发现/流已通，未验证规则是否已存在；无管理员权限读规则）
- [x] 确认环境变量 **没有** 设置 `ACAM_MOCK`（cmd 里 `set ACAM_MOCK=1 && ...` 会带尾随空格导致后端选择异常，见 docs/ros2.md 坑列表）
- [x] 原生层已构建：`cmake --build --preset msvc-x64 && cmake --install build --config RelWithDebInfo`

## 1. C ABI / Python 层

- [x] 发现：`query_devices(2000)` → 6 台（含 `.82` / `.229`）
- [x] 打开+采集：open → 3D sensor outputs=POINT3D|DEPTH|IR → pipeline trigger → wait_for_frames → 深度/点云/IR 形状与数值合理
- [x] 参数：`get/set("Exposure")` 回读一致；L2 现将 GenICam 树规范为扁平 `attributes[]`（与 mock 同 schema）
- [x] intrinsics() 返回真实内参（非 mock 的 1000/640/512）
- [x] 有 RGB 模块的机型：`.229` open 后 sensors=2，RGB 3248×2480；`.82` open 后仍仅 `3D Module`（无 RGB）

## 2. ROS2 层

- [x] `ros2 launch ainstec_bringup camera.launch.py camera_ip:=10.10.1.229` → SDK 日志打开 PRO-WR（rclpy INFO 在 Windows 下可能缓冲看不到）
- [x] `ros2 topic echo --once /ainstec_camera/depth/image_raw --no-arr --qos-reliability best_effort` → 1624×1240 `32FC1`（需 `FASTDDS_BUILTIN_TRANSPORTS=LARGE_DATA...`，见 activate_ros）
- [x] `ros2 topic echo --once /ainstec_camera/depth/camera_info --qos-reliability best_effort` → 1624×1240，K≈1791（真机内参）
- [x] `ros2 param set /ainstec_camera attr.Exposure <合法值>` → 成功（64 个 `attr.*`；Exposure 10↔11）
- [x] `ros2 service call /ainstec_camera/capture` → `success=true`；SDK 侧 frameIndex 递增
- [x] `ros2 service call /ainstec_camera/discover` → 返回 6 台 JSON
- [ ] continuous_hz:=1.0 连续运行 5 min 无错误日志（未跑）

## 3. UI 层

- [x] `ros2 launch ainstec_bringup viewer.launch.py camera_ip:=10.10.1.229` + `cd ui && npm run dev` → 浏览器 connected；Capture 后点云 preview（251720）、depth（真实 min/max m）、IR、RGB 均出图（需 viewer 将 rosbridge `max_message_size` 提到 64 MiB）
- [x] 参数面板 `attr.Exposure` 10→12→10，SDK 日志 `SetValue;key:Exposure` 一致

## 4. 记录

- [x] 机型 / 分辨率 / 内参记入下方表格
- [x] 发现的问题记入本节「已知问题」

### 已知问题（2026-08-03）

1. **属性树 schema**：（已修并复验）L2 `flatten_attribute_tree_json` → 扁平 `attributes[]`；ROS 上 `.229` declare 64 个 `attr.*`，`attr.Exposure` set/get OK。
2. **ROS 大消息投递**：（已修并复验）`FASTDDS_BUILTIN_TRANSPORTS=LARGE_DATA?max_msg_size=48MB&...`（activate_ros / ros_shell / launch）；`.229` 上 depth 8 MB、rgb/points 24 MB、preview 3 MB 一次 capture 全部收到。pub/sub 须同环境变量。
3. **`.82` 偶发 `Camera is busy`**：短时被占用时 open 返回 ret=3000；稍后可重试成功。
4. **UI / rosbridge**：（已修并复验）默认 `max_message_size=10MB` 不够 depth/RGB base64；`viewer.launch.py` 设为 64 MiB。点云 preview / depth / IR / RGB / `attr.Exposure` 均在浏览器验证通过（`.229`）。
## 实测记录

| 日期 | 相机 IP | 型号/固件 | 结果 | 备注 |
|---|---|---|---|---|
| 2026-08-03 | 10.10.1.229 | PRO-WR / serial CPWCBD0002303004 / SDK 3.0.24.1 | C/Python 通；ROS 服务+camera_info 通 | depth 1624×1240，intr fx=fy=1791.147034 cx=677.8204956 cy=648.4079666；RGB 3248×2480；Exposure 10↔11 OK；单帧 ~1.8s |
| 2026-08-03 | 10.10.1.82 | PRO-S-Enhanced / serial CPSCBDE002405T01 | C/Python 通 | depth 1280×1024，intr fx=fy=1556.75557 cx=395.5798569 cy=507.275425；无 RGB；首次 open busy，重试 OK |
