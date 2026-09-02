#!/usr/bin/env python3
"""地图校验器 —— `地图与场景设计.md` 第 8 节那 23 条的可执行形式。

第 8 节开头写「`tools/map_gen/` 的校验器是地图规范的可执行部分。手写地图与
生成地图都必须通过」。本文件就是那一句。

## 三种状态，以及为什么「未实现」也要显式报出来

**条目状态由报告末尾那行计数给出，此处刻意不写数字**——它每落一条就变，
而写死的数字对读者没有任何用处（同 `tests/CMakeLists.txt` 那条纪律）。
仍未跑的分两类，**都不静默跳过**：

- **阻塞** —— 判据本身还写不出来，逐条原因在 `CHECKS` 表的 `note` 里
- **待阈值** —— 判据清楚，缺的只是一个待标定的数

「跳过」若不出现在报告里，校验器就会随着条目增加而慢慢变成一个只查几条的东西，
而每次跑都是绿的 —— 这正是 #24 那一整轮「该红却绿」的形态。所以报告里三种状态
都逐条列出，末尾给出计数。

## 阈值从哪来：`thresholds.json`，且分 profile

第 1、2、5、14、23 条要的阈值全部待标定。**待定的是值，不是归口**——
`地图与场景设计.md` §10 明写「一切阈值……应集中在一份配置里，不要硬编码」，
那份配置就是 `thresholds.json`，读取层是 `thresholds.py`。有了归口，
这几条就能实现，占位值随时改而代码不动。

阈值分 `strict` 与 `fixture` 两档，**profile 只改数值、不改「哪些条目跑」**。
`fixture` 存在的唯一理由是 `game/testdata/fixture_min.json` 是一张 7×5 的
最小夹具、不是一张游戏地图：对它套用「边长 ≥ 64」或「行军占 episode 的 10–20%」
没有意义，而格式与拓扑检查对它完全适用。**防止 profile 退化成后门的手段是
一条自检**：strict 下那张夹具必须红（`selftest.py`「profile 不是无操作」一组）。

## 批量报告：一个通用诊断能力，不再挂在某个编号下

§8.1 早就写了「丢弃率、哪一条更常否决」这类诊断信息更可能是生成器的一份
报告，不是 `CHECKS` 里的一行。生成器落地后照此办了，落点是 `generate.py`
的批量报告（丢弃率 + 每条检查的否决计数）。**这个报告机制本身一直都在跑**，
服务的是当前表里全部检查，不是某一条编号的专属产出——第 20 条曾经是它
唯一的动机（「第 7 与第 10 条的联合可满足性」），但 2026-09-01 第 7 条随
森林带机制一起废除后，第 20 条也改记进了 `REMOVED`（见下），
`MOVED_TO_GENERATOR` 因此空了，留着这个名字是为了 `selftest.py` 的覆盖性
断言不用跟着改形状。

## 废除的条目留号空表（第 3 条，2026-08-31；第 7、20 条，2026-09-01）

组长拍板取缔「走廊」概念，第 3 条（每种 corridor 恰好一次）随
`spawns[].corridor` 字段一起废除。**编号留空不顺延**：顺延会把第 4–25 条
全部重编号，波及 selftest 四十多处断言与两份文档，纯 churn 无收益。
`REMOVED` 显式记着它，理由与 `MOVED_TO_GENERATOR` 同一条纪律。

第 7 条（外部资源点结构性不可围）随 2.1 的订正一并废除——团队决定放弃
「资源簇必须有一条 `Forest` 4 连通到地图边界的通道」这条反龟缩结构约束，
理由是承载它的森林带即便两轮加固（#117/#121）仍在长距离上明显读成直线，
视觉代价大于结构收益，详见 `地图与场景设计.md` §2.1 与 `generate.py` 的
`place_outer_clusters` docstring。**这不是「没删干净的残留」**——它是团队
2026-08-31 前经过候选甲/候选乙比较后**主动采纳**的机制，这次是重新权衡后
的**主动移除**，两次都是明确决定，不是遗漏。

## 一条防漂移的断言

`CHECKS` 表加上 `MOVED_TO_GENERATOR` 与 `REMOVED` 必须正好覆盖
1..SPEC_CHECK_COUNT、不重不漏，由 `selftest.py` 钉住。第 8 节将来若加条目，
这条断言会立刻变红提醒同步 —— 白名单漏登记抓不到，是 `tests/CMakeLists.txt`
里已经踩过的坑。

用法:
    py validate.py <地图文件或目录> [更多...]
    py validate.py --profile fixture <最小夹具>
    py validate.py --list          # 只列条目状态，不读地图
"""
import argparse
import sys
from collections import namedtuple
from pathlib import Path

import mapfile
import grid as gridmod
import thresholds as thmod
from grid import Grid

# 状态。IMPLEMENTED 之外的两种都不算失败，但一定会出现在报告里。
IMPLEMENTED = "已实现"
BLOCKED = "阻塞"
PENDING = "待阈值"

# `needs_th` 为真的检查签名是 (doc, grid, th)，否则 (doc, grid)。
# **不把 th 一律加进所有签名**：那要改 15 个已经写好并有自检盯着的函数，
# 而它们一个都不需要阈值。多一个字段比多 15 处改动安全。
Check = namedtuple("Check", "no title fn status note needs_th")
Check.__new__.__defaults__ = (False,)

# 第 8 节要求的总条目数。写成常量而不是 len(CHECKS)，这样「漏写一条」会被
# selftest 抓到 —— 用 len() 去校验 CHECKS 自己，等于用它证明它自己。
# 2026-09-02：25 → 32。《地图生成器大改方案》§3.7 规划了第 26–32 条，
# 其中 28/29/31 随第 2 步落地，26/27（地形，第 3 步）、30（河桥，第 4 步）、
# 32（入口分布，第 5 步）先登记为**阻塞**占位——编号从 26 起、不重排。
SPEC_CHECK_COUNT = 32

# 从 CHECKS 移走的条目：编号 -> 去哪了。见模块 docstring。
#
# **第 20 条不在这里了**：它原本是「第 7 与第 10 条的联合可满足性」，
# 2026-09-01 第 7 条随森林带机制一起废除后，「联合」这件事本身不再成立，
# 第 20 条随之改记进 `REMOVED`（不是继续留在这里假装还移得动去哪）。
MOVED_TO_GENERATOR = {}

# 从规范废除的条目：编号 -> 为什么。**编号留空不顺延**——顺延会把第 4–24 条
# 全部重编号，波及 selftest 四十多处断言与两份文档，纯 churn 无收益；
# 第 9 条阻塞留号的先例证明「表里有洞」是被接受的形态（有注释说明即可）。
REMOVED = {
    3: "每种 corridor 恰好出现一次 —— 2026-08-31 组长拍板取缔「走廊」概念"
       "（城外地形完全随机散布，AoE4 式），`spawns[].corridor` 字段随 6.2"
       "一并删除；集结点仍是固定边缘候选点，只是不再携带性质标签",
    7: "外部资源点结构性不可围 —— 2026-09-01 团队决定移除，原实现是森林带"
       "（每个 outer 资源簇一条 4 连通到地图边界的 Forest 通道）。判据本身没有"
       "问题，废除的原因是纯视觉：即便两轮加固（#117/#121）森林带在几十格长"
       "的距离上侧向偏移仍只有 3–5 格，实测基本还是一条直线，团队认为这份"
       "反龟缩收益不值这个代价。**这不是没删干净的残留**——它是团队 2026-08-31"
       "前经候选甲/候选乙比较后主动采纳的机制，这次是重新权衡后的主动移除，"
       "两次都是明确决定。「部分资源点在墙外」这条更基本的要求不受影响，"
       "现在完全由第 21 条守住。详见 `地图与场景设计.md` 2.1 的订正",
    20: "第 7 与第 10 条的联合可满足性 —— 随第 7 条一起废除。本条问的是"
        "「森林通道必须存在」与「森林不得构成遮蔽道」两条合起来是否还有可行解，"
        "前一半（第 7 条）没了，「联合」这件事本身不再成立。此前它曾移出 `CHECKS`"
        "落到生成器的批量报告（丢弃率 + 逐条否决计数），**那个批量报告机制本身"
        "没有被拆掉**，只是不再对应这个编号——它现在是服务当前表里全部检查的"
        "通用诊断能力，见 `generate.py` 的 `_report()`",
}


# --------------------------------------------------------------------------
# 各条检查。签名 (doc, grid) -> 问题列表（空列表 = 通过）；
# 需要阈值的那几条是 (doc, grid, th)，并在 CHECKS 里标 needs_th=True。
#
# 返回列表而不是布尔，是为了让报告能指出**具体是哪个集结点 / 哪一格**出的问题。
# 「第 4 条不通过」对着一张 96×96 的图没有任何指导意义。
# --------------------------------------------------------------------------

def check_spawn_reachable(doc, grid):
    """第 4 条：每个集结点到 keep 有通路（墙按高代价可通行）。

    通行定义用 ground_passable —— 它**不看墙**。5.1 明写墙是「高代价可通行」，
    把墙当障碍会让城圈一补完整就 field 无解，正是 DiNaO 被批评的那个失效。
    """
    reach = grid.flood([grid.keep], grid.ground_passable)
    return [
        f"集结点 {list(pos)} 到 keep {list(grid.keep)} 不通 —— "
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
    城外地形博弈（「看不清来了什么」要靠侦查花钱买）作废。

    「直达城墙」按「触到任一墙段的八邻」判定。地图没有 initial_walls 时这条
    自动通过 —— 没有墙就谈不上「直达城墙」，而 2.3 要求的初始城圈由第 8 条管。

    **穿角必须按「能不能走」判，不能按「是不是森林」判**（`corner=` 那个参数）。
    否则一条**斜向**森林链走得过去、检查却报通过：单位从 (5,1) 斜走到 (6,2) 时
    要求的是那两个角格可通行，而它们完全可以是 `Plain`。这条检查的极性与第 4、
    11、12 条**相反**（那几条是「必须连通」，少给通路是多报；这条是「不得存在
    通路」，少给通路是**漏报**），所以在它身上「保守」不是安全侧。详见
    `grid.py` 模块 docstring 里那张极性表。

    **本条取 8 连通**（而不是别的检查常见的 4 连通）：森林曾与第 7 条
    （已废除，2026-09-01）正面对立、连通性取反——那条要求森林**必须**通到
    地图边界、取 4 连通，本条要求森林**不得**通到城墙、取 8 连通。第 7 条
    不在了，但本条的判据没有变化，8 连通仍然是对的取法（问的是**移动**：
    单位能不能沿森林走到墙边，与 CLAUDE.md 的 8 方向移动一致）——留着这条
    历史对照是因为它解释了「为什么这条不是 4 连通」，不是因为对立仍然存在。
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
        reach = grid.flood(starts, is_forest, corner=grid.is_passable)
        hit = sorted(reach & near_wall)
        if hit:
            problems.append(
                f"集结点 {list(pos)} 存在一条全程 Forest 的"
                f"遮蔽通道直达城墙附近 {list(hit[0])} —— AI 会学到永远走它，"
                f"城外地形博弈作废（4.1）")
    return problems


def check_initial_breach(doc, grid):
    """第 8 条：初始城圈至少一处缺口。

    **这条原先标为「阻塞」，理由是城区推不出来。判据换掉之后它不需要城区。**

    先说为什么原判据走不通：6.2 说「缺口就是城圈上没有墙段的位置，由校验器
    推导」，但城圈本身推不出来 —— 一条不闭合的曲线不把平面分成内外，而本条
    正是强制要求它不闭合。更尖锐的形式：`grid.derive_city_area()` **当且仅当**
    在本条该判失败的地图上推导成功（闭合环 → 成功；留缺口 → 抛
    `CityAreaUndecidable`）。所以任何以城区为前提的判据都不可能实现本条。

    换成直接问它的**目的**。第 8 节那张表写的是：

        初始城圈至少一处缺口 | 「AI 利用已有缺口」指标第 1 波测不了（2.3）

    那个性质等价于：**存在一条从任一集结点到 `keep`、全程不穿过任何墙段的通路。**
    原语现成，就是 `grid.blocked_by_walls_too`。

    它**比字面判据更贴合意图**：一个缺口若被第二道墙堵在后面，字面判据（城圈上
    有一格没墙）会放行，而这条判它没缺口 —— 那才是对的，AI 确实进不来。
    `Gate` 算墙、要打才过，也自动落在正确一侧。

    没有 `initial_walls` 时自动通过：没有墙，「进得来」显然成立。
    （「那圈初始城墙压根不存在」是另一条检查该管的事，见第 10 节待加的条目。）
    """
    if not grid.walls:
        return []
    reach = grid.flood(list(grid.spawns), grid.blocked_by_walls_too)
    if grid.keep in reach:
        return []
    return ["初始城圈没有缺口 —— 任一集结点都无法在不拆墙的情况下走到 keep。"
            "2.3 要求地图初始就带缺口，否则核心评估指标「AI 是否发现并利用已有"
            "缺口」第 1 波测不了（对标产品 DiNaO 正是被批评「敌人无视已有缺口」）"]


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
    """第 12 条：`Water` 不得在无桥的情况下完全切断任一集结点到 keep 的通路。

    **2026-08-31 重构**：走廊概念取缔，本条改语义为「集结点到 keep 的通路」——
    判据一行未动（本来就是「归因」式可达性：正常通行 vs 把 Water 当可通行）。

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
        f"集结点 {list(pos)} 到 keep 的通路被水完全切断且没有桥 —— "
        f"这条通路等于不存在（4.1）"
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


def check_entity_cells(doc, grid):
    """第 16 条：`resources` 与 `keep` 必须落在**可通行且可建造**的格上。

    §8.1 给这条标的来源是「实测」，症状写得很准：把资源点摆进水里、
    把另一个摆进岩壁里，**格式层与那 15 条全部放行** —— 手画的图渲出来一切正常，
    直到 `rts_core` 发现采石场盖不上去。

    「可通行**且**可建造」按 §8.1 字面写两个条件，但要知道**今天只有后者在做功**：
    五种地形里 `buildable` 的只有 `Plain`，而 `Plain` 可通行，所以
    `buildable ⇒ passable` 恒成立。两个都写是为了地形表将来长出
    「可建造但不可通行」那一格时不必回头改这条 —— **不是因为它现在能抓到什么**。
    写明这点，免得有人把它「化简」掉，也免得有人以为那一半在保护什么。

    `no_build` 也算在内，因为 `is_buildable()` 就是 4.2 那条组合规则
    （`terrain.buildable AND NOT no_build`）的唯一实现处。
    """
    problems = []

    def bad(x, y):
        return not (grid.is_passable(x, y) and grid.is_buildable(x, y))

    kx, ky = grid.keep
    if bad(kx, ky):
        problems.append(
            f"keep {list(grid.keep)} 落在 {grid.terrain_at(kx, ky)} 上"
            f"（no_build={grid.no_build[ky][kx]}）—— 堡垒盖不上去")
    for r in doc["resources"]:
        x, y = r["pos"]
        if bad(x, y):
            problems.append(
                f"{r['tier']} 资源点 {r['type']} {list(r['pos'])} 落在 "
                f"{grid.terrain_at(x, y)} 上（no_build={grid.no_build[y][x]}）"
                f"—— 采集建筑盖不上去，而地图渲出来看不出任何异常")
    return problems


def check_placement_conflicts(doc, grid):
    """第 17 条：墙不落在 `Rock` / `Water` 上；同格不得两条墙；集结点不得重合。

    三条都来自实测，且**格式层查不到**：它按**字段**查（`spawns[i].id` 不重复、
    坐标在界内），而这三条问的是**坐标之间**的关系。

    墙的重复必须查 `doc["initial_walls"]`，**不能查 `grid.walls`** —— 后者是按
    坐标索引的字典，重复在建 `Grid` 时就被后写的覆盖了（`grid.py` 那里明写
    「重复本身由 validate 报」）。拿 `grid` 去找重复，等于用一个已经把证据丢掉的
    结构找证据，而它会**永远报通过**。

    **墙那一条按 §8.1 字面只查 `Rock` / `Water`，没有扩大成「必须可建造」。**
    后者更严（会连 `Forest` 一起拒）。**原有一条支持扩大的理由**——2.1 的
    森林带靠「森林不可建造」封住围墙，地图作者若把初始墙直接摆在带子上，
    那条保证就被绕过去了——**已随 2.1 那条结构约束移除（2026-09-01）作废**，
    现在没有理由要求扩大，维持原样。
    """
    problems = []

    seen_wall = {}
    for i, w in enumerate(doc["initial_walls"]):
        pos = tuple(w["pos"])
        x, y = pos
        t = grid.terrain_at(x, y)
        if t in gridmod.NATURAL_BLOCKERS:
            problems.append(
                f"initial_walls[{i}]（{w['kind']}）落在 {t} 上 {list(pos)} —— "
                f"天然屏障上摆墙：渲出来是一段墙压在岩壁/水里，而寻路那侧"
                f"「墙是高代价可通行」与「{t} 不可通行」互相矛盾")
        if pos in seen_wall:
            problems.append(
                f"initial_walls[{i}] 与 [{seen_wall[pos]}] 同在 {list(pos)} —— "
                f"同一格两条墙。载入时后写的覆盖先写的，于是**文件里的血量与"
                f"实际生效的不一致**，而两者都看不出错")
        else:
            seen_wall[pos] = i

    seen_spawn = {}
    for s in doc["spawns"]:
        pos = tuple(s["pos"])
        if pos in seen_spawn:
            problems.append(
                f"集结点 id={s['id']} 与 id={seen_spawn[pos]} 同在 {list(pos)} —— "
                f"格式层只查了 id 不重复。两个集结点重合让「分兵佯攻」在几何上"
                f"不成立：同一个位置只有一路兵")
        else:
            seen_spawn[pos] = s["id"]
    return problems


OBSTACLE_TYPES = frozenset({"Stump", "Sapling", "Rubble"})


def check_resource_unlock_wave(doc, grid):
    """第 18 条：`resources` 的解禁波数序列按距离单调——解禁越晚的簇，离 keep 越远。

    **此前缺的是字段，不是判据**（6.2 新增 `resources[].unlock_wave` 之后，
    本条与第 19 条走的是同一条路：机制/字段先落地，校验器才有东西可查）。

    判据逐字对应 CLAUDE.md「资源点随波数解禁」那句「越晚解禁的越远」：任取两个
    资源点 A、B，若 `unlock_wave[A] < unlock_wave[B]`，则 A 到 `keep` 的距离
    不得大于 B 到 `keep` 的距离——反过来（解禁更早却离得更远）才是那句话字面
    描述的反例。**允许同波、允许同距离**，只拦"更晚解禁反而更近"这一种组合。

    距离取切比雪夫（`grid.chebyshev`）。**这条与第 14 条取它的理由不同**——
    第 14 条是为了在圆/方视野两种未决读法下都保守，本条不涉及视野，纯粹是
    不想在同一份校验器里为"距离"另开一种量法。**注意切比雪夫与欧氏在个别
    布局下会给出不同的相对顺序**（例如 (5,0) 与 (4,4) 到原点：切比雪夫下前者
    更远，欧氏下后者更远），所以这条判据的结论依赖度量选择——地图作者摆点时
    如果沿对角线方向拉开距离，要按切比雪夫（斜向移动的实际代价）想，不要按
    直觉的欧氏距离想。
    """
    keep = tuple(doc["keep"])
    resources = doc["resources"]
    problems = []
    for i, a in enumerate(resources):
        for j, b in enumerate(resources):
            if i == j or a["unlock_wave"] >= b["unlock_wave"]:
                continue
            da = gridmod.chebyshev(keep, tuple(a["pos"]))
            db = gridmod.chebyshev(keep, tuple(b["pos"]))
            if da > db:
                problems.append(
                    f"resources[{i}]（{a['type']} {list(a['pos'])}，解禁波 "
                    f"{a['unlock_wave']}）到 keep 的距离 {da} 大于 "
                    f"resources[{j}]（{b['type']} {list(b['pos'])}，解禁波 "
                    f"{b['unlock_wave']}）的距离 {db}，但前者解禁反而更早——"
                    f"「越往后越要往外走」这条节奏失效，解禁退化成随机给钱")
    return problems


def check_obstacle_types(doc, grid):
    """第 19 条：`Forest` 与 `Rock` 不得出现在可破坏障碍列表里。

    **此前是双重阻塞，两条现在都解了**：§6 的机制那一半于 2026-08-29 表决通过，
    6.2 随之新增 `obstacles` 字段，本条终于有东西可查。

    它保护 2.1 那条视野双向遮蔽的收益：玩家若能砍掉森林，外围经济的「要么驻兵、
    要么认损」这条野战压力就没了（2.1 曾经还论证过"不可破坏"能封住围墙，那条
    结构约束已于 2026-09-01 移除，见该节订正）。`Rock` 那一侧同理——城外随机
    散布的岩壁团块与隘口地形依赖它保持完整，可破坏则地形形态会被玩家清理掉。

    **查白名单而不是查「不是 Forest / Rock」**，两条理由：

      * 白名单同时抓住拼写错误（`Stmup`）与未来新增的地形名，
        而黑名单只抓住今天已知的那两个
      * 4.1 那张表把「地形一律不可破坏」写成了结论，所以任何地形名都不该出现，
        不只是那两个

    白名单本身要与 `rts::ObstacleType` 保持一致，那一侧由
    `tests/roster_test.cpp` 钉住不与地形枚举重名——**两边各查一半，合起来才完整**：
    这里查地图文件写了什么，那里查代码里的枚举是什么。
    """
    problems = []
    for i, o in enumerate(doc["obstacles"]):
        t = o["type"]
        if t in OBSTACLE_TYPES:
            continue
        why = ("——它是**地形**名。地形一律不可破坏（4.1），而 `Forest` 那一条是"
               "硬性的：能砍森林就能围住外部资源簇，2.1 整节退化为数值劝退（2.1.5）"
               ) if t in mapfile.TERRAIN_PALETTE else "——不是三种可破坏障碍之一（拼错了？）"
        problems.append(
            f"obstacles[{i}].type = {t!r} {why}。"
            f"合法取值只有 {sorted(OBSTACLE_TYPES)}")
    return problems


def check_obstacle_placement(doc, grid):
    """第 22 条：`obstacles` 的摆放冲突。

    **这条是加 `obstacles` 字段时顺带查出来的**，性质与第 16、17 条完全相同：
    新增一类点位实体就新增一片摆放冲突面，而那一片没有任何现存检查覆盖。

    **刻意不并进第 17 条**，尽管两者查的事很像。第 17 条已实现、已有测试，
    把新的实体类别塞进它等于让一条绿着的检查悄悄扩大职责，而 review 时看不出
    覆盖面变了——同 §8.1 那段说明。

    四类冲突：

      * 落在 `Rock` / `Water` 上 —— 渲出来是一棵长在水里的树，直到有人去打它
      * 同格两处障碍 —— 同 第 17 条那条墙的重复：`live_obstacle_count` 会是 2，
        而玩家看到一个
      * 与 `keep` / `resources` / `initial_walls` 同格 —— 那一格上既有障碍
        又有建筑，「清野腾出建造格」这条收益的判定会取决于遍历顺序
      * 与 `spawns` 同格 —— 攻方在自己的集结点里刷出来就卡在一个障碍上
    """
    problems = []

    occupied = {tuple(doc["keep"]): "keep"}
    for i, r in enumerate(doc["resources"]):
        occupied.setdefault(tuple(r["pos"]), f"resources[{i}]")
    for i, w in enumerate(doc["initial_walls"]):
        occupied.setdefault(tuple(w["pos"]), f"initial_walls[{i}]")
    for sp in doc["spawns"]:
        occupied.setdefault(tuple(sp["pos"]), f"spawns id={sp['id']}")

    seen = {}
    for i, o in enumerate(doc["obstacles"]):
        pos = tuple(o["pos"])
        x, y = pos
        t = grid.terrain_at(x, y)
        if t in gridmod.NATURAL_BLOCKERS:
            problems.append(
                f"obstacles[{i}]（{o['type']}）落在 {t} 上 {list(pos)} —— "
                f"天然屏障上摆一处可破坏障碍：渲出来是一棵长在水里/岩壁里的树，"
                f"而寻路那侧「障碍是高代价可通行」与「{t} 不可通行」互相矛盾")
        if pos in seen:
            problems.append(
                f"obstacles[{i}] 与 [{seen[pos]}] 同在 {list(pos)} —— 同一格两处障碍。"
                f"实体数会是 2 而玩家看到一个，于是「打掉它」之后那一格仍然挡路")
        else:
            seen[pos] = i
        if pos in occupied:
            problems.append(
                f"obstacles[{i}]（{o['type']}）与 {occupied[pos]} 同在 {list(pos)} —— "
                f"一格上既有障碍又有别的点位实体。「清野腾出可建造格」这条收益的"
                f"判定会取决于遍历顺序，而两侧都看不出错")
    return problems



# 采集建筑必须踩在对应资源点上才产出（`mechanics.cpp` 的 `tick_economy`：
# `is_gatherer(bt)` 时要求"需在资源点上"）——这不是摆放冲突，是这三座建筑
# 唯一能生效的摆法。与 `rts::gatherer_of()` 逐字对应，改动请同步两处。
GATHERER_MATCH = {"Quarry": "stone", "Lumber": "wood", "Mine": "gold"}


def check_building_placement(doc, grid):
    """第 24 条：`buildings`（玩家开局已拥有的其余建筑）的摆放冲突。

    **2026-08-31 随 6.2 新增 `buildings` 字段一起补的检查**，性质与第 16、17、22
    条完全相同——新增一类点位实体就新增一片摆放冲突面，此前没有任何检查覆盖它。

    **刻意不并进第 16/22 条**，理由同 §8.1 那段：已实现、已有测试的检查不该
    被新实体类别悄悄扩大职责。

    五类冲突（比第 22 条多一类，因为建筑要真的"盖得上去"，同第 16 条对
    `resources`/`keep` 的要求——障碍是原生景物、不需要这条）：

      * **不落在可通行且可建造的格上**（同第 16 条）—— 一座箭塔盖在岩壁上，
        地图渲出来正常，直到 `rts_core` 发现它盖不上去
      * 落在 `Rock` / `Water` 上 —— 上一条的具体成因之一，单独列出方便定位
      * 同格两处建筑 —— 载入时后写的覆盖先写的，文件里两座建筑、实际只留一座
      * 与 `keep` / `initial_walls` / `obstacles` 同格 —— 那一格上有两个点位
        实体，谁先生效取决于遍历顺序
      * 与 `spawns` 同格 —— 攻方在自己的集结点里刷出来就撞上一座箭塔

    **与 `resources` 同格不算冲突**——`Quarry`/`Lumber`/`Mine` 就该踩在
    对应资源点上（见 `GATHERER_MATCH`），那是这三座建筑生效的唯一摆法。
    只有"建筑与资源类型不匹配"（例如 `Tower` 摆在金矿上）才报。
    """
    problems = []

    def bad_ground(x, y):
        return not (grid.is_passable(x, y) and grid.is_buildable(x, y))

    resource_at = {tuple(r["pos"]): (i, r["type"])
                   for i, r in enumerate(doc["resources"])}

    occupied = {tuple(doc["keep"]): "keep"}
    for i, w in enumerate(doc["initial_walls"]):
        occupied.setdefault(tuple(w["pos"]), f"initial_walls[{i}]")
    for i, o in enumerate(doc["obstacles"]):
        occupied.setdefault(tuple(o["pos"]), f"obstacles[{i}]")
    for sp in doc["spawns"]:
        occupied.setdefault(tuple(sp["pos"]), f"spawns id={sp['id']}")

    seen = {}
    for i, b in enumerate(doc["buildings"]):
        pos = tuple(b["pos"])
        x, y = pos
        btype = b["type"]
        if bad_ground(x, y):
            t = grid.terrain_at(x, y)
            problems.append(
                f"buildings[{i}]（{btype}）落在 {t} 上 {list(pos)} "
                f"（no_build={grid.no_build[y][x]}）—— 盖不上去，而地图渲出来"
                f"看不出任何异常")
        if pos in seen:
            problems.append(
                f"buildings[{i}] 与 [{seen[pos]}] 同在 {list(pos)} —— 同一格两座"
                f"建筑。载入时后写的覆盖先写的，文件里两座、实际生效只有一座")
        else:
            seen[pos] = i
        if pos in occupied:
            problems.append(
                f"buildings[{i}]（{btype}）与 {occupied[pos]} 同在 {list(pos)} "
                f"—— 一格上有两个点位实体，谁先生效取决于遍历顺序")
        if pos in resource_at:
            ridx, rtype = resource_at[pos]
            want = GATHERER_MATCH.get(btype)
            if want is None:
                problems.append(
                    f"buildings[{i}]（{btype}）与 resources[{ridx}]（{rtype}）同在 "
                    f"{list(pos)} —— {btype} 不是采集建筑，不该踩在资源点上")
            elif want != rtype:
                problems.append(
                    f"buildings[{i}]（{btype}）踩在 resources[{ridx}]（{rtype}）"
                    f"上 {list(pos)}，但 {btype} 该配 {want}——类型不匹配，"
                    f"这座建筑在这个点上产不出资源")
    return problems


def check_has_outer_resource(doc, grid):
    """第 21 条：至少要有一个 `outer` 资源点。

    **这条本是落地第 7 条时查出来的洞**，编号接在 §8.1 的 16..20 之后：
    第 7 条在一个 `outer` 都没有时空过，第 6 条只查 `inner` 侧三种齐全，
    于是**一张把所有资源都放在城里的地图能通过全部检查** —— 而那等于砍掉
    「部分资源点在墙外」，`CLAUDE.md` 把它列为**不能砍的两个机制之一**
    （另一条是「城墙可站人」）。**2026-09-01 第 7 条随森林带机制一起废除后，
    本条是「部分资源点在墙外」唯一的守门人**——不再有第 7 条的「不可围墙」
    做后盾，龟缩虽然不再被结构性禁止，但至少「有资源在墙外」这件事本身
    仍然是强制的。

    **只查「≥ 1」。** 具体要几个、三种如何分布、内外配比多少，全是数值，
    现在不定（4.4 只给了「外部偏木材」这个倾向）。而「≥ 1」是结构性的：
    0 与 1 的差别不是强度差别，是那条机制在不在。
    """
    n = sum(1 for r in doc["resources"] if r["tier"] == "outer")
    if n:
        return []
    return ["一个 outer 资源点都没有 —— 「部分资源点在墙外」是 CLAUDE.md 列为"
            "不能砍的两个机制之一（逼守方出城、让野战机制有用武之地）。"
            "第 7 条已废除，本条现在是这条要求唯一的检查"]


def check_outer_gold(doc, grid, th):
    """第 25 条：`outer` 资源点里金矿的数量 ≥ 阈值。

    **2026-08-31 实测查出**（`gen_01006000` 城外零金矿、多张图城外只有 1 个
    金点）：此前城外资源簇的种类是纯随机抽签，金矿可以整个缺席。「攻其必救」
    要成立，攻方在城外必须有值得打的三种资源——金矿断供的图把「打哪一种」
    的纹理削掉一条，且与「城内金矿被点掉更疼」的设计意图叠加后，
    玩家对金矿的经济焦虑完全消失。

    与第 21 条同源（那条保「≥1 个 outer 点」的存在性，本条保金矿**种类**的
    存在性）；放进校验器而非只靠生成器，是为了保护手写地图。
    **生成器侧的保证落在 `place_outer_clusters`（前两个成功落地的簇各强制
    一个 gold）**，校验器是最终裁决——簇全失败时本条会否掉整张图。
    """
    n = sum(1 for r in doc["resources"]
            if r["tier"] == "outer" and r["type"] == "gold")
    if n >= th.outer_gold_min:
        return []
    return [f"outer 资源里只有 {n} 个金矿，少于 {th.outer_gold_min}"
            f"（profile {th.name}）—— 攻其必救在城外少一条轴，"
            f"「打哪一种资源」的纹理被削掉（2026-08-31 试玩查出）"]


# --------------------------------------------------------------------------
# 簇的推导（第 28/29/31 条共用）与路线特征
#
# 地图文件里**没有簇 id 字段**（C++ 侧读图的字段不动），但生成器保证：
# 同簇任意两点切比雪夫距离 ≤ 8（都在簇心半径 `_OUTER_CLUSTER_RADIUS` = 4 内），
# 不同簇任意两点 ≥ 9。所以「≤ 8 连边求连通分量」恰好就是簇——推导是精确的，
# 不是近似。手写地图若不满足这条几何，分量仍然是一个合理的簇读法，
# 而第 28/29/31 条的报告会指出具体是哪一簇/哪一点。
# --------------------------------------------------------------------------

_CLUSTER_LINK_DIST = 8


def _outer_clusters(doc):
    """把 `outer` 资源点分成簇：切比雪夫距离 ≤ 8 连边的连通分量。

    每簇返回 {"members": [资源条目...], "points": [...], "center": (x, y)}；
    簇心是落点均值的确定性取整（`grid.cluster_center`），与生成器排解禁波
    用的簇心逐格一致。
    """
    members = [r for r in doc["resources"] if r["tier"] == "outer"]
    n = len(members)
    parent = list(range(n))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i in range(n):
        for j in range(i + 1, n):
            if gridmod.chebyshev(tuple(members[i]["pos"]),
                                 tuple(members[j]["pos"])) <= _CLUSTER_LINK_DIST:
                pi, pj = find(i), find(j)
                if pi != pj:
                    parent[pi] = pj
    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(members[i])
    out = []
    for ms in groups.values():
        pts = [tuple(m["pos"]) for m in ms]
        out.append({"members": ms, "points": pts,
                    "center": gridmod.cluster_center(pts)})
    return out


def _walk_shortest_path(grid, dist, pos):
    """沿 BFS 距离场从 pos 走回 keep，返回路径格列表（含两端）。

    `dist` 必须是 `grid.bfs_steps([grid.keep], grid.ground_passable)` 的产出。
    最短路有无数条，这里沿梯度任取一条——第 31 条与路线特征问的都是
    「最短路**附近**有什么」，梯度路径是其中一条代表。
    """
    path = [pos]
    cur = pos
    while cur != grid.keep:
        d = dist[cur]
        nxt = next((nb for nb in grid.neighbors(*cur, grid.ground_passable)
                    if dist.get(nb) == d - 1), None)
        if nxt is None:      # 不应发生：pos ∈ dist 已保证有 predecessor
            break
        path.append(nxt)
        cur = nxt
    return path


def check_cluster_unlock_wave(doc, grid):
    """第 28 条：解禁**按簇**——同簇所有点同 `unlock_wave`；簇解禁序按簇心到
    keep 的切比雪夫距离单调（2026-09-02 大改第 2 步，《方案》§3.3）。

    一簇是一个决策单元（「要不要去打这一簇」），同簇拆波解禁等于把一个决策
    拆成几次半吊子的。第 18 条的逐点版**保留**（它拦「更晚解禁反而更近」的
    点对），本条加的是簇这一层：同波是簇内的、单调是簇间的。
    """
    problems = []
    clusters = _outer_clusters(doc)
    for cl in clusters:
        waves = {m["unlock_wave"] for m in cl["members"]}
        if len(waves) > 1:
            problems.append(
                f"簇（簇心 {list(cl['center'])}，{len(cl['members'])} 点）的 "
                f"unlock_wave 不止一个：{sorted(waves)} —— 一簇是一个决策单元，"
                f"同簇拆波解禁把它拆成了几次半吊子的决策（§3.3）")
    # 簇解禁序按簇心距离单调：按簇心距离排序后，波号必须非降。
    ordered = sorted(clusters,
                     key=lambda cl: (gridmod.chebyshev(cl["center"],
                                                       tuple(doc["keep"])),
                                     cl["center"]))
    waves = [min(m["unlock_wave"] for m in cl["members"]) for cl in ordered]
    for i in range(1, len(ordered)):
        if waves[i] < waves[i - 1]:
            a, b = ordered[i - 1], ordered[i]
            problems.append(
                f"簇（簇心 {list(b['center'])}，解禁波 {waves[i]}）比簇"
                f"（簇心 {list(a['center'])}，解禁波 {waves[i - 1]}）离 keep "
                f"更远，解禁却更早——「越晚解禁的越远」在簇这一层失效")
    return problems


def check_cluster_type_quota(doc, grid):
    """第 29 条：类型配额——城外三类簇的簇数两两之差 ≤ 1；内环至少一簇含金
    （2026-09-02 大改第 2 步，《方案》§3.3）。

    簇的类型取簇内点数的**唯一众数**（生成器按金→石→木轮转分配主类型，
    主类型点数恒严格多于其余）——不再纯随机 + 强制两金（实测旧池图 11/12
    张金 ≥ 石）。

    「内环至少一簇含金」是金币冗余框架（`攻守实力模型与平衡分析.md` §6.1）：
    城内金只够不缩编，买活的钱要从城外金簇来，而早解禁的内环簇里必须有一簇
    带金。内/外环按**集结点环**分界（簇心在集结点环内侧 = 内环）——校验器
    不知道设计半径 R 与 D，但集结点环是 doc 里现成的，两个环带离它都有
    14 格的间隔，按它分界不会误分。
    """
    clusters = _outer_clusters(doc)
    if not clusters:
        return []      # 一个 outer 都没有归第 21 条管
    problems = []
    counts = {}
    for cl in clusters:
        tally = {}
        for m in cl["members"]:
            tally[m["type"]] = tally.get(m["type"], 0) + 1
        top = max(tally.values())
        winners = [t for t, v in tally.items() if v == top]
        if len(winners) > 1:
            problems.append(
                f"簇（簇心 {list(cl['center'])}）的类型 {sorted(winners)} 并列 "
                f"{top} 点，定不出唯一主类型——类型配额按簇计数，"
                f"簇的类型必须可读（§3.3）")
            continue
        primary = winners[0]
        counts[primary] = counts.get(primary, 0) + 1
    if not problems:
        tally = [counts.get(t, 0) for t in sorted(mapfile.RESOURCE_TYPES)]
        if max(tally) - min(tally) > 1:
            problems.append(
                f"城外三类簇数 {dict(zip(sorted(mapfile.RESOURCE_TYPES), tally))} "
                f"两两之差超过 1——纯随机曾让金矿整个缺席或一边倒"
                f"（2026-08-31 实测），配额轮转就是要按住这种方差")

    keep = tuple(doc["keep"])
    spawn_d = [gridmod.chebyshev(tuple(s["pos"]), keep) for s in doc["spawns"]]
    inner = [cl for cl in clusters
             if gridmod.chebyshev(cl["center"], keep) < min(spawn_d)]
    # 内环一簇都没有时本半条无适用对象，自动通过（同第 5/10 条「没有墙
    # 自动通过」的先例）——fixture 那张 5×7 最小图的「内环」只剩 keep 一格，
    # 客观放不进簇。strict 档的图内环恒有簇（生成器 `inner_band_clusters`），
    # 这条照常查。
    if inner and not any(m["type"] == "gold" for cl in inner for m in cl["members"]):
        problems.append(
            "内环（集结点环内侧）没有一簇含金 —— 城内金只够不缩编，买活的钱"
            "要从城外金簇来，而早解禁的内环必须有一簇带金（实力模型 §6.1）")
    return problems


def check_route_passby(doc, grid, th):
    """第 31 条：「路过」约束——每条 spawn→keep 最短路 `route_cluster_window`
    格内至少一簇；每簇（的点）到任一集结点 ≥ `cluster_spawn_distance_min`
    （2026-09-02 大改第 2 步，《方案》§3.3）。

    前半条是「攻其必救有纹理」的几何形式：攻方顺路就能拆到一簇，资源争夺
    不需要专门绕路才会发生。最短路按 `ground_passable` 的 BFS（墙按高代价
    可通行，5.1），沿梯度取一条代表路径，问有没有簇的点落在路径窗口内。
    集结点走不到 keep 的图跳过前半条——那归第 4 条管。

    后半条是反面：簇不能落在集结区里（§3.3「每簇到任一集结点 ≥ 16」），
    否则「打集结区」与「抢资源」被强行焊成同一件事。16 > 禁建环 13，
    所以它比第 14 条的禁建区更宽一圈。

    两个数都收在阈值配置里（§10 的归口纪律），负数 = 本档弃用对应半条
    （只有 `fixture` 用，同 `static_vision_radius_max: -1` 的手法）。
    """
    problems = []
    clusters = _outer_clusters(doc)
    window = th.route_cluster_window
    if window >= 0 and clusters:
        dist = grid.bfs_steps([grid.keep], grid.ground_passable)
        for pos, s in sorted(grid.spawns.items()):
            if pos not in dist:
                continue      # 不通归第 4 条
            path = _walk_shortest_path(grid, dist, pos)
            hit = None
            for cl in clusters:
                if any(min(gridmod.chebyshev(p, c) for c in path) <= window
                       for p in cl["points"]):
                    hit = cl
                    break
            if hit is None:
                problems.append(
                    f"集结点 {list(pos)} 到 keep 的最短路（{dist[pos]} 格）"
                    f"{window} 格内没有任何资源簇 —— 「攻其必救」要求每条进攻"
                    f"路线顺路就有一簇可拆，否则资源争夺要专门绕路才会发生"
                    f"（§3.3）")
    d_min = th.cluster_spawn_distance_min
    if d_min >= 0:
        for cl in clusters:
            for p in cl["points"]:
                for pos, s in sorted(grid.spawns.items()):
                    d = gridmod.chebyshev(p, pos)
                    if d < d_min:
                        problems.append(
                            f"簇（簇心 {list(cl['center'])}）的点 {list(p)} 到集结点 "
                            f"{list(pos)} 只有 {d} 格（要求 ≥ {d_min}）—— 簇落在"
                            f"集结区里，「打集结区」与「抢资源」就被焊成了同一件事"
                            f"（§3.3）")
    return problems


def route_features(doc, grid):
    """逐集结点的路线特征（《方案》§3.1）：**派生量，进校验报告、不进地图文件**。

    2026-09-02 第 1 步先落两项可算的：最短路长度、沿途 6 格内资源簇数。
    地形相关项（最窄处宽度 / 路上 `Forest` 遮蔽比例 / 是否过桥）等第 3–5 步的
    地形落地后再补——键先留好（`None` = 未落地），验收判据（§7：集结点之间
    至少两项特征有显著差异）届时直接读这份结构。
    """
    clusters = _outer_clusters(doc)
    dist = grid.bfs_steps([grid.keep], grid.ground_passable)
    feats = []
    for pos, s in sorted(grid.spawns.items()):
        if pos in dist:
            path = _walk_shortest_path(grid, dist, pos)
            near = sum(1 for cl in clusters
                       if any(min(gridmod.chebyshev(p, c) for c in path) <= 6
                              for p in cl["points"]))
            feats.append({"spawn": pos, "path_len": dist[pos],
                          "clusters_near_path": near,
                          "min_width": None, "forest_cover": None,
                          "crosses_bridge": None})
        else:
            feats.append({"spawn": pos, "path_len": None,
                          "clusters_near_path": None,
                          "min_width": None, "forest_cover": None,
                          "crosses_bridge": None})
    return feats


# --------------------------------------------------------------------------
# 注册表
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# 需要阈值的那几条（第 1、2、5、14、23 条）。签名多一个 `th`（一档 Profile）。
#
# 它们此前标着「待阈值」，而**缺的从来不是判据，是阈值的归口**。
# `地图与场景设计.md` §10 早就写了「应集中在一份配置里，不要硬编码」，
# 落地就是 `thresholds.json`。占位值随时改，这四条的代码不动。
# --------------------------------------------------------------------------

# `Ram` 速度读一次就缓存：四条里只有第 5 条用它，而它会被每张地图调一次。
# 可用 `reset_ram_speed_cache()` 清掉——selftest 要在同一个进程里换数值表。
_ram_speed_cache = None


def reset_ram_speed_cache():
    global _ram_speed_cache
    _ram_speed_cache = None


def _ram_speed():
    global _ram_speed_cache
    if _ram_speed_cache is None:
        _ram_speed_cache = thmod.ram_speed_cells_per_tick()
    return _ram_speed_cache


# 建筑视野的最大值同理缓存。**它与 `Ram` 速度是同一类东西**：判据在这里，
# 数值在 `game/data/stats_placeholder.json`，本文件刻意不抄一份。
_max_bld_vision_cache = None


def reset_max_bld_vision_cache():
    global _max_bld_vision_cache
    _max_bld_vision_cache = None


def _max_bld_vision():
    global _max_bld_vision_cache
    if _max_bld_vision_cache is None:
        _max_bld_vision_cache = thmod.max_building_vision()
    return _max_bld_vision_cache


def check_size_range(doc, grid, th):
    """第 1 条：`size` 的两条边都落在给定区间。

    **两条边分别查，不查面积**：一张 4×2000 的图面积正常而形状荒谬，
    而城区半径、行军距离全部按边长推。

    **2026-09-02 文案改**（《地图生成器大改方案》§3.7）：边长由四个环相加
    得出（`size = 2×(R + D + 14 + W_far + 3)`，§2）——D 与 W_far 才是
    设计量，size 是它们的函数；不再「由城区半径 + 集结点到城墙推出」
    （那是贴边时代的写法）。
    """
    problems = []
    for name, v in (("宽", doc["size"][0]), ("高", doc["size"][1])):
        if not (th.size_min <= v <= th.size_max):
            problems.append(
                f"{name} = {v}，不在 [{th.size_min}, {th.size_max}] 内"
                f"（profile {th.name}）—— §2 的边长由四个环相加得出"
                f"（size = 2×(R + D + 14 + W_far + 3)），"
                f"而 D 受行军占比不变量约束（第 5 条）")
    return problems


def check_spawn_count_and_wall_distance(doc, grid, th):
    """第 2 条：集结点数量落在区间内，且每个到最近**墙格**的距离 ∈
    `spawn_wall_distance_range`。

    **数量取区间而不是等于某个数**：集结点数量是待定数值（CLAUDE.md
    「关于数值」），而地图规范曾把它写成「定为 4」——那是把平衡旋钮当成结构
    结论，是那条规则记下的唯一一次违反。区间表达「还没定」。

    **2026-09-02 大改第 1 步**（《地图生成器大改方案》§3.1）：判据从「贴近
    地图边缘」改为「到最近墙格的切比雪夫距离 ∈ [D_lo, D_hi]」——集结点不再
    贴边，而是落在集结点环（切比雪夫半径 R+D 的方环，按角度采样、任意角度）
    上；D=40 是 §2.1 拍板值（集结期已随 #128 落地，「先落集结期再取 40」）。
    集结点贴边的时代，到墙的距离是 size 的被动结果；现在 D 是独立设计量，
    边长反而由它推出（第 1 条），所以本条直接查它。

    地图没有 `initial_walls` 时距离半条自动通过——没有墙就谈不上「到墙的
    距离」，同第 5、10 条的先例；初始城圈的存在性由第 8 条管。数量半条
    不受影响。
    """
    problems = []
    n = len(doc["spawns"])
    if not (th.spawn_count_min <= n <= th.spawn_count_max):
        problems.append(
            f"集结点 {n} 个，不在 [{th.spawn_count_min}, {th.spawn_count_max}] 内"
            f"（profile {th.name}）—— 它直接决定玩家每波要防几个方向")
    if not grid.walls:
        return problems
    d_lo, d_hi = th.spawn_wall_distance_range
    walls = sorted(grid.walls)
    for s in doc["spawns"]:
        pos = (s["pos"][0], s["pos"][1])
        d = min(gridmod.chebyshev(pos, w) for w in walls)
        if not (d_lo <= d <= d_hi):
            problems.append(
                f"集结点 {list(pos)} 到最近墙格的切比雪夫距离为 {d}，"
                f"不在 [{d_lo}, {d_hi}] 内（profile {th.name}）—— §3.1 的集结点"
                f"环在城墙外 D 格处，过近则弓手调兵窗口被压没（§2.1），过远则"
                f"行军吞掉整个 episode（第 5 条）")
    return problems


def check_ram_march_fraction(doc, grid, th):
    """第 5 条：`Ram` 从集结点走到城墙外沿的时间占 episode 的比例落在区间。

    §3 唯一锁死的不变量。它把「地图尺寸 ↔ 单位速度 ↔ episode 长度」三者的耦合
    变成一条可执行的检查——而那三个量分散在三处（地图文件、数值表、
    `thresholds.json`），此前没有任何东西保证它们自洽。

    ## 三个近似，都写明

    **1. 「城墙外沿」取最近的墙格。** 严格的「外沿」需要城区（内外之分），
    而城区推导不出来（见第 9 条）。取最近墙格对本条足够：`Ram` 要拆的就是
    离它最近那段墙。

    **2. 距离取 8 邻接 BFS 的步数，对角步按 1 格算。** 与 CLAUDE.md 的
    「8 方向移动」一致，但真实的对角位移是 √2 格，所以这是**低估**行军时间。
    本条是双边区间、没有单调保守的方向，所以这个近似必须写出来而不能靠「保守」
    打发。真实值取决于 1c 的移动实现（每 tick 沿方向走 speed，还是归一化），
    等它定了这里要复核。

    **3. episode 长度是一个区间，所以占比也是一个区间。** 判据取
    **「存在一个合法的 episode 长度使占比落在目标区间内」**，即两个区间有交集
    就通过。理由与第 14 条取切比雪夫同一个手法：
    **挑一个对未决数值单调保守的取法，而不是留一个等着被拨错的旋钮。**
    episode 长度定下来之后这里会自动变严，不需要改代码。

    地图没有 `initial_walls` 时自动通过——没有墙就谈不上「走到城墙外沿」，
    同第 10 条的先例；初始城圈的存在性由第 8 条管。
    """
    if not grid.walls:
        return []
    try:
        speed = _ram_speed()
    except thmod.ThresholdError as e:
        # **把它报成一条问题而不是让它崩**，但要说清是谁的问题：
        # 这是数值表/配置读不到，不是这张地图画错了。
        return [f"读不到 `Ram` 速度，本条无从计算 —— **这是数值表或配置的问题，"
                f"不是这张地图的问题**：{e}"]

    problems = []
    walls = sorted(grid.walls)
    for pos, s in sorted(grid.spawns.items()):
        dist = grid.bfs_steps([pos], grid.ground_passable)
        reach = [dist[w] for w in walls if w in dist]
        if not reach:
            problems.append(
                f"集结点 {list(pos)} 走不到任何墙段 —— "
                f"第 4 条查的是到 `keep` 的通路，而「到墙」是另一件事："
                f"墙是高代价可通行，所以第 4 条可能靠**穿墙**通过")
            continue
        steps = min(reach)
        march_ticks = steps / speed
        lo = march_ticks / th.episode_ticks_max
        hi = march_ticks / th.episode_ticks_min
        if hi < th.ram_march_fraction_min or lo > th.ram_march_fraction_max:
            problems.append(
                f"集结点 {list(pos)} 到最近墙段 {steps} 格，"
                f"`Ram` 速度 {speed} 格/tick ⇒ 行军 {march_ticks:.0f} tick，"
                f"占 episode（{th.episode_ticks_min}–{th.episode_ticks_max} tick）"
                f"的 {lo:.1%}–{hi:.1%}，与目标区间 "
                f"{th.ram_march_fraction_min:.0%}–{th.ram_march_fraction_max:.0%} "
                f"没有交集。§3：低于下界玩家来不及反应，侦查与「攻其必救」都没有"
                f"时间窗口；高于上界 episode 大半花在行军上，RL 的有效决策步数"
                f"被浪费")
    return problems


def check_spawn_buildable_distance(doc, grid, th):
    """第 14 条：集结点到最近**可建造格**的切比雪夫距离必须 > 静态建筑视野半径上限。

    这是「任何静态建筑的视野都不得覆盖集结区」那条**结构约束**的可校验形式
    （CLAUDE.md「集结区」把它单列并明写「这是结构约束，不是数值」）。

    要防的不是强度问题而是性质变化：一座永久建筑若能照亮集结区，
    「这波要不要花钱侦查」就被**一次性买断**——`Scout`、`Wraith` 屏蔽、
    佯攻诱饵会一起失效。**所以它不能靠「把视野半径调小一点」解决。**

    ## 度量取切比雪夫，且这个选择不依赖「视野是圆还是方」

    方形球包含同半径的圆形球，所以切比雪夫对两种视野形状都保守。
    见 `grid.chebyshev` 与 #29 第三条：挑一个对未决问题单调保守的取法。

    查的是**可建造格**而不是「已有建筑」：地图上现在没建筑不代表玩家不能在那儿
    建，而这条约束要挡住的正是「玩家造一座瞭望塔就永久照亮集结区」。

    ## 上限从**数值表**推导，`static_vision_radius_max` 只是一条声明

    那个键曾是手写的 8，而 `game/data/stats_placeholder.json` 里 `Watch` 是 12。
    于是本条判定合法的图上，一座瞭望塔仍然照亮集结区——**结构约束被违反，
    而没有任何东西会红**。实测：生成器默认产出的四个集结点到最近可建造格都是
    10 格，`Watch(12)` 全部够得着，16/16。那不是边界情形，是默认产出。

    所以权威值改由 `thresholds.max_building_vision()` 从表里取最大值
    （同第 5 条取 `Ram` 速度那条先例——数值进仿真只有那一条路，
    抄一份就是第二个真相来源）。手写的那个键留着，但它现在的唯一作用是
    **被核对**：声明比表里的最大值小，就是上面那种「绿着的违反」，本条直接报。

    声明**大于**表里最大值是允许的（更保守），代价由生成器自己承担——禁建环
    跟着变宽，图更难生成，那是看得见的。
    """
    problems = []
    buildable = [c for c in grid.all_cells() if grid.is_buildable(*c)]
    if not buildable:
        return []      # 一格都不能建的图有别的问题，不在本条管

    declared = th.static_vision_radius_max
    if declared < 0:
        # 负数 = 本档显式弃用本条（只有 `fixture` 用，理由见 thresholds.json）。
        # **弃用的档不去核对声明**——没有声明可核对，而 -1 有它自己的理由。
        return []

    table_max, worst_bld = _max_bld_vision()
    if declared < table_max:
        problems.append(
            f"profile {th.name} 声明的 static_vision_radius_max = {declared}，"
            f"小于数值表里建筑视野的最大值 {table_max:g}（{worst_bld}）—— "
            f"**本条的上限本身失效**：一座 {worst_bld} 就能照亮集结区，"
            f"而按声明的上限查会全部通过。两处二选一改齐："
            f"抬高 tools/map_gen/thresholds.json 的声明（禁建环跟着变宽），"
            f"或压低 game/data/stats_placeholder.json 里 {worst_bld} 的视野")
    cap = max(declared, table_max)

    for pos, s in sorted(grid.spawns.items()):
        best, where = None, None
        for c in buildable:
            d = gridmod.chebyshev(pos, c)
            if best is None or d < best:
                best, where = d, c
        if best <= cap:
            problems.append(
                f"集结点 {list(pos)} 到最近可建造格 "
                f"{list(where)} 的切比雪夫距离只有 {best}，"
                f"不大于静态建筑视野半径上限 {cap:g}"
                f"（profile {th.name}；表里最大的是 {worst_bld} 的 {table_max:g}）"
                f"—— 一座永久建筑就能照亮集结区，"
                f"「这波要不要花钱侦查」被一次性买断，整条侦查博弈失效")
    return problems


def check_forest_cohesion(doc, grid, th):
    """第 23 条：每个 `Forest` 4 连通块的格数必须 ≥ 阈值。

    ## 它守的是森林的**职责**，而职责要的是连片、不是总面积

    `Forest` 在设计里干两件事（**曾经三件**——2.1.1 的森林带机制已于
    2026-09-01 移除，见 2.1 的订正）：城外散布森林的「看不清来了什么」
    （2026-08-31 前由林地走廊承载）、以及把杀伤区压成一条薄带。
    **两件都依赖连片**——同样 100 格森林，撒成 50 对散点与聚成几片，
    遮蔽效果完全不同，而**总面积一模一样**。

    所以「森林够不够」此前没有任何检查在管：第 10 条只禁止**过度**连通
    （不得构成连续遮蔽通道），管的是连通的**上界**，不管「一片林子会不会
    碎成粉尘」。

    ## 现在这条会平凡通过，那不是理由不加

    实测（6 张生成图，33 个 4 连通块）：最小块 4 格，大小 ≤2 的粉尘块 **0 个**
    ——因为生成器画的是连通团块（2026-08-31 重构后为随机游走集，此前是
    半径 1–3 的圆盘）。

    但那是**生成策略**保证的，不是约束保证的。同第 10 条那句「这不等于第 10 条
    无用——它保护的是手写地图，以及将来允许森林进集结点的更激进策略」：
    §9 那张手写演示地图还没做，而它正是最可能撒出粉尘的地方
    （手画时很容易点几格森林当装饰）。

    ## 取 4 连通、取「块格数」而不是「块厚度」

    - **4 连通**：落在保守那一侧——4 连通把块切得更碎，于是**报得更多**，
      而本条是一条下界约束，多报是安全的那侧。
    - **块格数，不是厚度。** 城外散布的森林小簇可以窄到 1 格宽的连接处，
      所以任何「厚度 ≥ 2」的判据都会把合法的形态判红。而一条 1 格宽、
      20 格长的窄连接，格数是 20，本条照样通过。
    """
    thr = th.forest_min_component_cells
    if thr <= 1:
        return []      # ≤1 = 本档弃用本条（一格也算一片，判据恒真）

    def is_forest(x, y):
        return grid.terrain_at(x, y) == "Forest"

    seen = set()
    problems = []
    for cell in grid.all_cells():
        if cell in seen or not is_forest(*cell):
            continue
        comp = grid.flood([cell], is_forest, diagonal=False)
        seen |= comp
        if len(comp) < thr:
            where = sorted(comp)
            problems.append(
                f"Forest 连通块 {where} 只有 {len(comp)} 格，少于 {thr}"
                f"（profile {th.name}，4 连通）—— 森林的两项职责"
                f"（§2.2 的「看不清来了什么」、把杀伤区压成薄带）"
                f"都依赖连片，而碎成粉尘时总面积不变、遮蔽效果没了")
    return problems


CHECKS = [
    Check(1, "size 落在给定区间", check_size_range, IMPLEMENTED, "",
          True),
    Check(2, "集结点数量与到城墙的距离", check_spawn_count_and_wall_distance,
          IMPLEMENTED, "", True),
    # 第 3 条已废除（2026-08-31，走廊概念取缔，`REMOVED`），编号留空。
    Check(4, "每个集结点到 keep 有通路", check_spawn_reachable, IMPLEMENTED, ""),
    Check(5, "Ram 行军时间占 episode 的比例", check_ram_march_fraction,
          IMPLEMENTED, "", True),
    Check(6, "inner 资源点三种各 ≥ 1", check_inner_resources, IMPLEMENTED, ""),
    # 第 7 条已废除（2026-09-01，森林带机制移除，`REMOVED`），编号留空。
    Check(8, "初始城圈至少一处缺口", check_initial_breach, IMPLEMENTED, ""),
    Check(9, "需人工设防的正面总长落在区间", None, BLOCKED,
          "**它自己有两个阻塞，各记一次**（不是「唯一一条阻塞」——阻塞共几条以报告头为准）："
          "(a) 「正面」的定义依赖城区，"
          "而城区无法从现有字段推导（见 #29 第一条）；(b) 阈值本身也待标定。"
          "**不要合并计数** —— 只解开一个它仍然写不出来。"
          "**注意 2.1 当年定案没有解开它，反而把 (a) 变难了**：城区 mask 本来是"
          "候选乙带来的，而乙已作废，所以现在没有任何一处会顺带产出城区，"
          "要它就得为它单独改一次 6.2。第 8 条早已换掉判据、**不再需要城区**；"
          "第 7 条也曾不需要城区，但已随 2.1 的订正整条废除（2026-09-01），"
          "所以本条现在与第 7、8 条都不同源，只剩自己这一份阻塞"),
    Check(10, "Forest 不构成连续遮蔽通道", check_forest_corridor, IMPLEMENTED, ""),
    Check(11, "无不可达的 Plain 孤岛", check_plain_islands, IMPLEMENTED, ""),
    Check(12, "Water 不得无桥切断集结点通路", check_water_cuts_corridor,
          IMPLEMENTED, ""),
    Check(13, "Bridge 四邻至少一格 Water", check_bridge_on_water, IMPLEMENTED, ""),
    Check(14, "集结点到最近可建造格的距离", check_spawn_buildable_distance,
          IMPLEMENTED, "", True),
    Check(15, "content_hash 与内容一致", check_content_hash, IMPLEMENTED, ""),
    # —— 8.1 起的条目。编号在那里排定，落地时按那个顺序，不要重排 ——
    Check(16, "resources 与 keep 落在可通行且可建造的格上",
          check_entity_cells, IMPLEMENTED, ""),
    Check(17, "墙不在 Rock/Water 上、同格不叠墙、集结点不重合",
          check_placement_conflicts, IMPLEMENTED, ""),
    Check(18, "resources 的解禁波数序列按距离单调", check_resource_unlock_wave,
          IMPLEMENTED, ""),
    Check(19, "Forest 与 Rock 不得出现在可破坏障碍列表里",
          check_obstacle_types, IMPLEMENTED, ""),
    # 第 20 条**不在这张表里**：它已按 §8.1 自己写的那句移到生成器的批量报告去了
    # （`MOVED_TO_GENERATOR`，理由见模块 docstring）。这里刻意留一条注释，
    # 因为「表里没有第 20 条」与「有人漏登记了第 20 条」在读表时长得一模一样。
    Check(21, "至少有一个 outer 资源点",
          check_has_outer_resource, IMPLEMENTED, ""),
    Check(22, "obstacles 的摆放冲突",
          check_obstacle_placement, IMPLEMENTED, ""),
    Check(23, "Forest 连通块不得碎成粉尘", check_forest_cohesion,
          IMPLEMENTED, "", True),
    Check(24, "buildings 的摆放冲突", check_building_placement, IMPLEMENTED, ""),
    Check(25, "outer 资源里金矿不少于阈值", check_outer_gold,
          IMPLEMENTED, "", True),
    # —— 《地图生成器大改方案》§3.7 的新条目（2026-09-02）：编号从 26 起、
    # 不重排。28/29/31 随第 2 步落地；26/27（地形，第 3 步）、30（河桥，
    # 第 4 步）、32（入口分布，第 5 步）先登记为阻塞占位，随各自那步实现 ——
    # 阻塞条目不静默跳过（模块 docstring 三种状态那条纪律），占位的 note
    # 写清它等的是哪一步。
    Check(26, "争夺带阻挡率落在区间", None, BLOCKED,
          "等第 3 步（地形三形态 + 隘口，《方案》§3.2）落地：阻挡率要有林子与"
          "岩脊可算才有意义，阈值 [lo, hi] 本身也是占位（10–20%，类比 AoE4）"),
    Check(27, "每条路线的最窄处或开阔标记", None, BLOCKED,
          "等第 3 步（§3.2）：隘口是设计放置的，地形落地后「最窄处 ∈ [2,4] "
          "或标记开阔、全图两类路线都有」才有可查的对象"),
    Check(28, "解禁按簇：同簇同波、簇序按簇心距离单调",
          check_cluster_unlock_wave, IMPLEMENTED, ""),
    Check(29, "城外簇类型配额与内环含金", check_cluster_type_quota,
          IMPLEMENTED, ""),
    Check(30, "桥宽与被河切断的集结点数", None, BLOCKED,
          "等第 4 步（河桥受管理，《方案》§3.4）：桥宽 ≥ 2 与被切集结点 ≤ 1 "
          "都是那一步的生成策略改完才查得了的"),
    Check(31, "每条最短路 6 格内至少一簇、簇不进集结区", check_route_passby,
          IMPLEMENTED, "", True),
    Check(32, "入口分布：门与缺口不同面", None, BLOCKED,
          "等第 5 步（城区 R 与缺口异面，《方案》§3.5）：缺口现在仍可能落在"
          "门所在面，异面是那一步的生成策略改的"),
    # 第 3 条**不在这张表里**：2026-08-31 组长拍板取缔「走廊」概念，该条随
    # `spawns[].corridor` 字段一起废除、编号留空（`REMOVED`，理由见模块 docstring）。
]


def run(doc, grid=None, th=None):
    """跑全部检查，返回 [(Check, 问题列表)]。未实现的条目问题列表为 None。

    `th` 是一档 `thresholds.Profile`。**默认加载 strict 而不是跳过需要阈值的
    条目**——「不给阈值就静默少查四条」正是本文件通篇在防的形态。
    最小夹具那类图请显式传 `fixture` 档。
    """
    if grid is None:
        grid = Grid(doc)
    if th is None:
        th = thmod.load().profile("strict")
    out = []
    for chk in CHECKS:
        if chk.fn is None:
            out.append((chk, None))
        elif chk.needs_th:
            out.append((chk, chk.fn(doc, grid, th)))
        else:
            out.append((chk, chk.fn(doc, grid)))
    return out


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _marks():
    """状态记号。**在编码不了它们的控制台上退回 ASCII，而不是崩。**

    ✓ / ✗ / ⊘ / … 都不在 cp936 里，而 Windows 中文环境的控制台默认就是 cp936。
    于是 `py validate.py maps/demo.json` 会抛
    `UnicodeEncodeError: 'gbk' codec can't encode character '\\u2713'`
    —— 报告一个字都印不出来，而**报错信息完全不指向真正的原因**（人会以为
    地图坏了）。中文本身反而没事：cp936 是中文码页，方框记号才是问题。

    **这条现在有回归了**：ctest 的 `map_gen_validate_fixture` 拿
    `game/testdata/fixture_min.json` 真的走一遍打印路径。

    在那条 ctest 之前它是看不见的，而当时给的理由是「仓库里还没有地图文件」——
    **那个理由是错的**：那张夹具一直都在，只是没人把 Python 校验器指向 C++ 侧的
    测试数据。所以它不是「等第一张地图入库就会现形」，而是**已经坏着、只是没人
    踩到**。这两种说法在处置上不同：前者可以等，后者不能。

    与 #47 那两条编码边界同源：**修在边界上**，不要求每个人记得
    `set PYTHONIOENCODING=utf-8`。
    """
    fancy = {"pass": "✓", "fail": "✗", BLOCKED: "⊘", PENDING: "…"}
    enc = getattr(sys.stdout, "encoding", None) or "ascii"
    try:
        for m in fancy.values():
            m.encode(enc)
    except (UnicodeEncodeError, LookupError):
        return {"pass": "[ok]", "fail": "[X]", BLOCKED: "[--]", PENDING: "[..]"}
    return fancy


_MARK = _marks()


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
            print(f"  [{chk.no:2}] {_MARK['fail']} {chk.title}")
            for p in problems:
                print(f"         {p}")
        else:
            print(f"  [{chk.no:2}] {_MARK['pass']} {chk.title}")
    return failed


def _iter_maps(targets, missing=None):
    """产出目标里的地图文件。不存在的路径记进 `missing`。

    **不存在的路径必须被调用方判为失败**，理由与下面「一张地图都没读到即失败」
    一字不差：原先它只往 stderr 打一句、退出码仍是 0，于是
    `maps/train maps/demo` 分成两个目录、其中一个改名之后，**覆盖面悄悄减半而
    ctest 全绿**。防住了「一张都没读到」却没防住「读到了一些」不算防住。
    """
    for t in targets:
        p = Path(t)
        if p.is_file():
            yield p
        elif p.is_dir():
            for f in sorted(p.rglob("*.json")):
                yield f
        else:
            print(f"路径不存在：{p}", file=sys.stderr)
            if missing is not None:
                missing.append(p)


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
    # **移走的条目也要列出来。** 不列的话，`--list` 报的条目数会比规范少一条，
    # 而读者无从知道少的那条去哪了——那正是「白名单漏登记」的样子。
    if MOVED_TO_GENERATOR:
        print(f"已移出本表（{len(MOVED_TO_GENERATOR)} 条）:")
        for no, why in sorted(MOVED_TO_GENERATOR.items()):
            print(f"  [{no:2}] {why}")
        print()
    # 废除的条目同理：留号空表，读者要知道那个编号为什么不在（2026-08-31）。
    if REMOVED:
        print(f"已废除（{len(REMOVED)} 条，编号留空）:")
        for no, why in sorted(REMOVED.items()):
            print(f"  [{no:2}] {why}")
        print()
    print(f"规范（第 8 节）共 {SPEC_CHECK_COUNT} 条 = 表内 {len(CHECKS)} "
          f"+ 已移出 {len(MOVED_TO_GENERATOR)} + 已废除 {len(REMOVED)}")
    return 0


def main():
    ap = argparse.ArgumentParser(description="地图校验器（第 8 节）")
    ap.add_argument("targets", nargs="*", help="地图文件或目录")
    ap.add_argument("--list", action="store_true",
                    help="只列条目状态，不读地图")
    ap.add_argument("--profile", default="strict",
                    help="阈值档，默认 strict；最小夹具用 fixture")
    ap.add_argument("--thresholds", default=None,
                    help="阈值文件路径，默认 tools/map_gen/thresholds.json")
    a = ap.parse_args()

    if a.list:
        return cmd_list()
    if not a.targets:
        ap.error("需要至少一个地图文件或目录，或用 --list")

    # **阈值在读任何地图之前就加载。** 配置写坏了要以「配置坏了」的名义失败，
    # 而不是变成每张地图上一条看起来像地图问题的报错——同 `StatsMismatch`
    # 与「跑歪了」必须分开那条（`rts_core 接口契约.md` §4.7）。
    try:
        th = thmod.load(a.thresholds).profile(a.profile)
    except thmod.ThresholdError as e:
        print("阈值配置有问题，一张地图都没读：")
        print(f"    {e}")
        return 1

    missing = []
    maps = list(_iter_maps(a.targets, missing))
    # 一张地图都没读到必须红。同 check_determinism_bans.py 的「扫到 0 个文件即失败」：
    # 目录改名或路径写错时，「通过（0 张）」是最坏的那种绿。
    if not maps:
        print("校验失败：一张地图都没读到。")
        print(f"    目标：{'、'.join(a.targets)}")
        print("    这条检查的全部价值在于它真的读过地图，所以路径错时必须红。")
        return 1
    # **路径写错一半也必须红**，理由与上一条同源：覆盖面减半而退出码是 0，
    # 是同一种「该红却绿」，只是不那么显眼。
    if missing:
        print("校验失败：有目标路径不存在。")
        for p in missing:
            print(f"    {p}")
        print(f"    另外 {len(maps)} 张地图读到了，但覆盖面已经不是你以为的那个 —— "
              "退出码必须红，否则目录改名之后没有任何东西会提示。")
        return 1

    total_failed = 0
    for path in maps:
        try:
            doc = mapfile.load(path)
        except mapfile.MapFormatError as e:
            print(f"=== {path} ===")
            print(f"  格式层就没过，后面各条无从谈起：{e}")
            total_failed += 1
            continue
        total_failed += _print_results(run(doc, th=th), str(path))
        # 逐集结点的路线特征（§3.1）：派生量，只进报告、不进地图文件。
        # 验收判据（《方案》§7）是「集结点之间至少两项特征有显著差异」——
        # 它不落成一条检查，是因为「显著」是分布层面的判断，只能在报告里看。
        feats = route_features(doc, Grid(doc))
        if feats:
            print("  路线特征（最窄处/遮蔽/过桥待第 3–5 步地形落地后补）：")
            for f in feats:
                if f["path_len"] is None:
                    print(f"    集结点 {list(f['spawn'])}：走不到 keep（见第 4 条）")
                else:
                    print(f"    集结点 {list(f['spawn'])}：最短路 "
                          f"{f['path_len']} 格，沿途 6 格内资源簇 "
                          f"{f['clusters_near_path']} 个")

    n_impl = sum(1 for c in CHECKS if c.status == IMPLEMENTED)
    n_block = sum(1 for c in CHECKS if c.status == BLOCKED)
    n_pend = sum(1 for c in CHECKS if c.status == PENDING)
    print(f"\n{len(maps)} 张地图（阈值档 {th.name}）；条目状态："
          f"{n_impl} 已实现 / {n_block} 阻塞 / {n_pend} 待阈值"
          f"（表内 {len(CHECKS)} + 已移出 {len(MOVED_TO_GENERATOR)} "
          f"+ 已废除 {len(REMOVED)} = 规范 {SPEC_CHECK_COUNT}）")
    if total_failed:
        print(f"失败：{total_failed} 条检查不通过")
        return 1
    print("全部已实现的检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
