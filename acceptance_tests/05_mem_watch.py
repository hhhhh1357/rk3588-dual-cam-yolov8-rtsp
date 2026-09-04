#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# =============================================================
# 05_mem_watch.py —— 稳定性验收：内存泄漏检查（VmRSS 曲线）
#
# 测试原理：
#   真正的泄漏很难用 valgrind 在板子上跑（太重），工程上最实用的信号是
#   RSS 曲线：进程虚拟内存只涨不回落 = 泄漏的典型特征。
#   做法：每隔 interval 秒 ssh 读一次 /proc/<pid>/status 的 VmRSS，
#   记录成时间序列，最后算"涨速"（MB/小时）。偶发抖动是正常的，
#   所以用整段的净增长来判定，而不是看单次采样。
#
# 输出（两个文件都在 artifacts/）:
#   memwatch_<时间戳>.csv   —— 原始数据：每条采样一行(时间+VmRSS)
#   memwatch_<时间戳>.log   —— 配置 + 结论 + 涨速分析
#
# 判定：
#   平均涨速 ≤ 阈值(默认 10MB/h) → PASS；持续涨速大 → FAIL(有泄漏)。
#
# 用法:
#   python3 05_mem_watch.py --samples 60 --interval 60     # 1 小时
#   python3 05_mem_watch.py --duration-h 24               # 或直接跑 24 小时
# =============================================================
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "artifacts")
os.makedirs(ART, exist_ok=True)

def ssh_one(host, user, cmd):
    p = subprocess.run(["ssh", "-o", "BatchMode=yes", "%s@%s" % (user, host), cmd],
                       capture_output=True, text=True, timeout=30)
    if p.returncode != 0:
        raise RuntimeError("ssh 失败: %s" % p.stderr.strip())
    return p.stdout.strip()

def main():
    ap = argparse.ArgumentParser(description="VmRSS 内存泄漏监控")
    ap.add_argument("--host", default="192.168.1.50")
    ap.add_argument("--user", default="root")
    ap.add_argument("--pattern", default="rknn_yolov8_dual_mpp")
    ap.add_argument("--samples", type=int, default=60)
    ap.add_argument("--interval", type=int, default=60, help="采样间隔秒")
    ap.add_argument("--duration-h", type=float, default=0,
                    help="若>0 则按小时换算 samples，覆盖 --samples")
    ap.add_argument("--limit-mb-h", type=float, default=10.0, help="判定阈值 MB/小时")
    ap.add_argument("--out", default=None, help="CSV 数据文件路径")
    ap.add_argument("--log", default=None, help="运行日志路径")
    a = ap.parse_args()

    if a.duration_h > 0:
        a.samples = max(3, int(a.duration_h * 3600 / a.interval))

    stamp = time.strftime("%Y%m%d_%H%M")
    out = a.out or os.path.join(ART, "memwatch_%s.csv" % stamp)
    log_path = a.log or os.path.join(ART, "memwatch_%s.log" % stamp)
    lf = open(log_path, "w", buffering=1)
    lf.write("========== 05 内存泄漏监控 %s ==========\n" % time.strftime("%F %T"))
    lf.write("host=%s pattern=%s samples=%d interval=%ds 阈值=%.1fMB/h\n"
             % (a.host, a.pattern, a.samples, a.interval, a.limit_mb_h))
    lf.write("数据文件: %s\n" % out)

    def emit(msg):
        print(msg, flush=True)
        lf.write(msg + "\n")

    # 定位真正的进程。注意：pgrep -f 会同时命中外层启动壳
    # (bash -c "…setsid ./rknn_yolov8_dual_mpp…"，命令行里含同样字符串)，
    # head -1 会取到那个 bash 壳 → 采到的是 260kB 恒定的假进程。
    # 所以必须再按 /proc/<pid>/exe 的真实可执行文件名过滤一次。
    app_bin = a.pattern.rsplit('/', 1)[-1]
    pat = "[%s]%s" % (a.pattern[0], a.pattern[1:])
    resolver = ("p=''; for q in $(pgrep -f '%s'); do "
                "e=$(readlink /proc/$q/exe 2>/dev/null); "
                "[ -n \"$e\" ] && [ \"$(basename \"$e\")\" = '%s' ] "
                "&& p=$q && break; done; echo \"$p\"" % (pat, app_bin))
    try:
        pid = ssh_one(a.host, a.user, resolver)
    except RuntimeError as e:
        emit("无法连接板子: %s" % e); lf.close(); return 2
    if not pid:
        emit("板上没找到进程 %s (按 exe 名 %s)" % (a.pattern, app_bin))
        lf.close(); return 2
    emit("监控进程 pid=%s (%s)" % (pid, a.pattern))

    rows = []
    t_start = time.time()          # 用主机单调时钟算斜率(板子 RTC 可能乱)
    with open(out, "w") as f:
        f.write("epoch_ms,t_rel_s,vmrss_kb\n")
        for i in range(a.samples):
            cmd = "awk '/VmRSS/{print $2}' /proc/%s/status" % pid
            try:
                rss_kb = int(ssh_one(a.host, a.user, cmd))
            except (RuntimeError, ValueError) as e:
                emit("第 %d 次采样失败(进程还在吗?): %s" % (i, e)); break
            th = time.time()
            rows.append((th, rss_kb))
            f.write("%.0f,%.1f,%d\n" % (th * 1000, th - t_start, rss_kb))
            f.flush()          # 逐行落盘，中途 Ctrl-C 也不丢已采样本
            print("t=%5.1fs  VmRSS=%6d kB" % (th - t_start, rss_kb), flush=True)
            if i < a.samples - 1:
                time.sleep(a.interval)

    if len(rows) < 2:
        emit("样本太少，无法判定")
        lf.close(); return 1
    t0, r0 = rows[0]
    t1, r1 = rows[-1]
    dur_h = (t1 - t0) / 3600.0
    grow_kb = r1 - r0
    rate = (grow_kb / 1024.0) / dur_h if dur_h > 0 else 0.0
    emit("------------------------------------------")
    emit("时长 %.2f h  净增长 %+.0f kB  平均涨速 %.2f MB/h" % (dur_h, grow_kb, rate))
    ok = rate <= a.limit_mb_h
    emit("结果: %s (阈值 %.1f MB/h)" % ("PASS" if ok else "FAIL", a.limit_mb_h))
    emit("数据文件: %s" % out)
    emit("运行日志: %s" % log_path)
    lf.close()
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
