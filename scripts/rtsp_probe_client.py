#!/usr/bin/env python3
# Quick RTSP probe client: does the full handshake over TCP interleaved and
# counts recv() calls (RTP data) over N seconds. Usage: rtsp_probe_client.py <host> <port> [duration]
import socket, time, re, sys

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.50"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8554
dur = int(sys.argv[3]) if len(sys.argv) > 3 else 4

s = socket.create_connection((host, port), timeout=5)
s.settimeout(5)
cseq = [0]
sess = [None]

def req(method, uri, extra=""):
    cseq[0] += 1
    extra += f"Session: {sess[0]}\r\n" if sess[0] else ""
    s.sendall(f"{method} {uri} RTSP/1.0\r\nCSeq: {cseq[0]}\r\n{extra}\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        c = s.recv(4096)
        if not c:
            break
        buf += c
    h, _, b = buf.partition(b"\r\n\r\n")
    m = re.search(rb"Session:\s*(\S+)", h)
    if m:
        sess[0] = m.group(1).decode()
    return h.split(b"\r\n")[0].decode()

req("OPTIONS", f"rtsp://{host}:{port}/live0")
st = req("DESCRIBE", f"rtsp://{host}:{port}/live0", "Accept: application/sdp\r\n")
print(f"[probe] DESCRIBE: {st}", flush=True)
st = req("SETUP", f"rtsp://{host}:{port}/live0/", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
print(f"[probe] SETUP: {st}", flush=True)
st = req("PLAY", f"rtsp://{host}:{port}/live0/")
print(f"[probe] PLAY: {st}", flush=True)
s.settimeout(1)
pkts = 0
t0 = time.time()
while time.time() - t0 < dur:
    try:
        d = s.recv(65536)
        if not d:
            print(f"[probe] closed @ {time.time()-t0:.1f}s", flush=True)
            break
        pkts += 1
    except socket.timeout:
        pass
print(f"[probe] recv calls={pkts} over {dur}s", flush=True)
s.close()
