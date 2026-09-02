// result_socket.cc
// TCP server that pushes YOLO detection results to remote clients.
//
// Framing (see result_socket.h for the full wire format): every message is a
// fixed 16-byte header carrying the payload length, followed by exactly that
// many payload bytes. This is the classic length-prefix framing that lets the
// receiver reassemble TCP's byte stream into complete messages, immune to both
// sticky packets (粘包: several messages arriving coalesced in one recv) and
// packet fragmentation (拆包: one message arriving split across several recvs).
//
// v2 changes (dual-camera support):
//   * The payload carries cam_id so the two cameras' detection events stay
//     distinguishable on the one socket.
//   * Delivery is a per-client FIFO broadcast instead of "latest frame wins".
//     result_socket_publish() builds the framed message once, then appends it
//     to every connected client's queue; a dedicated send thread per client
//     drains its own queue. With two inference threads calling publish(), the
//     old single-slot model let one camera's events overwrite the other's
//     before they were delivered. The FIFO guarantees every detection event
//     reaches every client, in publication order.
//   * A client that falls too far behind is dropped, not grown without bound.
//
// Design notes (kept from v1):
//   * One accept thread + one lightweight send thread per connected client.
//   * Client sockets are non-blocking with a bounded pending buffer, so a slow
//     or dead viewer can never stall the NPU inference thread (publish() only
//     copies into per-client queues).
//   * Sends use MSG_NOSIGNAL so a peer reset cannot raise SIGPIPE.

#include "result_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

// ---------------- wire protocol constants ----------------
static const uint32_t MSG_MAGIC      = 0x5A5A5A5Au;
static const uint16_t MSG_VERSION    = 2;
static const uint16_t MSG_HEADER_LEN = 16;
static const uint32_t MSG_RESERVED   = 0;
static const uint32_t PAYLOAD_FIXED  = 20;   // frame_id, cam_id, img_w, img_h, obj_count
static const uint32_t OBJ_BYTES      = 56;   // serialized socket_result_t

// Slow-client guard: if a client has more than this many unsent bytes queued it
// is dropped. At ~7 KB/frame (128 max detections) that is far more than a
// briefly-busy viewer needs.
static const size_t MAX_PENDING_BYTES = (1u << 20);

// ---------------- little-endian serializers ----------------
static void put_u16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xff));
    b.push_back((uint8_t)(v >> 8));
}
static void put_u32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back((uint8_t)(v & 0xff));
    b.push_back((uint8_t)((v >> 8) & 0xff));
    b.push_back((uint8_t)((v >> 16) & 0xff));
    b.push_back((uint8_t)((v >> 24) & 0xff));
}
static void put_f32(std::vector<uint8_t> &b, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    put_u32(b, bits);
}

// ---------------- server state ----------------
static int g_listen_fd = -1;
static uint32_t g_img_w = 0, g_img_h = 0;
static std::atomic<bool> g_running{false};
static pthread_t g_accept_thread;

// Per-client send state. `fd` is owned exclusively by its client thread (set
// before the thread starts); `pending` is guarded by g_clients_mutex. `dropped`
// is written by publish() (when the viewer can't keep up) and read by the
// client thread; it is atomic so no field is ever raced between threads.
typedef struct {
    int fd;                         // owned by the client thread only
    std::atomic<int> dropped{0};    // 1 = too slow, drop this client
    std::vector<uint8_t> pending;   // framed bytes not yet handed to the client thread
} client_t;

// All live clients. publish() appends under this mutex; each client's send
// thread swaps its pending out under it; the accept thread and exiting client
// threads add/remove themselves under it.
static pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<client_t *> g_clients;

// Build one complete framed message from one detection event.
static std::vector<uint8_t> build_frame(uint32_t cam_id, uint32_t frame_id,
                                        const socket_result_t *results,
                                        uint32_t count) {
    std::vector<uint8_t> b;
    b.reserve(MSG_HEADER_LEN + PAYLOAD_FIXED + count * OBJ_BYTES);

    // header
    put_u32(b, MSG_MAGIC);
    put_u16(b, MSG_VERSION);
    put_u16(b, MSG_HEADER_LEN);
    put_u32(b, PAYLOAD_FIXED + count * OBJ_BYTES);
    put_u32(b, MSG_RESERVED);

    // payload: fixed fields
    put_u32(b, frame_id);
    put_u32(b, cam_id);
    put_u32(b, g_img_w);
    put_u32(b, g_img_h);
    put_u32(b, count);

    // payload: per-object fields
    for (uint32_t i = 0; i < count; i++) {
        put_u32(b, (uint32_t)results[i].cls_id);
        put_f32(b, results[i].prop);
        char label_fixed[32];
        memset(label_fixed, 0, sizeof(label_fixed));
        strncpy(label_fixed, results[i].label, sizeof(label_fixed) - 1);
        label_fixed[sizeof(label_fixed) - 1] = '\0';
        b.insert(b.end(), label_fixed, label_fixed + sizeof(label_fixed));
        put_u32(b, (uint32_t)results[i].left);
        put_u32(b, (uint32_t)results[i].top);
        put_u32(b, (uint32_t)results[i].right);
        put_u32(b, (uint32_t)results[i].bottom);
    }
    return b;
}

// One thread per connected client: drains its own FIFO to the socket.
static void *client_thread_func(void *arg) {
    client_t *cl = (client_t *)arg;
    std::vector<uint8_t> to_send;   // bytes grabbed from cl->pending, being sent
    size_t send_off = 0;

    while (g_running.load() && !cl->dropped.load() && cl->fd >= 0) {
        if (send_off >= to_send.size()) {
            // grab whatever new bytes are queued for this client (atomic swap)
            pthread_mutex_lock(&g_clients_mutex);
            if (cl->pending.empty()) {
                pthread_mutex_unlock(&g_clients_mutex);
                usleep(1000);
                continue;
            }
            to_send.clear();
            to_send.swap(cl->pending);
            send_off = 0;
            pthread_mutex_unlock(&g_clients_mutex);
        }

        ssize_t n = send(cl->fd, to_send.data() + send_off,
                         to_send.size() - send_off, MSG_NOSIGNAL);
        if (n > 0) { send_off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            usleep(1000);              // kernel send buffer full; retry soon
            continue;
        }
        cl->fd = -1;                   // EPIPE / ECONNRESET / ... : peer gone
    }

    // deregister + free our own state
    pthread_mutex_lock(&g_clients_mutex);
    for (size_t i = 0; i < g_clients.size(); i++) {
        if (g_clients[i] == cl) { g_clients.erase(g_clients.begin() + (long)i); break; }
    }
    pthread_mutex_unlock(&g_clients_mutex);

    if (cl->fd >= 0) close(cl->fd);
    delete cl;
    return nullptr;
}

// Accept loop: never blocks on any single client.
static void *accept_thread_func(void *) {
    while (g_running.load()) {
        struct pollfd pfd = { g_listen_fd, POLLIN, 0 };
        int pret = poll(&pfd, 1, 500);
        if (pret <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        int cfd = accept(g_listen_fd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        int fl = fcntl(cfd, F_GETFL, 0);
        fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // low latency

        client_t *cl = new client_t;
        cl->fd = cfd;

        pthread_mutex_lock(&g_clients_mutex);
        g_clients.push_back(cl);
        pthread_mutex_unlock(&g_clients_mutex);

        pthread_t tid;
        if (pthread_create(&tid, nullptr, client_thread_func, cl) != 0) {
            pthread_mutex_lock(&g_clients_mutex);
            for (size_t i = 0; i < g_clients.size(); i++) {
                if (g_clients[i] == cl) { g_clients.erase(g_clients.begin() + (long)i); break; }
            }
            pthread_mutex_unlock(&g_clients_mutex);
            close(cfd);
            delete cl;
            continue;
        }
        pthread_detach(tid);
        printf("result-socket: client connected\n");
    }
    return nullptr;
}

int result_socket_start(uint16_t port, uint32_t img_width, uint32_t img_height) {
    if (g_running.load()) return 0;

    g_img_w = img_width;
    g_img_h = img_height;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("result-socket: socket"); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("result-socket: bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        perror("result-socket: listen");
        close(fd);
        return -1;
    }
    g_listen_fd = fd;

    g_running.store(true);
    if (pthread_create(&g_accept_thread, nullptr, accept_thread_func, nullptr) != 0) {
        g_running.store(false);
        close(fd);
        g_listen_fd = -1;
        return -1;
    }
    printf("result-socket: listening on 0.0.0.0:%u\n", (unsigned)port);
    return 0;
}

void result_socket_publish(uint32_t cam_id, uint32_t frame_id,
                           const socket_result_t *results, uint32_t count) {
    if (!g_running.load()) return;

    std::vector<uint8_t> frame = build_frame(cam_id, frame_id, results, count);

    pthread_mutex_lock(&g_clients_mutex);
    for (size_t i = 0; i < g_clients.size(); i++) {
        client_t *cl = g_clients[i];
        if (cl->dropped.load()) continue;
        if (cl->pending.size() + frame.size() > MAX_PENDING_BYTES) {
            cl->dropped.store(1);   // viewer can't keep up; drop it
            continue;
        }
        cl->pending.insert(cl->pending.end(), frame.begin(), frame.end());
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

void result_socket_stop(void) {
    if (!g_running.load()) return;
    g_running.store(false);
    pthread_join(g_accept_thread, nullptr);
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    // Per-client threads are detached: they observe g_running == false on
    // their next loop pass, deregister, close their own socket and free their
    // own state.
    printf("result-socket: stopped\n");
}
