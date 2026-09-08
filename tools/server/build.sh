#!/usr/bin/env bash
#
# 在训练服务器（Linux/GCC）上构建并测试 rts_core。
#
# 这个脚本**在仓库里**而不是手写在服务器上，理由是它会随同步一起上去：
# 第一次把仓库同步到服务器之后，服务器上就有它了，不需要谁再照着文档敲一遍。
#
# 用法（在服务器上，仓库根目录或任意位置）：
#     tools/server/build.sh                # Release
#     tools/server/build.sh Debug
#     RTS_BUILD_DIR=/tmp/b tools/server/build.sh Release
#     RTS_CATCH2_DIR=… RTS_JSON_DIR=…      # 覆盖第三方本地副本的位置
#
# **网络不稳时建议放进 screen 里跑**：这台机器到 github.com 的连通性是间歇性的，
# 实测过一次 `curl` 直接把 SSH 会话 reset 掉，连带把前台构建也带走了。
#     screen -dmS <你>-gcc bash -lc 'cd ~/<你>/siege-rts-rl && tools/server/build.sh Release'
#
# 为什么每个配置一个构建目录：Makefiles 是**单配置**生成器，构建类型烤在缓存里。
# Windows 上用 VS 生成器是单目录多配置，所以这个区别在本地开发时不会遇到。
#
set -uo pipefail

CFG=${1:-Release}
case $CFG in
    Release|Debug|RelWithDebInfo|MinSizeRel) ;;
    *) echo "构建类型只接受 Release / Debug / RelWithDebInfo / MinSizeRel，收到：$CFG" >&2
       exit 2 ;;
esac

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${RTS_BUILD_DIR:-$REPO/build-$CFG}
# 日志放在仓库**外面**（同级的 logs/），因为仓库会被 sync.sh 的 ff-only 合并推进，
# 日志留在里面会变成未跟踪文件、干扰「工作区是否干净」这个判断。
LOG_DIR=${RTS_LOG_DIR:-$(cd -- "$REPO/.." && pwd)/logs}
mkdir -p "$LOG_DIR"
LOG=$LOG_DIR/gcc-$(printf '%s' "$CFG" | tr '[:upper:]' '[:lower:]').log

# 第三方依赖的本地副本（可选）。给了就不走网络 —— 这台机器到 github.com 的连通性
# 是间歇性的（见 `训练服务器环境.md` 第 3 节），而 FetchContent 拉不动时
# 会先等一个连接超时。目录若不存在就忽略，让 FetchContent 正常走网络。
#
# **每新增一个 FetchContent 依赖，这里要跟着加一条，`~/shared/deps/` 里也要放一份。**
# 这条是踩过才写的：`nlohmann_json` 由 #40 引入，而本脚本写于 #39，
# 于是它没有对应的口子——症状是构建在 configure 阶段就失败、
# 报的是 `Build step for nlohmann_json_single failed`，
# **看起来像依赖坏了，实际是这个脚本没跟上**。
#
# **它又踩了一次，这次是 pybind11**（`bindings/`，2026-08-30）：同样的症状、
# 同样的误导——报的是 `Each download failed / Timeout was reached`，
# 而真因是这张表没跟上。所以这句话现在有两个实例，别再让它有第三个。
DEPS=(
    "CATCH2:${RTS_CATCH2_DIR:-$HOME/shared/deps/Catch2-3.7.1}"
    "NLOHMANN_JSON_SINGLE:${RTS_JSON_DIR:-$HOME/shared/deps/nlohmann_json-3.11.3}"
    "PYBIND11:${RTS_PYBIND11_DIR:-$HOME/shared/deps/pybind11-v3.0.1}"
)
EXTRA=()
DEP_NOTES=()

# `RTS_BINDINGS=1` 时额外建 Python 绑定。**默认不建**，理由见顶层
# `RTS_BUILD_BINDINGS` 那段：多数机器没有 `Python.h`。
#
# **这台机器上系统 `python3` 就没有**（3.12，无 Python.h），而
# `/data0/am_data/miniforge3/bin/python`（同样 3.12）有，还自带
# torch 2.8.0+cu128 与 numpy。所以要显式指路，否则 `find_package(Python3
# COMPONENTS Development)` 会失败，而那句报错读起来像「没装 Python」。
if [ "${RTS_BINDINGS:-0}" = "1" ]; then
    PYEXE=${RTS_PYTHON:-/data0/am_data/miniforge3/bin/python}
    if [ ! -x "$PYEXE" ]; then
        echo "RTS_BINDINGS=1 但 $PYEXE 不可执行；用 RTS_PYTHON=... 指一个带开发头的 Python" >&2
        exit 2
    fi
    EXTRA+=("-DRTS_BUILD_BINDINGS=ON" "-DPython3_EXECUTABLE=$PYEXE")
    if [ "${RTS_TRAINING_TESTS:-0}" = "1" ]; then
        EXTRA+=("-DRTS_TEST_TRAINING=ON" "-DRTS_TRAIN_PYTHON=$PYEXE")
    fi
    DEP_NOTES+=("bindings 开，Python = $PYEXE")
fi
for entry in "${DEPS[@]}"; do
    name=${entry%%:*}
    dir=${entry#*:}
    if [ -d "$dir" ]; then
        EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_$name=$dir")
        DEP_NOTES+=("$name 用本地副本 $dir")
    else
        DEP_NOTES+=("$name 经 FetchContent 下载（$dir 不存在）")
    fi
done

{
    echo "### $(date -Is)"
    echo "### 仓库    $REPO @ $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo '非 git 工作区')"
    echo "### 配置    $CFG   构建目录 $BUILD"
    echo "### 工具链  g++ $(g++ -dumpfullversion) / cmake $(cmake --version | head -1 | awk '{print $3}') / nproc $(nproc)"
    for note in "${DEP_NOTES[@]}"; do
        echo "### 依赖    $note"
    done

    echo; echo "===== configure ====="
    cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE="$CFG" "${EXTRA[@]}" 2>&1 || exit 1

    echo; echo "===== build ====="
    cmake --build "$BUILD" -j"$(nproc)" 2>&1 || exit 1

    echo; echo "===== ctest ====="
    ctest --test-dir "$BUILD" --output-on-failure 2>&1
    rc=$?
    echo; echo "### ctest 退出码 $rc"
    exit $rc
} 2>&1 | tee "$LOG"

# tee 会吞掉退出码，取回管道左端那个 —— 不取的话构建失败也是 exit 0，
# 而「失败时一声不吭」正是 #24 那一整轮在处理的形态。
rc=${PIPESTATUS[0]}
echo "日志：$LOG"
exit "$rc"
