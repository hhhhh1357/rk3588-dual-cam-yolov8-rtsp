#!/usr/bin/env bash
# =============================================================
# 07_concurrent_clients.sh —— 并发验收：多路接入 + 慢客户端不互拖
#
# 设计意图（这是你架构里的核心承诺，专门验证它）：
#   "每个客户端独立 appsrc/编码线程广播 + 非阻塞 + 有界缓冲，
#    一个慢客户端不该拖垮正常客户端，也不该阻塞推理线程。"
#
# 场景构造：
#   · 参考客户端：03 探针拉 live0，全程统计 seq 缺口 —— 它是"正常用户"的代表；
#   · 背景流量：多个 ffmpeg 同时拉 live0/live1（模拟多路接入）；
#   · 慢客户端：ffplay 拉流后 SIGSTOP 冻结它 → 它停止读数据，服务端写给它的
#     缓冲会被灌满。这正是最容易暴露"一慢拖全部"的时刻。
#
# 怎么判：
#   参考探针全程 seq 缺口 = 0 且 正常 ffmpeg 都拉满全程 → PASS。
#   若参考探针出现缺口 = 慢客户端把正常客户端拖垮了（架构承诺失效）。
#
# 用法:  ./07_concurrent_clients.sh -d 20 -n 3
#        -d 持续时间秒(默认20)   -n 背景正常客户端数(默认3)
# =============================================================
set -uo pipefail
SRC="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# shellcheck source=config.env
source "$SRC/config.env"

D=20; NORM_N=3
while getopts "d:n:" o; do
    case "$o" in
        d) D="$OPTARG" ;;
        n) NORM_N="$OPTARG" ;;
        *) echo "用法: $0 [-d 秒] [-n 正常客户端数]"; exit 2 ;;
    esac
done

TS="$(date +%Y%m%d_%H%M%S)"
LOG="$LOG_DIR/07_concurrent_${TS}.log"
: > "$LOG"
url0="rtsp://${BOARD_IP}:${RTSP_PORT}/${LIVE0}"
url1="rtsp://${BOARD_IP}:${RTSP_PORT}/${LIVE1}"

echo "== 并发测试: 时长 ${D}s, 正常客户端 x${NORM_N}, 含 1 个慢客户端 ==" | tee -a "$LOG"

# --- 1) 参考客户端：后台跑探针，它是"正常用户"的裁判 ---
python3 "$SRC/03_rtsp_probe.py" --host "$BOARD_IP" --port "$RTSP_PORT" \
    --path "$LIVE0" --window "$D" >"$LOG_DIR/07_ref_${TS}.log" 2>&1 &
REF_PID=$!

# --- 2) 背景正常客户端：ffmpeg 拉满全程 ---
BG_PIDS=()
for i in $(seq 1 "$NORM_N"); do
    url="$url0"; [ $((i % 2)) -eq 0 ] && url="$url1"
    timeout "$((D + 5))" ffmpeg -nostdin -loglevel error -rtsp_transport tcp \
        -i "$url" -t "$D" -f null - >>"$LOG" 2>&1 &
    BG_PIDS+=($!)
done

# --- 3) 慢客户端：ffplay 拉流后冻结 2s~结束前，不读数据 ---
timeout "$((D + 8))" ffplay -nodisp -nostats -autoexit -rtsp_transport tcp \
    -i "$url0" >>"$LOG" 2>&1 &
FFPLAY=$!
sleep 2                      # 让它完成握手开始收流
kill -STOP "$FFPLAY" 2>/dev/null    # 冻结 → 服务端写缓冲被灌满
echo "  慢客户端已冻结(SIGSTOP), 开始观察参考路..." | tee -a "$LOG"

# --- 4) 收尾 ---
sleep "$D"
kill -9 "$FFPLAY" 2>/dev/null; wait "$FFPLAY" 2>/dev/null
for p in "${BG_PIDS[@]:-}"; do wait "$p" 2>/dev/null; done
wait "$REF_PID" 2>/dev/null
REF_RC=$?

echo "----------------------------------------------" | tee -a "$LOG"
cat "$LOG_DIR/07_ref_${TS}.log" | tee -a "$LOG"
echo "背景 ffmpeg 个数: ${#BG_PIDS[@]} (日志见上行 $LOG 内)" | tee -a "$LOG"

if [ "$REF_RC" -eq 0 ]; then
    echo "结果: PASS —— 慢客户端存在时参考路仍无 seq 缺口 (不互拖成立)" | tee -a "$LOG"
else
    echo "结果: FAIL —— 参考路出现丢包/停流，慢客户端拖垮了正常路" | tee -a "$LOG"
fi
echo "日志: $LOG" | tee -a "$LOG"
echo "参考客户端探针明细: $LOG_DIR/07_ref_${TS}.log" | tee -a "$LOG"
exit "$REF_RC"
