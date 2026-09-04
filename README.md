# rk3588-dual-cam-yolov8-rtsp

Two-camera YOLOv8 detection + H.265 RTSP streaming on RK3588, running on a
**true zero-copy DMA-BUF pipeline** (no CPU memcpy for frames).

RK3588 双路相机实时 YOLOv8 目标检测 + H.265 RTSP 推流示例。整条链路
`V4L2 → RGA → NPU → MPP → RTSP` 以 dma-buf fd 跨设备零拷贝流转，帧数据不经过 CPU 内存。

```
 per camera:  V4L2(rkisp) -> RGA (DMA-BUF) -> YOLOv8 RKNN (own NPU core)
              -> RGA BGR->NV12 -> external MPP H.265 encode (fd import)
              -> appsrc(video/x-h265) -> h265parse -> rtph265pay -> RTSP
```

## Overview / 概述

两路 IMX415（rkisp）各以 1920×1080@30fps NV12 采集，并行跑 YOLOv8 检测并把检测框画回画面，
随后两路都以 H.265 CBR 4 Mbps 推 RTSP（`/live0`、`/live1`）；检测到目标时，把结构化结果
（类别 / 置信度 / 坐标 / cam_id）经自定义 TCP 协议实时推给上位机。

**最核心的设计**：H.265 编码从 GStreamer 管线里移出，改由**外部 MPP 编码器按 fd 导入 NV12
显存**完成；GStreamer 只承载编码后几 KB 的 H.265 码流。这消除了旧方案里每帧约 3.1 MB 的
CPU memcpy 以及管线内编码的 RGA 转换，RTSP 侧负载从 ~3 MB/帧降到几 KB/帧。

## Features / 特性

- **全硬件加速、零拷贝**：rkisp/RGA/NPU/VPU 之间只传 dma-buf fd；只有 letterbox 缩放结果
  一次几十 KB 的 CPU 拷贝。
- **编码器外置**：MPP 直接 `mpp_buffer_import` RGA 写好的 NV12，GStreamer 不碰原始帧。
- **双摄并行**：两路分别绑定 NPU Core0 / Core1，互不抢核。
- **后加入即解码**：MPP 每 IDR 自带 SPS/PPS + h265parse `config-interval=-1`，客户端拿到
  关键帧即可开始解码。
- **多客户端不阻塞**：每个连接独立 media + appsrc，编码线程广播；慢客户端被
  `leaky=downstream` 队列丢弃。
- **内存不泄漏**：编码线程对每编码帧释放 `gst_buffer_new_allocate` 的基引用，VmRSS
  长时间采样实测涨速 ≈ 0 MB/h（曾丢失该 unref 导致每帧漏一个 GstBuffer ≈16.7KB，已修复）。
- **自带验收测试套件**：`acceptance_tests/` 9 个脚本覆盖起播/帧率/码率/RTP 丢包/内存曲线/
  并发韧性/TCP 结果协议，测试脚本按 `/proc/<pid>/exe` 校验被采进程真身（见下节）。
- **结果 TCP 服务器**：16 字节长度前缀帧协议解决粘包/拆包，事件 FIFO 广播，慢客户端
  超阈值自动断开，`MSG_NOSIGNAL` 防 SIGPIPE。

## Hardware & Software Requirements / 环境要求

**硬件**
- RK3588 平台 + 两路 MIPI 摄像头（rkisp 输出 NV12 的 `/dev/video*` 节点）
- Linux 内核需支持 `/dev/dma_heap/cma`（零拷贝 dma-buf 分配）

**软件（板端 sysroot，交叉编译时使用）**
- aarch64 buildroot 交叉工具链（默认 `/opt/atk-dlrk3588-toolchain`，可 `-DTOOLCHAIN_DIR=` 覆盖）
- sysroot 内需带：GStreamer 1.x（含 rtsp-server、app）、librga、librknnrt、
  rockchip_mpp（含 MPP 头文件）、OpenCV（core + imgproc）
- 模型：`model/yolov8.rknn`（YOLOv8n / COCO-80，见 `model/README.md`）

## Build / 编译

```bash
./build.sh                     # 用默认工具链路径
TOOLCHAIN_DIR=/opt/xxx ./build.sh   # 指定工具链
```

产物：`install/v4l2_yolov_dual_mpp`（含 `install/model/yolov8.rknn`）。

也可手动：

```bash
cmake -S . -B build \
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
      -DTOOLCHAIN_DIR=/opt/atk-dlrk3588-toolchain
cmake --build build -j
```

## Run on the Board / 板端运行

把 `install/` 拷到板子后：

```bash
./v4l2_yolov_dual_mpp <model_path> [v4l2_cam0] [v4l2_cam1] [rtsp_port] [result_port]
```

参数（带默认值）：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `model_path` | 必填 | `.rknn` 模型路径 |
| `v4l2_cam0` | `/dev/video62` | 相机 0 采集节点 |
| `v4l2_cam1` | `/dev/video71` | 相机 1 采集节点 |
| `rtsp_port` | `8554` | RTSP 服务端口 |
| `result_port` | `9000` | 检测结果 TCP 端口 |

例：`./v4l2_yolov_dual_mpp ./model/yolov8.rknn`

## View / 观看与结果

**视频流**（任一相机无传感器 / 无法 STREAMON 时会优雅降级，只挂能出流的一路）：

```bash
ffplay  rtsp://<板子IP>:8554/live0
ffplay  rtsp://<板子IP>:8554/live1
```

**检测结果**（脚本见 `scripts/`）：

```bash
python3 scripts/result_receiver_demo.py <板子IP> 9000
```

## Result TCP Protocol / TCP 结果协议

仅当**检测到目标**（count>0）时推送。每帧一条消息，长度前缀帧防粘包/拆包：

```
Header 16 B (little-endian):
  [0..3]   magic      0x5A5A5A5A
  [4..5]   version    2
  [6..7]   header_len 16
  [8..11]  payload_len
  [12..15] reserved   0

Payload (little-endian):
  frame_id, cam_id, img_width, img_height, obj_count
  then obj_count × { cls_id(int32), prop(float), label[32], left, top, right, bottom }
```

## Repository Layout / 目录结构

```
src/     v4l2_yolov_dual_mpp.cpp   双摄主程序（自研）
         result_socket.{h,cc}      TCP 检测结果服务器（自研）
support/ common.h image_utils.h file_utils.{h,c}
         postprocess.{h,cc} yolov8.{h,cc}
         —— 从 rknn_model_zoo 收编的 Apache-2.0 支持代码；
            yolov8.cc 已裁剪掉本工程不用的图像解码路径
model/   yolov8.rknn（demo 模型，见 model/README.md）
scripts/ result_receiver_demo.py / rtsp_probe_client.py（Python 客户端工具）
acceptance_tests/  主机侧验收测试套件 0X_*.{sh,py}（测什么/怎么判见下节）
```

## Acceptance Tests / 验收测试套件

`acceptance_tests/` 在**主机侧**运行，通过网络对「正在板子上运行的本程序」做可量化验收：
每个脚本自带「测什么 / 怎么测 / 怎么判」，原始采样写进 `acceptance_tests/artifacts/`
（已 gitignore，不入库；验收完打包该目录即一份带原始数据的记录）。

| 脚本 | 验收项 |
|---|---|
| `01_livestream_check.sh` | 双路起播、起播延迟、一路故障另一路不中断 |
| `02_fps_check.sh` | 解码层实际帧率（30 fps 标称）与 CBR 4 Mbps 码率 |
| `03_rtsp_probe.py` | 自实现 RTSP 握手，到 RTP 层统计序列号缺口（丢包） |
| `04_longrun_stability.sh` | 周期性探针长时间不掉流、零丢包 |
| `05_mem_watch.py` | 按 `/proc/<pid>/exe` 校验真进程后采 VmRSS 曲线，斜率判泄漏 |
| `06_perf_sample.sh` | 进程 CPU + NPU 核负载，带时间戳采样 |
| `07_concurrent_clients.sh` | 多客户端 + SIGSTOP 模拟慢客户端，验证不互拖 |
| `08_reconnect_stress.sh` | 反复断开/重连后服务存活、新连接可用 |
| `09_tcp_result_reconnect.py` | TCP 结果帧按长度前缀精确切帧、magic/版本校验 |

先决条件：主机有 `ffmpeg ffprobe python3 ssh`；板子 ssh 免密、本程序已在跑。
板子 IP / 端口 / 进程名在 `acceptance_tests/config.env` 统一配置，可被环境变量覆盖。

```bash
cd acceptance_tests
./01_livestream_check.sh                          # 起播 + 起播延迟
./02_fps_check.sh && ./02_fps_check.sh -b         # 帧率 + CBR 码率
python3 03_rtsp_probe.py                          # RTP 层零丢包
python3 05_mem_watch.py --duration-h 2            # VmRSS 内存曲线 2 小时
```

本仓库 H.265 零拷贝链路即由该套件完成验收：实测 30 fps、CBR 4.06 Mbps、RTP 长稳零丢包；
`gst_buffer_unref` 泄漏修复后 VmRSS 涨速 ≈0 MB/h（验收记录为本地 artifacts/ 日志，未入库）。

## Zero-Copy & Thread-Safety / 零拷贝与线程同步要点

- **stream / model 双缓冲**：采集线程 0/1 交替写，`model_writer_idx`(atomic, acquire/release)
  通知推理线程新帧就绪，推理 `rknn_inputs_set` 同步消费 → 高频路径无锁 SPSC。
- **NV12 6 槽环形缓冲**：MPP 异步编码，采集线程覆写前等 `nv12_inflight[i]==0`；
  槽号经互斥锁 + 有序 FIFO 交接，编码完置回 0 → 杜绝"覆写仍在编码的缓冲"。
- **检测结果** `result_mutex` 隔离推理(写)/采集(画框读)。
- **appsrc 列表** `appsrc_mutex` 只护 vector 本身，编码线程取 ref 后出锁再 push。
- **优雅降级**：采集线程 STREAMON 失败 → `capture_ready=-1` → 该路不挂 RTSP、推理/编码线程退出。

## License & Credits / 协议与致谢

- `src/` 自研代码：**MIT**（见 `LICENSE`）。
- `support/`：来自 [rknn_model_zoo](https://github.com/airockchip/rknn_model_zoo)，
  **Apache-2.0**，各文件保留原 license 头。
- `model/yolov8.rknn`：demo 模型，源自 rknn_model_zoo 示例转换。
