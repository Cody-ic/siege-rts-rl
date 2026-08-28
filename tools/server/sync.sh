#!/usr/bin/env bash
#
# 把本地仓库同步到训练服务器，**不在服务器上留任何凭据**。
#
# 为什么不直接在服务器上 git clone：仓库是私有的，而服务器的 `yys` 账号**四人共用**
# （见 `训练服务器环境.md` 第 1 节）。在共用账号上放 GitHub token 或部署密钥，
# 等于把自己的凭据交给另外三个人，且它会留在一台共享机器上。
#
# 改用 `git bundle`：它是 git 原生的「离线搬仓库」手段，带完整历史，
# 服务器侧是一个真正的 git 仓库（可以 log / diff / checkout），但不认识 GitHub。
# 首次约 100 MB，之后是**增量** bundle，通常几十 KB。
#
# 服务器上的布局：
#     ~/shared/siege-rts-rl.git      裸镜像，四个人共用一份（省掉各传一次 100 MB）
#     ~/shared/deps/                 第三方源码副本（Catch2 等），同样共用
#     ~/<你的名字>/siege-rts-rl      各自的工作区，origin 指向上面那个镜像
#
# 用法（在本地仓库根目录）：
#     tools/server/sync.sh                 # 同步当前 main 到服务器
#     tools/server/sync.sh --full          # 强制全量（镜像不存在或坏了时用）
#     RTS_SERVER=gpu RTS_WORKDIR=cody tools/server/sync.sh
#
set -euo pipefail

SERVER=${RTS_SERVER:-gpu}                 # 建议在 ~/.ssh/config 里配好 Host gpu
WORKDIR=${RTS_WORKDIR:-$(git config user.name)}
BRANCH=${RTS_BRANCH:-main}
MIRROR='~/shared/siege-rts-rl.git'
BUNDLE='~/shared/siege-rts-rl.bundle'
FULL=0
[ "${1:-}" = "--full" ] && FULL=1

[ -n "$WORKDIR" ] || { echo "无法确定工作目录名：设 RTS_WORKDIR 或配好 git config user.name" >&2; exit 2; }
git rev-parse --verify --quiet "$BRANCH" >/dev/null || { echo "本地没有分支 $BRANCH" >&2; exit 2; }

echo "本地 $BRANCH = $(git rev-parse --short "$BRANCH")"

# 镜像里已有的 tip。拿得到就发增量，拿不到就全量。
REMOTE_TIP=$(ssh "$SERVER" "git --git-dir=$MIRROR rev-parse --verify --quiet refs/heads/$BRANCH 2>/dev/null" || true)

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
if [ "$FULL" = 1 ] || [ -z "$REMOTE_TIP" ]; then
    echo "→ 全量 bundle（服务器上还没有镜像，或指定了 --full）"
    git bundle create "$TMP/sync.bundle" "$BRANCH"
    NEED_INIT=1
elif [ "$REMOTE_TIP" = "$(git rev-parse "$BRANCH")" ]; then
    echo "→ 服务器已是同一个提交（$( echo "$REMOTE_TIP" | cut -c1-7)），无需同步"
    exit 0
else
    echo "→ 增量 bundle（服务器在 $(echo "$REMOTE_TIP" | cut -c1-7)）"
    # 增量 bundle 带「前置提交」要求，服务器上缺了会在 fetch 时明确报错，
    # 不会静默产生一个不完整的仓库。
    git rev-parse --verify --quiet "$REMOTE_TIP^{commit}" >/dev/null \
        || { echo "本地没有服务器那个提交 $REMOTE_TIP，先 git fetch，或用 --full" >&2; exit 2; }
    git bundle create "$TMP/sync.bundle" "$REMOTE_TIP..$BRANCH"
    NEED_INIT=0
fi

echo "→ bundle 大小 $(du -h "$TMP/sync.bundle" | cut -f1)，传输中"
ssh "$SERVER" "mkdir -p ~/shared ~/$WORKDIR"
scp -q "$TMP/sync.bundle" "$SERVER:$BUNDLE"

ssh "$SERVER" "bash -s" <<REMOTE
set -euo pipefail
if [ "$NEED_INIT" = 1 ] || [ ! -d $MIRROR ]; then
    rm -rf $MIRROR
    git clone --mirror $BUNDLE $MIRROR >/dev/null 2>&1
    # bundle 里的 HEAD 是悬空的，镜像克隆完必须显式指一下，
    # 否则从镜像 clone 会得到一个空的 'master'（第一次就撞上了）。
    git --git-dir=$MIRROR symbolic-ref HEAD refs/heads/$BRANCH
else
    git --git-dir=$MIRROR fetch --update-head-ok $BUNDLE "$BRANCH:$BRANCH" 2>&1 | tail -2
fi
echo "镜像 $BRANCH = \$(git --git-dir=$MIRROR rev-parse --short $BRANCH)"

if [ -d ~/$WORKDIR/siege-rts-rl/.git ]; then
    cd ~/$WORKDIR/siege-rts-rl
    git fetch origin >/dev/null 2>&1
    if [ -n "\$(git status --porcelain --untracked-files=no)" ]; then
        echo "工作区有未提交改动，只 fetch 不动 HEAD。手动 git merge --ff-only origin/$BRANCH"
    else
        git checkout -q $BRANCH 2>/dev/null || git checkout -q -b $BRANCH origin/$BRANCH
        git merge --ff-only origin/$BRANCH 2>&1 | tail -1
    fi
else
    git clone -q $MIRROR ~/$WORKDIR/siege-rts-rl
    echo "已克隆工作区 ~/$WORKDIR/siege-rts-rl"
fi
cd ~/$WORKDIR/siege-rts-rl && echo "工作区 = \$(git rev-parse --short HEAD)  脏文件 \$(git status --porcelain --untracked-files=no | wc -l)"
REMOTE

echo "完成。构建： ssh $SERVER 'tools/server/build.sh Release' —— 或先 cd ~/$WORKDIR/siege-rts-rl"
