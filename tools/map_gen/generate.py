#!/usr/bin/env python3
"""训练地图生成器 —— `地图与场景设计.md` 第 9 节。

§9 要的是**约束式生成，不是自由噪声**：

- **固定**（配置给出）：地图尺寸（`size`，理由见下）
- **随机，且 2026-08-31 起范围扩大**（按种子，从 `thresholds.json` 的
  `generator` 段的区间/候选域里抽）：走廊种类与集结点数量（`corridors` 候选域
  + `corridor_count_range`）、城区半径（`city_radius_range`）、走廊口宽度
  （`corridor_mouth_width_range`）、城内资源点数量（`inner_resources_range`，
  三种各自独立抽，下界仍是 1）、外部资源簇数量（`outer_clusters_range`）、
  岩壁轮廓、森林斑块、外部资源点位置、初始缺口、墙的残血分布
- 每张生成结果跑第 8 节校验器，**不通过就丢弃重生成**
- `map_id` 编码生成种子，使任何一局训练都可复现

**这次扩大随机范围的动机**：实战对局不该总是同一张地图——玩家真正打开游戏时
应当每次看到不一样的布局，只有"装饰性的随机"（森林斑块位置之类）不够，连一部分
此前写死的**结构**参数（城区半径、走廊口宽度、走廊种类与数量、城内资源点配比）
也要跟着变，否则每张图读起来还是"同一座城，换了几棵树"。§9 原有的「固定项若也
随机，训练数据里会混入与策略无关的方差」这条顾虑，对**训练地图集**依然成立
（课程学习需要控制变量），但已不适用于**默认对局用的地图池**——那批图的目的从
「给 RL 一致的训练信号」变成「给玩家持久的可玩性」，取舍随之不同。两者共用同一个
生成器，只是消费方式不同：训练侧若仍需要窄范围的一批图，可以用同一份
`thresholds.json` 另开一档 profile 或临时收紧区间，本次不做那一半（当前只有对局
默认地图池这一个消费者）。

`size` 依然固定，理由见下面的 `_FINDING_size_vs_ram_speed`——它是唯一被生成器
与校验器**批量实测过自洽性**的值（74 格起 60/60 全部因第 5 条被否），随它一起
放开风险不对称：其余参数松开后不合法就重试即可，`size` 松开后可能在
`max_attempts` 内都收敛不到一张合法图。

## 为什么需要一批而不是一张

§9 开头：**单张地图训练不够**——策略会记住地图而非学会战术，而这直接威胁核心
论证材料：「AI 是否发现并利用已有缺口」在指标上无法区分「学会了找缺口」与
「记住了那个位置有缺口」。

## 第 20 条落在这里

规范第 20 条（第 7 与第 10 条的联合可满足性）此前挂在校验器的 `CHECKS` 里、
标着「阻塞」。§8.1 早写了它「更可能是生成器的一份报告，不是 `CHECKS` 里的一行」，
因为它问的不是「这张图坏没坏」而是「这组约束合起来还剩多少可行空间」，
**而那只有跑一批才知道**。所以它在这里：批量报告给出丢弃率与逐条否决计数。

丢弃率是一个**能力指标**，不是失败率：它高说明约束互相挤压、生成器需要改策略；
它为 0 也不一定好（可能意味着生成得过于保守，见下面「已知留白」）。

## 已知留白，都是刻意的

1. **不生成 `Water` / `Bridge`。** 于是第 12、13 条在生成图上不做实事。
   理由是收益与风险不对称：水域的价值是地形多样性，而画错一格就切断走廊
   （第 12 条），在「不通过就丢弃」的循环里表现为丢弃率飙升而原因难查。
   留给后续，手写的演示地图不受此限。
2. **集结点的八邻不放 `Forest`**，因此第 10 条在生成图上恒过
   （它的 `starts` 为空就 `continue`）。这不等于第 10 条无用——它保护的是
   手写地图，以及将来允许森林进集结点的更激进策略。
   代价写在这里：**当前策略下林地走廊的森林都在走廊中段**，
   而 §2.2 想要的「看不清来了什么」在集结点附近打了折。
3. **城圈是切比雪夫方环**（正方形），所以走廊最多四条（每边一个口）。
   §2.2 的走廊候选清单正好四种，但 `spawn_count_max` 允许更多——
   真要更多得换城圈形状，生成器会在启动时报错而不是悄悄挤在一条边上。
   **2026-08-31**：走廊数量与种类选择本身已经是随机项（`corridor_count_range`，
   见 `_resolve()`），但这条上限没变——`thresholds.py` 在配置加载时就会拒绝
   `corridor_count_range` 的上界超过候选域大小，本函数里那条
   `len(corridors) > len(_SIDES)` 检查是运行期的第二道防线。
"""

from __future__ import annotations

import argparse
import random
import sys
from collections import Counter
from pathlib import Path
from types import SimpleNamespace

import mapfile
import thresholds as thmod
import validate as validatemod
from grid import Grid

# 城圈四条边 -> (走廊朝哪个方向, 该边上口的中心)。顺序固定 = 生成确定。
# 名字用方位而不是索引：`_SIDES[2]` 在报错信息里没有意义。
_SIDES = ("north", "east", "south", "west")

_TERRAIN_CHAR = {name: str(i) for i, name in enumerate(mapfile.TERRAIN_PALETTE)}


class GenerationFailed(RuntimeError):
    """`max_attempts` 次都没生成出合法地图。携带否决计数，让失败可诊断。"""

    def __init__(self, message, tally=None):
        super().__init__(message)
        self.tally = tally or Counter()


class Canvas:
    """一张在建的地图。只管格子，不管 JSON。"""

    def __init__(self, size):
        self.size = size
        self.terrain = [["Plain"] * size for _ in range(size)]
        self.no_build = [[0] * size for _ in range(size)]
        self.walls = []          # [(kind, (x, y), hp_frac)]
        self.resources = []      # [(type, (x, y), tier)]
        self.obstacles = []      # [(type, (x, y))]
        self.spawns = []         # [((x, y), corridor)]
        self.keep = (size // 2, size // 2)

    # -- 基本操作 ---------------------------------------------------------

    def inside(self, x, y):
        return 0 <= x < self.size and 0 <= y < self.size

    def set_terrain(self, x, y, kind):
        if self.inside(x, y):
            self.terrain[y][x] = kind

    def at(self, x, y):
        return self.terrain[y][x]

    def set_no_build(self, x, y):
        if self.inside(x, y):
            self.no_build[y][x] = 1

    def occupied(self):
        """已被实体占掉的格。生成器自己维护，用于避免同格冲突（第 17、22 条）。"""
        out = {self.keep}
        out.update(p for _, p, _ in self.resources)
        out.update(p for _, p, _ in self.walls)
        out.update(p for _, p in self.obstacles)
        out.update(p for p, _ in self.spawns)
        return out

    def free_plain_cells(self, center=None, radius=None):
        """空闲的 `Plain` 格，可选限制在切比雪夫半径内/外。"""
        taken = self.occupied()
        out = []
        for y in range(self.size):
            for x in range(self.size):
                if self.terrain[y][x] != "Plain" or (x, y) in taken:
                    continue
                if center is not None and radius is not None:
                    d = max(abs(x - center[0]), abs(y - center[1]))
                    if d > radius:
                        continue
                out.append((x, y))
        return out

    # -- 组装 -------------------------------------------------------------

    def _resource_unlock_waves(self):
        """给每个资源点算 `unlock_wave`：`inner` 恒为 1；`outer` 按到 `keep`
        的**实际**距离严格递增排名（第 2 起）——第 18 条查的正是这条单调性。

        **按点排名，不按簇**：`outer` 点的候选格取自整条森林带（从簇心到地图
        边界），同一簇内的点到 `keep` 的距离跨度可以很大、且能与另一簇重叠
        （带子本身就伸到地图边界附近），所以"同一簇共享一个波"这个更好看的
        版本**在当前的带子取点范围下站不住**——实测过，按簇心的名义距离分配
        会在多数图上让某个近簇里"恰好取到带子远端"的点，比另一个远簇"恰好取到
        带子近端"的点还远，第 18 条随之报违反，60 次重试全部耗尽。按点严格
        排名不依赖这条假设，退化成"逐点单调"仍满足规范原句里"越晚解禁越远"
        这条要求，只是粒度比"按簇"更细。
        """
        kx, ky = self.keep

        def cheb(pos):
            return max(abs(pos[0] - kx), abs(pos[1] - ky))

        outer_idx = [i for i, r in enumerate(self.resources) if r[2] == "outer"]
        outer_idx.sort(key=lambda i: cheb(self.resources[i][1]))
        wave = {i: 1 for i, r in enumerate(self.resources) if r[2] == "inner"}
        for rank, i in enumerate(outer_idx):
            wave[i] = 2 + rank
        return wave

    def to_doc(self, map_id, name):
        rows = ["".join(_TERRAIN_CHAR[c] for c in row) for row in self.terrain]
        nb = ["".join(str(v) for v in row) for row in self.no_build]
        waves = self._resource_unlock_waves()
        doc = {
            "format": 1,
            "map_id": map_id,
            "name": name,
            "size": [self.size, self.size],
            "layers": {
                "terrain": {"palette": list(mapfile.TERRAIN_PALETTE), "rows": rows},
                "no_build": {"rows": nb},
            },
            "keep": list(self.keep),
            "spawns": [{"id": i, "pos": list(p), "corridor": c}
                       for i, (p, c) in enumerate(self.spawns)],
            "resources": [{"type": t, "pos": list(p), "tier": tier,
                          "unlock_wave": waves[i]}
                          for i, (t, p, tier) in enumerate(self.resources)],
            "initial_walls": [{"kind": k, "pos": list(p), "hp_frac": round(h, 2)}
                              for k, p, h in self.walls],
            "obstacles": [{"type": t, "pos": list(p)} for t, p in self.obstacles],
        }
        return mapfile.stamp_content_hash(doc)


# --------------------------------------------------------------------------
# 生成的各步。每一步只做一件事，且都拿 rng 与 cfg，不读全局。
# --------------------------------------------------------------------------

def _ring_cells(center, radius, size):
    """切比雪夫方环上的格，按固定顺序（上、右、下、左）产出。"""
    cx, cy = center
    out = []
    for x in range(cx - radius, cx + radius + 1):
        out.append((x, cy - radius))
    for y in range(cy - radius + 1, cy + radius + 1):
        out.append((cx + radius, y))
    for x in range(cx + radius - 1, cx - radius - 1, -1):
        out.append((x, cy + radius))
    for y in range(cy + radius - 1, cy - radius, -1):
        out.append((cx - radius, y))
    return [(x, y) for x, y in out if 0 <= x < size and 0 <= y < size]


def _mouth_cells(side, center, radius, width):
    """某条边上走廊口占的格（城圈上被挖开的那一段）。"""
    cx, cy = center
    half = width // 2
    if side == "north":
        return [(cx + d, cy - radius) for d in range(-half, half + 1)]
    if side == "south":
        return [(cx + d, cy + radius) for d in range(-half, half + 1)]
    if side == "west":
        return [(cx - radius, cy + d) for d in range(-half, half + 1)]
    return [(cx + radius, cy + d) for d in range(-half, half + 1)]


def _outward(side):
    return {"north": (0, -1), "south": (0, 1), "west": (-1, 0), "east": (1, 0)}[side]


def build_city_wall_ring(cv, cfg, rng, sides):
    """城圈：`Rock` 方环 + 每条使用中的边挖一个走廊口。

    §2.2：**天然岩壁承担大部分周长。** 城区其余方向由 `Rock` 封死，
    只有走廊口需要人工墙——这既让单位数上限下的攻击密度足够，也直接实现了
    「防御建筑造价高到无法覆盖所有墙段」：玩家的塔守不住每一个口，必须取舍。
    """
    R = cfg.city_radius
    mouths = {}
    for side in sides:
        mouths[side] = set(_mouth_cells(side, cv.keep, R, cfg.corridor_mouth_width))
    all_mouth = set().union(*mouths.values()) if mouths else set()
    for cell in _ring_cells(cv.keep, R, cv.size):
        if cell not in all_mouth:
            cv.set_terrain(*cell, "Rock")
    return mouths


def carve_corridor(cv, cfg, rng, side, corridor, mouth):
    """从走廊口向外挖到地图边界，并按走廊性质改地形。

    走廊性质对应 §2.2 那张表的四种候选。**性质必须真的体现在地形上**，
    否则「选集结点」这个动作没有可学的信号，分兵佯攻退化成随机分兵。
    """
    dx, dy = _outward(side)
    R = cfg.city_radius
    cx, cy = cv.keep
    # 走廊中心线：从口的中心往外
    mx = sum(c[0] for c in mouth) // len(mouth)
    my = sum(c[1] for c in mouth) // len(mouth)
    half = cfg.corridor_mouth_width // 2

    # 走廊比口略宽一点，免得口两侧的 Rock 把走廊夹成死胡同
    width_half = half + 1
    if corridor == "defile":
        # 隘口：`Rock` 夹出窄道 —— 攻方挤成密集队列，`Tower` 齐射与 `Ram` 的 AOE
        # 都高效；玩家一侧正面窄、好守，但一旦破口无回旋空间。
        width_half = max(1, half - 1)

    cells = []
    steps = cv.size          # 走到边界为止
    for t in range(0, steps):
        bx, by = mx + dx * t, my + dy * t
        if not cv.inside(bx, by):
            break
        for w in range(-width_half, width_half + 1):
            # 垂直于走廊方向铺开
            px = bx + (w if dx == 0 else 0)
            py = by + (w if dy == 0 else 0)
            if cv.inside(px, py):
                cells.append((px, py))
    # 走廊内一律先清成 Plain（可能覆盖城圈的 Rock，这正是「口」的意思）
    for x, y in cells:
        d = max(abs(x - cx), abs(y - cy))
        if d >= R or (x, y) in mouth:
            cv.set_terrain(x, y, "Plain")

    if corridor == "defile":
        # 窄道两侧补 Rock，让它真的窄。
        #
        # **`t` 从 1 起，不是从 `city_radius` 起。** `mx, my` 已经是走廊口的中心、
        # 即已经在城圈上了，再加 R 等于从半径 2R 处开始——那时已经贴着地图边缘。
        # 第一版就是这么写的，症状是「defile 走廊只在最外面几格有夹壁」，
        # 而那在 ASCII 预览里看着像「夹壁短了一点」而不像「起点错了一个半径」。
        for t in range(1, steps):
            bx, by = mx + dx * t, my + dy * t
            if not cv.inside(bx, by):
                break
            for w in (-width_half - 1, width_half + 1):
                px = bx + (w if dx == 0 else 0)
                py = by + (w if dy == 0 else 0)
                if cv.inside(px, py) and cv.at(px, py) == "Plain":
                    cv.set_terrain(px, py, "Rock")
    return cells


def scatter_corridor_forest(cv, cfg, rng, side, mouth, spawn_pos):
    """林地走廊：沿走廊撒 `Forest` 斑块，**斑块之间必须留 `Plain` 间隔**。

    第 10 条禁止森林从集结点连续通到城墙八邻，而 §2.2 的林地走廊又要森林遮蔽
    ——两者只能靠「斑块」而非「连续走廊」共存。这是生成器最容易被否决的地方。

    **集结点八邻不放森林**（见模块 docstring 的留白 2），所以从 `spawn` 起算
    留出两格。
    """
    dx, dy = _outward(side)
    mx = sum(c[0] for c in mouth) // len(mouth)
    my = sum(c[1] for c in mouth) // len(mouth)
    lo, hi = cfg.forest_patch_radius
    placed = 0
    # 从走廊口外两格起，到集结点前三格止；每隔 3..4 格放一簇，簇间留 Plain。
    #
    # **`t` 是「离走廊口多远」，不是「离 keep 多远」** —— `mx, my` 已经在城圈上。
    # 第一版从 `R + 2` 起算，于是每一簇都落在集结点两格以内、被下面那个
    # `continue` 全部跳过，结果**林地走廊一株森林都没有**，而它不报任何错。
    # 这类「循环跑了但一次都没生效」的 bug 只有看产出才发现，
    # 所以生成器的 ASCII 预览（`--preview`）不是玩具。
    t = 2
    end = max(abs(spawn_pos[0] - mx), abs(spawn_pos[1] - my)) - 3
    while t < end:
        bx, by = mx + dx * t, my + dy * t
        r = rng.randint(lo, min(hi, 2))
        for oy in range(-r, r + 1):
            for ox in range(-r, r + 1):
                px, py = bx + ox, by + oy
                if not cv.inside(px, py) or cv.at(px, py) != "Plain":
                    continue
                # 离集结点两格以内不放
                if max(abs(px - spawn_pos[0]), abs(py - spawn_pos[1])) <= 2:
                    continue
                cv.set_terrain(px, py, "Forest")
                placed += 1
        t += r + rng.randint(3, 4)      # 簇间的 Plain 间隔
    return placed


def place_spawns(cv, cfg, th, rng, sides, mouths):
    """集结点：每条走廊在地图边缘的端点，并在周围铺 `no_build` 环。

    `no_build` 环是第 14 条那条**结构约束**的直接实现手段：
    「任何静态建筑的视野都不得覆盖集结区」。用禁建区表达它是最自然的做法——
    一座永久建筑若能照亮集结区，「这波要不要花钱侦查」就被一次性买断，
    `Scout`、`Wraith` 屏蔽、佯攻诱饵会一起失效（CLAUDE.md「集结区」）。
    """
    margin = min(2, th.spawn_edge_distance_max)
    # 环宽取**校验器实际会用的那个上限**，而不是 thresholds.json 里手写的声明。
    # 第 14 条的上限从数值表推导（`Watch` 的视野），声明只是被核对的一方——
    # 这里若还读声明，就会出现「生成器按 8 铺环、校验器按 12 查」，每张图都被否决。
    table_max, _ = thmod.max_building_vision()
    ring = int(max(th.static_vision_radius_max, table_max)) + 1
    for side in sides:
        mouth = mouths[side]
        mx = sum(c[0] for c in mouth) // len(mouth)
        my = sum(c[1] for c in mouth) // len(mouth)
        dx, dy = _outward(side)
        if dx == 0:
            pos = (mx, margin if dy < 0 else cv.size - 1 - margin)
        else:
            pos = (margin if dx < 0 else cv.size - 1 - margin, my)
        cv.spawns.append((pos, side_to_corridor[side]))
        # 禁建环。**只标 no_build，不改地形** —— 走廊必须仍然可通行。
        for oy in range(-ring, ring + 1):
            for ox in range(-ring, ring + 1):
                cv.set_no_build(pos[0] + ox, pos[1] + oy)


def place_initial_walls(cv, cfg, rng, sides, mouths):
    """走廊口摆人工墙，随机留缺口，随机残血。

    §2.3：给初始城圈的三个理由（城内外由地图定义使保底收入稳定、4 周内做不出
    好用的建墙 UX、答辩第一眼就像一座城）；**留缺口**还额外让核心评估指标
    「AI 是否发现并利用已有缺口」**第 1 波就能测**。
    """
    lo, hi = cfg.initial_breaches
    n_breach = rng.randint(lo, hi)
    slots = []
    for side in sides:
        for cell in sorted(mouths[side]):
            slots.append(cell)
    if not slots:
        return
    breaches = set(rng.sample(slots, min(n_breach, len(slots) - 1)))
    hlo, hhi = cfg.wall_hp_frac_range
    for side in sides:
        cells = sorted(mouths[side])
        mid = cells[len(cells) // 2]
        for cell in cells:
            if cell in breaches:
                continue
            # 城门在口的正中：木制、破坏速率高于城墙，是结构上的既定薄弱点
            kind = "Gate" if cell == mid else "Wall"
            cv.walls.append((kind, cell, rng.uniform(hlo, hhi)))


def place_inner_resources(cv, cfg, rng):
    """城内资源点：保底收入的来源，三种都必须有（第 6 条）。"""
    R = cfg.city_radius
    for kind in ("stone", "wood", "gold"):
        want = int(cfg.inner_resources.get(kind, 0))
        for _ in range(want):
            cands = [c for c in cv.free_plain_cells(cv.keep, R - 2)
                     if c != cv.keep and cv.no_build[c[1]][c[0]] == 0]
            if not cands:
                return False
            cv.resources.append((kind, rng.choice(cands), "inner"))
    return True


def place_outer_clusters(cv, cfg, rng, sides, economy_side=None):
    """外部资源簇 + 每簇一条通到地图边界的 `Forest` 带。

    两条设计一起决定了这一步的形状：

    **CLAUDE.md「资源分布形态」**要「大散居、小聚居、交错杂居」——簇内混种类
    （所以每一簇都构成「必救」），簇间隔得开（所以玩家必须选），
    而各簇混合比例不同（所以攻方仍能通过选簇来选择打哪一种资源）。

    **§2.1.1 的森林带**：对每个 `outer` 资源点，必须存在一条从它出发、
    到地图边界、全程位于 `Forest` 内的 **4 连通**路径（校验器第 7 条）。
    否则玩家只要绕着森林带在外面画一圈更大的墙就行，全程走 `Plain`、
    一格森林都不碰——那是纯数值劝退，正是 2.1 要封的形态。

    资源点自己**不能**是 `Forest`（第 16 条要它落在可建造格上，而 `Forest`
    不可建造），所以森林带从它的正交邻格起算。
    """
    R = cfg.city_radius
    lo, hi = cfg.outer_cluster_size
    # 簇放在两条走廊之间的对角方向上，避开走廊本身与集结点的禁建环
    diagonals = [(1, 1), (1, -1), (-1, 1), (-1, -1)]
    rng.shuffle(diagonals)

    # **`economy` 走廊必须真的靠近外部资源簇**，否则 §2.2 那一行只是个标签：
    # 「威胁矿场而非城墙」要成立，走那条走廊的部队得比走别条的更容易摸到矿。
    # 手段是把与它同侧的两个对角优先分配给簇 —— `sort` 是稳定的，
    # 所以组内仍是上面 `shuffle` 给的随机顺序。
    #
    # 刻意**不**把簇直接贴到走廊侧面：那样森林带会挨近集结点与城墙，
    # 撞上第 10、14 条，表现为丢弃率飙升。对角已经足够产生「这条走廊通向钱」
    # 的不对称，而这条不对称正是宏观层要学的信号。
    if economy_side is not None:
        edx, edy = _outward(economy_side)
        diagonals.sort(key=lambda d: 0 if (d[0] == edx or d[1] == edy) else 1)

    n = min(cfg.outer_clusters, len(diagonals))
    spawn_pts = [p for p, _ in cv.spawns]
    for i in range(n):
        dx, dy = diagonals[i]
        # 簇心：城圈外一段距离的对角方向
        dist = rng.randint(R + 4, max(R + 5, cv.size // 2 - 4))
        cxx = cv.keep[0] + dx * dist
        cyy = cv.keep[1] + dy * dist
        if not cv.inside(cxx, cyy):
            continue

        # **顺序要紧：先挖森林带，再把资源点贴在带子旁边。**
        #
        # 反过来（先放点、再给第一个点挖带子）是错的，而它错得很安静：
        # 第 7 条要求**每个** `outer` 点都有一条自己的 4 连通森林路径，
        # 而一条从簇内某一点出发的带子只服务那一个点，同簇其余点仍然「可围」。
        # 实测症状是第 7 条 60 次尝试全否决 —— 丢弃率报告直接指出了是这一条。
        belt = _carve_forest_belt(cv, (cxx, cyy), (dx, dy), spawn_pts)
        if not belt:
            continue

        size_n = rng.randint(lo, hi)
        # 交错杂居：簇内混种类，且各簇比例不同（rng 决定）——于是每一簇都构成
        # 「必救」，而攻方仍能通过选簇来选择打哪一种资源。
        kinds = [rng.choice(("stone", "wood", "gold")) for _ in range(size_n)]
        for k in kinds:
            # 候选：带子的正交邻格里空闲、可建造的 `Plain`。
            # 贴着带子放保证了「资源点的四邻里有 Forest」，那正是第 7 条的起点条件。
            taken = cv.occupied()
            cands = []
            for bx, by in belt:
                for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    c = (bx + ox, by + oy)
                    if not cv.inside(*c) or c in taken or c in cands:
                        continue
                    if cv.at(*c) != "Plain" or cv.no_build[c[1]][c[0]] != 0:
                        continue
                    if any(max(abs(c[0] - s[0]), abs(c[1] - s[1])) <= 3
                           for s in spawn_pts):
                        continue
                    cands.append(c)
            if not cands:
                break
            cv.resources.append((k, rng.choice(sorted(cands)), "outer"))
    return True


def _carve_forest_belt(cv, start, direction, spawn_pts):
    """从 `start` 挖一条 4 连通的 `Forest` 带到最近的地图边界，返回带子的格。

    先走一个轴到边界即可——**不需要走完两个轴**，碰到任意一条边就满足
    「通到地图边界」。选距离更短的那个轴，森林占地更少，第 10 条的压力更小。

    **中途遇到挖不动的格就整条放弃（返回空），不跳过它继续。**
    跳过会留下一条断成两截的带子，而断了的带子仍然「存在」——
    第 7 条会否决它，但那时症状是「丢弃率高」而不是「这里有个 bug」。
    宁可整簇不放，让重生成去换个位置。
    """
    x, y = start
    dx, dy = direction
    to_x = (cv.size - 1 - x) if dx > 0 else x
    to_y = (cv.size - 1 - y) if dy > 0 else y
    if to_x <= to_y:
        step, n = (dx, 0), to_x
    else:
        step, n = (0, dy), to_y

    taken = cv.occupied()
    cells = []
    cx, cy = x, y
    for _ in range(n + 1):
        if not cv.inside(cx, cy):
            break
        # 岩壁不挖开（城圈的完整性比一条森林带重要）；实体格不覆盖；
        # 集结点附近不碰（留白 2：集结点八邻不放森林，第 10 条因此恒过）。
        if cv.at(cx, cy) == "Rock" or (cx, cy) in taken:
            return []
        if any(max(abs(cx - s[0]), abs(cy - s[1])) <= 2 for s in spawn_pts):
            return []
        cells.append((cx, cy))
        cx, cy = cx + step[0], cy + step[1]

    if not cells or not any(cv.inside(*c) and (c[0] in (0, cv.size - 1)
                                               or c[1] in (0, cv.size - 1))
                            for c in cells):
        return []          # 没通到边界，等于没用
    for c in cells:
        cv.set_terrain(*c, "Forest")
    return cells


def place_obstacles(cv, cfg, rng):
    """可破坏障碍。**它是一份一次性资源存量**（破坏后产出木材/石材，§4.4.1），
    所以撒多少不是纯装饰问题。
    """
    lo, hi = cfg.obstacles
    want = rng.randint(int(lo), int(hi))
    cands = cv.free_plain_cells()
    if not cands:
        return
    rng.shuffle(cands)
    types = ("Stump", "Sapling", "Rubble")
    for i in range(min(want, len(cands))):
        cv.obstacles.append((types[i % len(types)], cands[i]))


# `mapfile.CORRIDOR_KINDS` 是取值域；这里把「城圈的哪条边」映射到「走廊性质」。
# 顺序固定，且由配置的 corridors 列表决定用哪几种（§9 的「固定项」）。
side_to_corridor = {}


def _resolve(cfg, rng):
    """把 `thresholds.json` 里的区间/候选域抽样成**这一次生成**要用的具体值。

    **必须用调用方传入的、已经按种子播种的 `rng`**——`generate_one()` 用同一个
    种子重建的 `rng` 会重放出完全相同的抽样序列，`map_id` 编码种子、同种子
    逐字节复现这条不变量因此不受影响（`check_generator_is_deterministic` 钉住）。

    返回的对象字段名与旧的固定式 `Generator` 完全一致（`city_radius`、
    `corridor_mouth_width`、`outer_clusters`、`inner_resources`、`corridors`），
    所以下面 `build_city_wall_ring` 等消费函数一行都不用改——它们本来就不知道
    自己拿到的是"配置里唯一的值"还是"这次抽到的值"。
    """
    k_lo, k_hi = cfg.corridor_count_range
    k = rng.randint(k_lo, min(k_hi, len(cfg.corridors)))
    corridors = rng.sample(cfg.corridors, k)
    rng.shuffle(corridors)   # 哪条边对应哪种走廊性质也随之变化，不止选哪几种

    cr_lo, cr_hi = cfg.city_radius_range
    city_radius = rng.randint(cr_lo, cr_hi)

    cmw_lo, cmw_hi = cfg.corridor_mouth_width_range
    corridor_mouth_width = rng.randint(cmw_lo, cmw_hi)

    oc_lo, oc_hi = cfg.outer_clusters_range
    outer_clusters = rng.randint(oc_lo, oc_hi)

    inner_resources = {t: rng.randint(v[0], v[1])
                        for t, v in cfg.inner_resources_range.items()}

    return SimpleNamespace(
        size=cfg.size,
        city_radius=city_radius,
        corridors=corridors,
        corridor_mouth_width=corridor_mouth_width,
        inner_resources=inner_resources,
        outer_clusters=outer_clusters,
        outer_cluster_size=cfg.outer_cluster_size,
        initial_breaches=cfg.initial_breaches,
        wall_hp_frac_range=cfg.wall_hp_frac_range,
        forest_patches=cfg.forest_patches,
        forest_patch_radius=cfg.forest_patch_radius,
        obstacles=cfg.obstacles,
        max_attempts=cfg.max_attempts,
    )


def generate_one(seed, cfg, th):
    """按种子生成一张图。返回 doc；不保证合法，合法性由调用方跑校验器判。"""
    global side_to_corridor
    rng = random.Random(seed)
    cfg = _resolve(cfg, rng)

    corridors = list(cfg.corridors)
    if len(corridors) > len(_SIDES):
        raise GenerationFailed(
            f"配置给了 {len(corridors)} 条走廊，而当前城圈是方环、只有 "
            f"{len(_SIDES)} 条边可开口。要更多走廊得换城圈形状"
            f"（见 generate.py 模块 docstring 的留白 3）")
    sides = list(_SIDES[:len(corridors)])
    side_to_corridor = dict(zip(sides, corridors))

    cv = Canvas(cfg.size)
    mouths = build_city_wall_ring(cv, cfg, rng, sides)
    for side in sides:
        carve_corridor(cv, cfg, rng, side, side_to_corridor[side], mouths[side])
    place_spawns(cv, cfg, th, rng, sides, mouths)
    place_initial_walls(cv, cfg, rng, sides, mouths)
    if not place_inner_resources(cv, cfg, rng):
        raise GenerationFailed("城内放不下配置要求的资源点数量（city_radius 太小？）")
    eco_side = next((s for s in sides if side_to_corridor[s] == "economy"), None)
    place_outer_clusters(cv, cfg, rng, sides, eco_side)
    # 林地走廊的森林在墙与集结点都定好之后撒，这样才知道该避开哪里
    for side in sides:
        if side_to_corridor[side] == "forest":
            spawn_pos = next(p for p, c in cv.spawns if c == "forest")
            scatter_corridor_forest(cv, cfg, rng, side, mouths[side], spawn_pos)
    place_obstacles(cv, cfg, rng)

    return cv.to_doc(map_id=f"gen_{seed:08d}", name=f"生成图 {seed}")


def generate_valid(seed0, cfg, th, tally=None):
    """生成一张**通过全部检查**的图。丢弃就换种子重试。

    §9：「每张生成结果跑第 8 节校验器，不通过就丢弃重生成」。
    `tally` 累计每条检查的否决次数 —— 那是第 20 条要的诊断信息。
    """
    tally = tally if tally is not None else Counter()
    for attempt in range(cfg.max_attempts):
        seed = seed0 * 1000 + attempt
        try:
            doc = generate_one(seed, cfg, th)
        except GenerationFailed:
            raise
        results = validatemod.run(doc, Grid(doc), th)
        bad = [(chk, ps) for chk, ps in results if ps]
        if not bad:
            return doc, attempt + 1
        for chk, _ in bad:
            tally[chk.no] += 1
    raise GenerationFailed(
        f"{cfg.max_attempts} 次尝试都没生成出合法地图（种子基 {seed0}）", tally)


# ASCII 预览的记号。**全部是 ASCII**，理由同 `validate._marks()`：
# ✓ 之类不在 cp936 里，而 Windows 中文控制台默认就是它。这里从一开始就不用。
_PREVIEW_TERRAIN = {"Plain": ".", "Rock": "#", "Forest": "T",
                    "Water": "~", "Bridge": "="}
_PREVIEW_LEGEND = (
    ". 平地   # 岩壁   T 森林   ~ 水   = 桥\n"
    "K 堡垒   W 城墙   G 城门   X 集结点   o 可破坏障碍\n"
    "S/L/$ 城内 石/木/金   s/l/g 城外 石/木/金")


def preview(doc):
    """把一张地图打成 ASCII。

    **这不是玩具。** 生成器的两个真实 bug（林地走廊一株森林都没撒、
    `defile` 的夹壁起点错了一个半径）都不会让任何检查变红 ——
    校验器查的是「地图合不合法」，而它们是「走廊性质没体现出来」。
    合法但无趣的图会静默进训练集，然后 AI 学不到「选集结点」这个决策，
    而那是宏观层存在的理由。**只有看产出才能发现这类失败。**
    """
    rows = [[_PREVIEW_TERRAIN.get(c, "?") for c in row]
            for row in _terrain_names(doc)]

    def put(pos, ch):
        rows[pos[1]][pos[0]] = ch

    for o in doc["obstacles"]:
        put(o["pos"], "o")
    for r in doc["resources"]:
        inner = {"stone": "S", "wood": "L", "gold": "$"}
        outer = {"stone": "s", "wood": "l", "gold": "g"}
        put(r["pos"], (inner if r["tier"] == "inner" else outer)[r["type"]])
    for w in doc["initial_walls"]:
        put(w["pos"], "W" if w["kind"] == "Wall" else "G")
    for s in doc["spawns"]:
        put(s["pos"], "X")
    put(doc["keep"], "K")

    out = ["".join(r) for r in rows]
    return "\n".join(out) + "\n\n" + _PREVIEW_LEGEND


def _terrain_names(doc):
    palette = doc["layers"]["terrain"]["palette"]
    return [[palette[int(ch)] for ch in row]
            for row in doc["layers"]["terrain"]["rows"]]


def _report(tally, made, attempts_total, out_dir):
    """批量报告 = 规范第 20 条的落点。

    §8.1 明写第 20 条的产出应当是**诊断信息**（丢弃率、哪一条更常否决），
    而不是「这张图坏了」。所以它在这里，而不在 `validate.CHECKS` 里。
    """
    print()
    print("=== 生成报告（规范第 20 条：约束的联合可满足性）===")
    print(f"  产出 {made} 张 → {out_dir}")
    discarded = attempts_total - made
    rate = discarded / attempts_total if attempts_total else 0.0
    print(f"  尝试 {attempts_total} 次，丢弃 {discarded} 次，丢弃率 {rate:.1%}")
    if not tally:
        print("  没有任何检查否决过 —— **这不一定是好消息**：也可能是生成策略")
        print("  过于保守（generate.py 模块 docstring 列了三处刻意的留白）。")
        return
    print("  逐条否决计数（同一张图可能被多条同时否决）：")
    for no, n in sorted(tally.items(), key=lambda kv: (-kv[1], kv[0])):
        chk = next((c for c in validatemod.CHECKS if c.no == no), None)
        title = chk.title if chk else "?"
        print(f"    第 {no:2} 条  {n:4} 次   {title}")
    top = max(tally.items(), key=lambda kv: (kv[1], -kv[0]))
    print(f"  最常否决：第 {top[0]} 条。约束互相挤压时，这一行指出该先松哪一边"
          f"——而这正是第 20 条要回答的问题。")


def main():
    ap = argparse.ArgumentParser(description="训练地图生成器（第 9 节）")
    ap.add_argument("-n", "--count", type=int, default=4, help="生成几张")
    ap.add_argument("--seed", type=int, default=1, help="起始种子")
    ap.add_argument("-o", "--out", default=None,
                    help="输出目录；不给则只校验不落盘（干跑）")
    ap.add_argument("--thresholds", default=None, help="阈值文件")
    ap.add_argument("--preview", action="store_true",
                    help="把每张图打成 ASCII —— 合法但走廊性质没体现出来的图"
                         "不会让任何检查变红，只能看出来")
    a = ap.parse_args()

    try:
        th_all = thmod.load(a.thresholds)
    except thmod.ThresholdError as e:
        print("阈值配置有问题，一张图都没生成：")
        print(f"    {e}")
        return 1
    # **生成器只认 strict。** fixture 那一档是给最小夹具用的，
    # 拿它生成会产出一批「只在夹具标准下合法」的训练图。
    th = th_all.profile("strict")
    cfg = th_all.generator

    out_dir = Path(a.out) if a.out else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    tally = Counter()
    total_attempts = 0
    made = 0
    for i in range(a.count):
        try:
            doc, attempts = generate_valid(a.seed + i, cfg, th, tally)
        except GenerationFailed as e:
            print(f"生成失败：{e}")
            if e.tally:
                print("  否决计数：", dict(sorted(e.tally.items())))
            print("  这不是「地图坏了」，是**约束互相矛盾或生成策略太窄**。")
            return 1
        total_attempts += attempts
        made += 1
        if a.preview:
            print(f"\n--- {doc['map_id']} ---")
            print(preview(doc))
            print("  走廊：", ", ".join(
                f"{s['corridor']}@{s['pos']}" for s in doc["spawns"]))
        if out_dir:
            path = out_dir / f"{doc['map_id']}.json"
            mapfile.save(path, doc)
            print(f"  写出 {path}（{attempts} 次尝试）")
        else:
            print(f"  {doc['map_id']} 通过全部检查（{attempts} 次尝试）")

    _report(tally, made, total_attempts, out_dir or "(干跑，未落盘)")
    return 0


if __name__ == "__main__":
    # 不需要动 `sys.path`：Python 自动把**脚本所在目录**放在 `sys.path[0]`，
    # 所以 `import mapfile` 等在任何工作目录下都成立（已实测：仓库根与 `/` 都行）。
    # 第一版在这里插了一句 `sys.path.insert`，它既多余、又在 import 之后才执行
    # —— 真要靠它的话早就 ImportError 了。同 selftest.py / validate.py 的做法。
    sys.exit(main())
