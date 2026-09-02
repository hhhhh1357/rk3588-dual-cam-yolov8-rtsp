#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# result_receiver_demo.py  (v2, dual-camera)
#
# Demo TCP client for the detection-result socket pushed by
# v4l2_yolov_dual.cpp. Two cameras share ONE socket; every message carries a
# cam_id so the receiver can tell which camera detected what.
#
#   python3 result_receiver_demo.py [host] [port]   (defaults 127.0.0.1 9000)
#
# This client deliberately exercises BOTH TCP hazards the framing is designed
# against:
#   * 拆包 (fragmentation): one message split across several recv() calls ->
#     keep a leftover buffer, read exactly header_len bytes, then exactly
#     payload_len bytes; never assume one recv == one message.
#   * 粘包 (sticky packets): several messages coalesced into one recv() ->
#     after consuming one message, loop and consume the next complete one that
#     is already sitting in the leftover buffer before recv()-ing again.
#
# The server sends a message ONLY when at least one object is detected (the
# operator's requirement), so you will see gaps between prints whenever the
# scene has nothing recognizable.
#
# Wire format (all multi-byte fields little-endian):
#   Header (16 B): magic u32 = 0x5A5A5A5A | version u16=2 | header_len u16=16
#                  | payload_len u32 | reserved u32
#   Payload:       frame_id u32 | cam_id u32 | img_w u32 | img_h u32 | obj_count u32
#                  then obj_count x { cls_id i32 | prop f32 | label[32] |
#                                     left i32 | top i32 | right i32 | bottom i32 }

import socket
import struct
import sys

MSG_MAGIC = 0x5A5A5A5A
MSG_VERSION = 2
MSG_HEADER_LEN = 16
OBJ_BYTES = 56          # cls_id(4) + prop(4) + label(32) + box(16)
PAYLOAD_FIXED = 20      # frame_id + cam_id + img_w + img_h + obj_count

HEADER_FMT = "<IHHII"   # magic, version, header_len, payload_len, reserved
FIXED_FMT = "<IIIII"    # frame_id, cam_id, img_w, img_h, obj_count
OBJ_FMT = "<if32siiii"  # cls_id, prop, label, left, top, right, bottom


def recv_exact(sock, n):
    """Read exactly n bytes, looping across partial reads (拆包)."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed by peer")
        buf += chunk
    return buf


def parse_message(msg):
    """Parse one complete framed message. Returns (cam_id, frame_id, w, h, dets)."""
    assert len(msg) >= MSG_HEADER_LEN + PAYLOAD_FIXED
    magic, version, header_len, payload_len, _ = struct.unpack_from(HEADER_FMT, msg, 0)
    assert magic == MSG_MAGIC, "bad magic 0x%08X" % magic
    assert version == MSG_VERSION and header_len == MSG_HEADER_LEN
    assert len(msg) == MSG_HEADER_LEN + payload_len, "payload length mismatch"

    fid, cam_id, w, h, obj_count = struct.unpack_from(FIXED_FMT, msg, MSG_HEADER_LEN)
    assert obj_count <= 128

    off = MSG_HEADER_LEN + PAYLOAD_FIXED
    dets = []
    for _ in range(obj_count):
        cls_id, prop, label, left, top, right, bottom = \
            struct.unpack_from(OBJ_FMT, msg, off)
        label = label.split(b"\x00", 1)[0].decode("utf-8", "replace")
        dets.append((cls_id, label, prop, left, top, right, bottom))
        off += OBJ_BYTES
    return cam_id, fid, w, h, dets


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000

    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(10)
    print(f"connected to {host}:{port}, waiting for detection frames (Ctrl-C to quit)...")

    leftover = b""
    frame_no = 0
    while True:
        # Consume every complete message already buffered before recv()-ing
        # again. This is the 粘包 case: one recv() can hold several messages.
        while len(leftover) >= MSG_HEADER_LEN:
            _, _, _, payload_len, _ = struct.unpack_from(HEADER_FMT, leftover, 0)
            total = MSG_HEADER_LEN + payload_len
            if len(leftover) < total:
                break          # 拆包 case: this message is incomplete, read more
            msg, leftover = leftover[:total], leftover[total:]
            frame_no += 1
            cam_id, fid, w, h, dets = parse_message(msg)
            print(f"--- frame #{frame_no} cam {cam_id} (seq {fid}) {w}x{h}, "
                  f"{len(dets)} objects ---")
            for cls_id, label, prop, left, top, right, bottom in dets:
                print(f"    {label:>12s} cls={cls_id:<3d} conf={prop:.3f} "
                      f"box=({left},{top})-({right},{bottom})")

        # Wait for more bytes; append them to the leftover buffer.
        chunk = s.recv(65536)
        if not chunk:
            print("connection closed by server")
            break
        leftover += chunk


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nquit")
    except ConnectionError as e:
        print(f"error: {e}")
        sys.exit(1)
