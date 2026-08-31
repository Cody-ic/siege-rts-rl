#!/usr/bin/env python3
"""地图校验器 —— `地图与场景设计.md` 第 8 节那 22 条的可执行形式。

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

第 1、2、5、14 条要的阈值全部待标定。**待定的是值，不是归口**——
`地图与场景设计.md` §10 明写「一切阈值……应集中在一份配置里，不要硬编码」，
那份配置就是 `thresholds.json`，读取层是 `thresholds.py`。有了归口，
这四条就能实现，占位值随时改而代码不动。

阈值分 `strict` 与 `fixture` 两档，**profile 只改数值、不改「哪些条目跑」**。
`fixture` 存在的唯一理由是 `game/testdata/fixture_min.json` 是一张 7×5 的
最小夹具、不是一张游戏地图：对它套用「边长 ≥ 64」或「行军占 episode 的 10–20%」
没有意义，而格式与拓扑检查对它完全适用。**防止 profile 退化成后门的手段是
一条自检**：strict 下那张夹具必须红（`selftest.py`「profile 不是无操作」一组）。

## 第 20 条不在 `CHECKS` 里，它移到生成器去了

§8.1 早就写了它「更可能是生成器的一份报告，不是 `CHECKS` 里的一行 ——
真要那样就该从本表移走并在 §8.1 说明」。生成器落地后照此办了，
落点是 `generate.py` 的批量报告（丢弃率 + 每条检查的否决计数）。

**它是移走，不是删掉**：`MOVED_TO_GENERATOR` 显式记着它，
而 `selftest.py` 断言「CHECKS 的编号 + 移走的编号 = 规范的 1..22」。
直接删会让这一条从此无声消失，而那正是白名单漏登记那类错误的样子。

## 一条防漂移的断言

`CHECKS` 表加上 `MOVED_TO_GENERATOR` 必须正好覆盖 1..22、不重不漏，
由 `selftest.py` 钉住。第 8 节将来若加条目（例如 #26 的 A5 若通过，要加一条
「§2 必须配 §3」的检查），这条断言会立刻变红提醒同步 —— 白名单漏登记抓不到，
是 `tests/CMakeLists.txt` 里已经踩过的坑。

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
SPEC_CHECK_COUNT = 22

# 从 CHECKS 移走的条目：编号 -> 去哪了。见模块 docstring。
MOVED_TO_GENERATOR = {
    20: "第 7 与第 10 条的联合可满足性 —— §8.1 明写它的产出是**诊断信息**"
        "（丢弃率、哪一条更常否决），而那只有跑一批生成才有。逐图看它没有新东西"
        "可查：两条各自已在第 7、10 条里跑了。落点是 `generate.py` 的批量报告",
}


# --------------------------------------------------------------------------
# 各条检查。签名 (doc, grid) -> 问题列表（空列表 = 通过）；
# 需要阈值的那几条是 (doc, grid, th)，并在 CHECKS 里标 needs_th=True。
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


def check_outer_unenclosable(doc, grid):
    """第 7 条：外部资源点结构性不可围 —— 森林带必须通到地图边界。

    **这条原先标为「阻塞」，理由是判据依 2.1 采用甲还是乙而变。2.1 已定案
    （森林带，随 #23），判据随之定了，而且不需要任何新的地图字段。**
    2.1.1 那句话逐字就是判据：

        对每个 `outer` 资源点，存在一条从它出发、到地图边界、
        全程位于 `Forest` 内的路径。

    ## 连通性取 4 邻接 —— 而第 10 条取 8 邻接，两条都对

    这两条作用在同一层地形上、方向相反（2.1.3），**连通性也必须取反**，
    因为它们问的根本不是同一件事。

    第 10 条问「单位能不能沿森林走到墙边」，那是**移动**；`CLAUDE.md` 把动作定为
    8 方向移动，所以取 8 邻接（还要把穿角单独按可通行判，见那里）。

    本条问的是 2.1.1 的这句：

        连通条件保证任何闭合墙线都必须跨过那条通道

    那是一条**拓扑**论证，与谁能走过去无关：森林带是一条从资源点通到地图外的
    曲线，任何把资源点围在里面的闭合墙线都得跨过它；跨过就得在 `Forest` 上砌墙，
    而 `Forest` 不可建造 ⇒ 围不起来。

    **而斜向森林链撑不起这个论证。** 只靠斜向相连的一条森林带，墙线可以从两格
    之间的那个对角缺口穿过去，一格森林都不碰（`F` 森林、`W` 墙）：

            x=0 1 2
        y=0  F  W  .
        y=1  W  F  .        ← 墙走 (1,0)(0,1)，与 (0,0)(1,1) 两格森林均不重叠
        y=2  .  .  F

    所以本条的森林路径必须是 **4 连通**的。

    ### 真正会把这条写坏的是 `corner=`，不是 `diagonal=`

    这一点反直觉，实测出来的，写在这里免得下一个人像我一样先把探针指错：

    | 取法 | 斜链上的可达集 |
    |---|---|
    | `diagonal=False`（本条采用） | 1 格，**不通** |
    | `diagonal=True`（默认 `corner`） | 1 格，**也不通** —— 与上一行完全相同 |
    | `diagonal=True, corner=grid.is_passable`（第 10 条的写法） | 5 格，**通到边界** |

    中间那行是个恒等式而不是巧合：`neighbors()` 默认 `corner = passable`，
    于是一步斜向要求两个正交角格**都在集合里**，而那两格若都在集合里，
    4 连通本来就已经走到了。**所以「8 连通 + 同一个谓词判角」恒等于 4 连通。**
    （这同时解释了第 10 条为什么**必须**显式给 `corner=` 才能修好那次漏报。）

    因此本条真正的风险是有人照抄第 10 条那一行、把 `corner=grid.is_passable`
    也带过来 —— 那才会让斜链通过检查，而它恰好是本条要拦的东西。
    `selftest.py` 里那条斜链用例盯的是这个，破坏性验证也必须改 `corner=`
    才会红：**只把 `diagonal` 翻成 `True` 是个空探针，会得到一个骗人的绿。**

    `diagonal=False` 仍然明写着，因为它是「4 连通」这个意图的直接表达，
    且不依赖读者知道上面那个恒等式；但要清楚**做功的不是它**。

    ## 这个取法同时绕开一个还没定的问题

    「一圈只靠斜向相连的墙算不算闭合防线」取决于 1c 怎么实现墙：5.1 明写墙是
    **高代价可通行**，所以墙从来不真正阻断移动，「闭合」是防御语义而不是通行语义，
    而那个语义还没有代码。

    不必等它：**4 连通的森林路径会被 4 连通与 8 连通的闭合墙线同时跨过**，
    所以本条对两种答案都成立。这与第 14 条取切比雪夫是同一个手法
    （见 `grid.chebyshev`）—— 挑一个对未决问题单调保守的取法，
    而不是留一个等着被拨错的旋钮。

    ## 起点取四邻，不取八邻

    资源点自己那一格**不可能是 `Forest`**：§8.1 第 16 条要求 `resources` 落在
    可通行**且可建造**的格上，而 `Forest` 不可建造。所以路径只能从资源点旁边的
    森林格起算（仍然把自己那格算进去，免得第 16 条落地前后行为不一致）。

    取四邻的理由与上面同一条：森林带若只斜着挨着资源点，墙线可以从那个对角缺口
    塞进去，资源点与森林带之间根本没连上。

    本条的极性是「**必须存在**一条通路」（`grid.py` 那张极性表），所以少给通路 =
    多报 = 安全那一侧，上面两个取法都落在这一侧。

    没有 `outer` 资源点时自动通过。**这是一个已知的洞，不是判断**：
    「地图必须有 `outer` 资源点」现在一条检查都没查，而
    `CLAUDE.md` 把「部分资源点在墙外」列为不能砍的两个机制之一。见第 10 节。
    """
    def is_forest(x, y):
        return grid.terrain_at(x, y) == "Forest"

    problems = []
    for r in doc["resources"]:
        if r["tier"] != "outer":
            continue
        pos = tuple(r["pos"])
        starts = [n for n in grid.orthogonal_neighbors(*pos) if is_forest(*n)]
        if is_forest(*pos):
            starts.append(pos)
        if not starts:
            problems.append(
                f"outer 资源点 {r['type']} {list(pos)} 的**四邻**里没有 Forest —— "
                f"2.1.1 要求存在一条从它出发、全程 Forest、通到地图边界的路径，"
                f"否则玩家可以砌一圈墙把它圈进城，龟缩退化解复活")
            continue
        # 4 连通，理由见 docstring。**刻意不给 `corner=`** —— 把第 10 条那句
        # `corner=grid.is_passable` 抄过来，斜向森林链就会通过检查。
        reach = grid.flood(starts, is_forest, diagonal=False)
        if any(grid.is_edge(*c) for c in reach):
            continue
        problems.append(
            f"outer 资源点 {r['type']} {list(pos)} 周围的森林带没有通到地图边界"
            f"（4 连通下只覆盖 {len(reach)} 格）—— 玩家只要**绕着**森林带在外面"
            f"画一圈更大的墙就行，全程走 Plain、一格森林都不碰，代价只是墙更长。"
            f"那是纯数值劝退，正是 2.1 要封的形态")
    return problems


def check_forest_corridor(doc, grid):
    """第 10 条：`Forest` 不得构成任一集结点直达城墙的连续遮蔽通道。

    4.1：森林若能连成从集结点直达城墙的连续遮蔽通道，AI 会学到永远走林地潜行，
    其余走廊全部作废。

    「直达城墙」按「触到任一墙段的八邻」判定。地图没有 initial_walls 时这条
    自动通过 —— 没有墙就谈不上「直达城墙」，而 2.3 要求的初始城圈由第 8 条管。

    **穿角必须按「能不能走」判，不能按「是不是森林」判**（`corner=` 那个参数）。
    否则一条**斜向**森林链走得过去、检查却报通过：单位从 (5,1) 斜走到 (6,2) 时
    要求的是那两个角格可通行，而它们完全可以是 `Plain`。这条检查的极性与第 4、
    11、12 条**相反**（那几条是「必须连通」，少给通路是多报；这条是「不得存在
    通路」，少给通路是**漏报**），所以在它身上「保守」不是安全侧。详见
    `grid.py` 模块 docstring 里那张极性表。

    **本条与第 7 条正面对立，且连通性取反**（2.1.3）：那条要求森林**必须**通到
    地图边界、取 **4** 连通；本条要求森林**不得**通到城墙、取 **8** 连通。
    两条都要过。改任一条的连通性之前先读另一条的 docstring —— 两处取的邻接不同
    **不是笔误**，它们问的不是同一件事（本条是**移动**，第 7 条是**拓扑**）。
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
                f"集结点 {list(pos)}（{s['corridor']}）存在一条全程 Forest 的"
                f"遮蔽通道直达城墙附近 {list(hit[0])} —— AI 会学到永远走它，"
                f"其余走廊作废（4.1）")
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
    后者更严（会连 `Forest` 一起拒），而且有一条像样的理由：2.1 的森林带靠
    「森林不可建造」封住围墙，地图作者若把初始墙直接摆在带子上，那条保证就被
    绕过去了。但**扩大检查范围会拒掉今天合法的地图，那属于规范该不该改的问题，
    不该由实现单方面决定** —— 同第 11 条对 `Forest` / `Bridge` 孤岛的处理。
    已记进第 10 节。
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
                f"格式层只查了 id 不重复。两个集结点重合会让第 3 条要求的"
                f"「每条走廊性质不同」在几何上不成立：同一个位置没有两条走廊")
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

    它保护 2.1.5：森林带是 2.1 整节唯一的承载者，玩家若能砍掉一段森林就能照旧
    砌墙围住外部资源簇，于是 2.1 退化成纯数值劝退。`Rock` 那一侧的理由是 2.2
    ——岩壁承担大部分周长，可破坏则墙线随时可能被从侧后打穿。

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


def check_has_outer_resource(doc, grid):
    """第 21 条：至少要有一个 `outer` 资源点。

    **这条是落地第 7 条时查出来的洞**，编号接在 §8.1 的 16..20 之后。

    第 7 条在一个 `outer` 都没有时空过，第 6 条只查 `inner` 侧三种齐全，
    于是**一张把所有资源都放在城里的地图能通过全部检查** —— 而那等于砍掉
    「部分资源点在墙外」，`CLAUDE.md` 把它列为**不能砍的两个机制之一**
    （另一条是「城墙可站人」）。少了它，龟缩退化解不需要绕过 2.1，
    它压根不必出城。

    **只查「≥ 1」。** 具体要几个、三种如何分布、内外配比多少，全是数值，
    现在不定（4.4 只给了「外部偏木材」这个倾向）。而「≥ 1」是结构性的：
    0 与 1 的差别不是强度差别，是那条机制在不在。
    """
    n = sum(1 for r in doc["resources"] if r["tier"] == "outer")
    if n:
        return []
    return ["一个 outer 资源点都没有 —— 「部分资源点在墙外」是 CLAUDE.md 列为"
            "不能砍的两个机制之一（逼守方出城、杀死龟缩退化解、让野战机制有用"
            "武之地）。注意第 7 条在这种地图上**空过**，所以少了本条这张图能"
            "通过全部检查"]


# --------------------------------------------------------------------------
# 注册表
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# 需要阈值的四条（第 1、2、5、14 条）。签名多一个 `th`（一档 Profile）。
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
    而走廊、城区半径、行军距离全部按边长推。
    """
    problems = []
    for name, v in (("宽", doc["size"][0]), ("高", doc["size"][1])):
        if not (th.size_min <= v <= th.size_max):
            problems.append(
                f"{name} = {v}，不在 [{th.size_min}, {th.size_max}] 内"
                f"（profile {th.name}）—— §3 的边长由「城区半径 + 集结点到城墙的"
                f"距离」推出，而后者受行军占比不变量约束（第 5 条）")
    return problems


def check_spawn_count_and_edge(doc, grid, th):
    """第 2 条：集结点数量落在区间内，且每个都贴近地图边缘。

    **数量取区间而不是等于某个数**：集结点数量是待定数值（CLAUDE.md
    「关于数值」），而地图规范曾把它写成「定为 4」——那是把平衡旋钮当成结构
    结论，是那条规则记下的唯一一次违反。区间表达「还没定」。

    结构约束「集结点数 = 走廊种类数」不在这里，它由第 3 条查（那一条不需要阈值，
    所以也不该等阈值）。

    「贴近边缘」查的是到四条边的最小距离：§2.2 的集结区在地图边缘，
    集结点跑到地图中间意味着攻方在城边上凭空出现。
    """
    problems = []
    n = len(doc["spawns"])
    if not (th.spawn_count_min <= n <= th.spawn_count_max):
        problems.append(
            f"集结点 {n} 个，不在 [{th.spawn_count_min}, {th.spawn_count_max}] 内"
            f"（profile {th.name}）—— 它直接决定玩家每波要防几个方向")
    for s in doc["spawns"]:
        x, y = s["pos"]
        d = min(x, y, grid.width - 1 - x, grid.height - 1 - y)
        if d > th.spawn_edge_distance_max:
            problems.append(
                f"集结点 {[x, y]}（{s['corridor']}）距最近的地图边界 {d} 格，"
                f"超过 {th.spawn_edge_distance_max} —— §2.2 的集结区在地图边缘，"
                f"否则攻方等于在城边上凭空出现，侦查与行军时间窗口一起失效")
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
    就通过。理由与第 7 条取 4 连通、第 14 条取切比雪夫同一个手法：
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
                f"集结点 {list(pos)}（{s['corridor']}）走不到任何墙段 —— "
                f"第 4 条查的是到 `keep` 的通路，而「到墙」是另一件事："
                f"墙是高代价可通行，所以第 4 条可能靠**穿墙**通过")
            continue
        steps = min(reach)
        march_ticks = steps / speed
        lo = march_ticks / th.episode_ticks_max
        hi = march_ticks / th.episode_ticks_min
        if hi < th.ram_march_fraction_min or lo > th.ram_march_fraction_max:
            problems.append(
                f"集结点 {list(pos)}（{s['corridor']}）到最近墙段 {steps} 格，"
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
    见 `grid.chebyshev` 与 #29 第三条——同第 7 条那个手法：
    挑一个对未决问题单调保守的取法。

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
                f"集结点 {list(pos)}（{s['corridor']}）到最近可建造格 "
                f"{list(where)} 的切比雪夫距离只有 {best}，"
                f"不大于静态建筑视野半径上限 {cap:g}"
                f"（profile {th.name}；表里最大的是 {worst_bld} 的 {table_max:g}）"
                f"—— 一座永久建筑就能照亮集结区，"
                f"「这波要不要花钱侦查」被一次性买断，整条侦查博弈失效")
    return problems


CHECKS = [
    Check(1, "size 落在给定区间", check_size_range, IMPLEMENTED, "",
          True),
    Check(2, "集结点数量与距边界格数", check_spawn_count_and_edge,
          IMPLEMENTED, "", True),
    Check(3, "每种 corridor 恰好出现一次", check_corridor_kinds, IMPLEMENTED, ""),
    Check(4, "每个集结点到 keep 有通路", check_spawn_reachable, IMPLEMENTED, ""),
    Check(5, "Ram 行军时间占 episode 的比例", check_ram_march_fraction,
          IMPLEMENTED, "", True),
    Check(6, "inner 资源点三种各 ≥ 1", check_inner_resources, IMPLEMENTED, ""),
    Check(7, "外部资源点结构性不可围", check_outer_unenclosable, IMPLEMENTED, ""),
    Check(8, "初始城圈至少一处缺口", check_initial_breach, IMPLEMENTED, ""),
    Check(9, "需人工设防的正面总长落在区间", None, BLOCKED,
          "**它自己有两个阻塞，各记一次**（不是「唯一一条阻塞」——阻塞共几条以报告头为准）："
          "(a) 「正面」的定义依赖城区，"
          "而城区无法从现有字段推导（见 #29 第一条）；(b) 阈值本身也待标定。"
          "**不要合并计数** —— 只解开一个它仍然写不出来。"
          "**注意 2.1 定案没有解开它，反而把 (a) 变难了**：城区 mask 本来是"
          "候选乙带来的，而乙已作废（2.1.1），所以现在没有任何一处会顺带产出城区，"
          "要它就得为它单独改一次 6.2。第 8 与第 7 条都已换掉/定下判据、"
          "**都不再需要城区**，所以本条与它们都不同源"),
    Check(10, "Forest 不构成连续遮蔽通道", check_forest_corridor, IMPLEMENTED, ""),
    Check(11, "无不可达的 Plain 孤岛", check_plain_islands, IMPLEMENTED, ""),
    Check(12, "Water 不得无桥切断走廊", check_water_cuts_corridor, IMPLEMENTED, ""),
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
    print(f"规范（第 8 节）共 {SPEC_CHECK_COUNT} 条 = 表内 {len(CHECKS)} "
          f"+ 已移出 {len(MOVED_TO_GENERATOR)}")
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

    n_impl = sum(1 for c in CHECKS if c.status == IMPLEMENTED)
    n_block = sum(1 for c in CHECKS if c.status == BLOCKED)
    n_pend = sum(1 for c in CHECKS if c.status == PENDING)
    print(f"\n{len(maps)} 张地图（阈值档 {th.name}）；条目状态："
          f"{n_impl} 已实现 / {n_block} 阻塞 / {n_pend} 待阈值"
          f"（表内 {len(CHECKS)} + 已移出 {len(MOVED_TO_GENERATOR)} "
          f"= 规范 {SPEC_CHECK_COUNT}）")
    if total_failed:
        print(f"失败：{total_failed} 条检查不通过")
        return 1
    print("全部已实现的检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
