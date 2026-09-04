#!/usr/bin/env bash
# =============================================================
# 06_perf_sample.sh —— 性能验收：进程 CPU + NPU 占用采样
#
# 为什么要采样存盘而不是看一眼：
#   简历/面试里报的"CPU ~30%、NPU 双核 ~43%"必须能拿出带时间戳的采样记录。
#   这里每个采样点都带时间，跑完后可取平均值/峰值。
#
# 测什么：
#   1) 进程 CPU：优先 pidstat(procps 自带，-p pid 会把进程所有线程合计)；
#      板端若没有 pidstat 则退回 top 批处理，把所有属于该进程行的 %CPU 求和。
#   2) NPU 负载：读 /sys/kernel/debug/rknpu/load（RK 板常见节点，逐核一行）。
#      节点不存在会显示 N/A，不影响 CPU 部分。
#
# 用法:  ./06_perf_sample.sh -n 10 -i 2     # 采 10 次，每次间隔 2s
# =============================================================
set -uo pipefail
SRC="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# shellcheck source=config.env
source "$SRC/config.env"

N="${N_SAMPLES:-10}"; IV="${I_INTERVAL:-2}"
while getopts "n:i:" o; do
    case "$o" in
        n) N="$OPTARG" ;;
        i) IV="$OPTARG" ;;
        *) echo "用法: $0 [-n 次数] [-i 间隔秒]"; exit 2 ;;
    esac
done

TS="$(date +%Y%m%d_%H%M)"
LOG="$LOG_DIR/06_perf_${TS}.log"
# 括号技巧防 pgrep -f 自匹配（ssh 命令行里也含该串）
PAT="[${APP_PATTERN:0:1}]${APP_PATTERN:1}"
echo "目标进程: $APP_PATTERN  (采样 $N 次, 间隔 ${IV}s)"
echo "# perf 采样 $(date '+%F %T') pattern=$APP_PATTERN" > "$LOG"
echo "ts,cpu_pct,npu_load" >> "$LOG"

# ssh 执行远端脚本；远端脚本内容走 stdin，位置参数 PAT N IV APP_BIN 跟在 bash -s 后面
# APP_BIN 用于按 /proc/<pid>/exe 真实可执行名过滤——否则 pgrep -f 会命中外层
# 启动壳(bash -c "…app…"，命令行含同样字符串)，采到 0%CPU 的假进程。
APP_BIN="${APP_PATTERN##*/}"
ssh -o BatchMode=yes "${SSH_USER}@${BOARD_IP}" \
    bash -s "$PAT" "$N" "$IV" "$APP_BIN" >>"$LOG" <<REMOTE
pat="\$1"; n="\$2"; iv="\$3"; appbin="\$4"
pid=""
for q in \$(pgrep -f "\$pat"); do
    e=\$(readlink "/proc/\$q/exe" 2>/dev/null)
    [ -n "\$e" ] && [ "\$(basename "\$e")" = "\$appbin" ] && pid=\$q && break
done
if [ -z "\$pid" ]; then echo "NO_PROC: 找不到进程 \$pat (exe=\$appbin)"; exit 1; fi
echo "pid=\$pid"
if command -v pidstat >/dev/null 2>&1; then
    for i in \$(seq 1 "\$n"); do
        ts=\$(date '+%H:%M:%S')
        cpu=\$(pidstat -p "\$pid" 1 1 | awk 'NR==4{print \$NF+0}')
        npu=\$(cat /sys/kernel/debug/rknpu/load 2>/dev/null | tr '\n' '|')
        echo "\$ts,\$cpu,\${npu:-N/A}"
        [ "\$i" -lt "\$n" ] && sleep "\$iv"
    done
else
    echo "# (板端无 pidstat，退回 top 批处理)"
    for i in \$(seq 1 "\$n"); do
        ts=\$(date '+%H:%M:%S')
        cpu=\$(top -b -n1 | awk -v p="\$pid" '\$1==p{s+=\$9} END{print s+0}')
        npu=\$(cat /sys/kernel/debug/rknpu/load 2>/dev/null | tr '\n' '|')
        echo "\$ts,\$cpu,\${npu:-N/A}"
        [ "\$i" -lt "\$n" ] && sleep "\$iv"
    done
fi
REMOTE

if grep -q NO_PROC "$LOG"; then
    echo "[FAIL] 板上找不到进程"; echo "日志: $LOG"; exit 1
fi

# 数据行(ssh 输出)已经在 $LOG 里，摘要追加到同一日志，终端也显示
say2() { printf '%s\n' "$*" | tee -a "$LOG"; }
{
    echo ""
    echo "========== CPU 摘要 =========="
    awk -F, '/^[0-9]{2}:/{s+=$2;c++;if($2>m)m=$2}
             END{ if(c>0) printf "平均=%.1f%%  峰值=%.1f%%  (样本 %d)\n", s/c, m, c;
                  else print "无有效样本" }' "$LOG"
    echo "NPU 原始读数见上各行的第 3 字段"
} | tee -a "$LOG"
say2 "日志: $LOG"
exit 0
