#!/usr/bin/env bash
# =============================================================
# 08_reconnect_stress.sh —— 网络韧性：反复断连/重连不崩
#
# 场景：
#   真实现场网络抖动会让客户端不断"连上→断线→再连"。压力测试要看的不只是
#   过程中有没有报错，而是结束后：
#     1) 服务端进程还活着（没被搞崩）；
#     2) 还能正常服务新连接（没把资源/线程泄漏到不可用）。
#
# 做法：
#   循环 M 次：ffmpeg 拉流 t 秒 → 超时杀掉(=模拟对端突然断开) → 立刻用
#   nc 探测端口确认服务端仍可接受新连接。结束后再完整拉一次流收尾验证。
#
# 用法:  ./08_reconnect_stress.sh -m 15 -t 2
#        -m 断连次数(默认15)   -t 每次拉流秒数(默认2)
# =============================================================
set -uo pipefail
SRC="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# shellcheck source=config.env
source "$SRC/config.env"

M=15; T=2
while getopts "m:t:" o; do
    case "$o" in
        m) M="$OPTARG" ;;
        t) T="$OPTARG" ;;
        *) echo "用法: $0 [-m 次数] [-t 秒]"; exit 2 ;;
    esac
done

TS="$(date +%Y%m%d_%H%M%S)"
LOG="$LOG_DIR/08_reconnect_${TS}.log"
: > "$LOG"
url="rtsp://${BOARD_IP}:${RTSP_PORT}/${LIVE0}"

echo "== 断连重连压测: $M 次, 每次拉流 ${T}s ==" | tee -a "$LOG"
OK=0
for i in $(seq 1 "$M"); do
    # 拉 T 秒；-t 会让 ffmpeg 自然退出，但我们人为用 timeout 掐断以模拟"对端突然断开"
    timeout "$((T + 2))" ffmpeg -nostdin -loglevel error -rtsp_transport tcp \
        -i "$url" -t "$T" -f null - >>"$LOG" 2>&1
    # 立刻探测：服务端是否还接受新连接
    ts="$(date '+%H:%M:%S')"
    if nc -z -w 2 "$BOARD_IP" "$RTSP_PORT" >/dev/null 2>&1; then
        OK=$((OK + 1))
        echo "  [$ts] 第 $i/$M 次断连: 端口可达=YES" | tee -a "$LOG"
    else
        echo "  [$ts] 第 $i/$M 次断连: 端口可达=NO(失败!)" | tee -a "$LOG"
    fi
done
echo "" | tee -a "$LOG"

# --- 收尾验证：服务端进程存活 + 能完整拉起一路 ---
PAT="[${APP_PATTERN:0:1}]${APP_PATTERN:1}"
ALIVE=$(ssh -o BatchMode=yes "${SSH_USER}@${BOARD_IP}" \
    "pgrep -f '$PAT' | wc -l" 2>/dev/null || echo 0)
echo "压测后服务端进程数: $ALIVE (应为 ≥1)" | tee -a "$LOG"

FINAL_RC=0
timeout 15 ffmpeg -nostdin -loglevel error -rtsp_transport tcp \
    -i "$url" -t 5 -f null - >>"$LOG" 2>&1 || FINAL_RC=1
echo "收尾完整拉流: $([ "$FINAL_RC" -eq 0 ] && echo OK || echo FAIL)" | tee -a "$LOG"

if [ "$OK" -eq "$M" ] && [ "$ALIVE" -ge 1 ] && [ "$FINAL_RC" -eq 0 ]; then
    echo "结果: PASS —— $M 次断连后服务仍存活且可正常服务" | tee -a "$LOG"
    exit 0
else
    echo "结果: FAIL —— 见上方标记行" | tee -a "$LOG"
    exit 1
fi
