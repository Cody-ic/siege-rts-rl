#!/usr/bin/env python3
"""把「构建目录的配置比 CMake 源文件旧」变成一条会红的 ctest。

## 它防的是什么

`ctest` **不会**发现 `CMakeLists.txt` 变了。ctest 启动时读的是构建目录里生成好的
`CTestTestfile.cmake`，那份文件只在**配置**时重写；而只有 `cmake --build` 会顺带
触发重新配置，`ctest` 自己不会。

于是这条路径是静默的：

    git pull                 # 别人加了几个测试文件与 ctest 条目
    ctest --test-dir build   # 100% tests passed, 21/21   ← 该有 29 条

真事，就是本文件的起因：`build/` 是契约那个 PR 之前配置的，
`roster` / `action` / `command` / `obs` / `terrain` / `fog` / `world` / `replay`
八个标签的 ctest 条目**根本不存在**，所以 ctest 当然全绿。
只跑一次 `cmake -B build` 就从 21 变 29。

这与 `tests/CMakeLists.txt` 里记的「ctest 标签清单是白名单，写错抓得到、
**漏登记抓不到**」是同一种失败，但更隐蔽一层：**标签写错会红，条目不存在连红的
机会都没有** —— 它不是一条失败的测试，它是一条不存在的测试。

## 为什么不比 mtime

第一版想比 mtime（`CMakeLists.txt` 比 `CMakeCache.txt` 新就判陈旧）。**实测不行**：
CMake 只在**内容变了**时才重写生成物，所以一次什么都没改的重新配置**不会**更新
`CMakeCache.txt` 与 `CTestTestfile.cmake` 的 mtime。而 `git checkout` / `touch`
会更新源文件 mtime。两者一凑，那条守卫会**永远红** —— 一个永远红的检查等于没有检查
（`tools/sprite_gen/check_assets.py` 那里已经记过这句）。

所以按**内容**比：配置时把每个 CMake 输入文件的 sha256 记进构建目录，
跑测试时重算一遍对比。`touch` 不触发，`git checkout` 回同一份内容也不触发，
只有内容真的变了才红。

## 一份实现，两个模式

`--stamp` 与 `--verify` 走同一个 `collect()`，所以不存在「配置端与校验端各算一遍、
算法悄悄不一致」的可能 —— 那正是 `地图与场景设计.md` 6.3 把 `content_hash` 收进
`mapfile.py` 一个入口的理由。

## 两条已知的边界，都写在明处

1. **配置早于本守卫存在的构建目录，不会注册这条 ctest**，所以它对那种目录无效。
   这是自举问题，绕不过去：一个不存在的条目没法自己报告自己不存在。
   代价一次性 —— 任何人重新配置一次之后就落在守卫覆盖范围内。
2. **它只说「配置陈旧」，不说「陈旧到底影响了哪些条目」。** 报出改了哪些文件已经
   够定位了，而要精确到条目就得把测试清单也存一份并逐条比，不值那个复杂度。

用法:
    py check_cmake_freshness.py --stamp  <戳文件> --root <仓库根>
    py check_cmake_freshness.py --verify <戳文件> --root <仓库根>
"""
import argparse
import hashlib
import os
import sys

# 不进哈希的目录。
#
# `build*` 是要紧的那一条：`build/_deps/` 里有 Catch2 与 raylib 自己的
# CMakeLists.txt，把它们算进去，一次 FetchContent 下载就会让守卫红，
# 而那与「我们的配置陈旧了」毫无关系。
#
# 用前缀匹配而不是列举 `build/`、`build-Release/`：`.gitignore` 就是在这条上
# 踩过 —— 白名单式的 `build/` 与 `build-*/` 都不匹配临时开的 `build33/`，
# 一次 `git add -A` 带进去 902 个文件。
_SKIP_PREFIXES = ("build",)
_SKIP_EXACT = {".git", "__pycache__", ".venv", "venv", "node_modules",
               ".vs", ".idea", ".vscode"}


def _skip_dir(name):
    if name in _SKIP_EXACT:
        return True
    if name.startswith("."):
        return True
    return any(name.startswith(p) for p in _SKIP_PREFIXES)


def collect(root):
    """返回 [(相对路径, sha256十六进制)]，按路径排序。

    收 `CMakeLists.txt` 与 `*.cmake`。**按内容哈希，不看 mtime**（见模块 docstring）。

    路径一律用 `/` 分隔并算进哈希：只哈希内容的话，把一个 `.cmake` 改名而内容不动
    就检不出来，而改名同样会改变配置结果。
    """
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not _skip_dir(d))
        for fn in sorted(filenames):
            if fn != "CMakeLists.txt" and not fn.endswith(".cmake"):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            h = hashlib.sha256()
            h.update(rel.encode("utf-8"))
            h.update(b"\0")
            with open(full, "rb") as f:
                h.update(f.read())
            out.append((rel, h.hexdigest()))
    return out


def _write(path, entries):
    # LF 固定写死：这份文件由 Python 写、由 Python 读，但它躺在构建目录里，
    # 而构建目录在 Windows 上。留给平台默认换行没有好处，只会让「同一份内容
    # 在两个平台上不同」这种问题多一个可能的落点。
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for rel, digest in entries:
            f.write(f"{digest}  {rel}\n")


def _read(path):
    entries = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            digest, _, rel = line.partition("  ")
            entries.append((rel, digest))
    return entries


def cmd_stamp(root, stamp):
    entries = collect(root)
    if not entries:
        # 收到 0 个文件必须失败。同 `check_determinism_bans.py` 的「扫到 0 个
        # 文件即失败」与校验器的「一张地图都没读到即失败」：路径错时报「通过」
        # 是最坏的那种绿，而这里更糟 —— 它会写出一份空戳，
        # 于是 --verify 也永远通过。
        print("check_cmake_freshness: 一个 CMake 输入文件都没收到，"
              f"--root 是不是写错了？（root={root}）", file=sys.stderr)
        return 1
    _write(stamp, entries)
    return 0


def cmd_verify(root, stamp):
    if not os.path.exists(stamp):
        print("配置陈旧检查：找不到戳文件\n"
              f"    {stamp}\n"
              "  它由配置时的 execute_process 写出。没有它通常意味着这个构建目录\n"
              "  是在本守卫加入之前配置的（模块 docstring 里那条已知边界）。\n"
              "  跑一次 `cmake -B <构建目录>` 即可。")
        return 1

    want = dict(_read(stamp))
    got = dict(collect(root))
    if not got:
        print(f"配置陈旧检查：一个 CMake 输入文件都没收到（root={root}）")
        return 1

    added = sorted(set(got) - set(want))
    removed = sorted(set(want) - set(got))
    changed = sorted(k for k in set(got) & set(want) if got[k] != want[k])

    if not (added or removed or changed):
        return 0

    print("配置陈旧：CMake 的输入文件在上一次配置之后变过。")
    for rel in changed:
        print(f"    改了   {rel}")
    for rel in added:
        print(f"    新增   {rel}")
    for rel in removed:
        print(f"    删除   {rel}")
    print()
    print("  这条检查存在的理由：**ctest 不会自己发现这件事。** 它读的是构建目录里")
    print("  生成好的 CTestTestfile.cmake，而那只在配置时重写。所以新增的 ctest")
    print("  条目此刻根本不存在，ctest 会以「全部通过」收场 —— 跑的却是一个子集。")
    print("  这不是一条失败的测试，是一条**不存在**的测试。")
    print()
    print("  修法：`cmake --build <构建目录>`（会顺带重新配置），")
    print("        或直接 `cmake -B <构建目录>`。然后重跑 ctest。")
    return 1


def main():
    ap = argparse.ArgumentParser(description="CMake 配置是否比源文件旧")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--stamp", metavar="文件", help="写出当前输入的哈希清单")
    g.add_argument("--verify", metavar="文件", help="与清单比对")
    ap.add_argument("--root", required=True, help="仓库根目录")
    a = ap.parse_args()

    root = os.path.abspath(a.root)
    if not os.path.isdir(root):
        print(f"check_cmake_freshness: --root 不是目录：{root}", file=sys.stderr)
        return 1
    if a.stamp:
        return cmd_stamp(root, a.stamp)
    return cmd_verify(root, a.verify)


if __name__ == "__main__":
    sys.exit(main())
