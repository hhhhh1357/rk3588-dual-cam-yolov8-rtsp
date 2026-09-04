// v4l2_yolov_dual_mpp.cpp
// Two-camera YOLOv8 + RTSP pipeline, but the H.265 encoding runs OUTSIDE
// GStreamer: an external MPP (Media Process Platform) encoder imports each
// NV12 DMA-BUF by fd (true zero-copy, no CPU memcpy, no in-encoder RGA
// convert) and only the encoded H.265 packets are fed into GStreamer appsrc.
//
// Pipeline per camera:
//   V4L2 (rkisp) -> RGA (DMA-BUF, zero copy) -> YOLOv8 RKNN (own NPU core)
//   -> RGA BGR->NV12 -> external MPP H.265 encode (fd import, zero copy)
//   -> appsrc (video/x-h265) -> h265parse -> rtph265pay -> RTSP
//
// GStreamer no longer touches the raw NV12 at all: it only carries the few-KB
// encoded access units. This removes the 3.1 MB/frame memcpy from the old push
// thread AND the internal RGA conversion the in-pipeline mpph265enc used to
// do on system-memory input.
//
// NV12 buffer reuse: MPP encodes asynchronously, so the 2-slot double buffer
// is replaced by a 6-slot ring. The capture thread only writes slot i after
// the encoder retrieved that slot's packet (nv12_inflight[i]==0), and the
// encoder drains an in-order FIFO so no slot is skipped.
//
// This is the standalone, published variant of the two-camera pipeline.
// Rockchip rknn_model_zoo support code it depends on lives in ../support/.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <deque>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <rga/im2d.h>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

#include <opencv2/opencv.hpp>

#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <string>
#include <iostream>
#include <vector>

// --- MPP (external H.265 encoder) ---
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_err.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/rk_venc_rc.h>

#include "yolov8.h"
#include "postprocess.h"
#include "result_socket.h"

// ============== Configuration ==============
#define DEFAULT_V4L2_NODE_CAM0  "/dev/video62"
#define DEFAULT_V4L2_NODE_CAM1  "/dev/video71"
#define DEFAULT_CAPTURE_WIDTH   1920
#define DEFAULT_CAPTURE_HEIGHT  1080
#define DEFAULT_STREAM_WIDTH    1920
#define DEFAULT_STREAM_HEIGHT   1080
#define DEFAULT_MODEL_WIDTH     640
#define DEFAULT_MODEL_HEIGHT    640
#define DEFAULT_RTSP_PORT       8554
#define DEFAULT_SOCKET_PORT     9000
#define MAX_CAMS                2
#define BUFFER_COUNT            4
#define BUF_NUM                 2        // stream/model double buffering
#define N_NV12                  6        // NV12 encoder ring (MPP encodes async)
#define NV12_VER_STRIDE_PAD     1088     // padded height; RGA writes 1080 rows,
                                         // MPP reads ver_stride=1080 but the
                                         // extra rows give aligned-access headroom

#define RTSP_ENCODE_BPS         4000000  // 4 Mbps CBR per stream
#define RTSP_ENCODE_FPS         30
#define RTSP_ENCODE_GOP         15       // one keyframe every 0.5 s

// Watchdog role indices into CameraPipeline::wd_hb_us / wd_exited.  Defined
// before CameraPipeline so the struct can size its per-worker arrays.
#define WD_ROLE_CAP   0        // capture thread
#define WD_ROLE_INF   1        // inference thread
#define WD_ROLE_ENC   2        // MPP encode thread
#define WD_N_ROLES    3
#define WD_TICK_MS    1000             // watchdog checks the heartbeats every 1 s
#define WD_TIMEOUT_US 5000000ULL       // no heartbeat for > 5 s => worker wedged
                                       // (a healthy 30 fps pipeline refreshes
                                       //  every ~33 ms)

// V4L2 captured buffer fds
typedef struct {
    int dma_fd;
    __u32 length;
} v4l2_buf_t;

// ============== Per-camera pipeline state ==============
struct CameraPipeline {
    int  cam_id = 0;
    bool enabled = true;                 // false = this camera failed init, skip it
    const char *v4l2_node = nullptr;
    rknn_core_mask core_mask = RKNN_NPU_CORE_0;

    // V4L2
    int v4l_fd = -1;
    int capture_width = DEFAULT_CAPTURE_WIDTH;
    int capture_height = DEFAULT_CAPTURE_HEIGHT;
    v4l2_buf_t v4l2_bufs[BUFFER_COUNT] = {};

    // sizes
    int stream_width = DEFAULT_STREAM_WIDTH;
    int stream_height = DEFAULT_STREAM_HEIGHT;
    int model_width = DEFAULT_MODEL_WIDTH;
    int model_height = DEFAULT_MODEL_HEIGHT;

    // DMA buffers. stream/model stay 2-slot double-buffered (consumed in the
    // same capture iteration / synchronously by rknn_inputs_set). The NV12
    // ring is larger because the MPP encoder consumes it asynchronously.
    int   stream_dma_fd[BUF_NUM] = {-1, -1};
    void *stream_mapped[BUF_NUM] = {nullptr, nullptr};
    int   nv12_dma_fd[N_NV12] = {-1, -1, -1, -1, -1, -1};  // RGA BGR->NV12 output
    int   model_dma_fd[BUF_NUM] = {-1, -1};
    void *model_mapped[BUF_NUM] = {nullptr, nullptr};

    // Temporary letterbox-scaled buffer (RGB888, scaled_w x scaled_h)
    int   temp_letterbox_fd = -1;
    void *temp_letterbox_mapped = nullptr;
    int   temp_letterbox_w = 0, temp_letterbox_h = 0;

    // Thread-sync state
    std::atomic<int>  model_writer_idx{0};
    std::atomic<bool> inference_alive{false};

    // NV12 ring ownership: slot i is 1 while its packet is still being
    // encoded by MPP. The capture thread waits for 0 before rewriting it.
    std::atomic<int>  nv12_inflight[N_NV12];
    // In-order queue of ring slots waiting for the encoder (capture producer,
    // MPP encoder consumer). Bounded implicitly: capture waits for a free slot.
    std::mutex        enc_fifo_mutex;
    std::deque<int>   enc_fifo;

    // Capture thread handshake with main(): -1 = failed before streaming,
    // 0 = starting, 1 = streaming. main() only mounts the RTSP endpoint for
    // cameras that reach streaming; a /dev/videoXX that opens but fails at
    // STREAMON (e.g. no sensor attached) degrades cleanly like an open failure.
    std::atomic<int>  capture_ready{0};

    // Watchdog heartbeats (monotonic microseconds).  Each worker refreshes only
    // its own slot, and only when it makes progress; the watchdog thread treats
    // a stall longer than WD_TIMEOUT_US, or a worker exiting while the stream is
    // up, as a fault and forces a non-zero exit (see the watchdog section below).
    // g_cams is a static global, so these are zero-initialized
    // (0 = "worker not started yet"); no brace-init needed for C++11 atomics.
    std::atomic<uint64_t> wd_hb_us[WD_N_ROLES];
    std::atomic<int>      wd_exited[WD_N_ROLES];

    // Detection results (per camera, so boxes never cross cameras)
    std::mutex result_mutex;
    object_detect_result_list od_results;

    // RKNN
    rknn_app_context_t rknn_app_ctx;

    // letterbox params (mapped back to original coords by post_process)
    float lb_scale = 1.0f;
    int lb_x_pad = 0, lb_y_pad = 0;

    // RTSP push: one appsrc per CONNECTED CLIENT media (non-shared factories).
    // The encoder thread broadcasts every encoded packet to all live appsrcs.
    std::mutex appsrc_mutex;
    std::vector<GstElement *> appsrcs;
    guint64 out_frames = 0;              // encoded frames produced

    // External MPP encoder state
    MppCtx          mpp_ctx = nullptr;
    MppApi         *mpi     = nullptr;
    MppEncCfg       mpp_cfg = nullptr;
    MppBufferGroup  mpp_group = nullptr;
    MppBuffer       mpp_buf[N_NV12] = {nullptr};

    // per-camera socket frame counter
    uint32_t sock_frame_id = 0;
};

// ============== Globals ==============
static CameraPipeline g_cams[MAX_CAMS];
static int socket_port = DEFAULT_SOCKET_PORT;
static volatile bool g_running = true;

// ---------- Signal handler ----------
static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        fprintf(stderr, "\nReceived signal %d, stopping...\n", signo);
        g_running = false;
    }
}

// ============== Watchdog: heartbeat supervision of the worker threads ==============
// Unattended deployment: nothing in-process used to notice a capture/inference/
// encode thread that wedged (blocked in an ioctl / rknn / MPP call) or exited
// unexpectedly while RTSP was still mounted - the stream just silently died.
//
// Every worker therefore refreshes its own heartbeat slot (monotonic us) as it
// makes progress; a healthy 30 fps pipeline refreshes about every 33 ms.  The
// watchdog thread below checks, once per second, that the capture/inference/
// encode threads of every camera that is actually streaming (capture_ready==1)
// are both alive and recent.  If one is not, the pipeline cannot be unwound
// safely in-process (a wedged thread may never join, and re-entering
// V4L2/RGA/NPU/MPP risks leaks), so the watchdog logs the reason and _exit()s
// non-zero.  An external supervisor (systemd Restart=always, a launch-shell
// loop, ...) then restarts the whole application.
//
// Cameras that gracefully degraded during startup (e.g. no sensor attached:
// STREAMON failed, capture_ready==-1, never mounted on RTSP) are outside the
// watchdog's scope by design and will not cause false alarms.

// A scope guard that flips wd_exited[role] to 1 when its worker returns, for
// any reason.  The watchdog treats "exited while the stream is up" as a fault.
class WorkerExitFlag {
    std::atomic<int> *flag_;
public:
    explicit WorkerExitFlag(std::atomic<int> &f) : flag_(&f) {
        flag_->store(0, std::memory_order_release);
    }
    ~WorkerExitFlag() {
        flag_->store(1, std::memory_order_release);
    }
};

static inline uint64_t now_monotonic_us() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t).count();
}

static const char *g_wd_role_name[WD_N_ROLES] = {"capture", "inference", "encoder"};

enum WdFaultKind { WD_OK = 0, WD_EXITED, WD_STALLED };

// Returns WD_EXITED / WD_STALLED (and the offending role) if a camera that is
// actually streaming has a dead or wedged worker, else WD_OK.  Cameras that
// never reached streaming are skipped.
static int wd_camera_fault(CameraPipeline *cam, uint64_t now, int *role_out) {
    if (!cam->enabled || cam->capture_ready.load(std::memory_order_acquire) != 1)
        return WD_OK;
    for (int r = 0; r < WD_N_ROLES; r++) {
        if (cam->wd_exited[r].load(std::memory_order_acquire)) { *role_out = r; return WD_EXITED; }
        const uint64_t last = cam->wd_hb_us[r].load(std::memory_order_acquire);
        if (last && now - last > WD_TIMEOUT_US)                { *role_out = r; return WD_STALLED; }
    }
    return WD_OK;
}

static void watchdog_thread_func() {
    printf("Watchdog thread started (tick %d ms, heartbeat timeout %.1f s).\n",
           WD_TICK_MS, (double)WD_TIMEOUT_US / 1e6);
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(WD_TICK_MS));
        if (!g_running) break;
        const uint64_t now = now_monotonic_us();
        for (int ci = 0; ci < MAX_CAMS; ci++) {
            int role = 0;
            const int kind = wd_camera_fault(&g_cams[ci], now, &role);
            if (kind == WD_OK) continue;
            fprintf(stderr,
                    "\n[watchdog] camera %d: %s thread %s - forcing restart\n",
                    g_cams[ci].cam_id, g_wd_role_name[role],
                    kind == WD_EXITED ? "exited while streaming"
                                      : "stalled (no heartbeat for >5 s)");
            fflush(NULL);
            _exit(2);   // cannot safely unwind a possibly-wedged pipeline
                        // in-process; exit non-zero so the external supervisor
                        // (systemd / launch script) restarts the whole app.
        }
    }
    printf("Watchdog thread stopped.\n");
}

// ============== DMA-BUF alloc + mmap ==============
static int alloc_dmabuf(size_t size, int *fd_out, void **ptr_out) {
    const char *heap_paths[] = {"/dev/dma_heap/cma", "/dev/dma_heap/system"};
    for (auto path : heap_paths) {
        int heap_fd = open(path, O_RDWR);
        if (heap_fd < 0) continue;
        struct dma_heap_allocation_data alloc = {0};
        alloc.len = size;
        alloc.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) == 0) {
            *fd_out = alloc.fd;
            close(heap_fd);
            if (ptr_out) {
                *ptr_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd_out, 0);
                if (*ptr_out == MAP_FAILED) {
                    perror("mmap");
                    close(*fd_out);
                    *fd_out = -1;
                    return -1;
                }
            }
            return 0;
        }
        close(heap_fd);
    }
    return -1;
}

// Make a DMA-BUF cache-coherent for a CPU read/write after a device (RGA) wrote it.
static void dmabuf_sync(int fd, __u64 flags) {
    if (fd < 0) return;
    struct dma_buf_sync s = {0};
    s.flags = DMA_BUF_SYNC_START | flags;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
    s.flags = DMA_BUF_SYNC_END | flags;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
}

// ============== V4L2 init ==============
static int v4l2_init(CameraPipeline *cam, const char *node, int width, int height) {
    cam->v4l_fd = open(node, O_RDWR);
    if (cam->v4l_fd < 0) { perror("open v4l2"); return -1; }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(cam->v4l_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("S_FMT");
        return -1;
    }
    printf("V4L2[%d] format: %dx%d NV12\n", cam->cam_id, width, height);
    cam->capture_width = fmt.fmt.pix_mp.width;
    cam->capture_height = fmt.fmt.pix_mp.height;

    struct v4l2_requestbuffers req = {0};
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam->v4l_fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[2];
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 2;
        if (ioctl(cam->v4l_fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return -1; }

        __u32 total = planes[0].length + planes[1].length;
        struct v4l2_exportbuffer expbuf = {0};
        expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        expbuf.index = i;
        expbuf.plane = 0;
        if (ioctl(cam->v4l_fd, VIDIOC_EXPBUF, &expbuf) < 0) { perror("EXPBUF"); return -1; }

        cam->v4l2_bufs[i].dma_fd = expbuf.fd;
        cam->v4l2_bufs[i].length = total;
        printf("V4L2[%d] Buf %d: dma_fd=%d, size=%u\n", cam->cam_id, i, expbuf.fd, total);
    }
    return 0;
}

// ============== Capture thread: V4L2 + RGA ==============
static void capture_thread_func(CameraPipeline *cam) {
    WorkerExitFlag wd_flag(cam->wd_exited[WD_ROLE_CAP]);
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[2];
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 2;
        planes[0].length = cam->capture_width * cam->capture_height;
        planes[1].length = planes[0].length / 2;
        if (ioctl(cam->v4l_fd, VIDIOC_QBUF, &buf) < 0) perror("QBUF");
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(cam->v4l_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("STREAMON");
        // Device opened but cannot stream (e.g. no sensor on rkisp1-vir1).
        // Mark the camera dead so main() does not mount /liveX and the
        // inference/encoder threads for this camera exit.
        cam->enabled = false;
        cam->capture_ready.store(-1, std::memory_order_release);
        return;
    }
    cam->capture_ready.store(1, std::memory_order_release);
    cam->wd_hb_us[WD_ROLE_CAP].store(now_monotonic_us(), std::memory_order_release);
    printf("Capture thread[%d] started (MPP zero-copy).\n", cam->cam_id);

    float scale = std::min((float)cam->model_width / cam->capture_width,
                           (float)cam->model_height / cam->capture_height);
    int scaled_w = (int)(cam->capture_width * scale);
    int scaled_h = (int)(cam->capture_height * scale);
    int x_pad = (cam->model_width - scaled_w) / 2;
    int y_pad = (cam->model_height - scaled_h) / 2;

    if (alloc_dmabuf(scaled_w * scaled_h * 3, &cam->temp_letterbox_fd,
                     &cam->temp_letterbox_mapped) < 0) {
        fprintf(stderr, "camera %d: Failed to alloc temp letterbox buffer\n", cam->cam_id);
        cam->enabled = false;
        return;
    }
    cam->temp_letterbox_w = scaled_w;
    cam->temp_letterbox_h = scaled_h;

    for (int i = 0; i < BUF_NUM; i++) {
        memset(cam->model_mapped[i], 114, cam->model_width * cam->model_height * 3);
        __sync_synchronize();
    }

    int cur_nv12 = 0;      // NV12 ring index (0..N_NV12-1)
    int cur_buf  = 0;      // stream/model double-buffer index (0/1)

    while (g_running) {
        struct pollfd pfd = { .fd = cam->v4l_fd, .events = POLLIN };
        int pret = poll(&pfd, 1, 1000);
        if (pret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pret == 0) continue;

        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[2];
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 2;
        if (ioctl(cam->v4l_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            perror("DQBUF");
            break;
        }

        int sb = cur_buf & 1;              // stream/model double buffer
        int nv12_wb = cur_nv12;            // NV12 ring slot
        int nv12_fd = cam->v4l2_bufs[buf.index].dma_fd;

        // ----- RGA conversion (all on dma-buf fds, no CPU copies) -----
        rga_buffer_t src = wrapbuffer_fd(nv12_fd, cam->capture_width, cam->capture_height,
                                         RK_FORMAT_YCbCr_420_SP);

        // 1. NV12 -> full-res BGR for display / drawing
        rga_buffer_t stream_dst = wrapbuffer_fd(cam->stream_dma_fd[sb],
                                                cam->stream_width, cam->stream_height,
                                                RK_FORMAT_BGR_888);
        IM_STATUS st = imresize(src, stream_dst, 0.0, 0.0, 0, 1, NULL);
        if (st != IM_STATUS_SUCCESS) fprintf(stderr, "RGA stream error %d\n", st);
        dmabuf_sync(cam->stream_dma_fd[sb], DMA_BUF_SYNC_READ);

        // 2. letterbox: NV12 -> scaled RGB888 (temp buffer)
        rga_buffer_t temp_rgb = wrapbuffer_fd(cam->temp_letterbox_fd,
                                              scaled_w, scaled_h,
                                              RK_FORMAT_RGB_888);
        st = imresize(src, temp_rgb, 0.0, 0.0, 0, 1, NULL);
        if (st != IM_STATUS_SUCCESS) fprintf(stderr, "RGA letterbox resize error %d\n", st);
        dmabuf_sync(cam->temp_letterbox_fd, DMA_BUF_SYNC_READ);

        // 3. place the scaled image into the model buffer's centered region
        uint8_t *model_row = (uint8_t *)cam->model_mapped[sb]
                             + ((size_t)y_pad * cam->model_width + x_pad) * 3;
        const uint8_t *temp_row = (const uint8_t *)cam->temp_letterbox_mapped;
        size_t row_bytes = (size_t)scaled_w * 3;
        size_t model_stride = (size_t)cam->model_width * 3;
        for (int y = 0; y < scaled_h; y++) {
            memcpy(model_row + (size_t)y * model_stride,
                   temp_row + (size_t)y * row_bytes, row_bytes);
        }

        cam->lb_scale = scale;
        cam->lb_x_pad = x_pad;
        cam->lb_y_pad = y_pad;

        // 4. draw detection boxes directly on the stream BGR memory (zero copy)
        cv::Mat bgr(cam->stream_height, cam->stream_width, CV_8UC3,
                    cam->stream_mapped[sb]);
        if (cam->inference_alive.load()) {
            std::lock_guard<std::mutex> lock(cam->result_mutex);
            for (int i = 0; i < cam->od_results.count; i++) {
                object_detect_result *det = &cam->od_results.results[i];
                cv::rectangle(bgr, cv::Point(det->box.left, det->box.top),
                              cv::Point(det->box.right, det->box.bottom),
                              cv::Scalar(0, 255, 0), 3);
                cv::putText(bgr, coco_cls_to_name(det->cls_id),
                            cv::Point(det->box.left, det->box.top - 10),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8,
                            cv::Scalar(255, 0, 0), 2);
            }
        }
        dmabuf_sync(cam->stream_dma_fd[sb], DMA_BUF_SYNC_WRITE);

        // 5. RGA: BGR -> NV12 for the MPP encoder. The NV12 dma-buf is larger
        //    than the 1080 rows RGA writes (see NV12_VER_STRIDE_PAD) to give
        //    MPP aligned-access headroom. RGA writes chroma at
        //    w*h = 1920*1080, which matches MPP's ver_stride=1080 layout.
        //    Wait for the encoder to have finished with this ring slot first.
        while (g_running && cam->nv12_inflight[nv12_wb].load(std::memory_order_acquire) != 0)
            std::this_thread::sleep_for(std::chrono::microseconds(500));

        rga_buffer_t bgr_src = wrapbuffer_fd(cam->stream_dma_fd[sb],
                                             cam->stream_width, cam->stream_height,
                                             RK_FORMAT_BGR_888);
        rga_buffer_t nv12_dst = wrapbuffer_fd(cam->nv12_dma_fd[nv12_wb],
                                              cam->stream_width, cam->stream_height,
                                              RK_FORMAT_YCbCr_420_SP);
        st = imresize(bgr_src, nv12_dst, 0.0, 0.0, 0, 1, NULL);
        if (st != IM_STATUS_SUCCESS) fprintf(stderr, "RGA BGR->NV12 error %d\n", st);
        // RGA wrote via DMA; MPP reads via DMA. No CPU map of this buffer, so
        // no dma-buf cache sync is needed on the device-to-device path.

        // 6. notify the inference thread and the MPP encoder
        cam->model_writer_idx.store(sb, std::memory_order_release);
        cam->nv12_inflight[nv12_wb].store(1, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(cam->enc_fifo_mutex);
            cam->enc_fifo.push_back(nv12_wb);
        }

        cur_nv12 = (cur_nv12 + 1) % N_NV12;
        cur_buf  = (cur_buf + 1) & 1;

        planes[0].length = cam->capture_width * cam->capture_height;
        planes[1].length = planes[0].length / 2;
        if (ioctl(cam->v4l_fd, VIDIOC_QBUF, &buf) < 0) perror("QBUF");

        // A frame made it all the way through RGA -> encoder handoff: alive.
        cam->wd_hb_us[WD_ROLE_CAP].store(now_monotonic_us(), std::memory_order_release);
    }

    ioctl(cam->v4l_fd, VIDIOC_STREAMOFF, &type);
    printf("Capture thread[%d] stopped.\n", cam->cam_id);
}

// ============== Inference thread ==============
static void inference_thread_func(CameraPipeline *cam) {
    WorkerExitFlag wd_flag(cam->wd_exited[WD_ROLE_INF]);
    printf("Inference thread[%d] started.\n", cam->cam_id);
    int last_idx = -1;

    while (g_running) {
        // If the capture thread died before streaming (STREAMON failed), there
        // is nothing to infer; exit instead of spinning.
        if (cam->capture_ready.load(std::memory_order_acquire) < 0)
            break;
        int idx = cam->model_writer_idx.load(std::memory_order_acquire);
        if (idx == last_idx) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last_idx = idx;

        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].size = cam->model_width * cam->model_height * 3;
        inputs[0].buf = cam->model_mapped[idx];

        int ret = rknn_inputs_set(cam->rknn_app_ctx.rknn_ctx, 1, inputs);
        if (ret < 0) { fprintf(stderr, "camera %d: rknn_inputs_set failed %d\n", cam->cam_id, ret); break; }

        ret = rknn_run(cam->rknn_app_ctx.rknn_ctx, nullptr);
        if (ret < 0) { fprintf(stderr, "camera %d: rknn_run failed %d\n", cam->cam_id, ret); break; }

        rknn_output outputs[cam->rknn_app_ctx.io_num.n_output];
        memset(outputs, 0, sizeof(outputs));
        for (int i = 0; i < cam->rknn_app_ctx.io_num.n_output; i++) {
            outputs[i].index = i;
            outputs[i].want_float = (!cam->rknn_app_ctx.is_quant);
        }
        ret = rknn_outputs_get(cam->rknn_app_ctx.rknn_ctx, cam->rknn_app_ctx.io_num.n_output,
                               outputs, NULL);
        if (ret < 0) { fprintf(stderr, "camera %d: rknn_outputs_get failed %d\n", cam->cam_id, ret); break; }

        object_detect_result_list temp_results;
        memset(&temp_results, 0, sizeof(temp_results));

        letterbox_t lb;
        lb.scale = cam->lb_scale;
        lb.x_pad = cam->lb_x_pad;
        lb.y_pad = cam->lb_y_pad;
        post_process(&cam->rknn_app_ctx, outputs, &lb,
                     BOX_THRESH, NMS_THRESH, &temp_results);

        rknn_outputs_release(cam->rknn_app_ctx.rknn_ctx,
                             cam->rknn_app_ctx.io_num.n_output, outputs);

        {
            std::lock_guard<std::mutex> lock(cam->result_mutex);
            cam->od_results = temp_results;

            if (cam->od_results.count > 0) {
                std::vector<socket_result_t> sock_results;
                sock_results.reserve((size_t)cam->od_results.count);
                for (int i = 0; i < cam->od_results.count; i++) {
                    object_detect_result *det = &cam->od_results.results[i];
                    socket_result_t sr;
                    memset(&sr, 0, sizeof(sr));
                    sr.cls_id = det->cls_id;
                    sr.prop   = det->prop;
                    const char *name = coco_cls_to_name(det->cls_id);
                    strncpy(sr.label, name ? name : "unknown", sizeof(sr.label) - 1);
                    sr.left   = det->box.left;
                    sr.top    = det->box.top;
                    sr.right  = det->box.right;
                    sr.bottom = det->box.bottom;
                    sock_results.push_back(sr);
                }
                result_socket_publish((uint32_t)cam->cam_id, ++cam->sock_frame_id,
                                      sock_results.data(),
                                      (uint32_t)sock_results.size());
            }
        }
        cam->wd_hb_us[WD_ROLE_INF].store(now_monotonic_us(), std::memory_order_release);
        cam->inference_alive.store(true);
    }
    cam->inference_alive.store(false);
    printf("Inference thread[%d] stopped.\n", cam->cam_id);
}

// ============== External MPP encoder ==============
static int init_mpp_encoder(CameraPipeline *cam) {
    MppCtx ctx = nullptr;
    MppApi *mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        fprintf(stderr, "camera %d: mpp_create failed\n", cam->cam_id);
        return -1;
    }
    if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC) != MPP_OK) {
        fprintf(stderr, "camera %d: mpp_init(HEVC enc) failed\n", cam->cam_id);
        mpp_destroy(ctx);
        return -1;
    }
    cam->mpp_ctx = ctx;
    cam->mpi = mpi;

    MppEncCfg cfg = nullptr;
    mpp_enc_cfg_init(&cfg);
    mpp_enc_cfg_set_s32(cfg, "prep:width",     cam->stream_width);
    mpp_enc_cfg_set_s32(cfg, "prep:height",    cam->stream_height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", cam->stream_width);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", cam->stream_height);  // matches RGA plane layout
    mpp_enc_cfg_set_s32(cfg, "prep:format",    MPP_FMT_YUV420SP);     // NV12
    mpp_enc_cfg_set_s32(cfg, "prep:rotation",  0);
    mpp_enc_cfg_set_s32(cfg, "codec:type",     MPP_VIDEO_CodingHEVC);

    mpp_enc_cfg_set_s32(cfg, "rc:mode",        MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target",  RTSP_ENCODE_BPS);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max",     RTSP_ENCODE_BPS);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min",     RTSP_ENCODE_BPS);
    mpp_enc_cfg_set_s32(cfg, "rc:gop",         RTSP_ENCODE_GOP);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",  RTSP_ENCODE_FPS);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", RTSP_ENCODE_FPS);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:drop_mode",   MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_s32(cfg, "base:low_delay", 1);

    int ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    if (ret != MPP_OK) {
        fprintf(stderr, "camera %d: MPP_ENC_SET_CFG failed %d\n", cam->cam_id, ret);
        mpp_destroy(ctx);
        cam->mpp_ctx = nullptr; cam->mpi = nullptr;
        return -1;
    }

    // SPS/PPS before every IDR -> self-contained stream for late RTSP clients.
    MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
    mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &hm);

    // External DRM buffer group; import each NV12 dma-buf by fd (zero copy).
    if (mpp_buffer_group_get_external(&cam->mpp_group, MPP_BUFFER_TYPE_DRM) != MPP_OK ||
        !cam->mpp_group) {
        fprintf(stderr, "camera %d: mpp_buffer_group_get_external failed\n", cam->cam_id);
        mpp_destroy(ctx);
        cam->mpp_ctx = nullptr; cam->mpi = nullptr;
        return -1;
    }
    size_t nv12_size = (size_t)cam->stream_width * cam->stream_height * 3 / 2;
    for (int i = 0; i < N_NV12; i++) {
        MppBufferInfo info;
        memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_DRM;
        info.size = nv12_size;
        info.fd   = cam->nv12_dma_fd[i];
        MppBuffer mbuf = nullptr;
        if (mpp_buffer_import_with_tag(cam->mpp_group, &info, &mbuf, NULL, __func__) != MPP_OK || !mbuf) {
            fprintf(stderr, "camera %d: mpp_buffer_import(%d) failed\n", cam->cam_id, i);
            mpp_destroy(ctx);
            cam->mpp_ctx = nullptr; cam->mpi = nullptr;
            return -1;
        }
        cam->mpp_buf[i] = mbuf;
    }

    printf("camera %d: MPP H.265 encoder ready (%dx%d, %d Mbps CBR, gop=%d)\n",
           cam->cam_id, cam->stream_width, cam->stream_height,
           RTSP_ENCODE_BPS / 1000000, RTSP_ENCODE_GOP);
    return 0;
}

// The encoder thread. Drains the NV12 ring FIFO in order, hands each fd to MPP
// (encode_put_frame), retrieves the encoded H.265 access unit, copies the few
// KB into a GstBuffer and broadcasts it to every connected client's appsrc.
static void mpp_encode_thread_func(CameraPipeline *cam) {
    WorkerExitFlag wd_flag(cam->wd_exited[WD_ROLE_ENC]);
    printf("MPP encode thread[%d] started.\n", cam->cam_id);

    while (g_running) {
        // Same early exit as the inference thread: no capture, nothing to encode.
        if (cam->capture_ready.load(std::memory_order_acquire) < 0)
            break;
        int idx = -1;
        {
            std::lock_guard<std::mutex> lock(cam->enc_fifo_mutex);
            if (!cam->enc_fifo.empty()) {
                idx = cam->enc_fifo.front();
                cam->enc_fifo.pop_front();
            }
        }
        if (idx < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        MppFrame mframe = nullptr;
        if (mpp_frame_init(&mframe) != MPP_OK) {
            cam->nv12_inflight[idx].store(0, std::memory_order_release);
            continue;
        }
        mpp_frame_set_fmt(mframe, MPP_FMT_YUV420SP);
        mpp_frame_set_width(mframe, cam->stream_width);
        mpp_frame_set_height(mframe, cam->stream_height);
        mpp_frame_set_hor_stride(mframe, cam->stream_width);
        mpp_frame_set_ver_stride(mframe, cam->stream_height);
        mpp_frame_set_pts(mframe, (RK_S64)(cam->out_frames * (1000000 / RTSP_ENCODE_FPS)));
        mpp_frame_set_buffer(mframe, cam->mpp_buf[idx]);   // imported fd, no copy

        int ret = cam->mpi->encode_put_frame(cam->mpp_ctx, mframe);
        mpp_frame_deinit(&mframe);
        if (ret != MPP_OK) {
            fprintf(stderr, "camera %d: encode_put_frame failed %d\n", cam->cam_id, ret);
            cam->nv12_inflight[idx].store(0, std::memory_order_release);
            continue;
        }

        // Blocking: returns when this frame is encoded (single frame in flight).
        MppPacket pkt = nullptr;
        ret = cam->mpi->encode_get_packet(cam->mpp_ctx, &pkt);
        if (ret == MPP_OK && pkt) {
            void *data = mpp_packet_get_pos(pkt);
            size_t len  = mpp_packet_get_length(pkt);
            if (data && len > 0) {
                GstBuffer *buffer = gst_buffer_new_allocate(nullptr, len, nullptr);
                GstMapInfo map;
                if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
                    memcpy(map.data, data, len);
                    gst_buffer_unmap(buffer, &map);

                    cam->out_frames++;
                    cam->wd_hb_us[WD_ROLE_ENC].store(now_monotonic_us(),
                                                     std::memory_order_release);

                    // Broadcast to every connected client's appsrc (temp refs).
                    std::vector<GstElement *> live;
                    {
                        std::lock_guard<std::mutex> lock(cam->appsrc_mutex);
                        for (GstElement *a : cam->appsrcs) {
                            gst_object_ref(a);
                            live.push_back(a);
                        }
                    }
                    for (GstElement *a : live) {
                        gst_app_src_push_buffer(GST_APP_SRC(a), gst_buffer_ref(buffer));
                        gst_object_unref(a);
                    }
                    // Release the base reference from gst_buffer_new_allocate: each
                    // gst_app_src_push_buffer() consumes only the per-client +1 copy,
                    // so failing to unref here leaks one GstBuffer per encoded frame
                    // (≈ the H.265 bytes of that frame, ~16.7 KB/frame at 4 Mbps @30fps).
                    // With zero clients the loop body never runs, and this unref is
                    // exactly what returns the buffer to the pool.
                    gst_buffer_unref(buffer);
                } else {
                    gst_buffer_unref(buffer);
                }
            }
            mpp_packet_deinit(&pkt);
        }

        // MPP is done with this slot: the capture thread may rewrite it now.
        cam->nv12_inflight[idx].store(0, std::memory_order_release);
    }
    printf("MPP encode thread[%d] stopped.\n", cam->cam_id);
}

// ============== RTSP server ==============
static void on_appsrc_removed(GstBin *bin, GstElement *element, gpointer user_data) {
    CameraPipeline *cam = (CameraPipeline *)user_data;
    if (!GST_IS_APP_SRC(element)) return;
    std::lock_guard<std::mutex> lock(cam->appsrc_mutex);
    auto &v = cam->appsrcs;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (*it == element) {
            gst_object_unref(*it);
            v.erase(it);
            return;
        }
    }
}

static void media_configure(GstRTSPMediaFactory *factory, GstRTSPMedia *media,
                            gpointer user_data) {
    CameraPipeline *cam = (CameraPipeline *)user_data;
    GstElement *pipeline = gst_rtsp_media_get_element(media);
    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysrc");
    if (!appsrc) { std::cerr << "No mysrc" << std::endl; return; }

    // Encoded H.265 (Annex-B byte stream, one access unit per buffer) produced
    // by the external MPP encoder, not raw NV12.
    GstCaps *caps = gst_caps_new_simple("video/x-h265",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment",     G_TYPE_STRING, "au",
        "width",  G_TYPE_INT, cam->stream_width,
        "height", G_TYPE_INT, cam->stream_height,
        "framerate", GST_TYPE_FRACTION, RTSP_ENCODE_FPS, 1, NULL);

    g_object_set(appsrc,
        "caps", caps,
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        "do-timestamp", TRUE,   // stamp with THIS media's own pipeline clock
        "block", FALSE,
        "max-buffers", 4,
        nullptr);

    gst_caps_unref(caps);

    if (g_running) {
        gst_object_ref(appsrc);   // registration ref
        {
            std::lock_guard<std::mutex> lock(cam->appsrc_mutex);
            cam->appsrcs.push_back(appsrc);
        }
        g_signal_connect(pipeline, "element-removed",
                         G_CALLBACK(on_appsrc_removed), cam);
    }
    gst_object_unref(appsrc);
}

static void start_rtsp_server(int port, CameraPipeline *cam0, CameraPipeline *cam1) {
    GstRTSPServer *server = gst_rtsp_server_new();
    g_object_set(server, "service", std::to_string(port).c_str(), NULL);

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);

    // No encoder in the pipeline: appsrc already carries encoded H.265.
    // h265parse config-interval=-1 re-injects SPS/PPS before every IDR so a
    // late-joining client can start decoding at the next keyframe.
    const char *launch =
        "( appsrc name=mysrc is-live=true format=time "
        "! queue max-size-buffers=4 leaky=downstream "
        "! h265parse config-interval=-1 "
        "! queue max-size-time=200000000 max-size-buffers=4 leaky=downstream "
        "! rtph265pay name=pay0 pt=96 )";

    if (cam0->enabled) {
        GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_launch(factory, launch);
        gst_rtsp_media_factory_set_shared(factory, FALSE);
        g_signal_connect(factory, "media-configure",
                         G_CALLBACK(media_configure), cam0);
        gst_rtsp_mount_points_add_factory(mounts, "/live0", factory);
    }
    if (cam1->enabled) {
        GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_launch(factory, launch);
        gst_rtsp_media_factory_set_shared(factory, FALSE);
        g_signal_connect(factory, "media-configure",
                         G_CALLBACK(media_configure), cam1);
        gst_rtsp_mount_points_add_factory(mounts, "/live1", factory);
    }

    g_object_unref(mounts);
    gst_rtsp_server_attach(server, NULL);
    if (cam0->enabled) printf("RTSP server ready: rtsp://<board-ip>:%d/live0\n", port);
    if (cam1->enabled) printf("RTSP server ready: rtsp://<board-ip>:%d/live1\n", port);
}

// ============== Resource cleanup ==============
static void cleanup_camera(CameraPipeline *cam) {
    {
        std::lock_guard<std::mutex> lock(cam->appsrc_mutex);
        for (GstElement *a : cam->appsrcs) gst_object_unref(a);
        cam->appsrcs.clear();
    }

    if (cam->v4l_fd >= 0) close(cam->v4l_fd);

    for (int i = 0; i < N_NV12; i++) {
        if (cam->nv12_dma_fd[i] >= 0) close(cam->nv12_dma_fd[i]);
    }
    for (int i = 0; i < BUF_NUM; i++) {
        if (cam->stream_mapped[i]) munmap(cam->stream_mapped[i],
                                          cam->stream_width * cam->stream_height * 3);
        if (cam->stream_dma_fd[i] >= 0) close(cam->stream_dma_fd[i]);
        if (cam->model_mapped[i]) munmap(cam->model_mapped[i],
                                         cam->model_width * cam->model_height * 3);
        if (cam->model_dma_fd[i] >= 0) close(cam->model_dma_fd[i]);
    }

    if (cam->temp_letterbox_mapped) munmap(cam->temp_letterbox_mapped,
                                           cam->temp_letterbox_w * cam->temp_letterbox_h * 3);
    if (cam->temp_letterbox_fd >= 0) close(cam->temp_letterbox_fd);

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (cam->v4l2_bufs[i].dma_fd >= 0) close(cam->v4l2_bufs[i].dma_fd);
    }

    // MPP teardown: release the encoder context, then the imported-buffer group.
    if (cam->mpp_ctx) { mpp_destroy(cam->mpp_ctx); cam->mpp_ctx = nullptr; cam->mpi = nullptr; }
    if (cam->mpp_group) { mpp_buffer_group_put(cam->mpp_group); cam->mpp_group = nullptr; }

    release_yolov8_model(&cam->rknn_app_ctx);
}

static void cleanup() {
    result_socket_stop();

    for (int i = 0; i < MAX_CAMS; i++) {
        cleanup_camera(&g_cams[i]);
    }
}

// ============== main ==============
int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (argc < 2) {
        printf("Usage: %s <model_path> [v4l2_node_cam0] [v4l2_node_cam1] [rtsp_port] [result_port]\n",
               argv[0]);
        return -1;
    }
    const char *model_path = argv[1];
    const char *node0 = (argc > 2) ? argv[2] : DEFAULT_V4L2_NODE_CAM0;
    const char *node1 = (argc > 3) ? argv[3] : DEFAULT_V4L2_NODE_CAM1;
    int rtsp_port = (argc > 4) ? atoi(argv[4]) : DEFAULT_RTSP_PORT;
    socket_port = (argc > 5) ? atoi(argv[5]) : DEFAULT_SOCKET_PORT;

    init_post_process();

    g_cams[0].cam_id = 0;
    g_cams[0].v4l2_node = node0;
    g_cams[0].core_mask = RKNN_NPU_CORE_0;
    g_cams[1].cam_id = 1;
    g_cams[1].v4l2_node = node1;
    g_cams[1].core_mask = RKNN_NPU_CORE_1;

    for (int ci = 0; ci < MAX_CAMS; ci++) {
        CameraPipeline *cam = &g_cams[ci];
        printf("=== Initializing camera %d (%s) ===\n", cam->cam_id, cam->v4l2_node);

        memset(&cam->rknn_app_ctx, 0, sizeof(cam->rknn_app_ctx));
        if (init_yolov8_model(model_path, &cam->rknn_app_ctx) != 0) {
            printf("camera %d: init_yolov8_model failed, disabling\n", ci);
            cam->enabled = false;
            continue;
        }
        cam->model_width = cam->rknn_app_ctx.model_width;
        cam->model_height = cam->rknn_app_ctx.model_height;
        printf("camera %d: model input %dx%d\n", ci, cam->model_width, cam->model_height);

        int cm = rknn_set_core_mask(cam->rknn_app_ctx.rknn_ctx, cam->core_mask);
        if (cm != 0) {
            printf("camera %d: WARNING rknn_set_core_mask=%d (continuing in AUTO)\n", ci, cm);
        }

        bool dma_ok = true;
        for (int i = 0; i < BUF_NUM; i++) {
            if (alloc_dmabuf(cam->stream_width * cam->stream_height * 3,
                             &cam->stream_dma_fd[i], &cam->stream_mapped[i]) < 0) { dma_ok = false; break; }
            if (alloc_dmabuf(cam->model_width * cam->model_height * 3,
                             &cam->model_dma_fd[i], &cam->model_mapped[i]) < 0) { dma_ok = false; break; }
        }
        // NV12 ring: padded to NV12_VER_STRIDE_PAD rows for MPP aligned access.
        // Not CPU-mapped (RGA writes it, MPP reads it, both DMA).
        for (int i = 0; i < N_NV12; i++) {
            if (alloc_dmabuf((size_t)cam->stream_width * NV12_VER_STRIDE_PAD * 3 / 2,
                             &cam->nv12_dma_fd[i], nullptr) < 0) { dma_ok = false; break; }
        }
        if (!dma_ok) {
            printf("camera %d: DMA buffer alloc failed, disabling\n", ci);
            cam->enabled = false;
            continue;
        }
        printf("camera %d: DMA buffers allocated (stream x2, model x2, nv12 ring x%d)\n",
               ci, N_NV12);

        if (v4l2_init(cam, cam->v4l2_node, cam->capture_width, cam->capture_height) < 0) {
            printf("camera %d: V4L2 init failed, disabling\n", ci);
            cam->enabled = false;
            continue;
        }

        // External MPP H.265 encoder over the NV12 ring (zero-copy fd import).
        if (init_mpp_encoder(cam) != 0) {
            printf("camera %d: MPP encoder init failed, disabling\n", ci);
            cam->enabled = false;
            continue;
        }
    }

    std::vector<std::thread> threads;
    for (int ci = 0; ci < MAX_CAMS; ci++) {
        CameraPipeline *cam = &g_cams[ci];
        if (!cam->enabled) { printf("camera %d disabled, skipping threads\n", ci); continue; }
        threads.emplace_back(capture_thread_func, cam);
        threads.emplace_back(inference_thread_func, cam);
        threads.emplace_back(mpp_encode_thread_func, cam);
    }

    // Wait for each capture thread to reach STREAMON so that a camera whose
    // device opens but cannot stream (STREAMON EPERM) is not mounted on RTSP.
    for (int ci = 0; ci < MAX_CAMS; ci++) {
        CameraPipeline *cam = &g_cams[ci];
        if (!cam->enabled) continue;
        for (int tries = 0; tries < 50 && cam->capture_ready.load() == 0; tries++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (cam->capture_ready.load(std::memory_order_acquire) < 0) {
            printf("camera %d: capture thread could not stream, disabling\n", ci);
            cam->enabled = false;
        }
    }

    start_rtsp_server(rtsp_port, &g_cams[0], &g_cams[1]);

    // Watchdog supervises the worker threads from here on.
    std::thread watchdog(watchdog_thread_func);

    if (result_socket_start((uint16_t)socket_port,
                            (uint32_t)g_cams[0].stream_width,
                            (uint32_t)g_cams[0].stream_height) < 0) {
        printf("WARNING: result socket failed to start on port %d, continuing "
               "without it\n", socket_port);
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    while (g_running) {
        g_main_context_iteration(NULL, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    g_main_loop_unref(loop);

    for (auto &t : threads) t.join();
    if (watchdog.joinable()) watchdog.join();

    cleanup();
    printf("Exit normally\n");
    return 0;
}
