# 真机联调清单（P4）

当前状态：**mock 全链路已验证，真机未测**（2026-07-24 用户决定跳过）。
已知事实：真机后端发现功能正常——同一网段发现过 4 台相机（如 10.10.1.45、10.10.1.125，UDP 广播，端口 60000）。

执行前注意：open 可能独占相机控制权，capture 会触发激光投影器；若相机为多人共用，先协调。

## 0. 前置

- [ ] 相机与主机同一网段，能 ping 通相机 IP
- [ ] 管理员运行 `scripts/setup_firewall.ps1`（为 conda env 的 python.exe 加防火墙入站规则；否则 UDP 发现/数据会被拦）
- [ ] 确认环境变量 **没有** 设置 `ACAM_MOCK`（cmd 里 `set ACAM_MOCK=1 && ...` 会带尾随空格导致后端选择异常，见 docs/ros2.md 坑列表）
- [ ] 原生层已构建：`cmake --build --preset msvc-x64 && cmake --install build --config RelWithDebInfo`

## 1. C ABI / Python 层

- [ ] 发现：`PYTHONPATH=install/python python -c "import pyainstec; print([d.info for d in pyainstec.Context().query_devices(2000)])"` → 返回设备信息（name/model/ip/serial/mac）
- [ ] 打开+采集：open → 3D sensor outputs=POINT3D|DEPTH|IR → pipeline trigger → wait_for_frames → 校验 depth/points/ir 形状与数值合理（深度非全 0、点云数量有限）
- [ ] 参数：params() 属性树可读出；set("Exposure", v) → get 回读一致
- [ ] intrinsics() 返回真实内参（fx/fy/cx/cy 非 mock 的 1000/640/512）
- [ ] 有 RGB 模块的机型：open 后 sensors 数量为 2，rgb 帧形状正确

## 2. ROS2 层

- [ ] `scripts/activate_ros.ps1` 后 `ros2 launch ainstec_bringup camera.launch.py camera_ip:=<IP>` → 节点日志显示打开真实型号
- [ ] `ros2 topic echo --once /ainstec_camera/depth/image_raw --no-arr` → 32FC1，尺寸与机型一致
- [ ] `ros2 topic echo --once /ainstec_camera/depth/camera_info` → 真实内参
- [ ] `ros2 param set /ainstec_camera attr.Exposure <合法值>` → 成功且回读一致
- [ ] `ros2 service call /ainstec_camera/capture std_srvs/srv/Trigger "{}"` → success=true，话题收到新帧
- [ ] continuous_hz:=1.0 连续运行 5 min 无错误日志

## 3. UI 层

- [ ] `ros2 launch ainstec_bringup viewer.launch.py camera_ip:=<IP>` + `cd ui && npm run dev` → 浏览器深度图/点云渲染真实场景
- [ ] 参数面板修改曝光，画面亮度随之变化

## 4. 记录

- [ ] 机型 / 固件版本 / 各机型的 depthType 与分辨率差异记录回本文档
- [ ] 发现的问题开 issue 或记入 docs/architecture.md「厂商坑」一节

## 实测记录

| 日期 | 相机 IP | 型号/固件 | 结果 | 备注 |
|---|---|---|---|---|
| （待填） | | | | |
