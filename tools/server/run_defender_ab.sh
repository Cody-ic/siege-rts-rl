#!/usr/bin/env bash
#
# **接上真守方之后的第一轮完整实验**（2026-09-07）。
#
# ## 为什么要成套跑，而不是跑一轮看看
#
# 这一轮同时变了两件事：**奖励侧**（#148 加了显式胜利奖励与 `victory` 模式）
# 与**对手侧**（本轮接上了真守方）。**一轮跑不出归因** —— 若只跑
# 「新奖励 + 真守方」而胜率仍是 0，无从知道该往哪边查。
#
# 所以三条腿，改动量各差一项：
#
#   | 跑法 | 奖励 | 对手 | 它单独回答什么 |
#   |---|---|---|---|
#   | A `economic` + 守方 | 新 | 真 | **主线**：保留消耗战约定时能不能赢 |
#   | B `victory`  + 守方 | 新 | 真 | 只付胜利时能不能赢（回路结构上消失） |
#   | C `economic` + 空城 | 新 | 无 | **对照**：把「加了胜利奖励」这一项单独择出来 |
#
# C 是关键的一条：归档的 40M 是「旧奖励 + 空城」，而 C 是「新奖励 + 空城」
# ⇒ **A 与 C 的差 = 对手那一项；C 与归档的差 = 奖励那一项。** 少了 C，
# 两个变量就锁在一起了 —— 那正是 `配平工作交接.md` §2.7 那条教训
# （「一侧被改进了、另一侧没跟上，而读数照样好看」）的反面：一次只归因一项。
#
# ## 每一轮都必须归档，且必须用同一套设置评估
#
# 三轮写的是**同一个默认检查点路径**（`train/ppo_attacker.pt`），所以
# **跑完立刻归档**，否则下一轮覆盖掉。而 `evaluate.py` 的对手开关必须与
# 训练时一致 —— 对着空城量出来的胜率不能拿来评价对着真守方训练的策略。
#
# 用法（在服务器上、仓库根目录，已构建当前分支的绑定）：
#
#     tools/server/run_defender_ab.sh [总步数]
#
set -euo pipefail

STEPS=${1:-40000000}
PY=${RTS_PY:-/data0/am_data/miniforge3/bin/python}
BIND=${RTS_BINDINGS_DIR:-build-Release/bindings}
OUT=${RTS_ARCHIVE:-$HOME/Cody-ic/logs/archive}
STAMP=$(date +%m%d-%H%M)

[ -d "$BIND" ] || { echo "找不到绑定目录 $BIND —— 先 RTS_BINDINGS=1 tools/server/build.sh Release" >&2; exit 2; }
mkdir -p "$OUT"

# **开跑前看一眼显存。** 这台机器四人共用，而实测撞过一次「跑了五分钟才
# OOM」（`训练服务器环境.md`：GPU 空闲不等于 GPU 归你）。
nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader

run_one () {
    local tag=$1; shift
    echo "=========================================================="
    echo "== $tag  （步数 $STEPS）"
    echo "=========================================================="
    PYTHONPATH="$BIND" PYTHONIOENCODING=utf-8 "$PY" train/ppo.py \
        --total-steps "$STEPS" "$@" > "$OUT/ppo-$tag-$STAMP.log" 2>&1 || {
            echo "✗ $tag 训练失败，日志尾部："; tail -20 "$OUT/ppo-$tag-$STAMP.log"; return 1; }
    # **立刻归档**：三轮写同一个默认路径。
    cp train/ppo_attacker.pt   "$OUT/ppo-$tag-$STAMP.pt"
    cp train/ppo_attacker.json "$OUT/ppo-$tag-$STAMP.json"
    tail -2 "$OUT/ppo-$tag-$STAMP.log"

    # **冻结评估，对手与训练时一致。** `$@` 里若有 `--no-defender` 会一起
    # 传给评估器 —— 那是刻意的，两侧必须同一个对手。
    local ev_args=()
    for a in "$@"; do [ "$a" = "--no-defender" ] && ev_args+=("--no-defender"); done
    PYTHONPATH="$BIND" PYTHONIOENCODING=utf-8 "$PY" train/evaluate.py \
        --checkpoint "$OUT/ppo-$tag-$STAMP.pt" --episodes 32 --seed 100001 \
        --output "$OUT/eval-$tag-$STAMP.json" "${ev_args[@]}" || true
}

run_one "economic-defender" --reward-mode economic --win-reward 2000
run_one "victory-defender"  --reward-mode victory  --win-reward 1
run_one "economic-empty"    --reward-mode economic --win-reward 2000 --no-defender

echo
echo "=== 三轮完成，归档在 $OUT（时间戳 $STAMP）==="
ls -la "$OUT" | grep "$STAMP"
