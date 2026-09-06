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
#     ~/shared/siege-rts-rl.git      裸镜像，四人共用一份（省掉各传一次 100 MB）
#     ~/shared/deps/                 第三方源码副本（Catch2 等），同样共用
#     ~/<你的名字>/siege-rts-rl      各自的工作区，origin 指向上面那个镜像
#
# 用法（在本地仓库根目录）：
#     tools/server/sync.sh                 # 同步 main
#     tools/server/sync.sh --full          # 强制全量（镜像不存在或坏了时用）
#     RTS_BRANCH=feat/xxx tools/server/sync.sh
#     RTS_SERVER=gpu RTS_WORKDIR=cody tools/server/sync.sh
#
set -euo pipefail

SERVER=${RTS_SERVER:-gpu}                 # 在 ~/.ssh/config 里配好 Host gpu
WORKDIR=${RTS_WORKDIR:-$(git config user.name || true)}
BRANCH=${RTS_BRANCH:-main}

# 路径一律写成相对家目录，在**服务器侧**拼 $HOME。
# 不要写成 `~/shared/...`：`git --git-dir=~/x` 里的 `~` 不会被展开
# （`~` 只在词首展开，`=` 后面不算），git 会去找一个名叫 `~` 的目录然后报
# 「not a git repository」——而路径打印出来看着完全正常。这个坑第一次就踩了。
MIRROR_REL=shared/siege-rts-rl.git
BUNDLE_REL=shared/siege-rts-rl.bundle

FULL=0
[ "${1:-}" = "--full" ] && FULL=1

[ -n "$WORKDIR" ] || { echo "无法确定工作目录名：设 RTS_WORKDIR，或配好 git config user.name" >&2; exit 2; }
git rev-parse --verify --quiet "$BRANCH" >/dev/null || { echo "本地没有分支 $BRANCH" >&2; exit 2; }

LOCAL_TIP=$(git rev-parse "$BRANCH")
echo "本地 $BRANCH = $(git rev-parse --short "$BRANCH")"

# 问镜像要两样：同名分支的 tip（判断要不要动），以及**全部** ref 的 tip（当增量前置）。
#
# 前置只看同名分支是不够的：第一次同步某个特性分支时镜像里没有这个 ref，
# 于是退化成全量 102 MB —— 而那时镜像其实已经有 99.99% 的对象（`main` 就在里面）。
# 按全部 ref 求差之后，同一条链上的特性分支通常只有几十 KB。第一版正是只看同名分支，
# 实测同步一个两提交的分支照样传了 102 MB 才发现。
read -r -d '' PROBE <<PROBE_EOF || true
G="git --git-dir=\$HOME/$MIRROR_REL"
echo "TIP \$(\$G rev-parse --verify --quiet refs/heads/$BRANCH 2>/dev/null)"
\$G for-each-ref --format='REF %(objectname)' 2>/dev/null
PROBE_EOF
PROBE_OUT=$(ssh "$SERVER" "$PROBE" 2>/dev/null || true)

REMOTE_TIP=$(printf '%s' "$PROBE_OUT" | sed -n 's/^TIP //p' | tr -d '\r')
BASES=()
# `|| [ -n "$sha" ]` 那半句是必需的，不是防御性代码：`$(…)` 会剥掉末尾换行，
# 于是最后一行没有换行符，`read` 读到它之后返回非零、**循环体不执行**。
# 只有一个 ref 时就等于一个都没读到 —— 而症状不是报错，是悄悄退回全量 102 MB。
while read -r sha || [ -n "$sha" ]; do
    [ -n "$sha" ] || continue
    # 只保留本地也有的：本地没有的提交不能当 bundle 的前置。
    git rev-parse --verify --quiet "${sha}^{commit}" >/dev/null 2>&1 && BASES+=("$sha")
done < <(printf '%s' "$PROBE_OUT" | sed -n 's/^REF //p' | tr -d '\r')

if [ "$FULL" != 1 ] && [ "$REMOTE_TIP" = "$LOCAL_TIP" ]; then
    echo "→ 服务器上的 $BRANCH 已是同一个提交（${LOCAL_TIP:0:7}），无需同步"
    exit 0
fi

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NEED_INIT=0
if [ "$FULL" = 1 ] || [ ${#BASES[@]} -eq 0 ]; then
    [ "$FULL" = 1 ] && echo "→ 全量 bundle（--full）" \
                    || echo "→ 全量 bundle（镜像里没有可用作前置的提交）"
    git bundle create "$TMP/sync.bundle" "$BRANCH"
    NEED_INIT=1
elif printf '%s\n' "${BASES[@]}" | grep -qx "$LOCAL_TIP"; then
    # 对象全都在，只是 refs/heads/<分支> 没指过来（例：镜像有 main，现在要同步的
    # 分支恰好就指向 main 那个提交）。`git bundle` 会拒绝创建空 bundle，
    # 所以这一支直接改 ref，不传任何东西。
    echo "→ 对象已在镜像里，只需把 $BRANCH 指到 ${LOCAL_TIP:0:7}"
    ssh "$SERVER" "git --git-dir=\$HOME/$MIRROR_REL update-ref refs/heads/$BRANCH $LOCAL_TIP"
    SKIP_XFER=1
else
    echo "→ 增量 bundle（镜像有 ${#BASES[@]} 个可用前置）"
    # 增量 bundle 自带「前置提交」要求：服务器上若缺了，`git fetch` 会明确报错，
    # 不会静默产生一个不完整的仓库。
    git bundle create "$TMP/sync.bundle" "$BRANCH" --not "${BASES[@]}"
fi
SKIP_XFER=${SKIP_XFER:-0}

ssh "$SERVER" "mkdir -p \$HOME/shared \$HOME/$WORKDIR"
if [ "$SKIP_XFER" != 1 ]; then
    echo "→ bundle $(du -h "$TMP/sync.bundle" | cut -f1)，传输中"
    scp -q "$TMP/sync.bundle" "$SERVER:$BUNDLE_REL"
fi

# 注意这个 heredoc 是**不加引号**的，所以 $MIRROR_REL / $BRANCH 等在本地展开
# （它们是本地已知的常量），而 \$HOME / \$(…) 转义后留给服务器展开。
ssh "$SERVER" "bash -s" <<REMOTE
set -euo pipefail
MIRROR=\$HOME/$MIRROR_REL
BUNDLE=\$HOME/$BUNDLE_REL
WORK=\$HOME/$WORKDIR/siege-rts-rl
TMPLOG=\$(mktemp); trap 'rm -f "\$TMPLOG"' EXIT

if [ "$SKIP_XFER" = 1 ]; then
    : # 没有新对象要传，ref 已在上一步直接改好，这里不能去 fetch 一个旧 bundle
elif [ "$NEED_INIT" = 1 ] || [ ! -d "\$MIRROR" ]; then
    rm -rf "\$MIRROR"
    git clone --mirror "\$BUNDLE" "\$MIRROR" >/dev/null 2>&1
    # bundle 里的 HEAD 是悬空的，镜像克隆完必须显式指一下，否则从镜像 clone
    # 会得到一个**空的 master 分支且不报错**。第一次手动做时就撞上了。
    git --git-dir="\$MIRROR" symbolic-ref HEAD refs/heads/$BRANCH
else
    # **`+` 是必需的，不是图省事。** 没有它，本地 \`git rebase\` 之后这一步
    # 会以 \`(non-fast-forward)\` 被拒 —— 而 2026-09-06 实测的后果是
    # **服务器照旧编译上一个提交、48/48 全绿、报告成功**，我据此以为新代码
    # 在 GCC 上过了。那正是 \`CLAUDE.md\` 反复点名的「绿色的子集」这一类。
    #
    # 强制推一个特性分支在本工作流里是正常操作（分支是我自己的、服务器上
    # 那份是纯镜像、没有别人在它上面提交）；\`main\` 不走这条路径。
    #
    # ⚠️ **不能让管道吞掉退出码**（\`CLAUDE.md\` 里 \`\${PIPESTATUS[0]}\` 那条
    # 先例）：\`... | tail -2\` 取到的是 \`tail\` 的码、恒 0，于是 \`set -e\`
    # 不会触发 —— 上面那次静默失败就是这么漏过去的。
    git --git-dir="\$MIRROR" fetch --update-head-ok "\$BUNDLE" "+$BRANCH:$BRANCH" >"\$TMPLOG" 2>&1         || { echo "✗ 镜像 fetch 失败："; tail -5 "\$TMPLOG"; exit 1; }
    tail -2 "\$TMPLOG"
fi
echo "镜像 $BRANCH = \$(git --git-dir="\$MIRROR" rev-parse --short $BRANCH)"

if [ -d "\$WORK/.git" ]; then
    cd "\$WORK"
    git fetch origin >/dev/null 2>&1
    if [ -n "\$(git status --porcelain --untracked-files=no)" ]; then
        echo "⚠ 工作区有未提交改动，只 fetch，不动 HEAD。自行 git merge --ff-only origin/$BRANCH"
    else
        git checkout -q $BRANCH 2>/dev/null || git checkout -q -b $BRANCH origin/$BRANCH
        # **`--ff-only` 在 rebase 之后同样会拒**（历史被重写了 ⇒ 不是快进），
        # 而上面那个 `| tail -1` 又把退出码吞了 ⇒ 工作区停在旧提交、脚本
        # 报「工作区 = <旧 sha>」而人只会看最后那句「构建：」。所以这里
        # 先试快进，不成就 `reset --hard`：**上面已经确认过工作区是干净的**
        # （那个 `git status --porcelain` 分支），所以 reset 丢不掉任何东西。
        if ! git merge --ff-only "origin/$BRANCH" >/dev/null 2>&1; then
            echo "→ 历史被重写（本地 rebase 过），工作区硬重置到 origin/$BRANCH"
            git reset --hard "origin/$BRANCH" >/dev/null
        fi
    fi
else
    git clone -q --branch $BRANCH "\$MIRROR" "\$WORK"
    echo "已克隆工作区 \$WORK"
fi
cd "\$WORK"
echo "工作区 = \$(git rev-parse --short HEAD) (\$(git rev-parse --abbrev-ref HEAD))  脏文件 \$(git status --porcelain --untracked-files=no | wc -l)"
REMOTE

echo
echo "构建： ssh $SERVER 'cd \$HOME/$WORKDIR/siege-rts-rl && tools/server/build.sh Release'"
