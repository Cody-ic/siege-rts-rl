#!/usr/bin/env python3
"""训练地图生成器 —— `地图与场景设计.md` 第 9 节。

§9 要的是**约束式生成，不是自由噪声**：

- **固定**（配置给出）：地图尺寸（`size`，理由见下）
- **随机**（按种子，从 `thresholds.json` 的 `generator` 段区间里抽）：
  城区半径、集结点数量与选哪几条边、城内资源构成、外部资源簇数量与大小、
  城墙残血分布、野外散布（森林斑块 / Rock 团块）、预置建筑、初始缺口
- 每张生成结果跑第 8 节校验器，**不通过就丢弃重生成**
- `map_id` 编码生成种子，使任何一局训练都可复现

**2026-08-31 重构：取缔走廊、城圈改城墙实体、城外随机散布（AoE4 式）。**
组长拍板的三条结构决定，本文件是其唯一实现处：

1. **城圈 = 一整圈 `initial_walls` 实体**（`place_ring_walls`），不是 `Rock`
   地形——环上除一对相对城门与 1–2 个设计缺口外，每格都是 `Wall`。
   `Rock` 降级为城外随机散布的团块。旧 `build_city_wall_ring` + `carve_corridor`
   那套「天然岩壁承担大部分周长、只在口上摆人工墙」的做法作废
   （`地图与场景设计.md` 里那句表述是错的，已随本次一并订正）。
2. **「走廊」概念取缔**：`spawns[].corridor` 字段废除、四种走廊性质删除，
   集结点是固定边缘候选点（宏观层 PickSpawn 与分兵佯攻不变），城外地形由
   `scatter_wild_terrain` 随机散布。
3. **参考图的设计语言适用于所有随机图**：城内两片 3 格森林 + 2 石 / 2 金 /
   1–2 木、预置建筑（塔贴城门/缺口内侧、兵营在堡垒附近、伐木场/采石场精确
   踩匹配资源点）、城外簇点间距 ≥2、每簇 ≥2 种、全局城外金 ≥2、簇距随离城
   距离递增——试玩反馈「城区内外都缺金矿」「大片石头集中在一整块」的落点。

**扩大随机范围的动机**（2026-08-31 那轮）：实战对局不该总是同一张地图——玩家
真正打开游戏时应当每次看到不一样的布局，只有"装饰性的随机"不够。§9 原有的
「固定项若也随机，训练数据里会混入与策略无关的方差」这条顾虑，对**训练地图集**
依然成立（课程学习需要控制变量），但已不适用于**默认对局用的地图池**。两者
共用同一个生成器，只是消费方式不同。

`size` 依然固定，理由见 `thresholds.json` 的 `_FINDING_size_vs_ram_speed`——
它是唯一被生成器与校验器**批量实测过自洽性**的值（74 格起 60/60 全部因第 5 条
被否），松开后可能在 `max_attempts` 内都收敛不到一张合法图。

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
   理由是收益与风险不对称：水域的价值是地形多样性，而画错一格就切断集结点
   到 keep 的通路（第 12 条），在「不通过就丢弃」的循环里表现为丢弃率飙升
   而原因难查。留给后续，手写的演示地图不受此限。
2. **集结点八邻不放 `Forest` 也不放 `Rock`**，因此第 10 条在生成图上恒过
   （它的 `starts` 为空就 `continue`）。这不等于第 10 条无用——它保护的是
   手写地图，以及将来允许森林进集结点的更激进策略。
3. **城圈是切比雪夫方环**（正方形），所以集结点（边缘候选点）最多四条边
   各一个。**2026-08-31 重构**：走廊概念取缔后这条上限与走廊无关了，
   变成纯几何事实——`thresholds.py` 在配置加载时拒绝 `spawn_count_range`
   上界超过 4，`generate_one` 里 `rng.sample(_SIDES, n)` 是运行期的第二道防线。
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

# 城圈四条边。顺序固定 = 生成确定。名字用方位而不是索引：
# `_SIDES[2]` 在报错信息里没有意义。集结点在这四条边的地图边缘上抽。
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
        self.spawns = []         # [(x, y)]——2026-08-31 起不再携带 corridor 标签
        self.buildings = []      # [(type, (x, y))]——玩家开局已拥有的其余建筑
        self.gates = []          # 城门的格（place_ring_walls 填，城内内容要用）
        self.breaches = []       # 设计缺口的格（同上）
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
        """已被实体占掉的格。生成器自己维护，用于避免同格冲突（第 17、22、24 条）。

        **2026-08-31 重构后必须含 `buildings`**——预置建筑是点位实体，
        漏掉它则障碍/散布/后续建筑可与其同格，而第 24 条查的是产出 doc 的
        冲突、抓不到生成器内部的同格（症状是丢弃率飙升，极难定位）。
        """
        out = {self.keep}
        out.update(p for _, p, _ in self.resources)
        out.update(p for _, p, _ in self.walls)
        out.update(p for _, p in self.obstacles)
        out.update(self.spawns)
        out.update(p for _, p in self.buildings)
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
            "spawns": [{"id": i, "pos": list(p)} for i, p in enumerate(self.spawns)],
            "resources": [{"type": t, "pos": list(p), "tier": tier,
                          "unlock_wave": waves[i]}
                          for i, (t, p, tier) in enumerate(self.resources)],
            "initial_walls": [{"kind": k, "pos": list(p), "hp_frac": round(h, 2)}
                              for k, p, h in self.walls],
            "obstacles": [{"type": t, "pos": list(p)} for t, p in self.obstacles],
            # 玩家开局已拥有的其余建筑（6.2 的 `buildings`，2026-08-31 起
            # 生成器真的产）：塔贴城门/缺口内侧、兵营在堡垒附近、
            # 伐木场/采石场精确踩对应资源点。空数组也仍要写出来，
            # 同 `obstacles` 那条纪律：缺失与刻意为空不可区分。
            "buildings": [{"type": t, "pos": list(p)} for t, p in self.buildings],
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


def _outward(side):
    return {"north": (0, -1), "south": (0, 1), "west": (-1, 0), "east": (1, 0)}[side]


def place_ring_walls(cv, cfg, rng):
    """城圈 = 一整圈初始城墙**实体**（2026-08-31 重构：不是 `Rock` 地形）。

    组长拍板的结构决定：环上每一格都是 `initial_walls` 里的 `Wall`——
    `Rock` 不再承担周长，降级为城外随机散布的团块。环上开三样东西：

    * **一对相对城门**：随机挑一对相对边，各在其环中点放 `Gate`，
      **恒满血**——城门是结构上的既定薄弱点（木质、破坏速率更高），
      残破留给 Wall（§2.3「城圈必须残破」不延伸到门）。
    * **设计缺口**：`initial_breaches` 个随机非门格**不摆墙**——那是攻方
      第 1 波就能钻的洞（「AI 是否发现并利用已有缺口」从第 1 波就能测）。
    * 其余环格全 `Wall`，hp_frac 按区间随机（round 到两位，对齐 `to_doc`）。

    环格的地形保持 `Plain`（墙是实体、不改地形），第 17 条因此天然成立；
    完整性（8R − 2 门 − 缺口）在 doc 层可断言，由 selftest 钉住。
    """
    R = cfg.city_radius
    cx, cy = cv.keep
    ring = _ring_cells(cv.keep, R, cv.size)
    gate_of = {"north": (cx, cy - R), "south": (cx, cy + R),
               "east": (cx + R, cy), "west": (cx - R, cy)}
    pair = rng.choice((("north", "south"), ("east", "west")))
    gate_cells = [gate_of[s] for s in pair]
    cv.gates = gate_cells
    non_gate = [c for c in ring if c not in gate_cells]
    n_breach = rng.randint(*cfg.initial_breaches)
    breaches = set(rng.sample(non_gate, min(n_breach, len(non_gate))))
    cv.breaches = sorted(breaches)
    hlo, hhi = cfg.wall_hp_frac_range
    for cell in ring:
        if cell in gate_cells:
            cv.walls.append(("Gate", cell, 1.0))
        elif cell in breaches:
            continue
        else:
            cv.walls.append(("Wall", cell, round(rng.uniform(hlo, hhi), 2)))
    return cv.gates, cv.breaches


def place_spawns(cv, cfg, th, rng, sides):
    """集结点：固定边缘候选点里随机抽几条边，周围铺 `no_build` 环。

    **2026-08-31 重构**：走廊取缔后集结点不再携带性质标签——「固定候选点」
    这个形状保留（宏观层 PickSpawn 与分兵佯攻都挂在它上面），但每个点就是
    一条边在地图边缘的中点，没有走廊、没有口。

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
    cx, cy = cv.keep
    for side in sides:
        if side == "north":
            pos = (cx, margin)
        elif side == "south":
            pos = (cx, cv.size - 1 - margin)
        elif side == "west":
            pos = (margin, cy)
        else:
            pos = (cv.size - 1 - margin, cy)
        cv.spawns.append(pos)
        # 禁建环。**只标 no_build，不改地形** —— 集结点前的地面必须仍然可通行。
        for oy in range(-ring, ring + 1):
            for ox in range(-ring, ring + 1):
                cv.set_no_build(pos[0] + ox, pos[1] + oy)


def place_inner_content(cv, cfg, rng):
    """城内内容：两片 L 形森林 → 资源（2 石 / 2 金 / 1–2 木）→ 预置建筑。

    顺序是森林 → 资源 → 建筑：资源与建筑都从 `free_plain_cells` 抽，
    森林先落地自然被避开。**堡垒周围 3 格净空（cheb ≤ 3 不放任何内容）**——
    `demo_init` 在 keep+(1,±2) 预置 Tower/Flak、keep+(2,·) 撒 7 个单位，
    地图内容不得与它们撞车（2026-08-31 定，见 `地图与场景设计.md` 4.4.0）。
    返回 False = 放不下（走 `GenerationFailed` 路径，换种子重试）。
    """
    R = cfg.city_radius
    kx, ky = cv.keep

    def cheb(p):
        return max(abs(p[0] - kx), abs(p[1] - ky))

    def plain_cands(r_hi, r_lo=4):
        return [c for c in cv.free_plain_cells(cv.keep, r_hi)
                if r_lo <= cheb(c) and cv.no_build[c[1]][c[0]] == 0]

    # 两片 3 格 L 形森林（参考图同款设计语言）。L 形三格天然 4 连通，
    # 第 23 条（阈值 3）恒过；三格全部 cheb ≤ R−2 不贴墙/门——
    # 树冠糊住城墙、墙被拆后原地补不回（Forest 不可建造）是踩过的课。
    for _ in range(2):
        cands = [c for c in plain_cands(R - 2, 4) if cheb(c) <= R - 3]
        rng.shuffle(cands)
        placed = False
        for ax, ay in cands:
            cells = [(ax, ay), (ax + 1, ay), (ax, ay + 1)]
            if (all(cv.inside(*c) for c in cells)
                    and all(cv.at(*c) == "Plain" for c in cells)
                    and all(c not in cv.occupied() for c in cells)
                    and all(cheb(c) <= R - 2 for c in cells)):
                for c in cells:
                    cv.set_terrain(*c, "Forest")
                placed = True
                break
        if not placed:
            return False

    # 城内资源：构成 2 石 + 2 金 + 1–2 木（`inner_resources_range` 定死了
    # 石/金恒 2——试玩反馈「城区内外都缺金矿」的落点；第 6 条三种各 ≥1 恒成立）。
    placed = []
    for kind in ("stone", "wood", "gold"):
        for _ in range(cfg.inner_resources[kind]):
            cands = plain_cands(R - 2, 4)
            if not cands:
                return False
            pos = rng.choice(sorted(cands))
            cv.resources.append((kind, pos, "inner"))
            placed.append((kind, pos))

    # 预置建筑（微随机化）。塔 2–3 座贴城门/缺口内侧；兵营 1–2 座在堡垒附近
    # （净空区之外）；伐木场/采石场恒各一座、精确踩对应资源点（第 24 条豁免：
    # 采集建筑踩在**匹配**资源点上不算冲突，那是它唯一的生效摆法）。
    anchors = list(cv.gates) + list(cv.breaches)
    for _ in range(cfg.towers):
        cands = [c for c in plain_cands(R - 1, 4)
                 if any(max(abs(c[0] - a[0]), abs(c[1] - a[1])) <= 2
                        for a in anchors)]
        if not cands:
            return False
        cv.buildings.append(("Tower", rng.choice(sorted(cands))))
    for _ in range(cfg.barracks):
        cands = [c for c in plain_cands(R - 2, 4) if cheb(c) <= R - 3]
        if not cands:
            return False
        cv.buildings.append(("Barrack", rng.choice(sorted(cands))))
    wood_pts = [p for k, p in placed if k == "wood"]
    stone_pts = [p for k, p in placed if k == "stone"]
    if not wood_pts or not stone_pts:
        return False
    cv.buildings.append(("Lumber", rng.choice(wood_pts)))
    cv.buildings.append(("Quarry", rng.choice(stone_pts)))
    return True


def place_outer_clusters(cv, cfg, rng, spawn_pts=None):
    """外部资源簇 + 每簇一条通到地图边界的 `Forest` 带。

    CLAUDE.md「资源分布形态」要「大散居、小聚居、交错杂居」——簇内混种类、
    簇间隔得开、各簇混合比例不同。**2026-08-31 重构追加三条硬保证**
    （试玩反馈：一簇全石贴成 3×3 团、有图城外零金矿）：

    * **簇内点两两切比雪夫距离 ≥ 2**——「大片石头集中在一整块」的封法；
    * **每簇 ≥ 2 种类型**——簇内抽签后只有一种就补第二种；
    * **全局城外金 ≥ 2 点**——前两个**成功落地**的簇各强制一个 gold
      （簇数下界 2 使这条结构性成立；万一簇全失败，第 25 条会把图否掉，
      换种子重试——校验器是最终裁决）。

    另加一条参考图的设计语言：**簇距随离城距离递增**（越远越散、越少）。

    §2.1.1 的森林带（第 7 条）：对每个 `outer` 资源点，必须存在一条从它
    出发、到地图边界、全程 `Forest` 的 4 连通路径——顺序仍是**先挖带子、
    再把点贴在带子正交邻格**（先放点再挖带会只服务第一个点，60/60 全否的课）。
    """
    R = cfg.city_radius
    lo, hi = cfg.outer_cluster_size
    spawn_pts = list(spawn_pts) if spawn_pts is not None else list(cv.spawns)
    diagonals = [(1, 1), (1, -1), (-1, 1), (-1, -1)]
    rng.shuffle(diagonals)

    n = min(cfg.outer_clusters, len(diagonals))
    max_d = cv.size // 2 - 4
    if max_d <= R + 4:
        return []
    stride = (max_d - (R + 4)) // n
    placed_gold = 0
    clusters = []
    for i in range(n):
        dx, dy = diagonals[i]
        # 簇心：第 i 簇落在自己的距离带上（越远越散、越少）
        dist = R + 4 + i * stride + rng.randint(0, min(2, stride))
        cxx = cv.keep[0] + dx * dist
        cyy = cv.keep[1] + dy * dist
        if not cv.inside(cxx, cyy):
            continue

        belt = _carve_forest_belt(cv, (cxx, cyy), (dx, dy), spawn_pts)
        if not belt:
            continue

        size_n = rng.randint(lo, hi)
        kinds = [rng.choice(("stone", "wood", "gold")) for _ in range(size_n)]
        forced_gold = False
        if placed_gold < 2:
            kinds[rng.randrange(len(kinds))] = "gold"
            forced_gold = True
        if len(set(kinds)) < 2:
            kinds[rng.randrange(len(kinds))] = rng.choice(
                [k for k in ("stone", "wood", "gold") if k not in kinds])

        # **先攒局部列表，整簇成功才并入**——放不下弃整簇（滚回），
        # 不留下「半个簇」的残点（半个簇仍过校验，但破坏「小聚居」的形状）。
        local = []
        ok = True
        for k in kinds:
            taken = cv.occupied() | {p for _, p in local}
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
                    if any(max(abs(c[0] - pp[0]), abs(c[1] - pp[1])) < 2
                           for _, pp in local):
                        continue
                    cands.append(c)
            if not cands:
                ok = False
                break
            local.append((k, rng.choice(sorted(cands))))
        if not ok:
            continue
        for k, pos in local:
            cv.resources.append((k, pos, "outer"))
        if forced_gold:
            placed_gold += 1
        # 簇记录（供 selftest 断言簇级不变量用；`paint` 消费方忽略返回值）。
        clusters.append({"kinds": [k for k, _ in local],
                         "points": [p for _, p in local],
                         "dist": dist})
    return clusters


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


def scatter_wild_terrain(cv, cfg, rng):
    """城外随机散布（2026-08-31 重构：取缔走廊后的核心一步，AoE4 式）。

    森林斑块与 Rock 团块随机撒在城圈之外。硬约束：

    * **不碰墙/门格及其八邻**——树冠糊墙（2026-08-31 试玩那课）与岩壁贴墙
      （第 17 条：墙不得落在 Rock 上）都要防；
    * **不碰集结点八邻**（模块 docstring 留白 2 的手段，第 10 条恒过）；
    * **团块是随机游走的连通集**（每块 `*_patch_size` 格）——散点抽样会
      产出 1–2 格粉尘，被第 23 条全否；
    * 只落在 `Plain` 且非 `no_build` 的格上——资源簇的森林带与资源点
      （已进 `occupied()` 或已变地形）自然被避开。

    在 `place_outer_clusters` **之后**跑：那时带子与资源点都已落地。
    """
    R = cfg.city_radius
    kx, ky = cv.keep

    def cheb(p):
        return max(abs(p[0] - kx), abs(p[1] - ky))

    def zone(points, r=1):
        out = set()
        for px, py in points:
            for oy in range(-r, r + 1):
                for ox in range(-r, r + 1):
                    out.add((px + ox, py + oy))
        return out

    wall_zone = zone(p for _, p, _ in cv.walls)
    spawn_zone = zone(cv.spawns)
    forbidden = wall_zone | spawn_zone | cv.occupied()

    def valid(p):
        return (cv.inside(*p) and cv.at(*p) == "Plain"
                and cv.no_build[p[1]][p[0]] == 0
                and p not in forbidden and cheb(p) >= R + 2)

    def blob(want):
        """随机游走 want 格的 4 连通团块；撞上不可用格就提前停。"""
        steps = ((1, 0), (-1, 0), (0, 1), (0, -1))
        seeds = [c for c in cv.free_plain_cells()
                 if valid(c) and c not in forbidden]
        # free_plain_cells 已排 occupied，valid 再排墙/集结点邻域与城圈内侧
        if not seeds:
            return []
        cells = [rng.choice(seeds)]
        for _ in range(want - 1):
            for _ in range(8):   # 有限次试探，避免死循环
                x, y = rng.choice(cells)
                dx, dy = rng.choice(steps)
                c = (x + dx, y + dy)
                if valid(c) and c not in cells:
                    cells.append(c)
                    break
            else:
                break
        return cells

    want = rng.randint(*cfg.forest_patches)
    for _ in range(want):
        cells = blob(rng.randint(*cfg.forest_patch_size))
        if len(cells) < 3:
            continue   # 粉尘斑块直接放弃——第 23 条会否掉它
        for c in cells:
            cv.set_terrain(*c, "Forest")
    for _ in range(cfg.rock_patches):
        cells = blob(rng.randint(*cfg.rock_patch_size))
        if len(cells) < 3:
            continue
        for c in cells:
            cv.set_terrain(*c, "Rock")
    return True


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


def _resolve(cfg, rng):
    """把 `thresholds.json` 里的区间抽样成**这一次生成**要用的具体值。

    **必须用调用方传入的、已经按种子播种的 `rng`**——`generate_one()` 用同一个
    种子重建的 `rng` 会重放出完全相同的抽样序列，`map_id` 编码种子、同种子
    逐字节复现这条不变量因此不受影响（`check_generator_is_deterministic` 钉住）。

    返回对象的字段名与 `Generator` 一致（去掉 `_range` 后缀），所以下面
    各步骤函数不知道、也不必知道自己拿到的是"配置里唯一的值"还是"这次抽到的值"。
    """
    cr_lo, cr_hi = cfg.city_radius_range
    city_radius = rng.randint(cr_lo, cr_hi)

    sc_lo, sc_hi = cfg.spawn_count_range
    spawn_count = rng.randint(sc_lo, sc_hi)

    oc_lo, oc_hi = cfg.outer_clusters_range
    outer_clusters = rng.randint(oc_lo, oc_hi)

    tw_lo, tw_hi = cfg.towers_range
    towers = rng.randint(tw_lo, tw_hi)

    bk_lo, bk_hi = cfg.barracks_range
    barracks = rng.randint(bk_lo, bk_hi)

    rp_lo, rp_hi = cfg.rock_patches_range
    rock_patches = rng.randint(rp_lo, rp_hi)

    inner_resources = {t: rng.randint(v[0], v[1])
                        for t, v in cfg.inner_resources_range.items()}

    return SimpleNamespace(
        size=cfg.size,
        city_radius=city_radius,
        spawn_count=spawn_count,
        inner_resources=inner_resources,
        outer_clusters=outer_clusters,
        outer_cluster_size=cfg.outer_cluster_size,
        initial_breaches=cfg.initial_breaches,
        wall_hp_frac_range=cfg.wall_hp_frac_range,
        forest_patches=cfg.forest_patches,
        forest_patch_size=cfg.forest_patch_size,
        rock_patches=rock_patches,
        rock_patch_size=cfg.rock_patch_size,
        towers=towers,
        barracks=barracks,
        obstacles=cfg.obstacles,
        max_attempts=cfg.max_attempts,
    )


def paint(cv, cfg, th, rng):
    """把一次生成的全部步骤按**固定顺序**画进 `cv`（`generate_one` 与
    `build_reference_map.py` 共用，参考图「与随机图同一套生成逻辑」的落点）。

    顺序 = 环墙 → 集结点 → 城内内容 → 城外资源簇 → 野外散布 → 障碍。
    改动顺序会影响 rng 消费序列，进而改变同一批种子的产出——只能整条改。
    """
    r = _resolve(cfg, rng)
    sides = rng.sample(list(_SIDES), r.spawn_count)
    place_ring_walls(cv, r, rng)
    place_spawns(cv, r, th, rng, sides)
    if not place_inner_content(cv, r, rng):
        raise GenerationFailed("城内放不下配置要求的内容（city_radius 太小？）")
    place_outer_clusters(cv, r, rng)
    scatter_wild_terrain(cv, r, rng)
    place_obstacles(cv, r, rng)
    return r


def generate_one(seed, cfg, th):
    """按种子生成一张图。返回 doc；不保证合法，合法性由调用方跑校验器判。"""
    rng = random.Random(seed)
    cv = Canvas(cfg.size)
    paint(cv, cfg, th, rng)
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
    "K 堡垒   W 城墙   G 城门   X 集结点   o 可破坏障碍   B 预置建筑\n"
    "S/L/$ 城内 石/木/金   s/l/g 城外 石/木/金")


def preview(doc):
    """把一张地图打成 ASCII。

    **这不是玩具。** 生成器的真实 bug（林地走廊一株森林都没撒、`defile` 的
    夹壁起点错了一个半径）此前都不会让任何检查变红——校验器查的是「地图
    合不合法」，而它们是「设计语言没体现出来」。重构后同类风险仍在：
    森林/Rock 散布得是否像「随机散布」、资源簇是否「一整块石头」、
    预置建筑是否贴门——**只有看产出才能发现这类失败**（`--preview`）。
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
    for b in doc["buildings"]:
        put(b["pos"], "B")
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
                    help="把每张图打成 ASCII —— 合法但设计语言没体现出来的图"
                         "（资源簇全石、散布不像散布、建筑没贴门）"
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
            print("  集结点：", ", ".join(
                str(s["pos"]) for s in doc["spawns"]))
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
