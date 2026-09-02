// result_socket.h
// TCP server that pushes YOLO detection results to remote clients.
//
// Wire protocol (length-prefixed framing, handles TCP 粘包/拆包 on the wire):
//
//   Every message = fixed 16-byte header + payload.
//   A receiver MUST read exactly header_len bytes, parse payload_len, then
//   read exactly payload_len more bytes for that one message.
//
//   Header (16 bytes, all integers little-endian):
//     [0..3]   uint32 magic      = 0x5A5A5A5A
//     [4..5]   uint16 version    = 2
//     [6..7]   uint16 header_len = 16
//     [8..11]  uint32 payload_len   (bytes of payload following the header)
//     [12..15] uint32 reserved   = 0
//
//   Payload (little-endian):
//     [0..3]   uint32 frame_id
//     [4..7]   uint32 cam_id          <- which camera (0 or 1) detected it
//     [8..11]  uint32 img_width
//     [12..15] uint32 img_height
//     [16..19] uint32 obj_count
//     then obj_count x socket_result_t:
//       int32  cls_id
//       float  prop
//       char   label[32]
//       int32  left, top, right, bottom
//
// Little-endian is used end to end because the producer is an RK3588 (ARM64)
// board and typical consumers are little-endian x86/ARM64 or Python on those.
// The magic + length framing is what deframes the stream; a big-endian host
// would need to byte-swap the fields.
//
// Delivery model: this is an EVENT STREAM, not a "latest frame" broadcast.
// result_socket_publish() appends the framed message to every connected
// client's FIFO queue, so each message reaches every client exactly once, in
// publish order. With two cameras publishing into the same socket this is what
// keeps one camera's events from being overwritten by the other's before they
// are delivered. A client that falls too far behind is dropped, never stalled.
//
// Usage (from each camera's inference thread, ONLY when objects are detected):
//   result_socket_start(9000, width, height);
//   result_socket_publish(cam_id, frame_id, results, count);   // count > 0
//   result_socket_stop();

#ifndef RESULT_SOCKET_H_
#define RESULT_SOCKET_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One detection entry, serialized verbatim into the payload above.
typedef struct {
    int32_t cls_id;
    float   prop;
    char    label[32];
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} socket_result_t;

// Start the TCP result server on `port` (0.0.0.0). Image size is attached to
// every frame so clients don't need prior knowledge. Returns 0 on success.
int result_socket_start(uint16_t port, uint32_t img_width, uint32_t img_height);

// Publish one detection event. Thread-safe and never blocks the caller: the
// data is copied out immediately, so `results` can be reused or freed as soon
// as this returns. Broadcast to every connected client in FIFO order. Any
// thread (one per camera) may call this concurrently.
void result_socket_publish(uint32_t cam_id, uint32_t frame_id,
                           const socket_result_t *results, uint32_t count);

// Stop the server, close the listener and every client connection.
// Safe to call even if the server was never started; safe to call once.
void result_socket_stop(void);

#ifdef __cplusplus
}
#endif

#endif // RESULT_SOCKET_H_
