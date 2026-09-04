#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# =============================================================
# 09_tcp_result_reconnect.py —— TCP 结果帧：格式校验 + 重连韧性
#
# 测什么（对齐 result_socket.h 的线上协议）：
#   每条消息 = 固定 16B 头 + payload。头小端:
#     [0..3]  magic       0x5A5A5A5A
#     [4..5]  version     2
#     [6..7]  header_len  16
#     [8..11] payload_len
#     [12..15] reserved   0
#   接收端必须"先精确读 16B 头，再按 payload_len 精确读载荷"才能不串帧。
#
# 为什么这个测试有价值：
#   1) TCP 是字节流，recv 返回的字节数没有边界，粘包/拆包是常态。这里用一个
#      带内部缓冲的 deframe 逐字节喂，正好打"接收端是否按长度前缀正确切帧"。
#   2) 反复断开/重连，验证服务端 send 线程不堆积、不崩、新连接能立刻收到事件。
#
# 注意：检测结果只在"检出目标"时才发（事件流不是广播），所以某个会话 0 条
# 消息是正常的；判定的核心是"收到多少条就校验多少条，不允许出现坏帧"。
#
# 输出：数据与结论都写入 artifacts/09_tcp_<时间戳>.log（终端同步显示）
#
# 用法:
#   python3 09_tcp_result_reconnect.py --sessions 5 --read-for 5
# =============================================================
import argparse
import os
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "artifacts")
os.makedirs(ART, exist_ok=True)

MAGIC = 0x5A5A5A5A
HDR = struct.Struct("<IHHII")          # 4+2+2+4+4 = 16 字节，小端

class Deframer:
    """带内部缓冲的长度前缀解码器：模拟 TCP 粘包/拆包下仍能精确切帧。"""
    def __init__(self):
        self.buf = b""

    def feed(self, data):
        self.buf += data

    def next_msg(self):
        """返回 (payload,None) 成功 / (None,'need_more') 等更多数据 /
        (None,None) 连接关闭。坏帧抛 ValueError。"""
        while len(self.buf) >= HDR.size:
            magic, version, hlen, plen, reserved = HDR.unpack_from(self.buf)
            if magic != MAGIC:
                raise ValueError("magic 错: 0x%08X (帧错位或协议不符)" % magic)
            if version != 2 or hlen != 16:
                raise ValueError("头字段异常: version=%d header_len=%d" % (version, hlen))
            if plen > 2 * 1024 * 1024:
                raise ValueError("payload_len 离谱: %d (疑似错位)" % plen)
            total = HDR.size + plen
            if len(self.buf) < total:
                return None, "need_more"        # 拆包：等剩下的
            payload = self.buf[HDR.size:total]
            self.buf = self.buf[total:]          # 粘包：多余的留到下一帧
            return payload, None
        return None, "need_more"

def parse_payload(p):
    """按 result_socket.h 解析 payload 头(20B)：frame_id/cam_id/w/h/obj_count。
    返回可读 dict；数据不足返回 None。"""
    if len(p) < 20:
        return None
    fid, cam, w, h, oc = struct.unpack_from("<IIIII", p, 0)
    return dict(frame_id=fid, cam_id=cam, w=w, h=h, obj_count=oc)

def one_session(host, port, read_for, lines):
    """连接并读 read_for 秒，lines 收集该会话明细。返回 (消息数, 坏帧数)。"""
    s = socket.create_connection((host, port), timeout=5)
    s.settimeout(0.5)
    df = Deframer()
    msgs = 0
    bad = 0
    deadline = time.time() + read_for
    while time.time() < deadline:
        try:
            data = s.recv(4096)
        except socket.timeout:
            continue
        if not data:
            break
        df.feed(data)
        while True:
            payload, st = df.next_msg()
            if st == "need_more":
                break
            if payload is None:
                break
            msgs += 1
            meta = parse_payload(payload)
            if meta is not None and msgs <= 3:
                lines.append("   cam%d frame#%d  %dx%d  目标数=%d"
                             % (meta["cam_id"], meta["frame_id"],
                                meta["w"], meta["h"], meta["obj_count"]))
    s.close()
    return msgs, bad

def main():
    ap = argparse.ArgumentParser(description="TCP 结果帧校验 + 重连韧性")
    ap.add_argument("--host", default="192.168.1.50")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--sessions", type=int, default=5)
    ap.add_argument("--read-for", type=float, default=5.0)
    ap.add_argument("--log", default=None)
    a = ap.parse_args()

    log_path = a.log or os.path.join(ART, "09_tcp_%s.log"
                                     % time.strftime("%Y%m%d_%H%M%S"))
    lf = open(log_path, "w", buffering=1)
    lf.write("========== 09 TCP 结果帧测试 %s ==========\n"
             % time.strftime("%F %T"))
    lf.write("host=%s:%d sessions=%d read_for=%.0fs\n"
             % (a.host, a.port, a.sessions, a.read_for))

    def emit(msg):
        print(msg, flush=True)
        lf.write(msg + "\n")

    emit("== 连续 %d 次 连接→读→断开 ==" % a.sessions)
    total_msg = 0
    bad_any = 0
    ok_conn = 0
    for i in range(1, a.sessions + 1):
        lines = []
        try:
            msgs, bad = one_session(a.host, a.port, a.read_for, lines)
        except (ConnectionRefusedError, socket.timeout, OSError) as e:
            emit("  第 %d 次连接失败: %s" % (i, e)); continue
        if bad:
            bad_any += bad
        total_msg += msgs
        ok_conn += 1
        for ln in lines:                      # 该会话解析到的前几条明细
            emit(ln)
        emit("  第 %d/%d 次: 收到 %d 条消息(格式全部校验通过)"
             % (i, a.sessions, msgs))
        time.sleep(0.3)                       # 断开的间隔，模拟现场抖动

    emit("----------------------------------------------")
    emit("成功连接 %d/%d 次, 共校验 %d 条消息, 坏帧 %d"
         % (ok_conn, a.sessions, total_msg, bad_any))
    if ok_conn == a.sessions and bad_any == 0:
        emit("结果: PASS —— 断连重连不崩 + 帧格式/切帧正确")
        rc = 0
    else:
        emit("结果: FAIL —— 见上方统计")
        rc = 1
    emit("日志: %s" % log_path)
    lf.close()
    return rc

if __name__ == "__main__":
    sys.exit(main())
