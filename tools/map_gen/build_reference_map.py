# -*- coding: utf-8 -*-
"""按具体要求手工构造《边境要塞》参考演示地图（§9 的手写地图，2026-08-31 重设计）。

**不是生成器**（`generate.py` 是随机训练地图集用的，约束式 + 随机项）。
这是这一张特定手写地图的**权威来源**——`game/data/maps/reference_border_keep_01.json`
是它的产出，不要直接手改那份 JSON；改布局要求就改这个脚本再重新跑一遍
（`python build_reference_map.py`），理由与 `tools/sprite_gen/` 那批脚本是
"素材生成流水线的输入文件"同一条：CLAUDE.md 要求"软件源代码包括输入文件和输出文件"。

设计要求（用户原话，2026-08-31）：
- 堡垒居中，四周距离堡垒 7-8 格一圈城墙，两个相对方向开城门
- 开局玩家在城墙内部拥有 2 座箭塔 + 1 个兵营
- 城墙内部：2 片森林、2 处石矿、1 处金矿；森林旁一座伐木场、石矿旁一座采石场
- 城墙外的资源（森林/石矿/金矿）从城墙到地图边界逐渐变少、较为分散
- 参考 AoE4 1v1 对战地图

改这份脚本前先读一遍 `validate.py --list` 与本文件里对每条检查的注释——
每处坐标选择背后都有一条具体检查在盯着（避免下一个人改数字时无意踩坏它）。
跑完必须过 `python validate.py ../../game/data/maps/reference_border_keep_01.json`。
"""
import sys
sys.path.insert(0, ".")
import mapfile

SIZE = 56
CX, CY = 28, 28  # keep


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def new_grid(fill="0"):
    return [[fill] * SIZE for _ in range(SIZE)]


def set_cell(rows, x, y, ch):
    if 0 <= x < SIZE and 0 <= y < SIZE:
        rows[y][x] = ch


def build():
    terrain = new_grid()
    no_build = new_grid()

    walls = []       # (kind, pos, hp_frac)
    resources = []   # [type, pos, tier, wave] -- wave 稍后统一按距离重算
    buildings = []   # (type, pos)
    obstacles = []   # (type, pos)

    # ------------------------------------------------------------------
    # 1. 城墙环：切比雪夫距离 keep 恰好 8 的正方形环，两个相对方向（南/北）开门
    # ------------------------------------------------------------------
    R = 8
    ring = []
    for x in range(CX - R, CX + R + 1):
        ring.append((x, CY - R))
        ring.append((x, CY + R))
    for y in range(CY - R + 1, CY + R):
        ring.append((CX - R, y))
        ring.append((CX + R, y))
    ring = sorted(set(ring))

    gate_n = (CX, CY - R)
    gate_s = (CX, CY + R)
    # 缺口：东墙中段留一处（2.3 要求初始城圈至少一处缺口，不写墙段即缺口）
    breach = {(CX + R, CY - 2), (CX + R, CY - 1)}

    hp_cycle = [1.0, 0.85, 0.7, 0.55, 0.4, 0.6, 0.9, 0.75]
    i = 0
    for pos in ring:
        if pos in breach:
            continue
        kind = "Gate" if pos in (gate_n, gate_s) else "Wall"
        hp = 1.0 if kind == "Gate" else hp_cycle[i % len(hp_cycle)]
        walls.append((kind, pos, round(hp, 2)))
        i += 1

    wall_cells = {p for _, p, _ in walls}

    # ------------------------------------------------------------------
    # 2. 城墙内部：2 片森林、2 处石矿、1 处金矿、2 箭塔、1 兵营、1 伐木场、1 采石场
    #    全部落在切比雪夫距离 keep <= 6 的范围内（留 2 格余量不贴墙）
    # ------------------------------------------------------------------
    forest_a = [(23, 23), (24, 23), (23, 24)]
    forest_b = [(33, 33), (34, 33), (33, 34)]
    for x, y in forest_a + forest_b:
        set_cell(terrain, x, y, "2")

    wood_pos = (25, 24)
    resources.append(["wood", wood_pos, "inner", None])
    buildings.append(("Lumber", wood_pos))

    stone1_pos = (23, 32)
    resources.append(["stone", stone1_pos, "inner", None])
    buildings.append(("Quarry", stone1_pos))

    stone2_pos = (33, 24)
    resources.append(["stone", stone2_pos, "inner", None])

    gold_pos = (31, 32)
    resources.append(["gold", gold_pos, "inner", None])

    buildings.append(("Tower", (CX, CY - R + 2)))
    buildings.append(("Tower", (CX, CY + R - 2)))
    buildings.append(("Barrack", (CX - 6, CY)))

    inner_cells = ({(28, 28)} | set(forest_a) | set(forest_b) |
                    {wood_pos, stone1_pos, stone2_pos, gold_pos} |
                    {p for _, p in buildings})

    # ------------------------------------------------------------------
    # 3. 南侧"隘口"走廊：Rock 夹出窄道，攻方被挤成密集队列
    # ------------------------------------------------------------------
    defile_y0, defile_y1 = CY + R + 2, CY + R + 14
    rock_cells = set()
    for y in range(defile_y0, defile_y1):
        for x in range(CX - 12, CX - 2):
            set_cell(terrain, x, y, "1")
            rock_cells.add((x, y))
        for x in range(CX + 3, CX + 13):
            set_cell(terrain, x, y, "1")
            rock_cells.add((x, y))

    # ------------------------------------------------------------------
    # 4. 两个集结点：北="open"（开阔平原），南="defile"（隘口，对齐上面的窄道）
    # ------------------------------------------------------------------
    spawn_n = (CX, 2)
    spawn_s = (CX, SIZE - 3)
    spawns = [
        {"id": 0, "pos": list(spawn_n), "corridor": "open"},
        {"id": 1, "pos": list(spawn_s), "corridor": "defile"},
    ]

    NB_R = 13
    for cx, cy in (spawn_n, spawn_s):
        for y in range(max(0, cy - NB_R), min(SIZE, cy + NB_R + 1)):
            for x in range(max(0, cx - NB_R), min(SIZE, cx + NB_R + 1)):
                if max(abs(x - cx), abs(y - cy)) <= NB_R:
                    no_build[y][x] = "1"

    # ------------------------------------------------------------------
    # 5. 城墙外资源：沿东西两侧分布，从近到远逐渐稀疏、间距变大。
    #    每簇资源点排成一行（同 y，x 间隔 2），森林带贴在其**正上方一行**
    #    （y-1）并一路铺到地图边界——resource 的北邻格是 Forest，满足第 7 条
    #    "起点取资源点四邻"；resource 本身留在 Plain 上，满足第 16 条。
    # ------------------------------------------------------------------
    outer_specs = [
        # (簇, [(type, x偏移)], belt_y, direction)
        ((44, 18), [("wood", 0), ("stone", 3)], "east"),
        ((48, 42), [("gold", 0), ("wood", 3)], "east"),
        ((52, 30), [("stone", 0)], "east"),
        ((12, 35), [("wood", 0), ("gold", 3)], "west"),
        ((9, 14), [("stone", 0), ("wood", 3)], "west"),
        ((3, 28), [("gold", 0)], "west"),
    ]

    for (cx0, cy0), items, direction in outer_specs:
        belt_y = cy0 - 2   # 贴在簇正上方，隔 1 行缓冲，避免与簇本身相邻但重叠
        for t, xoff in items:
            resources.append([t, (cx0 + xoff, cy0), "outer", None])
        if direction == "east":
            x_lo = min(cx0 + xo for _, xo in items) - 1
            for x in range(x_lo, SIZE):
                set_cell(terrain, x, belt_y, "2")
        else:
            x_hi = max(cx0 + xo for _, xo in items) + 1
            for x in range(0, x_hi + 1):
                set_cell(terrain, x, belt_y, "2")
        # 连接簇资源格与森林带：资源正上方那一格（belt_y+1 与 cy0 之间）铺 Forest，
        # 使 "资源点的北邻" 直接落在带子上（belt_y = cy0-2 时中间还差一格）。
        for t, xoff in items:
            set_cell(terrain, cx0 + xoff, cy0 - 1, "2")

    # ------------------------------------------------------------------
    # 6. 统一按"到 keep 的切比雪夫距离"重算 unlock_wave，严格单调（第 18 条）
    #    inner 全部第 1 波解禁；outer 按距离升序依次给 2, 3, 4...
    # ------------------------------------------------------------------
    for r in resources:
        if r[2] == "inner":
            r[3] = 1
    outer_idx = [i for i, r in enumerate(resources) if r[2] == "outer"]
    outer_idx.sort(key=lambda i: cheb((CX, CY), resources[i][1]))
    for wave, i in enumerate(outer_idx, start=2):
        resources[i][3] = wave

    # ------------------------------------------------------------------
    # 7. 少量可破坏障碍（景物）：挑北侧开阔走廊里明确空闲的 Plain 格，
    #    远离墙环、Rock、其余点位实体。
    # ------------------------------------------------------------------
    occupied_all = (inner_cells | wall_cells | rock_cells |
                    {tuple(r[1]) for r in resources} |
                    {tuple(s["pos"]) for s in spawns})
    obstacle_candidates = [
        (18, 10), (38, 10), (28, 14), (20, 6), (36, 6),
    ]
    for pos in obstacle_candidates:
        assert pos not in occupied_all, f"障碍候选点 {pos} 与既有实体冲突"
        assert terrain[pos[1]][pos[0]] == "0", f"障碍候选点 {pos} 不是 Plain"
    obstacles = [
        ("Stump", obstacle_candidates[0]),
        ("Sapling", obstacle_candidates[1]),
        ("Rubble", obstacle_candidates[2]),
        ("Stump", obstacle_candidates[3]),
        ("Sapling", obstacle_candidates[4]),
    ]

    doc = {
        "format": 1,
        "map_id": "border_keep_01",
        "name": "边境要塞",
        "size": [SIZE, SIZE],
        "layers": {
            "terrain": {
                "palette": list(mapfile.TERRAIN_PALETTE),
                "rows": ["".join(row) for row in terrain],
            },
            "no_build": {"rows": ["".join(row) for row in no_build]},
        },
        "keep": [CX, CY],
        "spawns": spawns,
        "resources": [
            {"type": t, "pos": list(p), "tier": tier, "unlock_wave": w}
            for t, p, tier, w in resources
        ],
        "initial_walls": [
            {"kind": k, "pos": list(p), "hp_frac": h} for k, p, h in walls
        ],
        "obstacles": [{"type": t, "pos": list(p)} for t, p in obstacles],
        "buildings": [{"type": t, "pos": list(p)} for t, p in buildings],
    }
    return mapfile.stamp_content_hash(doc)


if __name__ == "__main__":
    doc = build()
    out = "../../game/data/maps/reference_border_keep_01.json"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(mapfile.canonical_dumps(doc))
    print("written:", out)
