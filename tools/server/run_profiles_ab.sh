#!/usr/bin/env bash
#
# 守方风格轮换 A/B（攻方 PPO）：control（原始脚本）vs profiles（三风格轮换），
# 各两个训练种子，四条流水线并行。计划与门槛冻结在
# docs/rl-results/2026-09-09-defender-profiles-ab-plan.json，跑之前先读它。
#
# 用法（服务器，仓库根目录，绑定已构建）：
#     tools/server/run_profiles_ab.sh                      # 四条一起，各进一个 screen
#     RTS_AB_ROOT=~/Cody-ic/runs/profiles-ab-20260909 tools/server/run_profiles_ab.sh
#     tools/server/run_profiles_ab.sh control 1            # 只跑一条（补跑 / 续跑）
#
# 重跑同一条命令即续跑：pipeline.py 复用已完成阶段，改了源码或参数会拒绝。
# 与本仓库其它服务器脚本一样，日志放仓库外（logs/），免得污染工作区。
set -uo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
PY=${RTS_PYTHON:-/data0/am_data/miniforge3/bin/python}
BUILD=${RTS_BUILD_DIR:-$REPO/build-Release}
ROOT=${RTS_AB_ROOT:-$HOME/Cody-ic/runs/profiles-ab-20260909}
LOG_DIR=${RTS_LOG_DIR:-$(cd -- "$REPO/.." && pwd)/logs}
mkdir -p "$ROOT" "$LOG_DIR"
[ -x "$PY" ] || { echo "Python 不可执行：$PY" >&2; exit 2; }
[ -d "$BUILD/bindings" ] || { echo "找不到绑定构建目录 $BUILD/bindings；先 RTS_BINDINGS=1 tools/server/build.sh Release" >&2; exit 2; }

POOL=game/data/maps/pool
TRAIN=$POOL/gen_01001000.json,$POOL/gen_01004000.json,$POOL/gen_01005000.json,$POOL/gen_01006001.json
EVAL=$POOL/gen_01007000.json,$POOL/gen_01008000.json,$POOL/gen_01009000.json
STYLES=balanced,fortified,mobile

one() {   # one <arm> <seed>
    local arm=$1 seed=$2 extra=()
    case $arm in
        control)  ;;
        profiles) extra=(--train-profiles "$STYLES") ;;
        *) echo "arm 只接受 control / profiles，收到：$arm" >&2; return 2 ;;
    esac
    cd "$REPO" || return 1
    PYTHONPATH=$BUILD/bindings PYTHONIOENCODING=utf-8 \
    "$PY" train/pipeline.py --run-dir "$ROOT/$arm-s$seed" \
        --map-pool "$TRAIN" --eval-maps "$EVAL" --levels 1,4,8,16 \
        --device cpu --envs 16 --threads 4 --torch-threads 2 \
        --prepare-ticks 900 --collect-steps 600 --fit-epochs 16 \
        --total-steps 65536 --eval-episodes 32 --seed "$seed" \
        --eval-profiles "$STYLES" "${extra[@]}"
}

if [ $# -eq 2 ]; then
    one "$1" "$2" 2>&1 | tee -a "$LOG_DIR/profiles-ab-$1-s$2.log"
    exit "${PIPESTATUS[0]}"
fi

for arm in control profiles; do
    for seed in 1 2; do
        name=cody-ab-$arm-s$seed
        screen -dmS "$name" bash -lc "'$REPO/tools/server/run_profiles_ab.sh' $arm $seed"
        echo "→ screen $name  日志 $LOG_DIR/profiles-ab-$arm-s$seed.log  目录 $ROOT/$arm-s$seed"
    done
done
echo "看进度： screen -ls | grep cody-ab ;  tail -n 3 $LOG_DIR/profiles-ab-*.log"
