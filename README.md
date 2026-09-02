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
```

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
