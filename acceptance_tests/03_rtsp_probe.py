#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# =============================================================
# 03_rtsp_probe.py —— RTSP(RTP/AVP/TCP) 质量探针
#
# 为什么不用 ffplay/ffprobe 干这件事？
#   它们只能告诉你"能不能看"，拿不到 RTP 层细节。本脚本自己实现
#   RTSP 握手(OPTIONS/DESCRIBE/SETUP/PLAY)，然后直接读 interleaved
#   RTP 包，能做三件 ffplay 做不到的事：
#     1) 序列号(seq)连续性统计 —— RTP 层丢包的最直接证据；
#     2) RTP 时间戳(ts)是否单调推进 —— 判断"流是不是真在动"（有的
#        故障是画面冻结但连接还活着）；
#     3) 程序化输出(CSV)，才能被 04 长稳压测循环调用、累计成曲线。
#
# 测试原理：
#   · H.265 一帧常被拆成多个 RTP 包(FU-A)，所以这里"数包"≠"数帧"，
#     要测帧率请用 02(ffmpeg 解码数帧)。
#   · 本脚本测的是 RTP 传输质量：有没有丢包(seq 缺口)、有没有停流。
#
# 用法:
#   python3 03_rtsp_probe.py --path live0 --window 10
#   python3 03_rtsp_probe.py --csv stability_live0.csv   # 追加一行
#
# 判定:
#   seq 缺口数 = 0 且 收到 RTP 包 > 0  → exit 0 (PASS)
#   否则 → exit 1，便于 shell 脚本直接拿返回值判失败
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

def log(msg):
    print(msg, flush=True)

# ---------------- RTSP over TCP 消息层 ----------------

def send_request(sock, cseq, method, uri, extra_hdrs=""):
    req = ("%s %s RTSP/1.0\r\n"
           "CSeq: %d\r\n"
           "User-Agent: rtsp_probe\r\n"
           "%s"
           "\r\n") % (method, uri, cseq, extra_hdrs)
    sock.sendall(req.encode("latin1"))

def read_response(sock, buf):
    """读一个完整 RTSP 响应；返回 (status, headers, body, 剩余buf)。
    buf 里可能已积了上一个响应之后立刻到达的 RTP interleaved 数据，
    必须原样吐回，不能丢。"""
    while b"\r\n\r\n" not in buf:
        d = sock.recv(4096)
        if not d:
            raise ConnectionError("连接被关闭(等响应头时)")
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    clen = int(headers.get("content-length", "0") or 0)
    while len(rest) < clen:
        rest += sock.recv(4096)
    body = rest[:clen]
    return status, headers, body, rest[clen:]

# ---------------- interleaved RTP 读取 ----------------

def next_rtp_chunk(sock, buf):
    """从 TCP 流里取下一个 interleaved 块，返回 (channel, payload, 剩余buf)。
    RTSP-over-TCP 的帧格式：0x24 '$' + channel(1B) + 长度(2B 大端) + 载荷。
    非 0x24 开头说明对端发了我们没解析的 RTSP 数据，丢弃一字节继续找。"""
    while True:
        while len(buf) < 4:
            d = sock.recv(4096)
            if not d:
                return None, None, buf
            buf += d
        if buf[0] != 0x24:
            buf = buf[1:]          # 头部残留，跳过
            continue
        channel = buf[1]
        length = struct.unpack(">H", buf[2:4])[0]
        while len(buf) < 4 + length:
            d = sock.recv(4096)
            if not d:
                return None, None, buf
            buf += d
        payload = buf[4:4 + length]
        buf = buf[4 + length:]
        return channel, payload, buf

def parse_rtp(payload):
    """解析 RTP 头(12B)。返回 dict 或 None(不是有效 RTP)。"""
    if len(payload) < 12 or (payload[0] >> 6) != 2:
        return None
    b1 = payload[1]
    pt = b1 & 0x7F
    if pt >= 192:                      # >=192 是 RTCP(SR=200 等)，不是 RTP
        return None
    return {
        "pt": pt,
        "seq": struct.unpack(">H", payload[2:4])[0],
        "ts": struct.unpack(">I", payload[4:8])[0],
        "ssrc": struct.unpack(">I", payload[8:12])[0],
    }

# ---------------- RTSP 会话流程 ----------------

def do_probe(host, port, path, window, stall=0.0):
    """window>0 正常探测；stall>0 时 PLAY 后休眠 stall 秒且不读数据，
    用于模拟"拉了流但不消费"的慢客户端（服务端写缓冲会被灌满）。"""
    url = "rtsp://%s:%d/%s" % (host, port, path)
    s = socket.create_connection((host, port), timeout=5)
    s.settimeout(2)
    buf = b""
    cseq = 0

    def req(method, uri, extra=""):
        nonlocal cseq
        cseq += 1
        send_request(s, cseq, method, uri, extra)
        st, hd, body, buf2 = read_response(s, buf)   # 注意 buf 要带回来
        return st, hd, body, buf2

    st, _, _, buf = req("OPTIONS", url)
    if st != 200:
        raise RuntimeError("OPTIONS 返回 %d" % st)

    st, _, sdp, buf = req("DESCRIBE", url, "Accept: application/sdp\r\n")
    if st != 200 or not sdp:
        raise RuntimeError("DESCRIBE 返回 %d (拿不到 SDP)" % st)

    # SDP 里 a=control:xxx 是媒体轨地址；拼成完整 URL 用于 SETUP
    control = ""
    for line in sdp.decode("latin1").splitlines():
        if line.lower().startswith("a=control:"):
            control = line[len("a=control:"):].strip()
    if control and not control.startswith("rtsp://"):
        control = url.rstrip("/") + "/" + control

    track = control or url
    st, setup_hdrs, _, buf = req("SETUP", track,
                                 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    if st != 200:
        raise RuntimeError("SETUP 返回 %d (TCP interleaved 传输协商失败)" % st)

    # RTSP 规范：SETUP 成功后，后续 PLAY/TEARDOWN 必须带上响应里的 Session
    # ID（可能带 ;timeout=60 之类的参数，取 ; 前部分）。gst-rtsp-server 严格
    # 校验，缺 Session 会回 454 Session Not Found —— 之前就是这么挂的。
    session = (setup_hdrs.get("session") or "").split(";")[0].strip()
    if not session:
        raise RuntimeError("SETUP 响应里没有 Session 字段")

    st, _, _, buf = req("PLAY", url, "Session: %s\r\n" % session)
    if st != 200:
        raise RuntimeError("PLAY 返回 %d (Session=%s)" % (st, session))

    if stall > 0:
        # 慢客户端模式：握手完成后故意不读，让对端发送缓冲被灌满
        time.sleep(stall)
        s.close()
        return {"url": url, "window": stall, "total": 0, "gaps": 0,
                "idle_tail": stall, "ts_span_90k": 0}

    # ---- 收 RTP，统计到窗口结束 ----
    deadline = time.time() + window
    total = 0
    gaps = 0
    first_ts = last_ts = None
    prev_seq = {}        # ssrc -> 上一个 seq，用于连续性检查
    last_pkt_t = time.time()

    while time.time() < deadline:
        ch, payload, buf = next_rtp_chunk(s, buf)
        if payload is None:                       # 超时或无数据
            continue
        if ch != 0:                               # 0=RTP, 1=RTCP，跳过 RTCP
            continue
        h = parse_rtp(payload)
        if h is None:
            continue
        total += 1
        last_pkt_t = time.time()
        if first_ts is None:
            first_ts = h["ts"]
        last_ts = h["ts"]
        # 序列号连续性（允许 65535→0 回绕）
        if h["ssrc"] in prev_seq:
            exp = (prev_seq[h["ssrc"]] + 1) & 0xFFFF
            if h["seq"] != exp:
                gaps += 1
        prev_seq[h["ssrc"]] = h["seq"]

    idle = time.time() - last_pkt_t
    s.close()

    return {"url": url, "window": window, "total": total, "gaps": gaps,
            "idle_tail": round(idle, 2),
            "ts_span_90k": (last_ts - first_ts) if (first_ts is not None
                                                    and last_ts is not None) else 0}

def main():
    ap = argparse.ArgumentParser(description="RTSP(TCP) 质量探针")
    ap.add_argument("--host", default="192.168.1.50")
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--path", default="live0")
    ap.add_argument("--window", type=float, default=10.0)
    ap.add_argument("--stall", type=float, default=0.0,
                    help="慢客户端模式: PLAY 后休眠该秒数且不读数据")
    ap.add_argument("--csv", default=None, help="追加一行到 CSV")
    ap.add_argument("--log", default=None,
                    help="结果日志文件(默认 artifacts/03_probe_<时间戳>.log, 追加模式)")
    a = ap.parse_args()

    # ---------- 日志：数据 + 结论都落盘，同时打到终端 ----------
    log_path = a.log or os.path.join(ART, "03_probe_%s.log"
                                     % time.strftime("%Y%m%d_%H%M%S"))
    lf = open(log_path, "a", buffering=1)
    lf.write("\n# ===== 03 RTSP 质量探针 %s  host=%s:%d path=%s window=%.0fs =====\n"
             % (time.strftime("%F %T"), a.host, a.port, a.path, a.window))

    def emit(msg):
        print(msg, flush=True)
        lf.write(msg + "\n")

    if a.stall > 0:
        try:
            do_probe(a.host, a.port, a.path, 0, stall=a.stall)
        except Exception as e:
            emit("stall 客户端异常(可忽略, 目的是压服务端): %s" % e)
        lf.close()
        return 0

    try:
        r = do_probe(a.host, a.port, a.path, a.window)
    except Exception as e:
        # 连不上/握手失败也输出一行，方便长稳日志知道"哪个时刻服务不可达"
        ts = time.strftime("%Y-%m-%d %H:%M:%S")
        emit("[%s] %s 探针失败: %s" % (ts, a.path, e))
        if a.csv:
            with open(a.csv, "a") as f:
                f.write("%s,%s,ERR,0,0,0\n" % (ts, a.path))
        lf.close()
        return 2

    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    ok = (r["total"] > 0 and r["gaps"] == 0)
    # RTP 视频时间戳按 90kHz 走：span/90000 = 媒体时长，可交叉验证窗口
    emit("[%s] %s: 收包=%d seq缺口=%d 空闲尾巴=%.2fs ts跨度=%.3fs %s"
         % (ts, a.path, r["total"], r["gaps"], r["idle_tail"],
            r["ts_span_90k"] / 90000.0, "PASS" if ok else "FAIL"))
    emit("结果日志: %s" % log_path)
    if a.csv:
        with open(a.csv, "a") as f:
            f.write("%s,%s,%s,%d,%d,%.2f\n"
                    % (ts, a.path, "PASS" if ok else "FAIL",
                       r["total"], r["gaps"], r["idle_tail"]))
    lf.close()
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
