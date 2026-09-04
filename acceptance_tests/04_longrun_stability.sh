#!/usr/bin/env bash
# =============================================================
# 04_longrun_stability.sh —— 稳定性验收：长时间不掉流/不丢包
#
# 测试思路：
#   "长时间稳定"不是跑一次长时间拉流，而是周期性采小样——每 N 秒做一次
#   WIN 秒的 RTP 质量探测（03_rtsp_probe.py），把结果逐行追加到 CSV。
#   好处：能精确知道"从哪个时间点开始变差"，而不是只得到"最后崩了"。
#
# 输出（两个文件都在 artifacts/）:
#   stability_<时间戳>.csv         —— 原始数据：每行一次探测结果
#   stability_run_<时间戳>.log     —— 运行过程 + 最终结论
#
# 怎么判：
#   所有样本无 seq 缺口 = PASS。CSV 里出现 RECV=0 或 GAPS>0 的行，
#   就是需要回去查的那个时间点。
#
# 用法（挂后台跑 24h，每 10 分钟探一次）:
#   nohup ./04_longrun_stability.sh -n 144 -i 600 -w 15 >/dev/null 2>&1 &
#   参数: -n 探测次数(默认60)  -i 间隔秒(默认600)  -w 窗口秒(默认15)
# =============================================================
set -uo pipefail
SRC="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# shellcheck source=config.env
source "$SRC/config.env"

N=60; INTERVAL="$TEST_INTERVAL"; WIN_SEC=15
while getopts "n:i:w:" o; do
    case "$o" in
        n) N="$OPTARG" ;;
        i) INTERVAL="$OPTARG" ;;
        w) WIN_SEC="$OPTARG" ;;
        *) echo "用法: $0 [-n 次数] [-i 间隔秒] [-w 窗口秒]"; exit 2 ;;
    esac
done

STAMP="$(date +%Y%m%d_%H%M)"
CSV="$LOG_DIR/stability_${STAMP}.csv"
RUNLOG="$LOG_DIR/stability_run_${STAMP}.log"
say() { printf '%s\n' "$*" | tee -a "$RUNLOG"; }

# 数据文件（机器可读）
echo "# 长稳压测 开始 $(date '+%F %T')  探测次数=$N 间隔=${INTERVAL}s 窗口=${WIN_SEC}s" > "$CSV"
echo "ts,live,verdict,recv,gaps,idle_s" >> "$CSV"
# 运行日志（人可读）
{
    echo "========== 04 长稳压测 $(date '+%F %T') =========="
    echo "板子 $BOARD_IP  探测次数 $N  间隔 ${INTERVAL}s  窗口 ${WIN_SEC}s"
    echo "数据文件: $CSV"
    echo "运行日志: $RUNLOG"
    echo "(Ctrl-C 可安全中断，已有数据不会丢)"
} > "$RUNLOG"
say "数据文件: $CSV"
say "运行日志: $RUNLOG"

FAIL_N=0
for i in $(seq 1 "$N"); do
    for live in "$LIVE0" "$LIVE1"; do
        # 单次探测；返回码 0=PASS 1=有缺口 2=连不上
        timeout "$((WIN_SEC + 20))" python3 "$SRC/03_rtsp_probe.py" \
            --host "$BOARD_IP" --port "$RTSP_PORT" --path "$live" \
            --window "$WIN_SEC" --csv "$CSV" >/dev/null 2>&1
        rc=$?
        if [ "$rc" -ne 0 ]; then FAIL_N=$((FAIL_N + 1)); fi
    done
    say "[$(date '+%T')] 第 $i/$N 轮完成, 累计异常 $FAIL_N 次"
    [ "$i" -lt "$N" ] && sleep "$INTERVAL"
done

say "==================== 汇总 ===================="
total=$(tail -n +3 "$CSV" | wc -l)
bad=$(grep -cE ',FAIL,|,ERR,' "$CSV" || true)
say "总样本: $total  异常样本: ${bad:-0}"
if [ "${bad:-0}" -eq 0 ]; then
    say "结果: PASS —— 全程无丢包/无停流"
else
    say "结果: FAIL —— 请打开 CSV 找 GAPS>0 或 ERR 行定位时间点"
    tail -5 "$CSV" | tee -a "$RUNLOG"
fi
say "数据文件: $CSV"
say "运行日志: $RUNLOG"
exit 0
