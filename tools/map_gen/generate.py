#!/usr/bin/env python3
"""训练地图生成器 —— `地图与场景设计.md` 第 9 节。

§9 要的是**约束式生成，不是自由噪声**：

- **固定**（配置给出）：地图尺寸（`size`，理由见下）
- **随机**（按种子，从 `thresholds.json` 的 `generator` 段区间里抽）：
  城区半径、集结点数量与选哪几条边、城内资源构成、外部资源簇数量与大小、
  野外散布（森林斑块 / Rock 团块 / 水域与桥）、预置建筑、初始缺口
  （「城墙残血分布」曾在此列，2026-09-01 起 `wall_hp_frac_range` 钉死
  [1.0, 1.0]——试玩反馈「新开局城墙不是满血」，见该键的 `_note_hp`）
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
   踩匹配资源点；**2026-09-01 起再加一座金矿场踩金点**——试玩反馈「完全
   不会刷新金矿」的落点之一，另一落在渲染层：资源点此前根本不画）、
   城外簇点间距 ≥2、每簇 ≥2 种、全局城外金 ≥2、簇距随离城
   距离递增——试玩反馈「城区内外都缺金矿」「大片石头集中在一整块」的落点。

**扩大随机范围的动机**（2026-08-31 那轮）：实战对局不该总是同一张地图——玩家
真正打开游戏时应当每次看到不一样的布局，只有"装饰性的随机"不够。§9 原有的
「固定项若也随机，训练数据里会混入与策略无关的方差」这条顾虑，对**训练地图集**
依然成立（课程学习需要控制变量），但已不适用于**默认对局用的地图池**。两者
共用同一个生成器，只是消费方式不同。

`size` 依然固定，理由见 `thresholds.json` 的 `_FINDING_size_vs_ram_speed`。
**2026-09-01：72 → 144（用户/组长试玩后拍板）**——城区半径不动，扩大的
全部是城外空间。行军占比不变量（第 5 条）随之上限放宽到 0.75，推导与实测
见 thresholds.json 的 `_note_march` 与那条 FINDING 的续篇。

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

1. **集结点八邻不放 `Forest` / `Rock` / 水域**，因此第 10 条在生成图上恒过
   （它的 `starts` 为空就 `continue`）。这不等于第 10 条无用——它保护的是
   手写地图，以及将来允许森林进集结点的更激进策略。
2. **城圈是切比雪夫方环**（正方形），所以集结点（边缘候选点）最多四条边
   各一个。**2026-08-31 重构**：走廊概念取缔后这条上限与走廊无关了，
   变成纯几何事实——`thresholds.py` 在配置加载时拒绝 `spawn_count_range`
   上界超过 4，`generate_one` 里 `rng.sample(_SIDES, n)` 是运行期的第二道防线。

~~不生成 `Water` / `Bridge`~~ **已于 2026-09-01 作废**：`place_water` 现在
真的画湖与河（河必架桥），并且每一块水体落地后做一次集结点→keep 连通
自校验、失败整片撤销——当年「画错一格就切断通路、丢弃率飙升而原因难查」
的担忧由这个自校验吸收，不再靠留白规避。第 12、13 条自此在生成图上做实事。
"""

from __future__ import annotations

import argparse
import math
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

        **按点排名，不按簇**：即便 2026-09-01 三续把 `outer` 点的候选格收紧到
        簇心切比雪夫半径 `_OUTER_CLUSTER_RADIUS` 内（移除森林带机制之后的
        写法，同一簇内的点到 `keep` 的距离跨度已经很小），也没有改回"同一簇
        共享一个波"——按簇心名义距离分配仍可能被半径内的随机取点小幅打乱
        顺序，第 18 条要的是**逐点**严格单调，按点排名不依赖"簇内顺序也单调"
        这条更强的假设，退化路径最稳。
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
    * 其余环格全 `Wall`，hp_frac 按区间取（round 到两位，对齐 `to_doc`）。
      **2026-09-01 起该区间钉死 [1.0, 1.0]**（组长试玩后拍板，池图与演示图
      手感一致；§2.3 的残血设计让位，见 thresholds.json `_note_hp`）。

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
    """城内内容：两片 L 形森林 → 资源（恰好 1 石 / 1 金 / 1 木，2026-09-01 从
    「2 石 / 2 金 / 1–2 木」收紧，试玩反馈「城墙内资源点太多」）→ 预置建筑。

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

    # 城内资源：恰好 1 石 + 1 金 + 1 木（`inner_resources_range` 三种恒 1，
    # 2026-09-01 从「石/金恒 2」收紧——试玩反馈「城墙内资源点太多」；
    # 第 6 条三种各 ≥1 恒成立）。
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
    # （净空区之外）；伐木场/采石场/**金矿场**恒各一座、精确踩对应资源点
    # （第 24 条豁免：采集建筑踩在**匹配**资源点上不算冲突，那是它唯一的
    # 生效摆法）。金矿场是 2026-09-01 追加的——试玩反馈「城内外完全不会
    # 刷新金矿」：金点在数据里一直在，但资源点没有可见物，木/石有预置建筑
    # 当地标而金没有，等于不存在。
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
    gold_pts = [p for k, p in placed if k == "gold"]
    if not wood_pts or not stone_pts or not gold_pts:
        return False
    cv.buildings.append(("Lumber", rng.choice(wood_pts)))
    cv.buildings.append(("Quarry", rng.choice(stone_pts)))
    cv.buildings.append(("Mine", rng.choice(gold_pts)))
    return True


# 簇内点的候选格：簇心切比雪夫半径内的空闲 Plain 格。半径决定「小聚居」
# 有多紧——2026-09-01 三续（团队决定移除「不可围墙」的森林带机制，见
# `place_outer_clusters` docstring 与 `地图与场景设计.md` §2.1 的订正）
# 替代原来「点贴森林带近簇心一截」（`_BELT_POINT_WINDOW`）的写法，取值
# 大致对应原窗口覆盖的范围。
_OUTER_CLUSTER_RADIUS = 4


def place_outer_clusters(cv, cfg, rng, spawn_pts=None):
    """外部资源簇：小聚居的资源点组。

    CLAUDE.md「资源分布形态」要「大散居、小聚居、交错杂居」——簇内混种类、
    簇间隔得开、各簇混合比例不同。**2026-08-31 重构追加三条硬保证**
    （试玩反馈：一簇全石贴成 3×3 团、有图城外零金矿）：

    * **簇内点两两切比雪夫距离 ≥ 2**——「大片石头集中在一整块」的封法；
    * **每簇 ≥ 2 种类型**——簇内抽签后只有一种就补第二种；
    * **全局城外金 ≥ 2 点**——前两个**成功落地**的簇各强制一个 gold
      （簇数下界 2 使这条结构性成立；万一簇全失败，第 25 条会把图否掉，
      换种子重试——校验器是最终裁决）。

    另加一条参考图的设计语言：**簇距随离城距离递增**（越远越散、越少）。

    **2026-09-01 三续：移除「不可围墙」的森林带强制连通要求**（团队决定，
    详见 `地图与场景设计.md` §2.1 的订正）。原机制是 §2.1.1 的结构约束：
    每个 `outer` 资源点必须有一条 `Forest` 4 连通到地图边界的通道（校验器
    第 7 条），实现是 `_carve_forest_belt` 沿主方向挖一条折线森林带、点贴
    在带子靠近簇心的一截上（`_BELT_POINT_WINDOW`）。**移除的直接原因是
    视觉效果**：即便经过两轮加固（#117 加折步、#121 因带子被 #119 拉长后
    再加密折步、加大偏移上限），实测参考图与池图里这条带子在几十格长的
    距离上侧向偏移仍只有 3–5 格——不管调多细的折步参数，玩家看到的仍然
    基本是一条直线，试玩反馈直接指出了这一点。**代价是认下的，不是没想到**：
    去掉这条通道之后，玩家理论上可以砌一整圈墙把外部资源点圈进城内，
    2.1 原文论证的「龟缩退化解复活」风险不再有结构性屏障——团队认为一条
    看起来还是直线的森林带，换来的「反龟缩」保护没有价值到值得保留，
    情愿承担这个风险也要让地图更好看。城外森林现在完全交给
    `scatter_wild_terrain` 的纯装饰性小撮散布。
    """
    R = cfg.city_radius
    lo, hi = cfg.outer_cluster_size
    spawn_pts = list(spawn_pts) if spawn_pts is not None else list(cv.spawns)
    # **8 个方位**（2026-09-01：地图 144 后城外面积 ~4 倍，簇数上界跟着面积走，
    # 只走 4 条对角线装不下；正方向簇与正方向集结点相邻时靠 spawn 距离限制
    # 兜底）。方向列表洗牌后逐个尝试，失败的簇不留残点。
    directions = [(1, 1), (1, -1), (-1, 1), (-1, -1),
                  (1, 0), (-1, 0), (0, 1), (0, -1)]
    rng.shuffle(directions)

    n = min(cfg.outer_clusters, len(directions))
    max_d = min(cv.size // 2 - 4, R + 4 + cfg.outer_cluster_span)
    if max_d <= R + 4:
        return []
    stride = (max_d - (R + 4)) // n
    placed_gold = 0
    clusters = []
    for i in range(n):
        dx, dy = directions[i]
        # 簇心：第 i 簇落在自己的距离带上（越远越散、越少）
        dist = R + 4 + i * stride + rng.randint(0, min(2, stride))
        cxx = cv.keep[0] + dx * dist
        cyy = cv.keep[1] + dy * dist
        if not cv.inside(cxx, cyy):
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
            for c in cv.free_plain_cells(center=(cxx, cyy),
                                         radius=_OUTER_CLUSTER_RADIUS):
                if c in taken or c in cands:
                    continue
                if cv.no_build[c[1]][c[0]] != 0:
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


def scatter_wild_terrain(cv, cfg, rng, spacing=8):
    """城外随机散布（2026-08-31 重构：取缔走廊后的核心一步，AoE4 式）。

    森林斑块与 Rock 团块随机撒在城圈之外。硬约束：

    * **不碰墙/门格及其八邻**——树冠糊墙（2026-08-31 试玩那课）与岩壁贴墙
      （第 17 条：墙不得落在 Rock 上）都要防；
    * **不碰集结点八邻**（模块 docstring 留白 1 的手段，第 10 条恒过）；
    * **团块是「半径填充式圆团」**（2026-09-01 改法，试玩反馈城外散布不美观）：
      在种子的切比雪夫半径内按概率填充、只保留最大 4 连通块——旧的纯随机游走
      产出的是细长蠕虫条带；圆团更接近 AoE4 的林地/岩壁团块观感。
      团块中心之间有最小间距（`spacing`），且**任何新团块不得贴着已有的
      Forest/Rock 生长**（已有的含先落地的团块，#117）——双重隔离，避免
      几块糊成一团；
    * 只落在 `Plain` 且非 `no_build` 的格上——资源点（已进 `occupied()`）
      自然被避开。

    在 `place_outer_clusters` **之后**跑：那时资源点都已落地。
    **2026-09-01 三续**：资源簇不再自带森林带（团队移除了「不可围墙」的
    强制连通要求，见 `place_outer_clusters` docstring）——这里的森林/岩壁
    现在是城外唯一的森林来源，纯装饰性，不承载任何结构约束。
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
    existing_wild = [(x, y) for y in range(cv.size) for x in range(cv.size)
                     if cv.at(x, y) in ("Forest", "Rock")]
    forbidden = wall_zone | spawn_zone | cv.occupied() | zone(existing_wild)

    def valid(p):
        return (cv.inside(*p) and cv.at(*p) == "Plain"
                and cv.no_build[p[1]][p[0]] == 0
                and p not in forbidden and cheb(p) >= R + 2)

    centers = []

    def blob(want):
        """以随机种子为圆心填一个圆团，只保留与圆心 4 连通的部分。"""
        seeds = [c for c in cv.free_plain_cells()
                 if valid(c) and c not in forbidden
                 and all(max(abs(c[0] - s[0]), abs(c[1] - s[1])) >= spacing
                         for s in centers)]
        if not seeds:
            return []
        sx, sy = rng.choice(seeds)
        radius = max(2, math.isqrt(want))
        cells = [(sx, sy)]
        cands = [(x, y) for y in range(sy - radius, sy + radius + 1)
                 for x in range(sx - radius, sx + radius + 1)
                 if (x, y) != (sx, sy)
                 and max(abs(x - sx), abs(y - sy)) <= radius]
        rng.shuffle(cands)
        for c in cands:
            if len(cells) >= want:
                break
            # 越靠外缘概率越低（圆团而不是方团），且必须贴着已长出的格——
            # 不贴的结果是一片内部带空洞的渣，而不是一个团块。
            d = max(abs(c[0] - sx), abs(c[1] - sy))
            if rng.random() > 1.0 - 0.35 * (d / radius) ** 2:
                continue
            if not valid(c):
                continue
            if not any((c[0] + ox, c[1] + oy) in cells
                       for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                continue
            cells.append(c)
        if len(cells) >= 3:
            centers.append((sx, sy))
        return cells

    want = rng.randint(*cfg.forest_patches)
    for _ in range(want):
        cells = blob(rng.randint(*cfg.forest_patch_size))
        if len(cells) < 3:
            continue   # 粉尘斑块直接放弃——第 23 条会否掉它
        for c in cells:
            cv.set_terrain(*c, "Forest")
        forbidden.update(zone(cells))
    for _ in range(cfg.rock_patches):
        cells = blob(rng.randint(*cfg.rock_patch_size))
        if len(cells) < 3:
            continue
        for c in cells:
            cv.set_terrain(*c, "Rock")
        forbidden.update(zone(cells))
    return True


def _connected_to_keep(cv):
    """所有集结点都能走到 keep，且没有 Plain 孤岛（第 4/11/12 条的生成器侧自查）。

    通行定义与 `grid.ground_passable` 一致：Plain/Forest/Bridge 可走，
    Rock/Water 不可走（墙不是障碍，5.1）；8 邻不穿角。**这块逻辑刻意不在
    `grid.py` 里**——Grid 从 doc 解码，而这里查的是在建的 Canvas。
    """
    from collections import deque
    _PASS = ("Plain", "Forest", "Bridge")

    def ok(x, y):
        return cv.inside(x, y) and cv.at(x, y) in _PASS

    kx, ky = cv.keep
    seen = {(kx, ky)}
    q = deque([(kx, ky)])
    while q:
        x, y = q.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if ok(nx, ny) and (nx, ny) not in seen:
                seen.add((nx, ny))
                q.append((nx, ny))
        for dx, dy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
            nx, ny = x + dx, y + dy
            if not (ok(nx, ny) and (nx, ny) not in seen):
                continue
            if ok(x + dx, y) and ok(x, y + dy):   # 不穿角
                seen.add((nx, ny))
                q.append((nx, ny))
    if not all(tuple(s) in seen for s in cv.spawns):
        return False
    for y in range(cv.size):
        for x in range(cv.size):
            if cv.at(x, y) == "Plain" and (x, y) not in seen:
                return False
    return True


def place_water(cv, cfg, rng):
    """水域与桥（**2026-09-01 新增**，模块 docstring 留白 1 作废）。

    * **湖**：`water_lakes_range` 个半径填充式团块（同 `scatter_wild_terrain`
      的圆团画法），落在城外、不贴资源点与集结点；
    * **河**：`rivers_range` 条边到边的正交随机游走（宽 1 格），每约 14 格
      在平直段架一座桥（`Bridge`）——河不架桥会撞上第 12 条，而「靠丢弃
      重生成碰运气」正是当年留白不画水的原因；
    * **每一块水体落地后跑一次 `_connected_to_keep` 自校验，失败整片撤销**
      （湖整个还原、河整条还原）。第 12 条因此仍然会做实事，但不再是
      丢弃率的主要来源——这是本函数敢存在的全部理由。

    零配置（两个 range 上界都是 0）时**不消费任何 rng 就返回**——参考图
    靠固定种子逐字节复现，多一次 `randint(0,0)` 都会改变同一批种子的产出。
    """
    if cfg.water_lakes_range[1] == 0 and cfg.rivers_range[1] == 0:
        return True

    R = cfg.city_radius
    kx, ky = cv.keep

    def cheb(p):
        return max(abs(p[0] - kx), abs(p[1] - ky))

    res_zone = set()
    for _, (px, py), _ in cv.resources:
        for oy in range(-1, 2):
            for ox in range(-1, 2):
                res_zone.add((px + ox, py + oy))

    def water_ok(p, r_pad=0):
        """水域可落的格：城外、Plain、不贴墙/集结点/资源点。

        `r_pad` 把离城与离集结点的距离要求再放宽——湖的**允许带**（斜接瓣
        能探到的范围）比目标半径大，播种时要按允许带留余量，否则种子贴着
        禁区、湖只能往一侧长（实测出来的一锅歪湖）。
        """
        x, y = p
        return (cv.inside(x, y) and cv.at(x, y) == "Plain"
                and cheb(p) >= R + 3 + r_pad
                and all(max(abs(x - s[0]), abs(y - s[1])) >= 4 + r_pad
                        for s in cv.spawns)
                and p not in res_zone
                and p not in cv.occupied())

    # ---- 湖 ----------------------------------------------------------------
    def lake_blob(n):
        # **形状的目标半径与实际允许的半径分开**：目标是「看起来是湖」
        # （半径大、边界不规则——两片湖瓣之间允许只斜接），实际是
        # 「不切断通路」（第 11/12 条镜像判据是连通性而不是形状）。
        # 旧写法两者共用 isqrt(n)，湖被压成 3 格半径的圆点。
        radius = max(5, math.isqrt(n) + 3)
        allow = 2 * radius + 3      # 斜接瓣所在的格仍在允许半径内
        seeds = [c for c in cv.free_plain_cells()
                 if water_ok(c, allow)]
        if not seeds:
            return []
        sx, sy = rng.choice(seeds)
        cells = [(sx, sy)]
        cands = [(x, y) for y in range(sy - allow, sy + allow + 1)
                 for x in range(sx - allow, sx + allow + 1)
                 if (x, y) != (sx, sy)
                 and max(abs(x - sx), abs(y - sy)) <= allow]
        rng.shuffle(cands)
        for c in cands:
            if len(cells) >= n:
                break
            if not water_ok(c):
                continue
            # 越靠目标半径外缘概率越低；目标半径之外（允许带里）再折半——
            # 瓣因此比芯稀疏，湖岸线不规则而不是一个实心方块。
            d = max(abs(c[0] - sx), abs(c[1] - sy))
            p = 1.0 - 0.30 * (d / radius) ** 2
            if d > radius:
                p *= 0.5
            if rng.random() > p:
                continue
            # 八邻贴着已长出的格即可——允许斜接，湖才会长出瓣；
            # 不贴的结果是一片内部带空洞的渣，而不是一个水体。
            if not any((c[0] + ox, c[1] + oy) in cells
                       for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                                      (1, 1), (1, -1), (-1, 1), (-1, -1))):
                continue
            cells.append(c)
        return cells

    for _ in range(rng.randint(*cfg.water_lakes_range)):
        cells = lake_blob(rng.randint(*cfg.water_lake_size))
        if len(cells) < 8:
            continue
        for c in cells:
            cv.set_terrain(*c, "Water")
        if not _connected_to_keep(cv):
            for c in cells:
                cv.set_terrain(*c, "Plain")

    # ---- 河（含桥） ---------------------------------------------------------
    def river_spine():
        """边到边的正交随机游走（宽 1 的主干）。两端点离集结点 ≥ 8、离角 ≥ 6。"""
        size = cv.size
        edges = [("north", lambda t: (t, 0)), ("south", lambda t: (t, size - 1)),
                 ("west", lambda t: (0, t)), ("east", lambda t: (size - 1, t))]
        ea, eb = rng.sample(edges, 2)

        def endpoint(make):
            for _ in range(20):
                t = rng.randint(6, size - 7)
                p = make(t)
                if all(max(abs(p[0] - s[0]), abs(p[1] - s[1])) >= 8
                       for s in cv.spawns):
                    return p
            return None

        start = endpoint(ea[1])
        target_edge = eb[0]

        def on_target(p):
            return {"north": p[1] == 0, "south": p[1] == size - 1,
                    "west": p[0] == 0, "east": p[0] == size - 1}[target_edge]

        def target_dist(p):
            return {"north": p[1], "south": size - 1 - p[1],
                    "west": p[0], "east": size - 1 - p[0]}[target_edge]

        if start is None or not water_ok(start):
            return None
        path = [start]
        visited = {start}
        cur = start
        for _step in range(4 * size):      # 有界：卡住就放弃这条河
            if on_target(cur):
                return path
            nxt = []
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                c = (cur[0] + dx, cur[1] + dy)
                if c in visited or not water_ok(c):
                    continue
                nxt.append(c)
            if not nxt:
                return None
            # 倾向缩短到目标边的距离（70%），否则横向漂——河形因此弯曲但总体过河。
            closer = [c for c in nxt if target_dist(c) < target_dist(cur)]
            cur = rng.choice(closer) if closer and rng.random() < 0.7 \
                else rng.choice(nxt)
            path.append(cur)
            visited.add(cur)
        return None

    def widen(spine):
        """主干 + 逐格随机的左/右/双侧邻居 ⇒ 宽 2–3 的带。

        **宽度沿两侧 jitter 而不是固定右舷**：河在边界附近折返时，「永远在
        同一侧加宽」会把水铺到地图外或压回主干另一侧，视觉上河贴着边走成
        一条沟。加宽格同样要过 `water_ok`（于是加宽不会贴上湖/资源点/集结点）。
        """
        w = set(spine)
        for x, y in spine:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                c = (x + dx, y + dy)
                if c not in w and water_ok(c) and rng.random() < 0.55:
                    w.add(c)
        return w

    def pick_bridges(spine, water):
        """沿主干每隔约 14 格挑一处架桥：桥 = 垂直于河向、贯穿整段水宽的一排格。

        三个条件缺一不可，都是实测踩过的：
        * 主干在该处**平直**（前后三格共线）——弯道上垂直线扫出的是弯内侧的
          斜水，桥头接不上岸；
        * 贯穿带的每一格都是水——否则桥断在水中央；
        * 带两端伸出去的格必须是**岸**（不是水）——河折返贴到自己时，
          不带这条的「桥」两端都泡在水里，不通路。

        逐带筛完还有一道**联合复验**（相邻两条带互为水邻的洞），见函数末尾。
        """
        n = len(spine)
        want = max(1, n // 14)
        out = []
        for k in range(want):
            i = min(n - 2, max(1, (2 * k + 1) * n // (2 * want)))
            for j in range(i, min(i + 14, n - 1)):
                a, b, c = spine[j - 1], spine[j], spine[j + 1]
                if a[0] == b[0] == c[0]:
                    perp = (1, 0)     # 河沿 y 向，桥沿 x 向
                elif a[1] == b[1] == c[1]:
                    perp = (0, 1)
                else:
                    continue
                strip = [b]
                for sign in (1, -1):
                    d = 1
                    while (b[0] + sign * perp[0] * d,
                           b[1] + sign * perp[1] * d) in water:
                        strip.append((b[0] + sign * perp[0] * d,
                                      b[1] + sign * perp[1] * d))
                        d += 1
                    bank = (b[0] + sign * perp[0] * d, b[1] + sign * perp[1] * d)
                    # 岸必须在界内、不是水（湖已先落地，那里是水）。
                    if not cv.inside(*bank) or bank in water \
                            or cv.at(*bank) == "Water":
                        break
                else:
                    # 第 13 条镜像：**每格**桥都要有正交的水邻——贯穿带的端格
                    # 在加宽不齐的河段可能只有斜水邻，那种格混进桥里校验器会红，
                    # 而整带筛成一格又跨不满水宽（桥断在水中央）。整条作废，
                    # 去下一个平直段再试。
                    strip_set = set(strip)
                    if all(any((c[0] + ox, c[1] + oy) in water - strip_set
                               for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                           for c in strip):
                        out.append(strip)
                        break
        # **联合复验（2026-09-01，实测踩到的洞）**：上面是逐带检查，而一条带
        # 的水邻可能是**另一条带**的格——河折返时两条带可以在空间上相邻。
        # 逐带检查时那格还是水（检查通过），两条带都落地后它变成了桥，
        # 于是桥格四邻无水、第 13 条红（`-n 30` 实测 2 次，都是这个形态）。
        # 联合再滤一遍：按「全体桥格都已落地」的终态算水邻，整条带有一格
        # 不过就整条作废——每带独立贯穿水宽，扔掉一条不影响其余。
        all_bridge = {c for strip in out for c in strip}
        return [strip for strip in out
                if all(any((c[0] + ox, c[1] + oy) in water - all_bridge
                           for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                       for c in strip)]

    for _ in range(rng.randint(*cfg.rivers_range)):
        for _try in range(40):
            spine = river_spine()
            if not spine or len(spine) < 12:
                continue
            water = widen(spine)
            bridges = pick_bridges(spine, water)
            if not bridges:
                continue
            bridge_cells = {c for strip in bridges for c in strip}
            for c in water:
                cv.set_terrain(*c, "Bridge" if c in bridge_cells else "Water")
            if _connected_to_keep(cv):
                break
            for c in water:      # 切断且桥没补上——整条还原，换下一条
                cv.set_terrain(*c, "Plain")
    return True


def place_obstacles(cv, cfg, rng):
    """可破坏障碍。**它是一份一次性资源存量**（破坏后产出木材/石材，§4.4.1），
    所以撒多少不是纯装饰问题。

    **2026-09-01（#117）**：只在城外生成（cheb ≥ R+2，与野外散布同域），
    按同类 1–3 个组成彼此分离的小团（组间留两圈空地），不再全图均匀撒。
    返回组列表供 selftest 断言分组不变量；`paint` 消费方忽略返回值。
    """
    lo, hi = cfg.obstacles
    want = rng.randint(int(lo), int(hi))
    kx, ky = cv.keep
    radius = cfg.city_radius
    types = ("Stump", "Rubble", "Sapling")
    occupied = cv.occupied()
    group_zone = set()
    group_index = 0
    groups = []

    def valid(cell):
        x, y = cell
        return (cv.inside(x, y) and cv.at(x, y) == "Plain"
                and cv.no_build[y][x] == 0 and cell not in occupied
                and cell not in group_zone
                and max(abs(x - kx), abs(y - ky)) >= radius + 2)

    while len(cv.obstacles) < want:
        seeds = [c for c in cv.free_plain_cells() if valid(c)]
        if not seeds:
            break
        seed = rng.choice(seeds)
        if group_index == 0 and want >= 3:
            target = 1
        elif group_index == 1 and want >= 3:
            target = 2
        else:
            target = rng.randint(1, 3)
        target = min(target, want - len(cv.obstacles))
        cells = [seed]
        frontier = [seed]
        while len(cells) < target and frontier:
            base = rng.choice(frontier)
            neighbors = [(base[0] + ox, base[1] + oy)
                         for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1))]
            rng.shuffle(neighbors)
            nxt = next((c for c in neighbors if c not in cells and valid(c)), None)
            if nxt is None:
                frontier.remove(base)
                continue
            cells.append(nxt)
            frontier.append(nxt)

        kind = types[group_index % len(types)]
        group_index += 1
        for cell in cells:
            cv.obstacles.append((kind, cell))
            occupied.add(cell)
        groups.append({"type": kind, "cells": list(cells)})
        for px, py in cells:
            for oy in range(-2, 3):
                for ox in range(-2, 3):
                    group_zone.add((px + ox, py + oy))
    return groups


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
        outer_cluster_span=cfg.outer_cluster_span,
        initial_breaches=cfg.initial_breaches,
        wall_hp_frac_range=cfg.wall_hp_frac_range,
        forest_patches=cfg.forest_patches,
        forest_patch_size=cfg.forest_patch_size,
        rock_patches=rock_patches,
        rock_patch_size=cfg.rock_patch_size,
        water_lakes_range=cfg.water_lakes_range,
        water_lake_size=cfg.water_lake_size,
        rivers_range=cfg.rivers_range,
        towers=towers,
        barracks=barracks,
        obstacles=cfg.obstacles,
        max_attempts=cfg.max_attempts,
    )


def paint(cv, cfg, th, rng):
    """把一次生成的全部步骤按**固定顺序**画进 `cv`（`generate_one` 与
    `build_reference_map.py` 共用，参考图「与随机图同一套生成逻辑」的落点）。

    顺序 = 环墙 → 集结点 → 城内内容 → 城外资源簇 → 野外散布 → 水域 → 障碍。
    改动顺序会影响 rng 消费序列，进而改变同一批种子的产出——只能整条改。

    水域放在散布**之后**（2026-09-01）：散布落地在先，河与湖只落 `Plain`，
    遇到已长的森林/岩壁团块就绕开或整片撤销——反过来的话团块挖到河上只能
    整片弃权，那种失败的代价比河改道高（团块数量不算少，逐个重试的成本
    更高）。
    """
    r = _resolve(cfg, rng)
    sides = rng.sample(list(_SIDES), r.spawn_count)
    place_ring_walls(cv, r, rng)
    place_spawns(cv, r, th, rng, sides)
    if not place_inner_content(cv, r, rng):
        raise GenerationFailed("城内放不下配置要求的内容（city_radius 太小？）")
    place_outer_clusters(cv, r, rng)
    scatter_wild_terrain(cv, r, rng)
    place_water(cv, r, rng)
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
