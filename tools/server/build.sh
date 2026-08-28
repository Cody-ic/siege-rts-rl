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
LOG_DIR=${RTS_LOG_DIR:-$REPO/../logs}
mkdir -p "$LOG_DIR"
LOG=$LOG_DIR/gcc-$(printf '%s' "$CFG" | tr '[:upper:]' '[:lower:]').log

# Catch2 的本地副本（可选）。给了就不走网络 —— 这台机器到 github.com 的连通性
# 是间歇性的（见 `训练服务器环境.md` 第 3 节），而 FetchContent 拉不动时
# 会先等一个连接超时。目录若不存在就忽略，让 FetchContent 正常走网络。
CATCH2_LOCAL=${RTS_CATCH2_DIR:-$HOME/shared/deps/Catch2-3.7.1}
EXTRA=()
if [ -d "$CATCH2_LOCAL" ]; then
    EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_CATCH2=$CATCH2_LOCAL")
fi

{
    echo "### $(date -Is)"
    echo "### 仓库    $REPO @ $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo '非 git 工作区')"
    echo "### 配置    $CFG   构建目录 $BUILD"
    echo "### 工具链  g++ $(g++ -dumpfullversion) / cmake $(cmake --version | head -1 | awk '{print $3}') / nproc $(nproc)"
    if [ ${#EXTRA[@]} -gt 0 ]; then
        echo "### Catch2  用本地副本 $CATCH2_LOCAL（不走网络）"
    else
        echo "### Catch2  经 FetchContent 下载（$CATCH2_LOCAL 不存在）"
    fi

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
