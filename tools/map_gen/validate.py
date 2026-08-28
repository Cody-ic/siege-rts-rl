#!/usr/bin/env python3
"""地图校验器 —— `地图与场景设计.md` 第 8 节那 15 条的可执行形式。

第 8 节开头写「`tools/map_gen/` 的校验器是地图规范的可执行部分。手写地图与
生成地图都必须通过」。本文件就是那一句。

## 三种状态，以及为什么「未实现」也要显式报出来

15 条里现在只有 8 条真正在跑。剩下的分两类，**都不静默跳过**：

- **阻塞**（第 7、8、9 条）—— 判据本身还写不出来，原因见 `README.md`
- **待阈值**（第 1、2、5、14 条）—— 判据清楚，但要等 `thresholds.json`（PR 3）

「跳过」若不出现在报告里，校验器就会随着条目增加而慢慢变成一个只查几条的东西，
而每次跑都是绿的 —— 这正是 #24 那一整轮「该红却绿」的形态。所以报告里三种状态
都逐条列出，末尾给出计数。

## 一条防漂移的断言

`CHECKS` 表必须正好覆盖 1..15、不重不漏，由 `selftest.py` 钉住。
第 8 节将来若加条目（例如 #26 的 A5 若通过，要加一条「§2 必须配 §3」的检查），
这条断言会立刻变红提醒同步 —— 白名单漏登记抓不到，是 `tests/CMakeLists.txt`
里已经踩过的坑。

用法:
    py validate.py <地图文件或目录> [更多...]
    py validate.py --list          # 只列 15 条的状态，不读地图
"""
import argparse
import sys
from collections import namedtuple
from pathlib import Path

import mapfile
import grid as gridmod
from grid import Grid

# 状态。IMPLEMENTED 之外的两种都不算失败，但一定会出现在报告里。
IMPLEMENTED = "已实现"
BLOCKED = "阻塞"
PENDING = "待阈值"

Check = namedtuple("Check", "no title fn status note")

# 第 8 节要求的总条目数。写成常量而不是 len(CHECKS)，这样「漏写一条」会被
# selftest 抓到 —— 用 len() 去校验 CHECKS 自己，等于用它证明它自己。
SPEC_CHECK_COUNT = 15


# --------------------------------------------------------------------------
# 各条检查。签名统一为 (doc, grid) -> 问题列表（空列表 = 通过）
#
# 返回列表而不是布尔，是为了让报告能指出**具体是哪个集结点 / 哪一格**出的问题。
# 「第 4 条不通过」对着一张 96×96 的图没有任何指导意义。
# --------------------------------------------------------------------------

def check_corridor_kinds(doc, grid):
    """第 3 条：每种 corridor 恰好出现一次，且种类数 = 集结点数。

    「恰好一次」蕴含「种类数 = 集结点数」，所以只需查重复。
    2.2 的走廊候选清单是**候选不是定数**，所以这里不查「必须四种都有」——
    那会把一个待定数值（集结点数量）当成结构结论，`#17` 正是为此开的。
    """
    corridors = [s["corridor"] for s in doc["spawns"]]
    problems = []
    for kind in sorted(set(corridors)):
        n = corridors.count(kind)
        if n > 1:
            where = [s["pos"] for s in doc["spawns"] if s["corridor"] == kind]
            problems.append(
                f"走廊性质 {kind!r} 出现了 {n} 次（集结点 {where}）；"
                f"2.2 要求每个集结点对应一条**性质不同**的走廊，"
                f"否则 AI 选哪个都一样，「选集结点」没有可学的信号")
    return problems


def check_spawn_reachable(doc, grid):
    """第 4 条：每个集结点到 keep 有通路（墙按高代价可通行）。

    通行定义用 ground_passable —— 它**不看墙**。5.1 明写墙是「高代价可通行」，
    把墙当障碍会让城圈一补完整就 field 无解，正是 DiNaO 被批评的那个失效。
    """
    reach = grid.flood([grid.keep], grid.ground_passable)
    return [
        f"集结点 {list(pos)}（{s['corridor']}）到 keep {list(grid.keep)} 不通 —— "
        f"注意这里墙已经按可通行算，所以不通只可能是 Rock/Water 封死"
        for pos, s in sorted(grid.spawns.items()) if pos not in reach
    ]


def check_inner_resources(doc, grid):
    """第 6 条：inner 资源点里石材、木材、金币三种各 ≥ 1。

    4.4：某一种保底为零，玩家在龟缩状态下对应的整条决策轴直接消失，
    而保底收入的职责是防死亡螺旋、不只是防龟缩。
    """
    inner = {r["type"] for r in doc["resources"] if r["tier"] == "inner"}
    missing = sorted(mapfile.RESOURCE_TYPES - inner)
    if not missing:
        return []
    return [f"inner 资源点缺少 {missing} —— 城内保底必须同时覆盖三种（4.4）"]


def check_forest_corridor(doc, grid):
    """第 10 条：`Forest` 不得构成任一集结点直达城墙的连续遮蔽通道。

    4.1：森林若能连成从集结点直达城墙的连续遮蔽通道，AI 会学到永远走林地潜行，
    其余走廊全部作废。

    「直达城墙」按「触到任一墙段的八邻」判定。地图没有 initial_walls 时这条
    自动通过 —— 没有墙就谈不上「直达城墙」，而 2.3 要求的初始城圈由第 8 条管。
    """
    if not grid.walls:
        return []
    near_wall = set()
    for w in grid.walls:
        near_wall.add(w)
        near_wall.update(grid.neighbors8(*w))

    def is_forest(x, y):
        return grid.terrain_at(x, y) == "Forest"

    problems = []
    for pos, s in sorted(grid.spawns.items()):
        starts = [n for n in list(grid.neighbors8(*pos)) + [pos] if is_forest(*n)]
        if not starts:
            continue
        hit = sorted(grid.flood(starts, is_forest) & near_wall)
        if hit:
            problems.append(
                f"集结点 {list(pos)}（{s['corridor']}）存在一条全程 Forest 的"
                f"遮蔽通道直达城墙附近 {list(hit[0])} —— AI 会学到永远走它，"
                f"其余走廊作废（4.1）")
    return problems


def check_plain_islands(doc, grid):
    """第 11 条：没有不可达的 `Plain` 孤岛，否则 flow field 出现洞。

    只查 `Plain`，按第 8 节字面。`Forest` / `Bridge` 孤岛同样会让 field 出洞，
    但把检查范围擅自扩大会拒掉合法地图（例如一片桥没接上是设计意图？），
    这属于规范该不该改的问题，不该由实现单方面决定。已记在 README 的空缺一览。
    """
    reach = grid.flood([grid.keep], grid.ground_passable)
    islands = sorted(
        (x, y) for x, y in grid.all_cells()
        if grid.terrain_at(x, y) == "Plain" and (x, y) not in reach)
    if not islands:
        return []
    shown = ", ".join(str(list(c)) for c in islands[:5])
    more = f"（共 {len(islands)} 格，只列前 5）" if len(islands) > 5 else ""
    return [f"存在从 keep 到不了的 Plain 格：{shown}{more}"]


def check_water_cuts_corridor(doc, grid):
    """第 12 条：`Water` 不得在无桥的情况下完全切断任一走廊。

    判据是「归因」而不只是「不通」：先算正常通行下的可达集，再算把 `Water`
    也当可通行时的可达集。**只在后者通、前者不通时**才判定是水切断的 ——
    否则被 Rock 封死也会报到这一条上，而那属于第 4 条。
    """
    normal = grid.flood([grid.keep], grid.ground_passable)
    if all(pos in normal for pos in grid.spawns):
        return []

    def passable_or_water(x, y):
        return grid.ground_passable(x, y) or grid.terrain_at(x, y) == "Water"

    with_water = grid.flood([grid.keep], passable_or_water)
    return [
        f"集结点 {list(pos)}（{s['corridor']}）的走廊被水完全切断且没有桥 —— "
        f"那条走廊等于不存在（4.1）"
        for pos, s in sorted(grid.spawns.items())
        if pos not in normal and pos in with_water
    ]


def check_bridge_on_water(doc, grid):
    """第 13 条：每个 `Bridge` 格的四邻中至少有一格是 `Water`。

    4.2.1 的底衬映射把 `Bridge` 画在 `Water` 上。桥若不在水边，渲染出来就是
    一块孤立的水 —— 这条只值一行代码，但不查的话第一个照抄示例的人就会中招。

    用四邻不用八邻：第 8 节第 13 条明写「四邻」。
    """
    problems = []
    for x, y in grid.all_cells():
        if grid.terrain_at(x, y) != "Bridge":
            continue
        if not any(grid.terrain_at(nx, ny) == "Water"
                   for nx, ny in grid.orthogonal_neighbors(x, y)):
            problems.append(
                f"Bridge 格 [{x}, {y}] 的四邻里没有 Water —— "
                f"底衬映射（4.2.1）会把它渲成一块孤立的水")
    return problems


def check_content_hash(doc, grid):
    """第 15 条：`content_hash` 与规范序列化后的内容一致。"""
    ok, stated, actual = mapfile.verify_content_hash(doc)
    if ok:
        return []
    if stated is None:
        return ["缺 content_hash 字段 —— 回放靠它检出「地图变了」（6.3）"]
    return [f"content_hash 对不上：文件里写的是 {stated}，实际算出 {actual}"]


# --------------------------------------------------------------------------
# 注册表
# --------------------------------------------------------------------------

CHECKS = [
    Check(1, "size 落在给定区间", None, PENDING,
          "阈值待标定，等 thresholds.json（PR 3）"),
    Check(2, "集结点数量与距边界格数", None, PENDING,
          "两个阈值都待标定（集结点数量属数值，见 CLAUDE.md「关于数值」）"),
    Check(3, "每种 corridor 恰好出现一次", check_corridor_kinds, IMPLEMENTED, ""),
    Check(4, "每个集结点到 keep 有通路", check_spawn_reachable, IMPLEMENTED, ""),
    Check(5, "Ram 行军时间占 episode 的比例", None, PENDING,
          "需要 Ram 速度与 episode 长度，两者皆待标定"),
    Check(6, "inner 资源点三种各 ≥ 1", check_inner_resources, IMPLEMENTED, ""),
    Check(7, "外部资源点结构性不可围", None, BLOCKED,
          "判据依 2.1 甲乙定案而变；候选乙还需要 layers.city 与 outposts 字段，"
          "而 6.2 里没有。见 #29 第二条、#26 的 A4"),
    Check(8, "初始城圈至少一处缺口", None, BLOCKED,
          "城区无法从现有字段推导（不闭合的曲线不分内外），见 README 与 #29 第一条"),
    Check(9, "需人工设防的正面总长落在区间", None, BLOCKED,
          "同第 8 条依赖城区；且阈值本身也待标定"),
    Check(10, "Forest 不构成连续遮蔽通道", check_forest_corridor, IMPLEMENTED, ""),
    Check(11, "无不可达的 Plain 孤岛", check_plain_islands, IMPLEMENTED, ""),
    Check(12, "Water 不得无桥切断走廊", check_water_cuts_corridor, IMPLEMENTED, ""),
    Check(13, "Bridge 四邻至少一格 Water", check_bridge_on_water, IMPLEMENTED, ""),
    Check(14, "集结点到最近可建造格的距离", None, PENDING,
          "阈值（静态建筑视野半径上限）待标定，且距离度量待定，见 #29 第三条"),
    Check(15, "content_hash 与内容一致", check_content_hash, IMPLEMENTED, ""),
]


def run(doc, grid=None):
    """跑全部检查，返回 [(Check, 问题列表)]。未实现的条目问题列表为 None。"""
    if grid is None:
        grid = Grid(doc)
    out = []
    for chk in CHECKS:
        if chk.fn is None:
            out.append((chk, None))
        else:
            out.append((chk, chk.fn(doc, grid)))
    return out


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

_MARK = {"pass": "✓", "fail": "✗", BLOCKED: "⊘", PENDING: "…"}


def _print_results(results, label):
    print(f"=== {label} ===")
    failed = 0
    for chk, problems in results:
        if problems is None:
            print(f"  [{chk.no:2}] {_MARK[chk.status]} {chk.title}"
                  f"   —— {chk.status}：{chk.note}")
            continue
        if problems:
            failed += 1
            print(f"  [{chk.no:2}] ✗ {chk.title}")
            for p in problems:
                print(f"         {p}")
        else:
            print(f"  [{chk.no:2}] ✓ {chk.title}")
    return failed


def _iter_maps(targets):
    for t in targets:
        p = Path(t)
        if p.is_file():
            yield p
        elif p.is_dir():
            for f in sorted(p.rglob("*.json")):
                yield f
        else:
            print(f"路径不存在：{p}", file=sys.stderr)


def cmd_list():
    by_status = {}
    for chk in CHECKS:
        by_status.setdefault(chk.status, []).append(chk)
    for status in (IMPLEMENTED, BLOCKED, PENDING):
        items = by_status.get(status, [])
        print(f"{status}（{len(items)} 条）:")
        for chk in items:
            note = f"  —— {chk.note}" if chk.note else ""
            print(f"  [{chk.no:2}] {chk.title}{note}")
        print()
    return 0


def main():
    ap = argparse.ArgumentParser(description="地图校验器（第 8 节）")
    ap.add_argument("targets", nargs="*", help="地图文件或目录")
    ap.add_argument("--list", action="store_true", help="只列 15 条的状态")
    a = ap.parse_args()

    if a.list:
        return cmd_list()
    if not a.targets:
        ap.error("需要至少一个地图文件或目录，或用 --list")

    maps = list(_iter_maps(a.targets))
    # 一张地图都没读到必须红。同 check_determinism_bans.py 的「扫到 0 个文件即失败」：
    # 目录改名或路径写错时，「通过（0 张）」是最坏的那种绿。
    if not maps:
        print("校验失败：一张地图都没读到。")
        print(f"    目标：{'、'.join(a.targets)}")
        print("    这条检查的全部价值在于它真的读过地图，所以路径错时必须红。")
        return 1

    total_failed = 0
    for path in maps:
        try:
            doc = mapfile.load(path)
        except mapfile.MapFormatError as e:
            print(f"=== {path} ===")
            print(f"  格式层就没过，后面 15 条无从谈起：{e}")
            total_failed += 1
            continue
        total_failed += _print_results(run(doc), str(path))

    n_impl = sum(1 for c in CHECKS if c.status == IMPLEMENTED)
    n_block = sum(1 for c in CHECKS if c.status == BLOCKED)
    n_pend = sum(1 for c in CHECKS if c.status == PENDING)
    print(f"\n{len(maps)} 张地图；条目状态：{n_impl} 已实现 / "
          f"{n_block} 阻塞 / {n_pend} 待阈值（共 {len(CHECKS)}）")
    if total_failed:
        print(f"失败：{total_failed} 条检查不通过")
        return 1
    print("全部已实现的检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
