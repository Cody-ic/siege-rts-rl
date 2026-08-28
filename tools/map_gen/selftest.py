#!/usr/bin/env python3
"""`tools/map_gen/` 的自检，注册为一条 ctest。

沿用 `tools/check_determinism_bans.py` 的做法：每条规则都配「应当报」与
「不应当报」两侧的用例，防的是**检查本身退化成永远绿**。这里额外多一层：
跑完若一条用例都没执行，直接判失败 —— 「通过（0 条）」是最坏的那种绿。

不使用 pytest：仓库目前的 Python 依赖只有 Pillow（`tools/sprite_gen/`），
为一条自检加一个测试框架依赖不划算，而 ctest 本来就只看退出码。

用法:
    py selftest.py
"""
import os
import sys
import tempfile

import mapfile
from mapfile import MapFormatError
import grid as gridmod
from grid import Grid


# --------------------------------------------------------------------------
# 断言收集器
# --------------------------------------------------------------------------

class Checks:
    def __init__(self):
        self.failures = []
        self.count = 0

    def true(self, cond, label):
        self.count += 1
        if not cond:
            self.failures.append(label)

    def eq(self, got, want, label):
        self.count += 1
        if got != want:
            self.failures.append(f"{label}：得到 {got!r}，期望 {want!r}")

    def raises(self, exc, fn, label):
        self.count += 1
        try:
            fn()
        except exc:
            return
        except Exception as e:            # noqa: BLE001 - 自检要报告任何意外
            self.failures.append(
                f"{label}：抛的是 {type(e).__name__}（{e}），"
                f"期望 {exc.__name__}")
            return
        self.failures.append(f"{label}：没有抛 {exc.__name__}")

    def no_raise(self, fn, label):
        self.count += 1
        try:
            fn()
        except Exception as e:            # noqa: BLE001
            self.failures.append(
                f"{label}：不该抛，却抛了 {type(e).__name__}：{e}")


# --------------------------------------------------------------------------
# 测试用地图
# --------------------------------------------------------------------------

def make_doc(rows, no_build=None, keep=None, spawns=None,
             resources=None, walls=None, name="测试图"):
    """从 terrain 行构造一份最小的合法 doc。

    行的字符就是 palette 下标：0=Plain 1=Rock 2=Forest 3=Water 4=Bridge。
    """
    h = len(rows)
    w = len(rows[0])
    if no_build is None:
        no_build = ["0" * w for _ in range(h)]
    return {
        "format": 1,
        "map_id": "selftest",
        "name": name,
        "size": [w, h],
        "layers": {
            "terrain": {"palette": list(mapfile.TERRAIN_PALETTE), "rows": list(rows)},
            "no_build": {"rows": list(no_build)},
        },
        "keep": list(keep or [w // 2, h // 2]),
        "spawns": spawns if spawns is not None else [
            {"id": 0, "pos": [w // 2, 0], "corridor": "open"},
        ],
        "resources": resources if resources is not None else [],
        "initial_walls": walls if walls is not None else [],
    }


# 非方形，7 宽 5 高。坐标弄反会立刻越界，方形地图抓不到这种错。
OBLONG = [
    "0000000",
    "0000010",   # (5,1) 是 Rock
    "0030000",   # (2,2) 是 Water
    "0000000",
    "0000000",
]

# 城圈闭合：一圈 Rock 围住 keep(3,3)，且不贴地图边界。
CLOSED = [
    "0000000",
    "0111110",
    "0100010",
    "0100010",
    "0100010",
    "0111110",
    "0000000",
]

# 同一张图，上方开一格口子（3,1）。第 8 条要求的正是这种形态。
BREACHED = [
    "0000000",
    "0110110",
    "0100010",
    "0100010",
    "0100010",
    "0111110",
    "0000000",
]


# --------------------------------------------------------------------------
# 各组检查
# --------------------------------------------------------------------------

def check_coordinates(c):
    """坐标约定：pos 是 [x, y]，rows[y][x]。"""
    g = Grid(make_doc(OBLONG, keep=[3, 3]))
    c.eq((g.width, g.height), (7, 5), "size 解读为 [宽, 高]")
    c.eq(g.terrain_at(5, 1), "Rock", "terrain_at(x=5, y=1) 应是 Rock")
    c.eq(g.terrain_at(2, 2), "Water", "terrain_at(x=2, y=2) 应是 Water")
    c.eq(g.terrain_at(1, 5) if g.in_bounds(1, 5) else "越界", "越界",
         "(1,5) 在 7×5 的图上应当越界（弄反 x/y 才会落在图内）")


def check_terrain_flags(c):
    """4.1 那张表：五种地形是五个互不相同的标志位组合。"""
    combos = {}
    for name, flags in gridmod.TERRAIN_FLAGS.items():
        combos.setdefault(flags, []).append(name)
    dupes = {k: v for k, v in combos.items() if len(v) > 1}
    c.true(not dupes, f"五种地形的标志位组合必须两两不同，重复的：{dupes}")
    c.eq(len(gridmod.TERRAIN_FLAGS), len(mapfile.TERRAIN_PALETTE),
         "TERRAIN_FLAGS 与 TERRAIN_PALETTE 的项数必须一致")
    for name in mapfile.TERRAIN_PALETTE:
        c.true(name in gridmod.TERRAIN_FLAGS, f"{name} 缺标志位定义")
    # 4.1 明写的那一格：Water 是唯一「挡路但不挡视野」的地形。
    blockers = [n for n, f in gridmod.TERRAIN_FLAGS.items()
                if not f[0] and not f[2]]
    c.eq(blockers, ["Water"], "「不可通行且不挡视野」应当只有 Water")


def check_buildable_rule(c):
    """4.2 的组合规则：terrain.buildable AND NOT no_build。"""
    rows = ["000", "040", "000"]          # (1,1) 是 Bridge
    nb = ["000", "000", "010"]            # (1,2) 被 no_build 盖住
    g = Grid(make_doc(rows, no_build=nb, keep=[0, 0]))
    c.true(g.is_buildable(0, 0), "Plain 且未被 no_build 覆盖 → 可建")
    c.true(not g.is_buildable(1, 1), "Bridge 不可建（4.1 明写）")
    c.true(not g.is_buildable(1, 2), "Plain 但被 no_build 覆盖 → 不可建")


def check_no_corner_cutting(c):
    """斜向不穿角。规则取的是最严的一种：两个正交邻格**都**可通行才让过。

    三种情形都要钉住，尤其中间那条 —— 「一侧是障碍」在宽松规则下是允许的，
    两种规则的差别正好落在这里。将来若为对齐 `rts_core` 的移动模型改成宽松版，
    改的是 grid.neighbors()，而这条用例会立刻变红提醒同步。
    """
    def corner(rows):
        g = Grid(make_doc(rows, keep=[0, 0], spawns=[
            {"id": 0, "pos": [0, 0], "corridor": "open"}]))
        return set(g.neighbors(0, 0, g.ground_passable))

    c.true((1, 1) not in corner(["01", "10"]),
           "两侧都是 Rock 时不得斜穿到 (1,1)")
    c.true((1, 1) not in corner(["00", "10"]),
           "一侧是 Rock 时也不得斜过（严格规则）")
    c.true((1, 1) in corner(["00", "00"]),
           "两侧都可通行时，斜向应当走得通")


def check_walls_are_not_obstacles(c):
    """5.1：墙是高代价可通行，不是障碍。

    墙摆在 BREACHED 的 (3,1) —— 那一格是 Plain（正是缺口的位置）。
    摆在 CLOSED 的同一坐标上测不出东西，那里本来就是 Rock。
    """
    g = Grid(make_doc(BREACHED, keep=[3, 3], walls=[
        {"kind": "Wall", "pos": [3, 1], "hp_frac": 1.0}]))
    c.eq(g.terrain_at(3, 1), "Plain", "前提：这一格得是 Plain，否则测的是 Rock")
    c.true(g.ground_passable(3, 1),
           "ground_passable 不看墙 —— 墙是高代价可通行（5.1）")
    c.true(not g.blocked_by_walls_too(3, 1),
           "blocked_by_walls_too 才把墙算作挡路")


def check_city_area(c):
    """城区推导：闭合可算，有缺口必须明确失败而不是给个错答案。"""
    g_closed = Grid(make_doc(CLOSED, keep=[3, 3]))
    area = gridmod.derive_city_area(g_closed)
    c.eq(len(area), 9, "3×3 的内部应当是 9 格")
    c.true((3, 3) in area, "keep 自身应在城区内")
    c.true((0, 0) not in area, "地图角落不应在城区内")

    g_breached = Grid(make_doc(BREACHED, keep=[3, 3]))
    c.raises(gridmod.CityAreaUndecidable,
             lambda: gridmod.derive_city_area(g_breached),
             "城圈有缺口时，城区推导必须抛 CityAreaUndecidable")

    # 缺口处补上一段墙，就又闭合了 —— 这一条钉住「墙参与围合」。
    g_walled = Grid(make_doc(BREACHED, keep=[3, 3], walls=[
        {"kind": "Wall", "pos": [3, 1], "hp_frac": 0.5}]))
    c.no_raise(lambda: gridmod.derive_city_area(g_walled),
               "缺口被墙补上后城区应当可推导")

    # 3×3 里正中那格 (3,3) 的八邻全在城区内，所以它不算城圈 —— 城圈是 8 而非 9。
    # 这条顺带钉住「城圈按八邻接判定」：若改成四邻，四个角格会因为
    # 正交方向仍在城区内而漏掉，结果会变成 4。
    ring = gridmod.derive_city_ring(g_closed, area)
    c.eq(len(ring), 8, "3×3 里只有正中那格八邻全在城区内，城圈应当是 8")
    front = gridmod.derive_defended_front(g_closed, ring)
    c.eq(len(front), 8, "内部全是 Plain，正面应当等于城圈")
    breaches = gridmod.derive_breaches(g_closed, front)
    c.eq(len(breaches), 8, "没有任何墙时，正面上每一格都是缺口")


def check_bfs(c):
    rows = ["00000", "01110", "00000"]
    g = Grid(make_doc(rows, keep=[0, 0], spawns=[
        {"id": 0, "pos": [0, 0], "corridor": "open"}]))
    dist = g.bfs_steps([(0, 0)], g.ground_passable)
    c.eq(dist[(0, 0)], 0, "起点步数为 0")
    c.eq(dist[(4, 0)], 4, "同一行走 4 步")
    c.true((1, 1) not in dist, "Rock 不应出现在可达集里")
    c.true((4, 2) in dist, "绕过 Rock 仍应到得了对侧")


def check_hash_invariance(c):
    """规范序列化要吃掉三种「同一张图、不同字节」的差异。"""
    doc = make_doc(OBLONG, keep=[3, 3], name="王国边境")
    base = mapfile.compute_content_hash(doc)

    # 1) 键顺序
    shuffled = {k: doc[k] for k in reversed(list(doc.keys()))}
    c.eq(mapfile.compute_content_hash(shuffled), base, "键顺序不得影响哈希")

    # 2) 缩进 / 分隔符：从两份排版不同的文本解析出同一张图
    import json
    t_compact = json.dumps(doc, ensure_ascii=False, separators=(",", ":"))
    t_wide = json.dumps(doc, ensure_ascii=True, indent=8)
    c.eq(mapfile.compute_content_hash(json.loads(t_compact)), base,
         "紧凑排版不得影响哈希")
    # 3) 非 ASCII 转义（t_wide 里中文是 \uXXXX）
    c.eq(mapfile.compute_content_hash(json.loads(t_wide)), base,
         "中文是否转义不得影响哈希")

    # 反过来：内容真的变了，哈希必须变。
    changed = make_doc(
        [OBLONG[0].replace("0", "1", 1)] + OBLONG[1:], keep=[3, 3],
        name="王国边境")
    c.true(mapfile.compute_content_hash(changed) != base,
           "改动一个格子后哈希必须变化（6.3 的全部价值在此）")

    # content_hash 字段自身不参与哈希。
    stamped = dict(doc)
    mapfile.stamp_content_hash(stamped)
    c.eq(mapfile.compute_content_hash(stamped), base,
         "已写入的 content_hash 不得参与自己的计算")
    ok, stated, actual = mapfile.verify_content_hash(stamped)
    c.true(ok, f"盖章后校验应当通过（写的 {stated}，算的 {actual}）")


def check_roundtrip(c):
    doc = make_doc(OBLONG, keep=[3, 3], name="王国边境",
                   resources=[{"type": "gold", "pos": [1, 1], "tier": "inner"}],
                   walls=[{"kind": "Gate", "pos": [3, 0], "hp_frac": 0.45}])
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "m.json")
        text = mapfile.save(p, doc)

        raw = open(p, "rb").read()
        c.true(b"\r" not in raw,
               "写出的文件不得含 CR —— 否则同一张图在两个平台上字节不同（6.3）")
        c.true(raw.endswith(b"\n"), "文件应以换行结尾（6.1 逐行可 diff）")

        back = mapfile.load(p)
        c.eq(mapfile.dumps(back), text, "读回来再写出，文本必须逐字节一致")
        ok, _, _ = mapfile.verify_content_hash(back)
        c.true(ok, "读回来的地图 content_hash 应当自洽")


def check_format_rejects(c):
    """格式层：坏输入必须报错，且不能是 KeyError/IndexError 这类裸异常。"""
    def rej(mutate, label):
        d = make_doc(OBLONG, keep=[3, 3])
        mutate(d)
        c.raises(MapFormatError, lambda: mapfile.check_format(d), label)

    rej(lambda d: d.pop("keep"), "缺 keep")
    rej(lambda d: d.update(format=2), "format 版本不认识")
    rej(lambda d: d.update(size=[7, 4]), "size 高度与 rows 行数对不上")
    rej(lambda d: d.update(size=[6, 5]), "size 宽度与行长对不上")
    rej(lambda d: d["layers"]["terrain"]["rows"].__setitem__(0, "0000005"),
        "terrain 出现调色板之外的字符")
    rej(lambda d: d["layers"]["no_build"]["rows"].__setitem__(0, "0000002"),
        "no_build 只认 0/1")
    rej(lambda d: d["layers"]["terrain"].update(palette=["Plain", "Rock"]),
        "palette 与约定不一致")
    rej(lambda d: d["layers"].update(no_bulid={"rows": []}),
        "未知的层（拼写错误）必须报错而不是忽略")
    rej(lambda d: d.update(keep=[99, 0]), "keep 越界")
    rej(lambda d: d.update(keep=[True, 0]), "坐标不接受 bool")
    rej(lambda d: d.update(spawns=[]), "spawns 不得为空")
    rej(lambda d: d.update(spawns=[
        {"id": 0, "pos": [0, 0], "corridor": "open"},
        {"id": 0, "pos": [1, 0], "corridor": "defile"}]), "spawn id 重复")
    rej(lambda d: d.update(spawns=[
        {"id": 0, "pos": [0, 0], "corridor": "swamp"}]), "corridor 不在候选清单里")
    rej(lambda d: d.update(resources=[
        {"type": "iron", "pos": [1, 1], "tier": "inner"}]), "资源类型不认识")
    rej(lambda d: d.update(resources=[
        {"type": "gold", "pos": [1, 1], "tier": "middle"}]), "tier 不认识")
    rej(lambda d: d.update(initial_walls=[
        {"kind": "Tower", "pos": [1, 1], "hp_frac": 1.0}]), "墙的 kind 不认识")
    rej(lambda d: d.update(initial_walls=[
        {"kind": "Wall", "pos": [1, 1], "hp_frac": 0.0}]),
        "hp_frac = 0 应当直接不写这条墙")
    rej(lambda d: d.update(initial_walls=[
        {"kind": "Wall", "pos": [1, 1], "hp_frac": 1.5}]), "hp_frac 超过 1")
    rej(lambda d: d.update(content_hash="0f3a"), "content_hash 缺 sha256: 前缀")

    c.raises(MapFormatError, lambda: mapfile.loads("{ 不是 json"),
             "非法 JSON 应当包成 MapFormatError")
    c.raises(MapFormatError, lambda: mapfile.loads("[1, 2, 3]"),
             "顶层不是对象应当报错")


def check_format_accepts(c):
    c.no_raise(lambda: mapfile.check_format(make_doc(OBLONG, keep=[3, 3])),
               "最小合法地图不该被拒")
    c.no_raise(
        lambda: mapfile.check_format(make_doc(
            OBLONG, keep=[3, 3], name="王国边境",
            resources=[{"type": "stone", "pos": [0, 0], "tier": "outer"}],
            walls=[{"kind": "Gate", "pos": [1, 1], "hp_frac": 1}])),
        "带中文名与整数 hp_frac 的地图不该被拒")
    c.no_raise(
        lambda: mapfile.check_format(
            mapfile.stamp_content_hash(make_doc(OBLONG, keep=[3, 3]))),
        "带 content_hash 的地图不该被拒")


GROUPS = [
    ("坐标约定", check_coordinates),
    ("地形标志位（4.1）", check_terrain_flags),
    ("可建造组合规则（4.2）", check_buildable_rule),
    ("斜向不穿角", check_no_corner_cutting),
    ("墙不是障碍（5.1）", check_walls_are_not_obstacles),
    ("城区 / 城圈 / 缺口", check_city_area),
    ("BFS 步数", check_bfs),
    ("content_hash 不变性（6.3）", check_hash_invariance),
    ("读写往返", check_roundtrip),
    ("格式层：应当拒绝", check_format_rejects),
    ("格式层：应当接受", check_format_accepts),
]


def main():
    c = Checks()
    for name, fn in GROUPS:
        try:
            fn(c)
        except Exception as e:            # noqa: BLE001
            c.failures.append(f"[{name}] 这一组自身抛了异常：{type(e).__name__}：{e}")

    # 一条用例都没跑 = 最坏的绿。与 check_determinism_bans.py 的
    # 「扫到 0 个文件即失败」是同一条原则。
    if c.count == 0:
        print("自检失败：一条断言都没执行 —— 用例表可能被清空或导入失败。")
        return 1

    if c.failures:
        print(f"自检失败（{len(c.failures)} / {c.count} 条）：")
        for f in c.failures:
            print("  " + f)
        return 1

    print(f"map_gen 自检通过（{c.count} 条断言，{len(GROUPS)} 组）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
