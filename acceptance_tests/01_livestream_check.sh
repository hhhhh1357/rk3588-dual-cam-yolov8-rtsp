#!/usr/bin/env bash
# =============================================================
# 01_livestream_check.sh —— 功能验收：双路可拉流 + 起播延迟
#
# 测什么：
#   1) /live0、/live1 各自能持续拉流并解码（连续 WIN 秒不报错）
#   2) "起播延迟"：客户端加入后到解码出第一帧画面的毫秒数
#      —— 这一步等价于"用户按下播放到看见画面的时间"，
#         视频流必须在收到关键帧后才能出图，所以它天然包含等 I 帧的时间。
#
# 怎么判：
#   PASS = 两路都连续拉流正常。起播延迟记录到日志，通常接近 1 个 GOP
#         间隔（GOP15 = 0.5s）；超过 2s 就要怀疑关键帧注入是否失效。
#
# 用法:  ./01_livestream_check.sh            # 用 config.env 默认值
#        BOARD_IP=192.168.1.50 ./01_livestream_check.sh
# =============================================================
set -uo pipefail
SRC="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# shellcheck source=config.env
source "$SRC/config.env"

TS="$(date +%Y%m%d_%H%M%S)"
LOG="$LOG_DIR/01_livestream_${TS}.log"
: > "$LOG"
PASS_ALL=0

for live in "$LIVE0" "$LIVE1"; do
    url="rtsp://${BOARD_IP}:${RTSP_PORT}/${live}"
    echo "================ $live  ($url) ================" | tee -a "$LOG"

    # --- (1) 连续拉流 WIN 秒 ---
    # -f null : 只解码不落盘，纯测"链路能持续出数据"
    # -rtsp_transport tcp : 走 TCP 传输，避免 UDP 端口/防火墙干扰，测的更稳
    timeout "$((WIN + 8))" ffmpeg -nostdin -loglevel warning -rtsp_transport tcp \
        -i "$url" -t "$WIN" -f null - >>"$LOG" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  [PASS] 连续拉流 ${WIN}s 解码正常" | tee -a "$LOG"
    else
        echo "  [FAIL] 连续拉流失败 (ffmpeg rc=$rc, 可能是超时/断流/超时无帧)" | tee -a "$LOG"
        PASS_ALL=1
    fi

    # --- (2) 起播延迟：到解码出第一帧 ---
    # -frames:v 1 : 只要 1 帧就退出；命令返回即"第一帧已解码"
    t0="$(date +%s%3N)"
    timeout 30 ffmpeg -nostdin -loglevel error -rtsp_transport tcp \
        -i "$url" -frames:v 1 -f null - >>"$LOG" 2>&1
    rc=$?
    t1="$(date +%s%3N)"
    if [ "$rc" -eq 0 ]; then
        ms=$((t1 - t0))
        echo "  [PASS] 起播到第一帧: ${ms} ms" | tee -a "$LOG"
    else
        echo "  [FAIL] 30s 内未等到关键帧/无法起播 (rc=$rc)" | tee -a "$LOG"
        PASS_ALL=1
    fi
done

echo "----------------------------------------------" | tee -a "$LOG"
if [ "$PASS_ALL" -eq 0 ]; then
    echo "结果: PASS —— 两路均正常。日志: $LOG" | tee -a "$LOG"
else
    echo "结果: FAIL —— 见上方 [FAIL] 行。日志: $LOG" | tee -a "$LOG"
fi
exit "$PASS_ALL"
