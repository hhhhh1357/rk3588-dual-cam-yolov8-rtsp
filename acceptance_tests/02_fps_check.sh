#!/usr/bin/env bash
# =============================================================
# 02_fps_check.sh —— 性能验收：实测帧率 + 码率（结果存日志）
#
# 为什么用 ffmpeg 解码数帧，而不是看 RTP 包数?
#   H.265 一帧可能被拆成多个 RTP 包（FU-A 分包），数包 ≠ 数帧。
#   "解码出多少帧" 才是最接近用户看到的帧率，所以这里让 ffmpeg
#   真的解码 WIN 秒，再用 -progress 拿到总帧数。
#
# 码率怎么测才准（-b）：
#   · 错误示范: tcpdump 把每个包打印成一行文本，再数文本行长度 ——
#     一个 1400B 的包被记成 ~80 字符，低估一个数量级（旧版就是这么错的）。
#   · 正确做法: 用 ffprobe -show_entries packet=size 拿到每个视频包的
#     **真实字节数**，累加后除以 pts 时间跨度 → 视频码率。
#
# 输出：本脚本的数据与结论都写入
#   artifacts/02_fps_<时间戳>.log  （终端同步显示）
#
# 用法:
#   ./02_fps_check.sh                      # 测 /live0 帧率
#   ./02_fps_check.sh -l live1             # 测另一路
#   ./02_fps_check.sh -b                   # 追加: 用 ffprobe 实测码率
#
# 怎么判:
#   帧率 ≥ 标称(30fps) × 0.95 = PASS；码率 ≈ 4Mbps CBR ±10% = PASS
# =============================================================
set -uo pipefail
SRC="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# shellcheck source=config.env
source "$SRC/config.env"

LIVE="$LIVE0"
DO_BITRATE=0
while getopts "l:b" o; do
    case "$o" in
        l) LIVE="$OPTARG" ;;
        b) DO_BITRATE=1 ;;
        *) echo "用法: $0 [-l live0|live1] [-b]"; exit 2 ;;
    esac
done

# ---------- 日志：数据 + 结论都进这一个文件 ----------
LOG="$LOG_DIR/02_fps_$(date +%Y%m%d_%H%M%S).log"
say() { printf '%s\n' "$*" | tee -a "$LOG"; }
{
    echo "# ========== 02 帧率/码率测试 $(date '+%F %T') =========="
    echo "# 板子 $BOARD_IP  路 $LIVE  窗口 ${WIN}s  标称帧率 ${FPS_NOMINAL}fps  标称码率 $((BITRATE_BPS/1000000))Mbps"
} > "$LOG"

url="rtsp://${BOARD_IP}:${RTSP_PORT}/${LIVE}"
say "== 实测帧率: $url =="

# --- 帧率 ---
# ffmpeg -progress pipe:1 会把 'frame=NNN' 这样的行打到 stdout
out="$(timeout "$((WIN + 8))" ffmpeg -nostdin -rtsp_transport tcp \
    -i "$url" -t "$WIN" -f null - -progress pipe:1 2>/dev/null)"
frames="$(printf '%s\n' "$out" | grep '^frame=' | tail -1 | tr -d ' ' | cut -d= -f2)"
if [ -z "${frames:-}" ] || ! [[ "$frames" =~ ^[0-9]+$ ]]; then
    say "[FAIL] 无法统计帧数（ffmpeg 可能没拿到流）"
    say "日志: $LOG"
    exit 1
fi
fps="$(python3 -c "print(f'{$frames/$WIN:.2f}')")"
say "  窗口 ${WIN}s 解码 ${frames} 帧 → ${fps} fps"

need="$(python3 -c "print(${FPS_NOMINAL}*0.95)")"
if python3 -c "exit(0 if ${fps} >= ${need} else 1)"; then
    say "[PASS] 帧率达标 (实测 ${fps}fps ≥ 标称×95% = ${need}fps)"
else
    say "[FAIL] 帧率不足 (实测 ${fps}fps < 标称×95% = ${need}fps)"
    say "日志: $LOG"
    exit 1
fi

# --- 码率（可选 -b）---
if [ "$DO_BITRATE" -eq 1 ]; then
    say "== 实测码率: ffprobe 累加视频包真实字节数 (窗口 ${WIN}s) =="
    # python 的输出捕获后统一走日志
    br_out="$(python3 - "$url" "$WIN" "$BITRATE_BPS" <<'PY' 2>&1
import subprocess, sys
url = sys.argv[1]
win = float(sys.argv[2])
nominal = float(sys.argv[3]) / 1e6     # bps -> Mbps

# 关键：直播流没有 EOF，ffprobe 若不限时读就永远不退出。
# -read_intervals "%+N" 让它只读前 N 秒就结束，这样 subprocess 才能正常返回。
intervals = "%%+%d" % int(win)
cmd = ["ffprobe", "-v", "error", "-rtsp_transport", "tcp",
       "-read_intervals", intervals,
       "-select_streams", "v:0",
       "-show_entries", "packet=size,pts_time",
       "-of", "default=noprint_wrappers=1", url]
try:
    p = subprocess.run(cmd, capture_output=True, text=True,
                       timeout=int(win) + 30)
except subprocess.TimeoutExpired:
    print("  [FAIL] ffprobe 拉取超时(仍未退出)"); sys.exit(1)

if p.returncode != 0:
    err = (p.stderr or "").strip().splitlines()
    print("  [FAIL] ffprobe 异常退出 rc=%d: %s"
          % (p.returncode, (err[-1] if err else "无输出")))
    sys.exit(1)

# 用带键名的输出(size= / pts_time=)，不要用 csv=p=0——
# ffprobe 的字段顺序不随 -show_entries 的写法变，csv 无键名容易被列序坑。
# pts 出现顺序不是排序的，所以时长取 max-min。
sizes = []
pts = []
cur = {}
for ln in p.stdout.splitlines():
    if ln.startswith("pts_time="):
        v = ln.split("=", 1)[1]
        cur["pts"] = None if v == "N/A" else float(v)
    elif ln.startswith("size="):
        v = ln.split("=", 1)[1]
        cur["size"] = None if v == "N/A" else int(v)
    if "pts" in cur and "size" in cur:
        if cur["pts"] is not None and cur["size"] is not None:
            pts.append(cur["pts"]); sizes.append(cur["size"])
        cur = {}

if not sizes:
    print("  [FAIL] 没拿到任何视频包"); sys.exit(1)

dur = (max(pts) - min(pts)) if len(pts) > 1 else win
if dur <= 0:
    dur = win
bps = sum(sizes) * 8.0 / dur          # 视频包字节和 / 时间跨度 = 码率
mbps = bps / 1e6

ok = abs(mbps - nominal) <= nominal * 0.10
print("  视频包 %d 个, pts 跨度 %.2fs, 累计 %d 字节"
      % (len(sizes), dur, sum(sizes)))
print("  实测视频码率 ≈ %.2f Mbps (标称 %.1f Mbps, ±10%% 判定)"
      % (mbps, nominal))
print("  %s" % ("[PASS] 码率达标(CBR)" if ok else "[FAIL] 码率偏离标称"))
sys.exit(0 if ok else 1)
PY
)"
    br_rc=$?
    printf '%s\n' "$br_out" | tee -a "$LOG"
    if [ "$br_rc" -ne 0 ]; then
        say "日志: $LOG"
        exit 1
    fi
fi

say "日志: $LOG"
exit 0
