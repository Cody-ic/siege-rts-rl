#!/usr/bin/env python3
"""训练地图生成器 —— `地图与场景设计.md` 第 9 节。

§9 要的是**约束式生成，不是自由噪声**：

- **固定**（配置给出/推导）：地图尺寸（`size`，2026-09-02 起由四环公式推导）
- **随机**（按种子，从 `thresholds.json` 的 `generator` 段区间里抽）：
  城区半径、集结点数量与角度（2026-09-02 起在集结点环上按角度采样）、
  城内资源构成、内/外环资源簇数量与大小、
  城外地形三形态（岩脊设计隘口 / 林子 / 小撮，§3.2，2026-09-02 第 3 步）、
  受管理的河与桥（桥宽 ≥2、至多切断 1 个集结点，§3.4，第 4 步）与湖、
  预置建筑、初始缺口（2026-09-02 第 5 步起两个缺口分占两个**无门面**，
  §3.5）
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

`size` 依然固定（一个数，不是区间），**但 2026-09-02 起它是推导量**：
`size = 2×(R + D + 14 + W_far + 3)`（《地图生成器大改方案》§2，D=40 已拍板、
W_far=18 占位），推导在 `thresholds.py` 的 `Generator` 里完成。
此前的手写历史（72 → 144）见 `thresholds.json` 的 `_FINDING_size_vs_ram_speed`。

## 为什么需要一批而不是一张

§9 开头：**单张地图训练不够**——策略会记住地图而非学会战术，而这直接威胁核心
论证材料：「AI 是否发现并利用已有缺口」在指标上无法区分「学会了找缺口」与
「记住了那个位置有缺口」。

## 批量报告落在这里

「这组约束合起来还剩多少可行空间」问的不是「这张图坏没坏」，**而那只有跑一批
才知道**。§8.1 早写了这类信息「更可能是生成器的一份报告，不是 `CHECKS` 里的
一行」。所以它在这里：批量报告给出丢弃率与逐条否决计数。

> **它曾经叫「规范第 20 条」，那个编号 2026-09-01 随第 7 条一起废除了**
> （第 20 条问的是「第 7 与第 10 条的联合可满足性」，前一半没了，「联合」就不
> 成立；见 `validate.py` 的 `REMOVED`）。**报告机制本身没有被拆掉**——它服务的
> 是当前表里的**全部**检查，而不是某一条编号的专属产出，所以这里只是把名字
> 摘掉。留着旧编号会让读者去 `--list` 里找一条已经不存在的条目。

丢弃率是一个**能力指标**，不是失败率：它高说明约束互相挤压、生成器需要改策略；
它为 0 也不一定好（可能意味着生成得过于保守，见下面「已知留白」）。

## 已知留白，都是刻意的

1. **集结点八邻不放 `Forest` / `Rock` / 水域**，因此第 10 条在生成图上恒过
   （它的 `starts` 为空就 `continue`）。这不等于第 10 条无用——它保护的是
   手写地图，以及将来允许森林进集结点的更激进策略。
2. ~~**城圈是切比雪夫方环**（正方形），所以集结点（边缘候选点）最多四条边
   各一个~~ **已于 2026-09-02 作废**（《地图生成器大改方案》§3.1，第 1 步）：
   集结点改为在集结点环（切比雪夫半径 R+D 的方环）上**按角度采样**，任意
   角度、相邻角差 ≥ 60°，数量 3–5。上界不再来自「方环四条边」，
   而来自 60° 间隔（6 个就只剩恰好等分一种摆法），`thresholds.py` 的
   `spawn_count_range` 上界检查随之从 4 改为 5。

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
import grid as gridmod
from grid import Grid

# 2026-09-02：`_SIDES`（四条边中点抽边）随《地图生成器大改方案》§3.1 删除——
# 集结点改为在集结点环上按角度采样（`place_spawns`），不再有「四条边」这个形状。

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
        # —— 2026-09-02 大改第 2 步（解禁按簇）——
        # `resource_cluster[i]` = resources[i] 所属簇的序号（城内资源为 None）。
        # `cluster_records[cid]` = place_outer_clusters 返回的簇记录（含簇心与
        # 成员资源下标），`_resource_unlock_waves` 按簇心到 keep 的距离排名分波。
        # 它们不进地图文件（簇成员关系校验器能从点位精确反推，见
        # `validate._outer_clusters`），只是生成期簿记。
        self.resource_cluster = []
        self.cluster_records = []
        self.cluster_unlock_per_wave = 1   # place_outer_clusters 按 cfg 覆写
        self.spawn_angles = []     # place_spawns 填（°），外环簇的扇区排除要用
        # —— 2026-09-02 大改第 3 步（设计隘口，§3.2）——
        # `choke_gaps` = 设计隘口的口的格子（scatter_wild_terrain 填）：
        # place_obstacles 不在这些格上放可破坏障碍（§3.6 拍板：清野不应等于
        # 拆掉地图给的隘口）。`ridge_records` 是生成期簿记（哪条路线被收窄、
        # 脊段与口各是哪些格），供 selftest 断言，不进地图文件。
        self.choke_gaps = set()
        self.ridge_records = []

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
        """给每个资源点算 `unlock_wave`：`inner` 恒为 1；`outer` **按簇**分波
        （2026-09-02 大改第 2 步，《地图生成器大改方案》§3.3）——同簇所有点同波，
        簇的解禁序按簇心到 keep 的切比雪夫距离单调（校验器第 28 条查的正是这条）。

        **按簇排名是按点排名的替代**：旧写法逐点严格排名（同簇的点也被半径内的
        随机取点拆到相邻两波），§3.3 要的是「同簇同波」——一簇是一个决策单元，
        拆波解禁等于把「要不要去打这一簇」拆成四次半吊子的决策。

        每波开几簇由 `cluster_unlock_per_wave` 控制（占位，与实力模型一起标）。
        **2026-09-02 第 6 步：撤掉「按点距并波」的兜底**——它存在只为迁就
        第 18 条（逐点单调），而那条已废除并入第 28 条（簇级单调不蕴含点级
        单调，两条结构性冲突，实测种子基 4001 曾因此白丢一张图）。现在严格按
        簇心排名每 `per_wave` 簇开一波，与第 28 条的判据逐字同构。
        """
        kx, ky = self.keep

        def cheb(pos):
            return max(abs(pos[0] - kx), abs(pos[1] - ky))

        wave = {i: 1 for i, r in enumerate(self.resources) if r[2] == "inner"}
        per_wave = max(1, self.cluster_unlock_per_wave)
        # 簇按簇心距离排名（簇心是落点后按 `grid.cluster_center` 算的，与校验器
        # 从 doc 反推的簇心逐格一致）。**tie-break 也必须与校验器同构**
        # （距离并列时按簇心坐标，不是落点顺序 cid）——2026-09-02 实测种子 1000：
        # 两对簇心等距（cheb 23 与 62），生成器按 cid 分波、校验器按簇心坐标
        # 排序，同距两簇波号一升一降，第 28 条红 2 处。两边 key 逐字相同后，
        # 第 28 条看到的顺序与本函数的分波顺序就是同一个。
        order = sorted(range(len(self.cluster_records)),
                       key=lambda cid: (cheb(self.cluster_records[cid]["center"]),
                                        self.cluster_records[cid]["center"]))
        for rank, cid in enumerate(order):
            cur_wave = 2 + rank // per_wave
            for i in self.cluster_records[cid]["resource_indices"]:
                wave[i] = cur_wave
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


def _ring_faces(cell, keep, R):
    """环格所属的「面」（north/south/east/west 的集合）。

    校验器第 32 条与本函数共用这一个归属规则。**角格同时属于两个面**
    （它既在北边又在西边）——所以生成器放缺口时避开角格（下面
    `place_ring_walls`），让「缺口在哪个面」永远有唯一答案。
    """
    x, y = cell
    cx, cy = keep
    faces = set()
    if y == cy - R:
        faces.add("north")
    if y == cy + R:
        faces.add("south")
    if x == cx - R:
        faces.add("west")
    if x == cx + R:
        faces.add("east")
    return faces


def place_ring_walls(cv, cfg, rng):
    """城圈 = 一整圈初始城墙**实体**（2026-08-31 重构：不是 `Rock` 地形）。

    组长拍板的结构决定：环上每一格都是 `initial_walls` 里的 `Wall`——
    `Rock` 不再承担周长，降级为城外随机散布的团块。环上开三样东西：

    * **一对相对城门**：随机挑一对相对边，各在其环中点放 `Gate`，
      **恒满血**——城门是结构上的既定薄弱点（木质、破坏速率更高），
      残破留给 Wall（§2.3「城圈必须残破」不延伸到门）。
    * **设计缺口**：`initial_breaches` 个环格**不摆墙**——那是攻方
      第 1 波就能钻的洞（「AI 是否发现并利用已有缺口」从第 1 波就能测）。
      **2026-09-02 大改第 5 步（《地图生成器大改方案》§3.5，决定项 6 已拍板）**：
      缺口不再是随机非门格，而是**设计放在没有门的面上**（两个缺口时分占
      两个无门面），且不放角格（角格同属两个面，归属歧义，见
      `_ring_faces`）——门与缺口不同面、≥ 3 个面有入口（校验器第 32 条），
      佯攻因此是打「另一个入口」而不是「另一座门的塔」。
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
    gate_faces = set(pair)
    free_faces = [f for f in ("north", "south", "east", "west")
                  if f not in gate_faces]
    # 缺口候选：不在门面、不是角格的环格（理由见 docstring 与 `_ring_faces`）。
    non_gate = [c for c in ring
                if c not in gate_cells
                and _ring_faces(c, cv.keep, R)
                and not (_ring_faces(c, cv.keep, R) & gate_faces)]
    n_breach = rng.randint(*cfg.initial_breaches)
    breaches = set()
    for i in range(min(n_breach, len(non_gate))):
        # 第 i 个缺口放在 free_faces 里第 i 个面上——两个缺口分占两个无门面，
        # 四个面都有入口；只有一个缺口时也是三个面有入口（第 32 条）。
        face = free_faces[i % len(free_faces)]
        cands = [c for c in non_gate
                 if _ring_faces(c, cv.keep, R) == {face} and c not in breaches]
        if not cands:
            break
        breaches.add(rng.choice(cands))
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


def place_spawns(cv, cfg, th, rng):
    """集结点：集结点环上**按角度采样** 3–5 个，相邻角差 ≥ 60°，周围铺
    `no_build` 环。

    **2026-09-02 大改第 1 步（《地图生成器大改方案》§3.1）**：不再是「四条边
    中点贴边」。集结点环 = 以 keep 为心、切比雪夫半径 R+D 的方环，D 取 strict
    档 `spawn_wall_distance_range` 的中点（当前 40——§2.1 拍板，集结期已随
    #128 落地）。取**方环而不是圆环**的原因：城圈是切比雪夫方环，方环上每
    一格到城圈环的切比雪夫距离**恰好**都是 D（圆环在对角方向会近到
    0.7D−0.3R ≈ 24，校验器第 2 条的 [D_lo, D_hi] 区间根本装不下）——角度
    采样采的是方向，落点是该方向射线与方环的交点。

    相邻角差 ≥ 60° 用**分层抽样**保证（不拒绝采样，rng 消费有界）：把 360°
    等分成 n 段，每段在 [段首+30°, 段尾−30°] 内均匀取一个角——相邻两角
    （含首尾环绕）的差恒 ≥ 60°，且每个方位都可能出现。

    `no_build` 环是第 14 条那条**结构约束**的直接实现手段：
    「任何静态建筑的视野都不得覆盖集结区」。用禁建区表达它是最自然的做法——
    一座永久建筑若能照亮集结区，「这波要不要花钱侦查」就被一次性买断，
    `Scout`、`Wraith` 屏蔽、佯攻诱饵会一起失效（CLAUDE.md「集结区」）。
    """
    # 环宽取**校验器实际会用的那个上限**，而不是 thresholds.json 里手写的声明。
    # 第 14 条的上限从数值表推导（`Watch` 的视野），声明只是被核对的一方——
    # 这里若还读声明，就会出现「生成器按 8 铺环、校验器按 12 查」，每张图都被否决。
    table_max, _ = thmod.max_building_vision()
    ring = int(max(th.static_vision_radius_max, table_max)) + 1
    cx, cy = cv.keep
    L = cfg.city_radius + cfg.spawn_wall_distance
    n = cfg.spawn_count
    sector = 360.0 / n
    angles = [i * sector + 30 + rng.uniform(0, sector - 60) for i in range(n)]
    cv.spawn_angles = angles       # place_outer_clusters 的 ±20° 扇区排除要用
    for ang in angles:
        rad = math.radians(ang)
        ca, sa = math.cos(rad), math.sin(rad)
        # 射线与切比雪夫方环（半径 L）的交点：按主导轴缩放。
        scale = L / max(abs(ca), abs(sa))
        pos = (int(round(cx + ca * scale)), int(round(cy + sa * scale)))
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
    `demo_init` 在 keep+(2,·) 撒 7 个开局单位，地图内容不得与它们撞车
    （2026-08-31 定，见 `地图与场景设计.md` 4.4.0）。

    > **净空那条原先还有一半理由是「`demo_init` 在 keep+(1,±2) 预置
    > Tower/Flak」，2026-09-01 不成立了**：那两座固定偏移的建筑是单张固定
    > 地图时期的写法，换成随机地图池后离墙线 10–13 格，而 `Tower.range = 7`、
    > `Flak.range = 5` ⇒ 永远打不到墙线。`Tower` 已删（本函数预置的 2–3 座
    > 位置本来就对），`Flak` 改为从墙线推（城门内侧）。**净空本身保留**——
    > 那 7 个开局单位仍要地方站。
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
            cv.resource_cluster.append(None)   # 城内资源不属于任何簇
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
# 有多紧——取值 4 自 2026-09-01 三续起沿用至今，2026-09-02 大改第 2 步
# （按环分布）没有动它（《地图生成器大改方案》§3.3：不变）。
# 它同时是校验器反推簇成员的依据：簇内任意两点 ≤ 2×4 = 8，而不同簇的点
# 被强制 ≥ 9（见 `place_outer_clusters`），连通分量（≤8 连边）恰好就是簇。
_OUTER_CLUSTER_RADIUS = 4


def place_outer_clusters(cv, cfg, th, rng):
    """外部资源簇：**按环分布**的小聚居资源点组（2026-09-02 大改第 2 步，
    《地图生成器大改方案》§3.3）。

    四环骨架（§2）里的两环各放一批：

    * **内环带** cheb ∈ [R+4, R+D−14]：`inner_band_clusters` 个近簇，早解禁。
      其中**每个集结点一簇「顺路簇」**钉在该集结点方向上（角度抖动 ±4°）——
      「每条 spawn→keep 最短路 6 格内至少一簇」（校验器第 31 条）的生成器侧
      落点：最短路大体沿径向走，顺路簇就在线的旁边。水域/岩壁的绕行可能把
      最终最短路推离超过 6 格——那种图由校验器否掉换种子（生成器兜底、
      校验器裁决，同本文件其余各步的分工）。
    * **外环带** cheb ∈ [R+D+14, 边缘−3]：`outer_band_clusters` 个远簇，
      晚解禁；**不在任何集结点的 ±20° 扇区内**（§2.2 拍板：到了后期玩家
      扩张要经过或绕过集结点环，但不该被迫打一场打不赢的仗）。远簇数
      ≥ 近簇数（`_resolve` 抽样后抬齐）。

    其余硬保证（校验器第 28/29/31 条的生成器侧）：

    * **类型配额**：每个簇有一个主类型（点数的唯一众数），主类型按
      金→石→木轮转分配（第一簇恒金 ⇒ 内环至少一簇含金，金币冗余框架
      `攻守实力模型与平衡分析.md` §6.1）；轮转使三类簇数两两之差 ≤ 1。
      不再纯随机 + 强制两金（实测旧池图 11/12 张金 ≥ 石）。
    * **簇内构成**：主类型点数 = 簇大小 − 1 或 − 2，其余 1–2 点取另外两种
      各至多一个（保证主类型是唯一众数、且每簇 ≥ 2 种——旧约束沿用）。
    * **不同簇的点两两切比雪夫距离 ≥ 9**：这让校验器能用「≤ 8 连边求连通
      分量」从 doc **精确**反推簇成员（簇内任意两点 ≤ 2×`_OUTER_CLUSTER_RADIUS`
      = 8），地图文件因此不需要新增簇 id 字段（C++ 侧读图的字段不动）。
    * **每簇（的点）到任一集结点 ≥ `cluster_spawn_distance_min`**（第 31 条
      后半：不能在集结区里；strict 档 16 > 禁建环 13，比「不落 no_build」
      更强）。**读阈值档、不硬编码**（2026-09-02 第 6 步：56 格参考图那一档
      客观达不到 16，见 `build_reference_map.py` 的注记）。
    * 簇心两两切比雪夫距离 ≥ 12（§3.3「解除每方位一簇的上限：按角度采样」
      的配套——方位上限没了，间距约束接住「簇间隔得开」）。

    簇内点两两切比雪夫距离 ≥ 2（防「一整块石头」）、每簇 ≥ 2 种：不变。

    簇记录写进 `cv.cluster_records`（解禁分波要用簇心排名，见
    `_resource_unlock_waves`），同时作为返回值供 selftest 断言簇级不变量；
    `paint` 消费方忽略返回值。
    """
    R = cfg.city_radius
    D = cfg.spawn_wall_distance
    inner_lo, inner_hi = R + 4, R + D - 14
    outer_lo, outer_hi = R + D + 14, cv.size // 2 - 3
    kx, ky = cv.keep
    cv.cluster_unlock_per_wave = cfg.cluster_unlock_per_wave

    def cheb(p):
        return max(abs(p[0] - kx), abs(p[1] - ky))

    def angle_of(p):
        return math.degrees(math.atan2(p[1] - ky, p[0] - kx)) % 360.0

    def ang_gap(a, b):
        d = abs(a - b) % 360.0
        return min(d, 360.0 - d)

    # 放置顺序 = 主类型轮转顺序：顺路簇（内环）→ 其余内环簇 → 外环簇。
    # 第一簇恒金 ⇒ 内环至少一簇含金（§6.1 的金币冗余落在内环）。
    _CYCLE = ("gold", "stone", "wood")
    plans = []          # (band, angle 或 None=随机, dist 或 None=随机)
    for ang in cv.spawn_angles:
        # 顺路簇钉在集结点方向、内环带中距（±4° 抖动）。抖动不能大：最短路
        # 被河/岩壁推开几格是常态（桥间隔 ~14 格），第 31 条的 6 格窗口里，
        # 角度抖动与绕行**叠加**——实测 ±8° 抖动下丢弃率明显上升（2026-09-02，
        # 种子基 1 干跑 40% 全红在第 31 条）。
        plans.append(("inner", ang + rng.uniform(-4, 4), R + 15))
    for _ in range(cfg.inner_band_clusters - len(cv.spawn_angles)):
        plans.append(("inner", None, None))
    for _ in range(cfg.outer_band_clusters):
        plans.append(("outer", None, None))

    centers = []        # 已落地簇的（名义）簇心
    placed_points = []  # 已落地簇的全部点（跨簇 ≥ 9 要用）

    def try_place(center, primary):
        """整簇成功才并入；放不下弃整簇（滚回），不留「半个簇」的残点。"""
        cxx, cyy = center
        size_n = rng.randint(*cfg.outer_cluster_size)
        # 异类点数 ≤ size_n − 2：主类型必须 ≥ 2 点才是簇内唯一众数（校验器
        # 第 29 条按众数定簇类型）。size_n ≥ 3 由 thresholds.Generator 断言。
        n_other = rng.randint(1, min(2, size_n - 2))
        # 其余 1–2 点各取一种**不同**的非主类型——主类型因此恒为唯一众数
        # （校验器第 29 条按众数定簇类型），且每簇 ≥ 2 种。
        others = rng.sample([t for t in _CYCLE if t != primary], n_other)
        kinds = [primary] * (size_n - n_other) + others
        rng.shuffle(kinds)
        local = []
        for k in kinds:
            cands = []
            for c in cv.free_plain_cells(center=center,
                                         radius=_OUTER_CLUSTER_RADIUS):
                if c in cands or c in {p for _, p in local}:
                    continue
                if cv.no_build[c[1]][c[0]] != 0:
                    continue
                # 第 31 条后半：簇的点到任一集结点的最小距离读阈值档
                # （strict 16，强于禁建环 13）。**不再硬编码 16**（2026-09-02
                # 第 6 步）：56 格参考图（`reference` 档，D=18）上 16 客观
                # 不可满足——内环带最外点与集结点的射向间距恒 ≈ D−8。负值 =
                # 本档弃用（fixture 手法），生成器不会遇到（只认 strict/
                # reference 两档，都是正值）。
                min_spawn_d = th.cluster_spawn_distance_min
                if min_spawn_d >= 0 and any(
                        max(abs(c[0] - s[0]), abs(c[1] - s[1])) < min_spawn_d
                        for s in cv.spawns):
                    continue
                # 簇内点两两 ≥ 2（防贴成一坨）。
                if any(max(abs(c[0] - pp[0]), abs(c[1] - pp[1])) < 2
                       for _, pp in local):
                    continue
                # 跨簇 ≥ 9：校验器按 ≤8 连边反推簇成员，这条保证反推精确。
                if any(max(abs(c[0] - pp[0]), abs(c[1] - pp[1])) < 9
                       for pp in placed_points):
                    continue
                cands.append(c)
            if not cands:
                return None
            local.append((k, rng.choice(sorted(cands))))
        return local

    clusters = []
    for i, (band, ang, _dist) in enumerate(plans):
        primary = _CYCLE[i % 3]
        lo, hi = (inner_lo, inner_hi) if band == "inner" else (outer_lo, outer_hi)
        if lo > hi:
            # 带为空集（2026-09-02：56 格参考图的外环带 [R+D+14, 半宽−3] 客观上
            # 不存在——《方案》§3.9「参考图可以没有外环，注明」）。直接跳过这簇；
            # 不跳的话 `rng.randint(lo, hi)` 会以 ValueError 炸掉，而那看起来
            # 像程序错误，不像「这条带不存在」这个几何事实。
            continue
        placed = None
        for _try in range(80):
            a = ang if ang is not None else rng.uniform(0, 360)
            # 外环簇不得落在任何集结点的 ±20° 扇区内（§2.2 拍板）。
            if band == "outer" and any(ang_gap(a, sa) <= 20.0
                                       for sa in cv.spawn_angles):
                if ang is not None:
                    break    # 顺路簇不会走这条分支，防御而已
                continue
            d = _dist if _dist is not None else rng.randint(lo, hi)
            d = max(lo, min(hi, d))
            rad = math.radians(a)
            center = (int(round(kx + math.cos(rad) * d)),
                      int(round(ky + math.sin(rad) * d)))
            if not cv.inside(*center) or not (lo <= cheb(center) <= hi):
                continue
            # 簇心间距 ≥ 12（方位上限解除后的「簇间隔得开」）。
            if any(max(abs(center[0] - c0[0]), abs(center[1] - c0[1])) < 12
                   for c0 in centers):
                continue
            local = try_place(center, primary)
            if local is None:
                continue
            placed = (center, local)
            break
        if placed is None:
            continue     # 整簇放弃；配额/环带计数因此破坏时校验器会否掉换种子
        center, local = placed
        cid = len(cv.cluster_records)
        indices = []
        for k, pos in local:
            indices.append(len(cv.resources))
            cv.resources.append((k, pos, "outer"))
            cv.resource_cluster.append(cid)
        centers.append(center)
        placed_points.extend(p for _, p in local)
        rec = {"kinds": [k for k, _ in local],
               "points": [p for _, p in local],
               "resource_indices": indices,
               # 簇心按**落点均值**算（`grid.cluster_center`），不用名义中心——
               # 校验器从 doc 反推出的就是这个值，第 28 条的单调性两边一致。
               "center": gridmod.cluster_center([p for _, p in local]),
               "band": band, "primary": primary}
        cv.cluster_records.append(rec)
        clusters.append(rec)
    return clusters


def scatter_wild_terrain(cv, cfg, rng, spacing=8):
    """城外随机散布（2026-08-31 重构：取缔走廊后的核心一步，AoE4 式）。

    **2026-09-02 大改第 3 步（《地图生成器大改方案》§3.2）：三种形态叠加**——

    * **岩脊（设计隘口）**：隘口是**设计放置**、不是随机筛（§5 决定项 4 已
      拍板）。先算每条 spawn→keep 最短路，对其中 `ridges // 2` 条（1–2 条）
      在内环带上横放一道岩脊线并留 `gap_width_range`（2–4）格的口——岩脊线
      被口分成两段，「两条岩脊之间留隘口」因此自动成立；段数为奇时多出来的
      一段做无口的孤脊（纯地形纹理）。其余路线保持开阔：「哪条路窄」是地图
      的一部分，攻守双方都能读到（校验器第 27 条裁决）。
    * **林子**：`woods` 个 `woods_size`（12–30）格的圆团，只放内环带与外环，
      不贴墙、不贴集结点——职责是遮视野（侦查有价值、伏击有位置）。
    * **小撮（现有）**：森林/岩石小团块，视觉「大散居」，不变。

    硬约束沿用：不碰墙/门八邻、不碰集结点八邻、每块 4 连通、块间留一格
    （新团块不得贴着已有的 Forest/Rock 生长 + 中心最小间距 `spacing`）、
    只落在 `Plain` 且非 `no_build` 的格上，第 10/11/23 条照旧。
    隘口的**口**保持开阔可读：口子格进 `forbidden`（不被后续地形盖住）
    并记入 `cv.choke_gaps`（`place_obstacles` 也不放障碍，§3.6 拍板）。

    阻挡率目标（争夺带，第 26 条；区间见 `thresholds.json`，此处刻意不写数）
    **不由本函数单独保证**——水域在之后才落地，占比要等水落地后才算得准，
    所以补足在 `topup_wild_terrain`（paint 里排在 `place_water` 之后）。
    **实测本函数在争夺带内只落约 1% 的阻挡格**（三形态大半落在带外的外环，
    小撮又铺满全图），带内那 4–9% 全是补量画的——所以「城外疏密」这个旋钮
    在 `wild_blocked_fraction_min` 上，不在本组的 `forest_patches` 上。

    在 `place_outer_clusters` **之后**跑：那时资源点都已落地。
    """
    R = cfg.city_radius
    D = cfg.spawn_wall_distance
    kx, ky = cv.keep
    # 四环骨架（§2）：内环争夺带 / 集结点环 / 外环。林子只放内环带与外环。
    inner_lo, inner_hi = R + 4, R + D - 14
    outer_lo, outer_hi = R + D + 14, cv.size // 2 - 3

    def cheb(p):
        return max(abs(p[0] - kx), abs(p[1] - ky))

    def in_bands(p):
        return inner_lo <= cheb(p) <= inner_hi or outer_lo <= cheb(p) <= outer_hi

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

    def blob(want, ok=valid):
        """以随机种子为圆心填一个圆团，只保留与圆心 4 连通的部分。

        圆团而不是随机游走（2026-09-01，游走产物是细长蠕虫条带）；越靠外缘
        概率越低，且必须贴着已长出的格——不贴的结果是一片内部带空洞的渣。
        `ok` 是落格谓词（林子的环带限制由此进）。
        """
        seeds = [c for c in cv.free_plain_cells()
                 if ok(c) and c not in forbidden
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
            d = max(abs(c[0] - sx), abs(c[1] - sy))
            if rng.random() > 1.0 - 0.35 * (d / radius) ** 2:
                continue
            if not ok(c):
                continue
            if not any((c[0] + ox, c[1] + oy) in cells
                       for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                continue
            cells.append(c)
        if len(cells) >= 3:
            centers.append((sx, sy))
        return cells

    # ---- 岩脊（设计隘口）---------------------------------------------------
    def try_ridge(path, gap_w, rock_n):
        """在 path 上找一个穿越点，横放一道岩脊线（两段、中间留 gap_w 格的口）。

        口的两侧岩段各 ≥ 3 格才读作「脊」（与「小撮 ≥ 3」同一条形态纪律），
        所以 rock_n ≥ 6 由 `thresholds.Generator` 的加载断言保证。
        返回 (脊段格, 口格) 或 None。
        """
        mid = (inner_lo + inner_hi) / 2
        # 穿越点候选：路径在内环带里的格（两端留 2 格余量，脊段沿轴向
        # 还要往外伸）。打乱后按「离带中距」排——隘口落在带的腹地，
        # 不贴带的边缘。
        cands = [i for i in range(1, len(path) - 1)
                 if inner_lo + 2 <= cheb(path[i]) <= inner_hi - 2]
        rng.shuffle(cands)
        cands.sort(key=lambda i: abs(cheb(path[i]) - mid))
        for i in cands:
            p = path[i]
            d = (path[i + 1][0] - path[i - 1][0], path[i + 1][1] - path[i - 1][1])
            # 脊线垂直于行进的主方向：路径主要在 x 向走，脊就沿 y 向横放。
            ax = (0, 1) if abs(d[0]) >= abs(d[1]) else (1, 0)
            glo, ghi = -((gap_w - 1) // 2), gap_w // 2
            seg_l = rock_n // 2
            seg_r = rock_n - seg_l
            rock = ([(p[0] + k * ax[0], p[1] + k * ax[1])
                     for k in range(glo - seg_l, glo)]
                    + [(p[0] + k * ax[0], p[1] + k * ax[1])
                       for k in range(ghi + 1, ghi + 1 + seg_r)])
            gap = [(p[0] + k * ax[0], p[1] + k * ax[1])
                   for k in range(glo, ghi + 1)]
            # 脊段每格：内环带内、空闲 Plain、不贴禁区；口保持开阔可读。
            if not all(valid(c) and inner_lo <= cheb(c) <= inner_hi
                       for c in rock):
                continue
            if not all(cv.at(*g) == "Plain" and cv.no_build[g[1]][g[0]] == 0
                       for g in gap):
                continue
            return rock, gap
        return None

    def lone_ridge(rng_len):
        """无口的孤脊：段数（`ridges_range`）为奇时多出来的那一段，纯地形纹理。"""
        seeds = [c for c in cv.free_plain_cells()
                 if valid(c) and inner_lo + 2 <= cheb(c) <= inner_hi - 2
                 and all(max(abs(c[0] - s[0]), abs(c[1] - s[1])) >= spacing
                         for s in centers)]
        if not seeds:
            return []
        for _try in range(30):
            sx, sy = rng.choice(seeds)
            ax = rng.choice(((1, 0), (0, 1)))
            half = rng_len // 2
            cells = [(sx + k * ax[0], sy + k * ax[1])
                     for k in range(-half, rng_len - half)]
            if all(valid(c) and inner_lo <= cheb(c) <= inner_hi for c in cells):
                centers.append((sx, sy))
                return cells
        return []

    n_segments = cfg.ridges
    n_narrow = min(n_segments // 2, len(cv.spawns) - 1)
    if n_narrow > 0:
        order = list(range(len(cv.spawns)))
        rng.shuffle(order)
        for si in order[:n_narrow]:
            dist = _canvas_dist(cv)
            pos = tuple(cv.spawns[si])
            if pos not in dist:
                continue      # 不通归第 4 条（校验器裁决）
            path = _canvas_path(cv, dist, pos)
            gap_w = rng.randint(*cfg.gap_width_range)
            rock_n = rng.randint(*cfg.ridge_length)
            placed = try_ridge(path, gap_w, rock_n)
            if placed is None:
                continue      # 这条路线放不出隘口；两类路线不齐时第 27 条否决
            rock, gap = placed
            for c in rock:
                cv.set_terrain(*c, "Rock")
            forbidden.update(zone(rock))
            forbidden.update(gap)   # 口保持开阔可读（森林也不盖）
            cv.choke_gaps.update(gap)
            cv.ridge_records.append({"spawn": pos, "rock": rock, "gap": gap})
    for _ in range(n_segments - 2 * n_narrow):
        cells = lone_ridge(rng.randint(*cfg.ridge_length))
        if len(cells) < 3:
            continue
        for c in cells:
            cv.set_terrain(*c, "Rock")
        forbidden.update(zone(cells))

    # ---- 林子（12–30 格圆团，只放内环带与外环，遮视野）-------------------
    for _ in range(cfg.woods):
        cells = blob(rng.randint(*cfg.woods_size), ok=lambda p: valid(p)
                     and in_bands(p))
        if len(cells) < 3:
            continue
        for c in cells:
            cv.set_terrain(*c, "Forest")
        forbidden.update(zone(cells))

    # ---- 小撮（现有形态，视觉「大散居」）-----------------------------------
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


def topup_wild_terrain(cv, cfg, th, rng):
    """争夺带阻挡率补足（2026-09-02 大改第 3 步，§3.2）：三形态铺完、**水域
    落地之后**，把争夺带（墙外 2 格到集结点环）里 Forest+Rock+Water 的占比
    补到 strict 档 `wild_blocked_fraction` 区间下界之上（第 26 条的生成器侧）。

    为什么不能并进 `scatter_wild_terrain`：水是占比的一大块（一条河几百格），
    而它在 paint 顺序里排在散布**之后**——散布阶段算出的占比必然是错的。
    补量只补 `Forest`：它可通行，补多少都不可能切断通路（Rock 补量会撞上
    第 11 条孤岛风险）。补不到下界就交给校验器否决换种子（生成器兜底、
    校验器裁决，同本文件其余各步的分工）。

    下界 ≤ 0 时本档弃用（只有假设中的自定义档会这么写；strict/fixture 一个
    有下界一个恒真，见 thresholds.json）——不消费 rng 直接返回。
    """
    lo = th.wild_blocked_fraction_min
    if lo <= 0:
        return
    R = cfg.city_radius
    D = cfg.spawn_wall_distance
    kx, ky = cv.keep
    size = cv.size

    def cheb(p):
        return max(abs(p[0] - kx), abs(p[1] - ky))

    band = [(x, y) for y in range(size) for x in range(size)
            if R + 2 <= cheb((x, y)) <= R + D]
    if not band:
        return
    band_set = set(band)
    blocked = sum(1 for c in band if cv.at(*c) in ("Forest", "Rock", "Water"))
    target = lo * len(band)
    if blocked >= target:
        return

    def zone(points, r=1):
        out = set()
        for px, py in points:
            for oy in range(-r, r + 1):
                for ox in range(-r, r + 1):
                    out.add((px + ox, py + oy))
        return out

    wall_zone = zone(p for _, p, _ in cv.walls)
    spawn_zone = zone(cv.spawns)
    existing_wild = [(x, y) for y in range(size) for x in range(size)
                     if cv.at(x, y) in ("Forest", "Rock")]
    forbidden = (wall_zone | spawn_zone | cv.occupied() | zone(existing_wild)
                 | cv.choke_gaps)

    def valid(p):
        return (cv.inside(*p) and cv.at(*p) == "Plain"
                and cv.no_build[p[1]][p[0]] == 0
                and p not in forbidden and cheb(p) >= R + 2)

    # 种子从**带内**采（`rng.choice(band)`）而不是全图均匀采——全图采样的
    # 实测（2026-09-02）只有约 1/4 的种子落在带内，300 次补不到下界（实测
    # 停在 5.7%），第 26 条把把否决。团块的生长也限在带内：补量的目标
    # 就是争夺带，带出界的格既不算数也浪费有界次数。
    for _try in range(300):
        if blocked >= target:
            break
        p = rng.choice(band)
        if not valid(p):
            continue
        want = rng.randint(4, 10)
        cells = [p]
        frontier = [p]
        while len(cells) < want and frontier:
            base = rng.choice(frontier)
            nbrs = [(base[0] + ox, base[1] + oy)
                    for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1))]
            rng.shuffle(nbrs)
            nxt = next((c for c in nbrs
                        if c not in cells and c in band_set and valid(c)), None)
            if nxt is None:
                frontier.remove(base)
                continue
            cells.append(nxt)
            frontier.append(nxt)
        if len(cells) < 3:
            continue   # 粉尘斑块直接放弃——第 23 条会否掉它
        for c in cells:
            cv.set_terrain(*c, "Forest")
        blocked += sum(1 for c in cells if c in band_set)
        forbidden.update(zone(cells))


def _canvas_dist(cv):
    """keep 出发的 BFS 距离场（8 邻不穿角），通行定义与 `_connected_to_keep`
    一致：Plain/Forest/Bridge 可走，Rock/Water 不可走（墙不是障碍，5.1）。

    **这块逻辑刻意不在 `grid.py` 里**——Grid 从 doc 解码，而这里查的是在建的
    Canvas。2026-09-02 第 3 步新增：隘口的设计放置（`scatter_wild_terrain`）
    要先知道每条 spawn→keep 最短路，`_connected_to_keep` 同步改用它。
    """
    from collections import deque
    _PASS = ("Plain", "Forest", "Bridge")

    def ok(x, y):
        return cv.inside(x, y) and cv.at(x, y) in _PASS

    dist = {cv.keep: 0}
    q = deque([cv.keep])
    while q:
        x, y = q.popleft()
        d = dist[(x, y)] + 1
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if ok(nx, ny) and (nx, ny) not in dist:
                dist[(nx, ny)] = d
                q.append((nx, ny))
        for dx, dy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
            nx, ny = x + dx, y + dy
            if not (ok(nx, ny) and (nx, ny) not in dist):
                continue
            if ok(x + dx, y) and ok(x, y + dy):   # 不穿角
                dist[(nx, ny)] = d
                q.append((nx, ny))
    return dist


def _canvas_path(cv, dist, pos):
    """沿距离场从 pos 走回 keep（梯度路径，`validate._walk_shortest_path`
    的 Canvas 版）。最短路有无数条，这里沿梯度任取一条代表。"""
    _PASS = ("Plain", "Forest", "Bridge")

    def ok(x, y):
        return cv.inside(x, y) and cv.at(x, y) in _PASS

    def neigh(x, y):
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if ok(nx, ny):
                yield nx, ny
        for dx, dy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
            nx, ny = x + dx, y + dy
            if ok(nx, ny) and ok(x + dx, y) and ok(x, y + dy):
                yield nx, ny

    path = [pos]
    cur = pos
    while cur != cv.keep:
        d = dist[cur]
        nxt = next((nb for nb in neigh(*cur) if dist.get(nb) == d - 1), None)
        if nxt is None:      # 不应发生：pos ∈ dist 已保证有 predecessor
            break
        path.append(nxt)
        cur = nxt
    return path


def _connected_to_keep(cv):
    """所有集结点都能走到 keep，且没有 Plain 孤岛（第 4/11/12 条的生成器侧自查）。

    通行定义与 `grid.ground_passable` 一致：Plain/Forest/Bridge 可走，
    Rock/Water 不可走（墙不是障碍，5.1）；8 邻不穿角。
    """
    seen = set(_canvas_dist(cv))
    if not all(tuple(s) in seen for s in cv.spawns):
        return False
    for y in range(cv.size):
        for x in range(cv.size):
            if cv.at(x, y) == "Plain" and (x, y) not in seen:
                return False
    return True


def _river_cut_spawns(cv):
    """不踩桥就走不到 keep 的集结点列表（第 30 条「被河切断」的生成器侧）。

    与 `_canvas_dist` 的唯一差别是 Bridge 也不算可通行——切断与否问的正是
    「没有桥还过不过得去」（§3.4：被切 ≤ 1 是拍板值，被切的那条路线在
    路线特征里标「过桥」）。
    """
    from collections import deque
    _PASS = ("Plain", "Forest")

    def ok(x, y):
        return cv.inside(x, y) and cv.at(x, y) in _PASS

    seen = {cv.keep}
    q = deque([cv.keep])
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
            if ok(x + dx, y) and ok(x, y + dy):
                seen.add((nx, ny))
                q.append((nx, ny))
    return [tuple(s) for s in cv.spawns if tuple(s) not in seen]


def place_water(cv, cfg, th, rng):
    """水域与桥（**2026-09-01 新增**，模块 docstring 留白 1 作废）。

    * **湖**：`water_lakes_range` 个半径填充式团块（同 `scatter_wild_terrain`
      的圆团画法），落在城外、不贴资源点与集结点——**照旧**（§3.4）。
    * **河（2026-09-02 大改第 4 步受管理，§3.4，决定项 5 已拍板）**：
      - 河只允许穿过**内环带或外环**：不穿禁建环（`river_ok` 直接查
        `no_build`）、不贴墙（墙外 ≥ 6 格）。注意集结点环那一圈**非禁建**
        的格必然要过——边到边的河从外环进内环必须穿过它，「不穿禁建环」
        管的是集结点周围 13 格的 no_build 环，不是整条环带；
      - **被河切断的集结点 ≤ `river_cut_spawns_max`（1）**——不是「河不切
        任何集结点」。生成策略：先拍「切哪一个（或不切）」，按它定河的
        **弦**（法向角 φ 对准要切的集结点、弦高 h 卡在「切得到它而切不到
        相邻集结点」的区间——相邻角差 ≥ 60° ⇒ 邻点投影 ≤ L·cos60° = L/2，
        h 取比 L/2 大即可），落地后仍逐条复核（`_river_cut_spawns`），
        超标整条撤销重试——生成器兜底、校验器（第 30 条）裁决；
      - **桥宽 2**：`pick_bridges` 的贯穿带取两列（两条相邻、平直、同向的
        带，水跨取交集、多出来的水格收窄成岸）——1 格宽的桥是「不受管理
        的随机隘口」时代的产物，2 格宽才谈得上「队伍过得去」。
    * **每一块水体落地后跑一次 `_connected_to_keep` 自校验，失败整片撤销**
      （湖整个还原、河整条还原）。第 12 条因此仍然会做实事，但不再是
      丢弃率的主要来源——这是本函数敢存在的全部理由。

    零配置（两个 range 上界都是 0）时**不消费任何 rng 就返回**——参考图
    靠固定种子逐字节复现，多一次 `randint(0,0)` 都会改变同一批种子的产出。
    """
    if cfg.water_lakes_range[1] == 0 and cfg.rivers_range[1] == 0:
        return True

    R = cfg.city_radius
    D = cfg.spawn_wall_distance
    kx, ky = cv.keep
    size = cv.size
    bridge_width = max(1, int(th.bridge_width_min))
    cut_cap = int(th.river_cut_spawns_max)

    def cheb(p):
        return max(abs(p[0] - kx), abs(p[1] - ky))

    res_zone = set()
    for _, (px, py), _ in cv.resources:
        for oy in range(-1, 2):
            for ox in range(-1, 2):
                res_zone.add((px + ox, py + oy))

    def water_ok(p, r_pad=0):
        """湖的可落格：城外、Plain、不贴墙/集结点/资源点（**照旧**，§3.4）。

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

    def river_ok(p):
        """河的可落格（2026-09-02 第 4 步）：不穿禁建环（no_build 直接查）、
        不贴墙（cheb ≥ R+6，即墙外 ≥ 6 格）、不压资源点八邻。"""
        x, y = p
        return (cv.inside(x, y) and cv.at(x, y) == "Plain"
                and cheb(p) >= R + 6
                and cv.no_build[y][x] == 0
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
    def river_plan():
        """拍「这条河切哪个集结点（或不切）」，给出弦的两端点。

        弦 = 直线 `p·n = h`（n 是法向）。切集结点 s：n 对准 s 的方向，
        h ∈ (L·cos60°, L) —— s 的投影 ≈ L > h（被切），相邻集结点角差 ≥ 60°
        投影 ≤ L/2 < h（不被切）；不切：h > L（整条弦在集结点环外侧，
        只穿外环）。游走会偏离弦几格，落地后 `_river_cut_spawns` 复核兜底。
        """
        L = R + D
        if cv.spawns and rng.random() < 0.5:
            s = rng.choice(cv.spawns)
            phi = math.atan2(s[1] - ky, s[0] - kx)
            h = rng.randint(L // 2 + 2, min(L - 6, L // 2 + 14))
        else:
            phi = rng.uniform(0, 2 * math.pi)
            h = rng.randint(L + 2, L + 10)
        nx, ny = math.cos(phi), math.sin(phi)
        mx, my = kx + nx * h, ky + ny * h
        dx, dy = -ny, nx
        pts = []
        if abs(dx) > 1e-9:
            for ex in (0, size - 1):
                t = (ex - mx) / dx
                ey = my + t * dy
                if 0 <= ey <= size - 1:
                    pts.append((ex, ey))
        if abs(dy) > 1e-9:
            for ey in (0, size - 1):
                t = (ey - my) / dy
                ex = mx + t * dx
                if 0 <= ex <= size - 1:
                    pts.append((ex, ey))
        # 去重（角上会算两次），取相距最远的两个做端点。
        pts = [(int(round(x)), int(round(y))) for x, y in pts]
        uniq = sorted(set(pts))
        if len(uniq) < 2:
            return None
        best = max(((a, b) for a in uniq for b in uniq if a < b),
                   key=lambda ab: abs(ab[0][0] - ab[1][0])
                   + abs(ab[0][1] - ab[1][1]))
        return best

    def river_walk(start, goal):
        """从弦的一端走到另一端的正交随机游走（70% 朝目标收缩）。"""
        if not river_ok(start) or not river_ok(goal):
            return None
        path = [start]
        visited = {start}
        cur = start

        def dist_goal(p):
            return math.hypot(p[0] - goal[0], p[1] - goal[1])

        for _step in range(6 * size):      # 有界：卡住就放弃这条河
            if cur == goal:
                return path
            nxt = []
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                c = (cur[0] + dx, cur[1] + dy)
                if c in visited or not river_ok(c):
                    continue
                nxt.append(c)
            if not nxt:
                return None
            closer = [c for c in nxt if dist_goal(c) < dist_goal(cur)]
            cur = rng.choice(closer) if closer and rng.random() < 0.7 \
                else rng.choice(nxt)
            path.append(cur)
            visited.add(cur)
        return None

    def widen(spine):
        """主干 + 逐格随机的左/右/双侧邻居 ⇒ 宽 2–3 的带。

        **宽度沿两侧 jitter 而不是固定右舷**：河在边界附近折返时，「永远在
        同一侧加宽」会把水铺到地图外或压回主干另一侧，视觉上河贴着边走成
        一条沟。加宽格同样要过 `river_ok`（于是加宽不会贴上湖/资源点/禁建环）。
        """
        w = set(spine)
        for x, y in spine:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                c = (x + dx, y + dy)
                if c not in w and river_ok(c) and rng.random() < 0.55:
                    w.add(c)
        return w

    def pick_bridges(spine, water):
        """沿主干每隔约 14 格挑一处架桥：**桥宽 = `bridge_width` 列**（2026-09-02
        第 4 步，strict 档 2）——取 bridge_width 条相邻、平直、同向的贯穿带，
        水跨取**交集**，多出来的水格收窄成岸（`trim`），桥因此是规则的
        水宽 × bridge_width 矩形（校验器第 30 条按矩形短边量桥宽）。

        一条带能成桥的条件（都是实测踩过的）：
        * 主干在该处**平直**（前后三格共线）——弯道上垂直线扫出的是弯内侧的
          斜水，桥头接不上岸；
        * 交集非空且带两端收得住（收窄后两端是岸）——否则桥断在水中央；
        * 两端不能是湖（湖先落地，那里是水但不是河——桥搭进湖里不通岸）。

        逐带筛完还有一道**终态复验**（按「全体桥格与收窄都已落地」的终态
        算水邻，有一格四邻无水的整条作废）——相邻两条带互为水邻的洞是
        2026-09-01 实测踩到的（`-n 30` 红 2 次），见函数末尾。

        返回 (桥带列表, 收窄格集合)；一条都找不到时桥带列表为空。
        """
        n = len(spine)

        def flow_dir(j):
            """主干在 j 处的流向（前后三格共线才有），不正返 None。"""
            a, b, c = spine[j - 1], spine[j], spine[j + 1]
            if a[0] == b[0] == c[0]:
                return (0, 1)     # 河沿 y 向流
            if a[1] == b[1] == c[1]:
                return (1, 0)
            return None

        def span(j, perp):
            """j 处沿垂直方向的水跨：返回 (lo, hi)，水格 = b + off·perp，
            off ∈ [-lo, hi]（含 0 = 主干格）。"""
            b = spine[j]
            lo = 0
            while (b[0] - (lo + 1) * perp[0],
                   b[1] - (lo + 1) * perp[1]) in water:
                lo += 1
            hi = 0
            while (b[0] + (hi + 1) * perp[0],
                   b[1] + (hi + 1) * perp[1]) in water:
                hi += 1
            return lo, hi

        want = max(1, n // 14)
        strips = []          # (桥格集合, 收窄格集合)
        for k in range(want):
            i = min(n - 2, max(1, (2 * k + 1) * n // (2 * want)))
            for j in range(i, min(i + 14, n - bridge_width)):
                lanes = list(range(j, j + bridge_width))
                dirs = [flow_dir(t) for t in lanes]
                if any(d is None for d in dirs) or len(set(dirs)) > 1:
                    continue
                perp = (1, 0) if dirs[0] == (0, 1) else (0, 1)
                # 交集要连**前后各一格护卫行**一起取：桥格的流向邻居
                # （护卫行上的格）必须是水，否则端格四邻无水、第 13 条红。
                # 桥带的水跨 = 所有行水跨的**交集** [-L, H]（L/H 取 min——
                # 桥格必须每格都是水）；各行多出来的水格收窄成岸（trim）。
                rows = [j - 1] + lanes + [j + bridge_width]
                spans = {t: span(t, perp) for t in rows}
                L = min(s[0] for s in spans.values())
                H = min(s[1] for s in spans.values())
                cells = set()
                trim = set()
                ok = True
                for t in lanes:
                    b = spine[t]
                    lo_t, hi_t = spans[t]
                    for off in range(-lo_t, hi_t + 1):
                        c = (b[0] + off * perp[0], b[1] + off * perp[1])
                        if -L <= off <= H:
                            cells.add(c)
                        else:
                            trim.add(c)
                    # 收窄后两端必须是岸：界内、不是湖（河以外的水）。
                    # （自然岸或收窄格都行；收窄格落地后是 Plain。）
                    for off in (-L - 1, H + 1):
                        bank = (b[0] + off * perp[0], b[1] + off * perp[1])
                        if not cv.inside(*bank):
                            ok = False
                        elif bank not in trim and cv.at(*bank) == "Water":
                            ok = False    # 桥头是湖——桥搭进湖里不通岸
                if ok and cells:
                    strips.append((cells, trim))
                    break
        all_bridge = {c for cells, _ in strips for c in cells}
        all_trim = {c for _, tr in strips for c in tr}
        # **终态复验**：按「桥与收窄全部落地」的终态算每格桥的水邻——
        # 一条带的水邻可能是另一条带的格（河折返时两带空间相邻），
        # 逐带检查时那格还是水，两条带都落地后它变成了桥。
        kept = []
        for cells, trim in strips:
            w_final = water - all_bridge - all_trim
            if all(any((c[0] + ox, c[1] + oy) in w_final
                       for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                   for c in cells):
                kept.append((cells, trim))
        return kept

    for _ in range(rng.randint(*cfg.rivers_range)):
        for _try in range(40):
            plan = river_plan()
            if plan is None:
                continue
            spine = river_walk(*plan)
            if not spine or len(spine) < 12:
                continue
            water = widen(spine)
            strips = pick_bridges(spine, water)
            if not strips:
                continue
            bridge_cells = {c for cells, _ in strips for c in cells}
            trim_cells = {c for _, tr in strips for c in tr}
            water -= trim_cells
            for c in water:
                cv.set_terrain(*c, "Bridge" if c in bridge_cells else "Water")
            # 两道自校验：连通（第 12/4/11 条镜像）+ 被切集结点 ≤ 上限
            # （第 30 条后半的生成器侧）。任一失败整条还原，换下一条。
            if _connected_to_keep(cv) and (
                    cut_cap < 0
                    or len(_river_cut_spawns(cv)) <= cut_cap):
                break
            for c in water:      # 切断且桥没补上/切了太多集结点——整条还原
                cv.set_terrain(*c, "Plain")
    return True


def place_obstacles(cv, cfg, rng):
    """可破坏障碍。**它是一份一次性资源存量**（破坏后产出木材/石材，§4.4.1），
    所以撒多少不是纯装饰问题。

    **2026-09-01（#117）**：只在城外生成（cheb ≥ R+2，与野外散布同域），
    按同类 1–3 个组成彼此分离的小团（组间留两圈空地），不再全图均匀撒。
    **2026-09-02 第 5 步（§3.6）**：不撒进设计隘口的口（`cv.choke_gaps`）——
    清野不应等于拆掉地图给的隘口，隘口是地图设计语言的一部分（拍板
    2026-09-02）。
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
                and cell not in cv.choke_gaps
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

    ib_lo, ib_hi = cfg.inner_band_clusters
    inner_band_clusters = rng.randint(ib_lo, ib_hi)

    ob_lo, ob_hi = cfg.outer_band_clusters
    outer_band_clusters = rng.randint(ob_lo, ob_hi)
    # 外环簇数 ≥ 内环簇数（§3.3：远簇晚解禁，后期扩张的取舍要够多）。
    # 抽到更少时抬齐而不是重抽——重抽会消费不确定次数的 rng，破坏
    # 「同种子逐字节复现」的 rng 序列稳定性。
    # **例外：ob_hi == 0 = 本图明确没有外环**（2026-09-02：56 格参考图，
    # 外环带 [R+D+14, 半宽−3] 为空集，《方案》§3.9 明说允许并注明）。
    # 不抬齐——抬了也放不下，反而把「没有外环」这个设计决定静默吞掉。
    if ob_hi > 0:
        outer_band_clusters = max(outer_band_clusters, inner_band_clusters)

    tw_lo, tw_hi = cfg.towers_range
    towers = rng.randint(tw_lo, tw_hi)

    bk_lo, bk_hi = cfg.barracks_range
    barracks = rng.randint(bk_lo, bk_hi)

    rp_lo, rp_hi = cfg.rock_patches_range
    rock_patches = rng.randint(rp_lo, rp_hi)

    wd_lo, wd_hi = cfg.woods_range
    woods = rng.randint(wd_lo, wd_hi)

    rg_lo, rg_hi = cfg.ridges_range
    ridges = rng.randint(rg_lo, rg_hi)

    inner_resources = {t: rng.randint(v[0], v[1])
                        for t, v in cfg.inner_resources_range.items()}

    return SimpleNamespace(
        size=cfg.size,
        city_radius=city_radius,
        spawn_count=spawn_count,
        spawn_wall_distance=cfg.spawn_wall_distance,
        inner_resources=inner_resources,
        inner_band_clusters=inner_band_clusters,
        outer_band_clusters=outer_band_clusters,
        outer_cluster_size=cfg.outer_cluster_size,
        cluster_unlock_per_wave=cfg.cluster_unlock_per_wave,
        initial_breaches=cfg.initial_breaches,
        wall_hp_frac_range=cfg.wall_hp_frac_range,
        forest_patches=cfg.forest_patches,
        forest_patch_size=cfg.forest_patch_size,
        rock_patches=rock_patches,
        rock_patch_size=cfg.rock_patch_size,
        woods=woods,
        woods_size=cfg.woods_size,
        ridges=ridges,
        ridge_length=cfg.ridge_length,
        gap_width_range=cfg.gap_width_range,
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

    顺序 = 环墙 → 集结点 → 城内内容 → 城外资源簇 → 野外散布 → 水域 →
    阻挡率补量 → 障碍。改动顺序会影响 rng 消费序列，进而改变同一批种子的
    产出——只能整条改。

    **2026-09-02 大改第 1/2 步**：集结点改集结点环角度采样（不再有「抽哪几条
    边」这一步，`rng.sample(_SIDES, n)` 随之删除）；资源簇改按内/外环带
    分布 + 按簇解禁（`place_outer_clusters` 需要 `th` 读
    `spawn_wall_distance_range` 来定位两个环带，签名随之加参）。

    水域放在散布**之后**（2026-09-01）：散布落地在先，河与湖只落 `Plain`，
    遇到已长的森林/岩壁团块就绕开或整片撤销——反过来的话团块挖到河上只能
    整片弃权，那种失败的代价比河改道高（团块数量不算少，逐个重试的成本
    更高）。**阻挡率补量（`topup_wild_terrain`）再排在水域之后**
    （2026-09-02 第 3 步）：水占争夺带阻挡率的一大块，占比要等水落地后
    才算得准。
    """
    r = _resolve(cfg, rng)
    place_ring_walls(cv, r, rng)
    place_spawns(cv, r, th, rng)
    if not place_inner_content(cv, r, rng):
        raise GenerationFailed("城内放不下配置要求的内容（city_radius 太小？）")
    place_outer_clusters(cv, r, th, rng)
    scatter_wild_terrain(cv, r, rng)
    place_water(cv, r, th, rng)
    topup_wild_terrain(cv, r, th, rng)
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
    `tally` 累计每条检查的否决次数 —— 批量报告要的诊断信息（见模块 docstring；
    那份报告曾以「第 20 条」为名，那个编号已随第 7 条废除，机制本身没变）。
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
    """批量报告：约束合起来还剩多少可行空间。

    §8.1 明写这类产出应当是**诊断信息**（丢弃率、哪一条更常否决），
    而不是「这张图坏了」。所以它在这里，而不在 `validate.CHECKS` 里。
    服务的是当前表里的**全部**检查（它曾以「第 20 条」为名，那个编号已随
    第 7 条废除，见模块 docstring）。
    """
    print()
    print("=== 生成报告：约束的联合可满足性 ===")
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
          f"——而这正是本报告要回答的问题。")


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
