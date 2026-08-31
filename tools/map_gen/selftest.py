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
import io
import os
import sys
import tempfile

import mapfile
from mapfile import MapFormatError
import grid as gridmod
from grid import Grid
import validate
import thresholds
import generate

# 仓库根。用于找 `game/testdata/fixture_min.json` 与数值表——两者都在
# `tools/map_gen/` 之外，而自检不能依赖 ctest 的工作目录（同本文件其余部分）。
_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


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
             resources=None, walls=None, obstacles=None, name="测试图"):
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
        # 6.2 的 `obstacles` 是**必填**的（缺失与刻意为空不可区分是个洞），
        # 所以默认给空数组而不是省掉这个键 —— 省掉会让第 19、22 条抛 KeyError，
        # 而那表现为「这一组自身抛了异常」，不是一条有用的失败。
        "obstacles": obstacles if obstacles is not None else [],
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
                   resources=[{"type": "gold", "pos": [1, 1], "tier": "inner",
                              "unlock_wave": 1}],
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
            resources=[{"type": "stone", "pos": [0, 0], "tier": "outer",
                       "unlock_wave": 2}],
            walls=[{"kind": "Gate", "pos": [1, 1], "hp_frac": 1}])),
        "带中文名与整数 hp_frac 的地图不该被拒")
    c.no_raise(
        lambda: mapfile.check_format(
            mapfile.stamp_content_hash(make_doc(OBLONG, keep=[3, 3]))),
        "带 content_hash 的地图不该被拒")


# --------------------------------------------------------------------------
# 第 8 节校验器
# --------------------------------------------------------------------------

def _gv(rows, **kw):
    """构造 (doc, grid) 一对，省得每条用例写两行。"""
    doc = make_doc(rows, **kw)
    return doc, Grid(doc)


def check_registry_covers_spec(c):
    """注册表必须正好覆盖第 8 节的 1..15，不重不漏。

    这一条防的是「白名单漏登记」——`tests/CMakeLists.txt` 里 ctest 标签清单
    已经踩过一次：写错抓得到，**没写进清单抓不到**。第 8 节将来加条目
    （例如 #26 的 A5 若通过要加「§2 必须配 §3」），这里会立刻红。
    """
    nos = [chk.no for chk in validate.CHECKS]
    moved = sorted(validate.MOVED_TO_GENERATOR)
    # **表内 + 已移出 = 规范全部条目。** 第 20 条按 §8.1 自己写的那句移到了
    # 生成器的批量报告去（丢弃率与逐条否决计数），但它**不是被删掉**——
    # 直接删会让一条规范条目无声消失，而那与「白名单漏登记」是同一类错误。
    c.eq(sorted(nos + moved), list(range(1, validate.SPEC_CHECK_COUNT + 1)),
         "CHECKS + MOVED_TO_GENERATOR 必须正好覆盖第 8 节的全部条目")
    c.eq(len(set(nos)), len(nos), "条目编号不得重复")
    c.true(not (set(nos) & set(moved)),
           "一条既在表内又标为已移出，说明移出时忘了删表里那一行")
    for no in moved:
        c.true(bool(validate.MOVED_TO_GENERATOR[no]),
               f"第 {no} 条标为已移出，必须写明去哪了")

    for chk in validate.CHECKS:
        if chk.status == validate.IMPLEMENTED:
            c.true(chk.fn is not None,
                   f"第 {chk.no} 条标为「已实现」却没挂函数")
        else:
            c.true(chk.fn is None,
                   f"第 {chk.no} 条标为「{chk.status}」却挂了函数")
            c.true(bool(chk.note),
                   f"第 {chk.no} 条未实现，必须写明原因（否则会慢慢变成默认跳过）")


def check_v3_corridors(c):
    two = [{"id": 0, "pos": [0, 0], "corridor": "open"},
           {"id": 1, "pos": [6, 0], "corridor": "open"}]
    doc, g = _gv(OBLONG, keep=[3, 3], spawns=two)
    c.true(validate.check_corridor_kinds(doc, g), "重复的 corridor 应当报")

    two[1]["corridor"] = "defile"
    doc, g = _gv(OBLONG, keep=[3, 3], spawns=two)
    c.true(not validate.check_corridor_kinds(doc, g),
           "各不相同的 corridor 不该报")


def check_v4_reachable(c):
    # spawn 被 Rock 团团围住。
    walled_in = ["00000", "01110", "01010", "01110", "00000"]
    doc, g = _gv(walled_in, keep=[0, 0], spawns=[
        {"id": 0, "pos": [2, 2], "corridor": "open"}])
    c.true(validate.check_spawn_reachable(doc, g),
           "被 Rock 围死的集结点应当报不通")

    doc, g = _gv(["000", "000", "000"], keep=[0, 0], spawns=[
        {"id": 0, "pos": [2, 2], "corridor": "open"}])
    c.true(not validate.check_spawn_reachable(doc, g), "空旷地图不该报")

    # 唯一通路上摆一段墙：墙是高代价可通行，仍应算通（5.1）。
    corridor = ["000", "101", "000"]
    doc, g = _gv(corridor, keep=[0, 0], spawns=[
        {"id": 0, "pos": [0, 2], "corridor": "open"}],
        walls=[{"kind": "Wall", "pos": [1, 1], "hp_frac": 1.0}])
    c.true(not validate.check_spawn_reachable(doc, g),
           "唯一通路被墙挡住仍应判为通 —— 墙是高代价可通行，不是障碍（5.1）")


def check_v6_inner_resources(c):
    three = [{"type": t, "pos": [i, 0], "tier": "inner"}
             for i, t in enumerate(["stone", "wood", "gold"])]
    doc, g = _gv(OBLONG, keep=[3, 3], resources=three)
    c.true(not validate.check_inner_resources(doc, g), "三种齐全不该报")

    doc, g = _gv(OBLONG, keep=[3, 3], resources=three[:2])
    c.true(validate.check_inner_resources(doc, g), "inner 缺 gold 应当报")

    # 只有 outer 有金币也不行 —— 4.4 要求的是 inner 侧三种全覆盖。
    mixed = three[:2] + [{"type": "gold", "pos": [4, 0], "tier": "outer"}]
    doc, g = _gv(OBLONG, keep=[3, 3], resources=mixed)
    c.true(validate.check_inner_resources(doc, g),
           "金币只在 outer 侧仍应报 —— 保底看的是 inner")


def check_v18_resource_unlock_wave(c):
    """第 18 条：`resources` 的解禁波数序列按距离单调——解禁越晚的簇，离 keep 越远。

    OBLONG 是 7×5（x: 0..6, y: 0..4），keep 取 (3,3)：
      * `near` = (4,3)，切比雪夫距离 1
      * `far`  = (6,0)，切比雪夫距离 3
      * `tied` = (0,3)，切比雪夫距离 3（与 far 相等，不等于 near）
    """
    near = {"type": "wood", "pos": [4, 3], "tier": "outer", "unlock_wave": 2}
    far = {"type": "stone", "pos": [6, 0], "tier": "outer", "unlock_wave": 3}
    doc, g = _gv(OBLONG, keep=[3, 3], resources=[far, near])
    c.true(not validate.check_resource_unlock_wave(doc, g),
           "远的点解禁更晚、近的点解禁更早，顺序正确，不该报")

    # 颠倒过来：近的反而解禁更晚。
    bad_near = {**near, "unlock_wave": 3}
    bad_far = {**far, "unlock_wave": 2}
    doc, g = _gv(OBLONG, keep=[3, 3], resources=[bad_far, bad_near])
    got = validate.check_resource_unlock_wave(doc, g)
    c.eq(len(got), 1, "近的点解禁反而更晚必须报")

    # 同波不该报——本条只拦"更晚解禁却更近"这一种组合，不要求严格递增。
    doc, g = _gv(OBLONG, keep=[3, 3],
                resources=[{**near, "unlock_wave": 2}, {**far, "unlock_wave": 2}])
    c.true(not validate.check_resource_unlock_wave(doc, g), "同波不该报")

    # 距离相等、解禁波不同：谁先谁后都不该报，因为判据只问"更远的是否更晚"，
    # 距离相等时没有"更远"这一方。
    tied = {"type": "gold", "pos": [0, 3], "tier": "outer", "unlock_wave": 5}
    doc, g = _gv(OBLONG, keep=[3, 3], resources=[tied, far])
    c.true(not validate.check_resource_unlock_wave(doc, g),
           "(0,3) 与 (6,0) 到 keep(3,3) 的切比雪夫距离都是 3，距离相等时"
           "解禁波不同不该报")


def _rows(w, h, forest=()):
    """w×h 全 `Plain`，再把 `forest` 里那些格改成 `Forest`。

    第 7、10 条的用例都是「一条森林带 + 大片平地」，逐行写字符串在 11×11 上
    既难读也容易数错列。
    """
    out = [["0"] * w for _ in range(h)]
    for x, y in forest:
        out[y][x] = "2"
    return ["".join(r) for r in out]


# 三种 inner 资源，用来让第 6 条别在第 7 条的用例里跟着报。
_INNER3 = [{"type": t, "pos": [i, 0], "tier": "inner"}
           for i, t in enumerate(["stone", "wood", "gold"])]


def make_clean_doc():
    """一张各条都过得去的地图。**只此一份**，两处用例共用。

    此前是两份拷贝（`check_validator_on_clean_map` 与那条 CLI 用例各一份），
    于是第 21 条落地时**只改了一份**，另一份红在「一张干净地图应当退出 0」
    这句话上 —— 而那条报错完全不指向真正的原因。两份「同一张图」的拷贝就是
    在等这件事发生。

    x=5 那两格 `Forest` 与 (5,2) 的 `outer` 资源点是**第 7 与第 21 条一起**
    要求的：21 条要至少有一个 `outer`，7 条要它有一条 4 连通的森林带通到边界。
    这张图原先只有 inner 资源点 —— 而它能通过当时的全部检查，**正是第 21 条
    存在的理由**。

    `keep` 从 (3,2) 挪到了 (3,3)：原来它与金币资源点**同格**。那不违反任何已有
    条目（第 17 条按 §8.1 字面只管墙与集结点），但一张「干净地图」不该演示一个
    我们其实不希望出现的摆法。keep 与资源点重合值不值得单开一条，
    记在 `地图与场景设计.md` 第 10 节。
    """
    return make_doc(
        ["0000020", "0000020", "0000000", "0000000", "0000000"],
        keep=[3, 3],
        spawns=[{"id": 0, "pos": [0, 0], "corridor": "open"},
                {"id": 1, "pos": [6, 4], "corridor": "defile"}],
        resources=[{"type": t, "pos": [i + 1, 2], "tier": "inner",
                   "unlock_wave": 1}
                   for i, t in enumerate(["stone", "wood", "gold"])]
                  + [{"type": "wood", "pos": [5, 2], "tier": "outer",
                     "unlock_wave": 2}])


def check_v7_outer_unenclosable(c):
    """第 7 条：`outer` 资源点周围的森林带必须 **4 连通**地通到地图边界。

    这一组里最要紧的是**斜向森林链必须判失败**。它与第 10 条那条斜链用例形似
    而结论相反，原因是两条问的不是同一件事（移动 vs 拓扑，见 validate 里的
    docstring）：斜向森林链**走得过去**，所以第 10 条判它是遮蔽通道；
    但它撑不起 2.1.1 的拓扑论证——墙线能从两格之间那个对角缺口穿过去，
    一格森林都不碰，所以第 7 条必须判它没连上。

    **有效的破坏性探针是给实现加上 `corner=grid.is_passable`（第 10 条那种写法），
    加了这一组会红 2 条。**把 `diagonal=False` 翻成 `True` 是个**空探针**：
    `neighbors()` 默认 `corner = passable`，一步斜向要求两个正交角格都在集合里，
    而那两格都在的话 4 连通本来就走到了，所以两者恒等 —— 实测两种取法在斜链上
    都只覆盖 1 格。这条写在这里是因为**空探针给出的绿与「测试没盯住」长得一样**，
    而我第一次就指错了。

    反过来若少了下面那条「改成竖直链就该通过」，上一条就可能只是「总是报」。
    """
    def outer_at(pos, tier="outer"):
        return _INNER3 + [{"type": "wood", "pos": list(pos), "tier": tier}]

    # -- 竖直森林带通到上边界：通过 --------------------------------------
    band = ["0002000", "0002000", "0000000", "0000000", "0000000"]
    doc, g = _gv(band, keep=[5, 4], resources=outer_at([3, 2]))
    c.true(not validate.check_outer_unenclosable(doc, g),
           "森林带 4 连通通到地图边界，不该报")

    # -- 森林带存在但没通到边界：应当报 ----------------------------------
    stub = ["0000000", "0000000", "0002000", "0000000", "0000000"]
    doc, g = _gv(stub, keep=[5, 4], resources=outer_at([3, 3]))
    c.true(validate.check_outer_unenclosable(doc, g),
           "森林带没通到地图边界应当报 —— 玩家可以绕着它在外面画一圈更大的墙")

    # -- 森林只**斜着**挨着资源点：应当报 --------------------------------
    #
    # (2,2) 与资源点 (3,3) 是对角相邻，而那条森林带本身一路通到左边界。
    # 起点取四邻正是为了拦它：斜着挨上等于没连上，墙线能从对角缺口塞进去。
    diag_touch = ["0000000", "0000000", "2220000", "0000000", "0000000"]
    doc, g = _gv(diag_touch, keep=[5, 4], resources=outer_at([3, 3]))
    c.true(validate.check_outer_unenclosable(doc, g),
           "森林带只斜着挨着资源点应当报 —— 起点取四邻，不取八邻")

    # -- 同一张图，改成 inner：本条不管 ----------------------------------
    doc, g = _gv(diag_touch, keep=[5, 4],
                 resources=outer_at([3, 3], tier="inner"))
    c.true(not validate.check_outer_unenclosable(doc, g),
           "inner 资源点不受本条约束（它本来就在城里）")

    # -- 一个 outer 都没有：空过 -----------------------------------------
    doc, g = _gv(diag_touch, keep=[5, 4], resources=_INNER3)
    c.true(not validate.check_outer_unenclosable(doc, g),
           "没有 outer 资源点时空过（**这是个已知的洞**，见第 10 节）")

    # -- 斜向森林链：本条的关键用例 --------------------------------------
    #
    # (5,4) 起，(4,3)(3,2)(2,1)(1,0) 一路斜到左上边界。8 连通下它「通到了边界」，
    # 4 连通下 flood 只覆盖 (5,4) 一格。判失败才是对的。
    chain = [(5, 4), (4, 3), (3, 2), (2, 1), (1, 0)]
    doc, g = _gv(_rows(11, 11, chain), keep=[5, 8],
                 resources=outer_at([5, 5]))
    problems = validate.check_outer_unenclosable(doc, g)
    c.true(problems,
           "**斜向**森林链应当报 —— 它撑不起 2.1.1 的拓扑论证，"
           "墙线能从对角缺口穿过去（与第 10 条结论相反，因为问的不是同一件事）")
    c.true(any("4 连通" in p for p in problems),
           "报告要点明是 4 连通下不通，否则收到失败的人会去查森林画错没")

    # -- 同一张图，把斜链改成竖直链：应当通过 ----------------------------
    straight = [(5, y) for y in range(5)]
    doc, g = _gv(_rows(11, 11, straight), keep=[5, 8],
                 resources=outer_at([5, 5]))
    c.true(not validate.check_outer_unenclosable(doc, g),
           "竖直森林链不该报 —— 否则上一条只是「总是报」")


def check_v7_v10_tension_is_real(c):
    """第 7 与第 10 条**正面对立**（2.1.3），这一组把那句话变成两张具体的图。

    §8.1 把「两条的联合可满足性」排为第 20 条，理由是「两条各自通过、合起来
    不通过时，生成器陷入高丢弃率而不报原因」。第 20 条还没落地，
    但**对立是不是真的**现在就能钉住，而且这两张图将来正好是它的夹具。

    刻意只断言这两条，不要求整张图干净 —— 这一组问的是两条之间的关系。
    """
    # 墙横在 y=6，x=2..8，(5,6) 留缺口（第 8 条要的那种形态）。
    walls = [{"kind": "Wall", "pos": [x, 6], "hp_frac": 1.0}
             for x in (2, 3, 4, 6, 7, 8)]

    # -- 可以同时满足：森林带在远离集结点与城墙的一侧 --------------------
    far = [(0, 1), (0, 2), (0, 3), (1, 3)]
    doc, g = _gv(_rows(11, 11, far), keep=[5, 8], walls=walls,
                 spawns=[{"id": 0, "pos": [5, 0], "corridor": "open"}],
                 resources=_INNER3 + [{"type": "wood", "pos": [2, 3],
                                       "tier": "outer"}])
    c.true(not validate.check_outer_unenclosable(doc, g),
           "联合可满足：森林带通到左边界，第 7 条该过")
    c.true(not validate.check_forest_corridor(doc, g),
           "联合可满足：同一张图上森林没通到城墙，第 10 条也该过")

    # -- 对立是真的：同一条森林带同时喂饱第 7 条、踩中第 10 条 ------------
    #
    # 森林 x=3 那一列从上边界一路下来（第 7 条要的），再斜挂一格 (2,5) ——
    # 而 (2,5) 是墙 (3,6) 的八邻，于是它同时是「集结点直达城墙的遮蔽道」。
    # 集结点挪到 (2,0)，紧挨着那条林带。
    both = [(3, y) for y in range(5)] + [(2, 5)]
    doc, g = _gv(_rows(11, 11, both), keep=[5, 8], walls=walls,
                 spawns=[{"id": 0, "pos": [2, 0], "corridor": "forest"}],
                 resources=_INNER3 + [{"type": "wood", "pos": [3, 5],
                                       "tier": "outer"}])
    c.true(not validate.check_outer_unenclosable(doc, g),
           "对立用例：第 7 条该过（森林带 4 连通通到上边界）")
    c.true(validate.check_forest_corridor(doc, g),
           "对立用例：第 10 条该报 —— 同一条林带既是必须存在的通道，"
           "又是通到城墙的遮蔽道。这就是 2.1.3 说的那件事")


def check_v10_forest_corridor(c):
    # (1,0)-(2,0)-(2,1)-(2,2) 一条森林带，墙在 (2,3) 紧挨着它。
    forest_lane = ["0220000", "0020000", "0020000", "0000000", "0000000"]
    spawns = [{"id": 0, "pos": [1, 0], "corridor": "forest"}]
    wall = [{"kind": "Wall", "pos": [2, 3], "hp_frac": 1.0}]
    doc, g = _gv(forest_lane, keep=[5, 4], spawns=spawns, walls=wall)
    c.true(validate.check_forest_corridor(doc, g),
           "从集结点直达城墙的连续森林带应当报（4.1）")

    # 同一张图，把森林带在中间断开一格。
    broken = ["0220000", "0000000", "0020000", "0000000", "0000000"]
    doc, g = _gv(broken, keep=[5, 4], spawns=spawns, walls=wall)
    c.true(not validate.check_forest_corridor(doc, g), "森林带断开后不该报")

    # 没有墙就谈不上「直达城墙」。
    doc, g = _gv(forest_lane, keep=[5, 4], spawns=spawns)
    c.true(not validate.check_forest_corridor(doc, g),
           "地图没有 initial_walls 时这条应当自动通过")

    # -- 斜向森林链：这条曾经漏报 -----------------------------------------
    #
    # 森林只有 (5,1) → (6,2) → (7,3) 这条**斜**链，四个角格全是 Plain。
    # 单位沿它走得过去（角格可通行），全程站在森林上、全程隐蔽。
    # 而 flood 若把穿角也按「是不是森林」判，角格不是森林 ⇒ 斜向走不通 ⇒
    # **报「通过」**。破坏性验证过：去掉 `corner=` 这条用例会红。
    #
    # 斜向林带不是刁钻构造 —— 生成器随机铺森林时它是常见形态。
    diag = []
    for y in range(11):
        row = ["0"] * 11
        if y == 1:
            row[5] = "2"
        if y == 2:
            row[6] = "2"
        if y == 3:
            row[7] = "2"
        diag.append("".join(row))
    diag_walls = [{"kind": "Wall", "pos": [x, 4], "hp_frac": 1.0}
                  for x in range(11)]
    doc, g = _gv(diag, keep=[5, 8],
                 spawns=[{"id": 0, "pos": [5, 0], "corridor": "forest"}],
                 walls=diag_walls)
    c.true(validate.check_forest_corridor(doc, g),
           "**斜向**森林链同样应当报 —— 穿角要按「能不能走」判，"
           "不能按「是不是森林」判（第 10 条的极性与第 4/11/12 条相反）")

    # 同一张图，把斜链中间那格挪开使它不再相邻，则确实走不通。
    broken_diag = list(diag)
    broken_diag[2] = "0" * 11
    doc, g = _gv(broken_diag, keep=[5, 8],
                 spawns=[{"id": 0, "pos": [5, 0], "corridor": "forest"}],
                 walls=diag_walls)
    c.true(not validate.check_forest_corridor(doc, g),
           "斜链断开后不该报 —— 否则上一条用例只是「总是报」")


def check_v8_initial_breach(c):
    """第 8 条：初始城圈至少一处缺口。

    判据是「存在一条从任一集结点到 keep、全程不穿墙的通路」，**不经过城区**。
    这一组同时钉住那条论证：`derive_city_area()` 恰好在本条该判失败的图上
    推导成功、在该判通过的图上失败，所以它不可能作为本条的前提。
    """
    ring = [{"kind": "Wall", "pos": [x, 1], "hp_frac": 1.0} for x in range(2, 5)]
    ring += [{"kind": "Wall", "pos": [x, 5], "hp_frac": 1.0} for x in range(2, 5)]
    ring += [{"kind": "Wall", "pos": [2, y], "hp_frac": 1.0} for y in (2, 3, 4)]
    ring += [{"kind": "Wall", "pos": [4, y], "hp_frac": 1.0} for y in (2, 3, 4)]

    plain = ["0000000"] * 7
    spawns = [{"id": 0, "pos": [0, 0], "corridor": "open"}]

    # 闭合的一圈墙 —— 进不来，本条必须报。
    doc, g = _gv(plain, keep=[3, 3], spawns=spawns, walls=ring)
    c.true(validate.check_initial_breach(doc, g),
           "闭合的初始城圈应当报 —— 2.3 要求留缺口，否则「AI 利用已有缺口」"
           "这条核心指标第 1 波测不了")

    # 同一圈，去掉 (3,1) 那一段 = 留一处缺口。
    gapped = [w for w in ring if tuple(w["pos"]) != (3, 1)]
    doc, g = _gv(plain, keep=[3, 3], spawns=spawns, walls=gapped)
    c.true(not validate.check_initial_breach(doc, g), "留了缺口就不该报")

    # 缺口被第二道墙堵在后面：字面判据（城圈上有一格没墙）会放行，
    # 而本条的判据判它没缺口 —— 那才是对的，AI 确实进不来。
    blocked = gapped + [{"kind": "Wall", "pos": [3, 0], "hp_frac": 1.0}]
    doc, g = _gv(plain, keep=[3, 3], spawns=spawns, walls=blocked)
    c.true(validate.check_initial_breach(doc, g),
           "缺口被第二道墙堵住时应当报 —— 这正是本条判据比字面判据更贴合意图之处")

    # 没有 initial_walls 时自动通过（没有墙，「进得来」显然成立）。
    doc, g = _gv(plain, keep=[3, 3], spawns=spawns)
    c.true(not validate.check_initial_breach(doc, g),
           "没有 initial_walls 时应当自动通过")

    # 「为什么不能用城区」那条论证已经被「城区 / 城圈 / 缺口」那一组钉住了
    # （`derive_city_area` 在 CLOSED 上成功、在 BREACHED 上抛
    # `CityAreaUndecidable`）。这里不再重复断言一遍 —— 同一件事测在两处，
    # 改动时必然只改一处。


def check_v11_islands(c):
    island = ["00000", "01110", "01010", "01110", "00000"]
    doc, g = _gv(island, keep=[0, 0], spawns=[
        {"id": 0, "pos": [0, 4], "corridor": "open"}])
    c.true(validate.check_plain_islands(doc, g),
           "被 Rock 围住的 Plain 格应当报为孤岛")

    doc, g = _gv(["000", "010", "000"], keep=[0, 0], spawns=[
        {"id": 0, "pos": [2, 2], "corridor": "open"}])
    c.true(not validate.check_plain_islands(doc, g),
           "孤零零一块 Rock 不制造孤岛")


def check_v12_water_cuts(c):
    spawns = [{"id": 0, "pos": [0, 2], "corridor": "open"}]
    river = ["00000", "33333", "00000"]
    doc, g = _gv(river, keep=[0, 0], spawns=spawns)
    c.true(validate.check_water_cuts_corridor(doc, g),
           "被河横断且无桥应当报")

    bridged = ["00000", "33433", "00000"]
    doc, g = _gv(bridged, keep=[0, 0], spawns=spawns)
    c.true(not validate.check_water_cuts_corridor(doc, g), "有桥就不该报")

    # 被 Rock 断开也不通，但那属第 4 条，第 12 条不该抢着报。
    rocked = ["00000", "11111", "00000"]
    doc, g = _gv(rocked, keep=[0, 0], spawns=spawns)
    c.true(not validate.check_water_cuts_corridor(doc, g),
           "Rock 切断应当归第 4 条，第 12 条只管水")
    c.true(validate.check_spawn_reachable(doc, g),
           "前提：Rock 切断确实会被第 4 条报出来")


def check_v13_bridge_on_water(c):
    doc, g = _gv(["000", "040", "000"], keep=[0, 0])
    c.true(validate.check_bridge_on_water(doc, g),
           "四邻没有 Water 的桥应当报（4.2.1 的底衬映射会渲出孤立的水）")

    doc, g = _gv(["030", "040", "000"], keep=[0, 0])
    c.true(not validate.check_bridge_on_water(doc, g), "桥挨着水就不该报")

    # 只在对角有水不算 —— 第 13 条明写「四邻」。
    doc, g = _gv(["300", "040", "000"], keep=[0, 0])
    c.true(validate.check_bridge_on_water(doc, g),
           "水只在对角时应当报 —— 第 13 条要求的是四邻")


def check_v15_hash(c):
    doc = make_doc(OBLONG, keep=[3, 3])
    g = Grid(doc)
    c.true(validate.check_content_hash(doc, g), "没有 content_hash 字段应当报")

    mapfile.stamp_content_hash(doc)
    c.true(not validate.check_content_hash(doc, g), "盖过章的地图不该报")

    doc["name"] = "改了名字但没重新盖章"
    c.true(validate.check_content_hash(doc, g), "内容改了而哈希没更新应当报")


def check_v16_entity_cells(c):
    """第 16 条：`resources` 与 `keep` 必须落在可通行且可建造的格上。

    来源是实测：把资源点摆进水里、把另一个摆进岩壁里，**格式层与那 15 条全部
    放行**。所以这一组逐个把实体摆到四种「盖不上去」的格上。
    """
    # OBLONG: (5,1) 是 Rock，(2,2) 是 Water，其余 Plain。
    ok = [{"type": t, "pos": [i, 0], "tier": "inner"}
          for i, t in enumerate(["stone", "wood", "gold"])]
    doc, g = _gv(OBLONG, keep=[3, 3], resources=ok)
    c.true(not validate.check_entity_cells(doc, g), "都在 Plain 上不该报")

    for bad_pos, what in ([5, 1], "Rock"), ([2, 2], "Water"):
        doc, g = _gv(OBLONG, keep=[3, 3],
                     resources=ok[:2] + [{"type": "gold", "pos": bad_pos,
                                          "tier": "inner"}])
        problems = validate.check_entity_cells(doc, g)
        c.true(problems, f"资源点摆在 {what} 上应当报")
        c.true(any(what in p for p in problems),
               f"报告要点出是 {what}，否则收到失败的人得自己去数格子")

    doc, g = _gv(OBLONG, keep=[5, 1], resources=ok)
    c.true(validate.check_entity_cells(doc, g), "keep 摆在 Rock 上应当报")

    # no_build 也算 —— 4.2 的组合规则是 buildable AND NOT no_build。
    nb = ["0000000", "0000000", "0000000", "0001000", "0000000"]
    doc, g = _gv(OBLONG, keep=[3, 3], no_build=nb, resources=ok)
    problems = validate.check_entity_cells(doc, g)
    c.true(problems, "keep 落在 no_build 格上应当报（地形是 Plain，但盖不了）")
    c.true(any("no_build=True" in p for p in problems),
           "报告要说明是 no_build 挡的，否则看到「地形是 Plain」会以为是误报")


def check_v17_placement_conflicts(c):
    """第 17 条：墙不在 `Rock`/`Water` 上、同格不叠墙、集结点不重合。

    **同格叠墙必须查 `doc` 而不是 `grid`**：`Grid.walls` 是按坐标索引的字典，
    重复在构造时就被覆盖了。这一组会抓到「拿 grid 去找重复」这种写法 ——
    那样写它永远报通过。
    """
    doc, g = _gv(OBLONG, keep=[3, 3],
                 walls=[{"kind": "Wall", "pos": [0, 0], "hp_frac": 1.0}])
    c.true(not validate.check_placement_conflicts(doc, g), "干净摆放不该报")

    for bad_pos, what in ([5, 1], "Rock"), ([2, 2], "Water"):
        doc, g = _gv(OBLONG, keep=[3, 3],
                     walls=[{"kind": "Wall", "pos": bad_pos, "hp_frac": 1.0}])
        problems = validate.check_placement_conflicts(doc, g)
        c.true(problems, f"墙摆在 {what} 上应当报")
        c.true(any(what in p for p in problems), f"报告要点出是 {what}")

    dup = [{"kind": "Wall", "pos": [1, 1], "hp_frac": 1.0},
           {"kind": "Gate", "pos": [1, 1], "hp_frac": 0.5}]
    doc, g = _gv(OBLONG, keep=[3, 3], walls=dup)
    c.true(len(g.walls) == 1,
           "前提：Grid 把同格重复墙折叠掉了（所以本条只能查 doc）")
    c.true(validate.check_placement_conflicts(doc, g), "同格两条墙应当报")

    same = [{"id": 0, "pos": [0, 0], "corridor": "open"},
            {"id": 1, "pos": [0, 0], "corridor": "defile"}]
    doc, g = _gv(OBLONG, keep=[3, 3], spawns=same)
    c.true(validate.check_placement_conflicts(doc, g),
           "两个集结点重合应当报 —— 格式层只查了 id 不重复")
    # 反面：id 相同才是格式层的事，本条不该越界去管。
    c.no_raise(lambda: mapfile.check_format(make_doc(OBLONG, keep=[3, 3],
                                                     spawns=same)),
               "pos 重合但 id 不同，格式层应当放行（所以必须由本条兜住）")


def check_v19_obstacle_types(c):
    """第 19 条：可破坏障碍列表里不得出现地形名。

    它保护 2.1.5——能砍森林就能围住外部资源簇，2.1 整节退化为数值劝退。

    **查白名单而不是黑名单**，所以这里连拼写错误一起验：黑名单只挡今天已知的
    `Forest` / `Rock`，白名单还挡住 `Stmup` 与将来新增的地形名。
    """
    doc = make_clean_doc()
    c.eq(validate.check_obstacle_types(doc, Grid(doc)), [],
         "干净地图（obstacles 为空）不该报")

    for bad in ("Forest", "Rock", "Water", "Plain", "Bridge"):
        d = make_clean_doc()
        d["obstacles"] = [{"type": bad, "pos": [0, 4]}]
        got = validate.check_obstacle_types(d, Grid(d))
        c.eq(len(got), 1, f"障碍类型写成地形名 {bad} 必须报")
        c.true("地形" in got[0], f"{bad} 的报错要点明它是地形名，实际：{got[0]}")

    d = make_clean_doc()
    d["obstacles"] = [{"type": "Stmup", "pos": [0, 4]}]
    got = validate.check_obstacle_types(d, Grid(d))
    c.eq(len(got), 1, "拼错的类型也必须报（白名单的附带收益）")
    c.true("拼错" in got[0], "拼错的报错不该说它是地形名")

    d = make_clean_doc()
    d["obstacles"] = [{"type": t, "pos": [i, 4]}
                      for i, t in enumerate(sorted(validate.OBSTACLE_TYPES))]
    c.eq(validate.check_obstacle_types(d, Grid(d)), [],
         "三种合法类型一个都不该报")


def check_v22_obstacle_placement(c):
    """第 22 条：`obstacles` 的摆放冲突。

    四类各验一次。**刻意不与第 17 条共用实现**，理由见 `validate.py` 那段
    docstring：把新实体塞进一条绿着的检查会让覆盖面在 review 里看不出变化。
    """
    doc = make_clean_doc()
    c.eq(validate.check_obstacle_placement(doc, Grid(doc)), [],
         "干净地图（obstacles 为空）不该报")

    # (1) 摆在天然屏障上。make_clean_doc 的行是 "0000020"×2 + "0000000"×3，
    #     一格 Rock / Water 都没有，所以先造一格出来。
    d = make_clean_doc()
    d["layers"]["terrain"]["rows"][4] = "0003000"     # (3,4) 变 Water
    d["obstacles"] = [{"type": "Stump", "pos": [3, 4]}]
    got = validate.check_obstacle_placement(d, Grid(d))
    c.eq(len(got), 1, "障碍摆在 Water 上必须报")

    # (2) 同格两处障碍。
    d = make_clean_doc()
    d["obstacles"] = [{"type": "Stump", "pos": [0, 4]},
                      {"type": "Rubble", "pos": [0, 4]}]
    got = validate.check_obstacle_placement(d, Grid(d))
    c.eq(len(got), 1, "同一格两处障碍必须报")

    # (3) 与别的点位实体同格。四种各验一次 —— 只验一种的话，
    #     occupied 那张表少收一类也不会有人知道。
    d0 = make_clean_doc()
    for what, pos in (("keep", list(d0["keep"])),
                      ("资源点", list(d0["resources"][0]["pos"])),
                      ("集结点", list(d0["spawns"][0]["pos"]))):
        d = make_clean_doc()
        d["obstacles"] = [{"type": "Stump", "pos": pos}]
        got = validate.check_obstacle_placement(d, Grid(d))
        c.true(len(got) >= 1, f"障碍与{what}同格必须报，实际：{got}")

    d = make_clean_doc()
    d["initial_walls"] = [{"kind": "Wall", "pos": [0, 4], "hp_frac": 1.0}]
    d["obstacles"] = [{"type": "Stump", "pos": [0, 4]}]
    got = validate.check_obstacle_placement(d, Grid(d))
    c.true(len(got) >= 1, f"障碍与墙段同格必须报，实际：{got}")

    # 合法摆放不该报：空闲的 Plain 格。
    d = make_clean_doc()
    d["obstacles"] = [{"type": "Stump", "pos": [0, 4]},
                      {"type": "Rubble", "pos": [1, 4]}]
    c.eq(validate.check_obstacle_placement(d, Grid(d)), [],
         "两处分开摆在空闲 Plain 格上不该报")


def check_v21_has_outer(c):
    """第 21 条：至少一个 `outer` 资源点。

    **这一组同时钉住这条为什么必须存在**：第 7 条在没有 `outer` 时空过，
    第 6 条只查 `inner`，所以少了本条，一张把所有资源都放在城里的地图
    能通过全部检查。下面第二段就是那张图。
    """
    inner_only = [{"type": t, "pos": [i, 0], "tier": "inner"}
                  for i, t in enumerate(["stone", "wood", "gold"])]
    doc, g = _gv(OBLONG, keep=[3, 3], resources=inner_only)
    c.true(validate.check_has_outer_resource(doc, g),
           "一个 outer 都没有应当报")
    # 那张图确实能过第 6、7 条 —— 这才是本条存在的理由。
    c.true(not validate.check_inner_resources(doc, g),
           "前提：全在城里的图过得了第 6 条")
    c.true(not validate.check_outer_unenclosable(doc, g),
           "前提：全在城里的图**空过**第 7 条，所以第 7 条兜不住这个洞")

    with_outer = inner_only + [{"type": "wood", "pos": [4, 0],
                                "tier": "outer"}]
    doc, g = _gv(OBLONG, keep=[3, 3], resources=with_outer)
    c.true(not validate.check_has_outer_resource(doc, g),
           "有一个 outer 就够（数量属数值，本条只查 ≥ 1）")


def check_validator_on_clean_map(c):
    """一张各条都过得去的地图，跑完整 run() 不应有任何失败。

    这一条是上面各条的反面：单条检查各自「不该报」不等于合在一起也干净，
    比如某条检查的测试地图恰好触发了另一条。
    """
    doc = make_clean_doc()
    mapfile.stamp_content_hash(doc)

    # **用 fixture 档。** 这张图是 5×7 的最小图，而第 1、5、14 条查的是
    # 尺寸与行军类的不变量——对一张最小图套用它们没有意义（详见
    # `thresholds.json` 的 `_note_two_profiles`）。下一组专门证明
    # 「换成 strict 它会红」，所以这里传 fixture 不是把检查关掉。
    th = thresholds.load().profile("fixture")
    failures = [(chk.no, problems) for chk, problems in validate.run(doc, th=th)
                if problems]
    c.true(not failures, f"干净地图不该有任何失败，实际：{failures}")

    # 顺带钉住三类计数，避免有人「顺手」把某条改成跳过。
    n_impl = sum(1 for chk in validate.CHECKS
                 if chk.status == validate.IMPLEMENTED)
    n_block = sum(1 for chk in validate.CHECKS
                  if chk.status == validate.BLOCKED)
    n_pend = sum(1 for chk in validate.CHECKS
                 if chk.status == validate.PENDING)
    c.eq((n_impl, n_block, n_pend), (20, 1, 0),
         "条目状态计数变了：改动状态时要同步这条断言与 README 的进度表")


def check_profile_is_not_a_noop(c):
    """**profile 不能是「把检查关掉」的后门。**

    `fixture` 那一档把尺寸与行军类阈值放宽到最小夹具也能过。这条自检证明它
    确实只是放宽阈值、而那些检查真的在跑：**同一张图换成 `strict` 必须红**，
    且红的是尺寸那一条。

    少了这条，`fixture` 与「把第 1、2、5、14 条删掉」在行为上不可区分，
    而后者是这个仓库反复防的那种「该红却绿」。
    """
    doc = make_clean_doc()
    mapfile.stamp_content_hash(doc)
    all_th = thresholds.load()

    strict_bad = {chk.no for chk, ps in
                  validate.run(doc, th=all_th.profile("strict")) if ps}
    fixture_bad = {chk.no for chk, ps in
                   validate.run(doc, th=all_th.profile("fixture")) if ps}

    c.true(1 in strict_bad,
           "5×7 的最小图在 strict 档下必须被第 1 条（size 区间）否决 —— "
           "否则说明那一条根本没在查")
    c.true(not fixture_bad,
           f"同一张图在 fixture 档下应当全过，实际红了：{sorted(fixture_bad)}")
    c.true(strict_bad != fixture_bad,
           "两档给出同一个结果，说明 profile 没起作用")

    # 真夹具（7×5，全仓唯一一张真地图）也走一遍同样的对照：
    # ctest 的 map_gen_validate_fixture 用的就是 fixture 档，这条钉住它没被放水。
    fixture_path = os.path.join(_REPO, "game", "testdata", "fixture_min.json")
    if os.path.exists(fixture_path):
        real = mapfile.load(fixture_path)
        r_strict = {chk.no for chk, ps in
                    validate.run(real, th=all_th.profile("strict")) if ps}
        r_fixture = {chk.no for chk, ps in
                     validate.run(real, th=all_th.profile("fixture")) if ps}
        c.true(1 in r_strict,
               "game/testdata/fixture_min.json 在 strict 下必须红在第 1 条")
        c.true(not r_fixture,
               f"它在 fixture 下必须全过（ctest 就是这么跑的），"
               f"实际红了：{sorted(r_fixture)}")


def check_chebyshev_is_conservative(c):
    """第 14 条的距离度量取切比雪夫，且这个选择对「视野是圆还是方」保守。

    钉住的是那条不等式本身：`cheb ≤ euc` 恒成立 ⇒ 方形球 ⊇ 同半径圆形球 ⇒
    `cheb > R` 对两种形状都够。同时钉住欧氏为什么不行（`dx=dy=R` 的反例）。
    """
    import math
    c.eq(gridmod.chebyshev((0, 0), (3, 4)), 4, "cheb((0,0),(3,4)) 应当是 4")
    c.eq(gridmod.chebyshev((5, 5), (5, 5)), 0, "同一格距离为 0")
    c.eq(gridmod.chebyshev((7, 2), (2, 3)), 5, "取两轴之差的最大值，与方向无关")

    # cheb ≤ euc 在一批点上都成立（这是「方形球 ⊇ 圆形球」的等价形式）。
    bad = [(dx, dy) for dx in range(-6, 7) for dy in range(-6, 7)
           if gridmod.chebyshev((0, 0), (dx, dy)) > math.hypot(dx, dy)]
    c.true(not bad, f"cheb ≤ euc 必须恒成立，反例：{bad[:3]}")

    # 欧氏的反例：R=3 时 (3,3) 在方形视野内（看得见），欧氏却判它出界。
    r = 3
    c.true(gridmod.chebyshev((0, 0), (3, 3)) <= r,
           "(3,3) 在半径 3 的方形视野内 —— 若视野是方的，这一格看得见")
    c.true(math.hypot(3, 3) > r,
           "而欧氏距离 3√2 > 3，欧氏判据会**放行** —— 这正是第 14 条要防的")


def check_palette_bound(c):
    """调色板超过 10 项必须报错，而不是静默失效。

    `mapfile.py` 的注释一直写着「越界要报错」，但在加那行 assert 之前它不会报：
    允许字符集是 `{str(i) for i in range(len(TERRAIN_PALETTE))}`，第 11 项时
    放进去的是**两字符**的 "10"，而 `set(row)` 里全是单字符，于是永远匹配不上
    —— 第 11 种地形既编不进去、也不会有任何东西红。地形枚举这一周刚从 3 变 5。
    """
    c.true(len(mapfile.TERRAIN_PALETTE) <= 10,
           "调色板必须 ≤ 10 项（rows 是单字符编码）")
    # 钉住那条断言真的存在：它是模块级的（导入时就跑完了），所以只能查源码。
    with open(mapfile.__file__, "r", encoding="utf-8") as f:
        src = f.read()
    c.true("assert len(TERRAIN_PALETTE) <= 10" in src,
           "mapfile.py 必须有一条模块级断言挡住调色板越界 —— "
           "否则那条注释说的「越界要报错」是假的")


def check_cli_missing_path_is_red(c):
    """路径写错一半也必须红。与「一张地图都没读到即失败」同源。

    原先「路径不存在」只往 stderr 打一句、退出码是 0，于是 `maps/train maps/demo`
    分成两个目录、其中一个改名之后，**覆盖面悄悄减半而 ctest 全绿**。
    """
    missing = []
    got = list(validate._iter_maps(["definitely_not_a_real_path_xyz"], missing))
    c.eq((len(got), len(missing)), (0, 1), "不存在的路径要被记下来")

    with tempfile.TemporaryDirectory() as d:
        good = os.path.join(d, "clean.json")
        mapfile.save(good, make_clean_doc())

        # 吞掉 main() 的报告：这一组只关心退出码，而把整份校验报告灌进自检
        # 日志会把真正的失败信息淹掉。失败时再原样打出来。
        def run_cli(*args):
            argv, out = sys.argv, sys.stdout
            buf = io.StringIO()
            try:
                sys.argv = ["validate.py", *args]
                sys.stdout = buf
                return validate.main(), buf.getvalue()
            finally:
                sys.argv, sys.stdout = argv, out

        # `--profile fixture`：这张图是最小图，尺寸类不变量对它不适用
        # （同 check_validator_on_clean_map，理由在那里）。这一组测的是
        # **退出码与路径处理**，不是阈值。
        rc, _ = run_cli("--profile", "fixture", good)
        c.eq(rc, 0, "一张干净地图应当退出 0")

        # 阈值档写错必须以「配置坏了」的名义红，而且**一张地图都不读**。
        rc, text = run_cli("--profile", "no_such_profile", good)
        c.eq(rc, 1, "认不出的 profile 必须红")
        c.true("阈值配置有问题" in text,
               "报错要说清是配置的问题，而不是变成每张地图上一条看起来"
               "像地图问题的报错")

        # 同一张干净地图 + 一个不存在的路径 ⇒ **必须** 非 0。
        rc, text = run_cli("--profile", "fixture", good,
                           os.path.join(d, "typo_dir"))
        c.eq(rc, 1,
             "读到了一些、另一些路径不存在时必须红 —— "
             "否则目录改名后覆盖面减半而没有任何东西提示")
        c.true("路径不存在" in text or "不存在" in text,
               "报告里要说清是哪条路径不存在，否则收到失败的人无从下手")


def check_thresholds_loader(c):
    """阈值读取层：**手误不得静默落回默认值。**

    与 `game::StatsLoader` 那条纪律逐字相同。这里的失败形态是
    「`spawn_count_min` 少个 `s` ⇒ 落回 0 ⇒ 第 2 条永远通过」，
    而那离病因极远。
    """
    import json as _json

    good = thresholds.load()
    c.eq(good.profile("strict").name, "strict", "strict 档必须能取到")
    c.true(good.generator.size > 0, "生成器配置必须能取到")

    def reject(raw, why):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "th.json")
            with open(p, "w", encoding="utf-8") as f:
                _json.dump(raw, f)
            try:
                thresholds.load(p)
            except thresholds.ThresholdError:
                return
            c.failures.append(f"[阈值] 本该被拒：{why}")
            c.count += 1

    base = _json.loads(open(thresholds.DEFAULT_PATH, encoding="utf-8").read())

    # schema 不认识（含上一格）——旧版少的键会被静默补默认值。
    bad = _json.loads(_json.dumps(base)); bad["schema"] = "map-thresholds/999"
    reject(bad, "schema 认不出")

    # 缺键。
    bad = _json.loads(_json.dumps(base))
    del bad["profiles"]["strict"]["size_min"]
    reject(bad, "profile 缺 size_min")

    # 拼错键（多一个认不出的）。**与漏掉它的后果相同，所以两者都要报。**
    bad = _json.loads(_json.dumps(base))
    bad["profiles"]["strict"]["size_mn"] = 1
    reject(bad, "profile 有拼错的键")

    # 区间写反：症状是「所有地图都红」，看起来像地图坏了。
    bad = _json.loads(_json.dumps(base))
    bad["profiles"]["strict"]["size_min"] = 200
    reject(bad, "size 区间是空的")

    # **生成器的 size 不在校验器的 size 区间内。**
    # 不拦的话生成器会产出一批第 1 条必然否决的图，而症状（丢弃率 100%）
    # 指不出根因是配置自己互相矛盾。
    bad = _json.loads(_json.dumps(base))
    bad["generator"]["size"] = base["profiles"]["strict"]["size_max"] + 8
    reject(bad, "generator.size 越出 strict 的 size 区间")

    # 走廊重复 = 两个集结点等价，§2.2 的信号消失。
    bad = _json.loads(_json.dumps(base))
    bad["generator"]["corridors"] = ["open", "open"]
    reject(bad, "corridors 有重复项")

    # inner 三种缺一（第 6 条要求各 ≥ 1）。
    bad = _json.loads(_json.dumps(base))
    bad["generator"]["inner_resources"] = {"stone": 1, "wood": 1, "gold": 0}
    reject(bad, "inner_resources 缺 gold")

    # 缺 strict 档本身。
    bad = _json.loads(_json.dumps(base))
    del bad["profiles"]["strict"]
    reject(bad, "没有 strict 档")


def check_ram_speed_comes_from_stats_table(c):
    """`Ram` 速度必须从数值表读，**不得在 thresholds.json 里另存一份**。

    CLAUDE.md 写明数值进仿真只有 `game/data/stats_placeholder.json` 这一条路。
    抄一份的后果是两份数值迟早分叉，那时第 5 条会拿一个仿真里根本不存在的速度
    算行军时间，而**没有任何东西会红**。
    """
    v = thresholds.ram_speed_cells_per_tick()
    c.true(v > 0, "读到的 Ram 速度必须是正数")

    # **按键判，不按文本判。** 第一版是 `"ram_speed" not in 文件文本`，
    # 而它撞上了那段解释「为什么 size 取 64」的注释键
    # （`_FINDING_size_vs_ram_speed`）—— 探针把**讨论**速度也当成了**存**速度。
    # 要挡的是后者：一个真的能被读走的键。
    import json as _json
    raw = _json.loads(open(thresholds.DEFAULT_PATH, encoding="utf-8").read())

    def real_keys(obj, path=""):
        if isinstance(obj, dict):
            for k, v in obj.items():
                if k.startswith("_"):      # `_note*` 是注释，不会被读走
                    continue
                yield f"{path}.{k}" if path else k
                yield from real_keys(v, f"{path}.{k}" if path else k)

    offenders = [k for k in real_keys(raw)
                 if "speed" in k.lower() or k.split(".")[-1] == "Ram"]
    c.true(not offenders,
           f"thresholds.json 里有存速度的键：{offenders} —— "
           f"速度只能有一个真相来源，就是数值表。抄一份的后果是两份数值迟早"
           f"分叉，那时第 5 条会拿一个仿真里根本不存在的速度算行军时间，"
           f"而没有任何东西会红")

    # 读不到时必须抛，而不是给个猜的默认值。
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "stats.json")
        with open(p, "w", encoding="utf-8") as f:
            f.write('{"units": {}}')
        try:
            thresholds.ram_speed_cells_per_tick(p)
            c.failures.append("[阈值] 数值表里没有 Ram 时本该抛")
            c.count += 1
        except thresholds.ThresholdError:
            c.count += 1


def _big_doc(size=64, spawn_at=None, walls_at=None):
    """一张够大的空白图，用来测尺寸/行军类的四条。

    全 `Plain` + `no_build` 全 0；调用方按需摆集结点与墙。
    刻意不复用 `make_clean_doc()`（那是 5×7 的最小图，正是这四条不适用的形状）。
    """
    rows = ["0" * size for _ in range(size)]
    nb = ["0" * size for _ in range(size)]
    doc = {
        "format": 1, "map_id": "big", "name": "大图",
        "size": [size, size],
        "layers": {"terrain": {"palette": list(mapfile.TERRAIN_PALETTE),
                               "rows": rows},
                   "no_build": {"rows": nb}},
        "keep": [size // 2, size // 2],
        "spawns": [{"id": 0, "pos": list(spawn_at or [size // 2, 2]),
                    "corridor": "open"}],
        "resources": [{"type": t, "pos": [size // 2 - 3 + i, size // 2],
                       "tier": "inner"}
                      for i, t in enumerate(["stone", "wood", "gold"])],
        "initial_walls": [{"kind": "Wall", "pos": list(p), "hp_frac": 1.0}
                          for p in (walls_at or [])],
        "obstacles": [],
    }
    return mapfile.stamp_content_hash(doc)


def check_v1_size_range(c):
    """第 1 条：两条边**分别**查，不查面积。"""
    th = thresholds.load().profile("strict")
    doc = _big_doc(th.size_min)
    c.true(not validate.check_size_range(doc, Grid(doc), th),
           "边长正好等于下界应当通过（区间是闭的）")

    doc = _big_doc(th.size_min)
    doc["size"] = [th.size_min, th.size_min - 1]
    c.true(validate.check_size_range(doc, Grid(doc), th),
           "有一条边越界就必须报 —— 一张 4×2000 的图面积正常而形状荒谬")


def check_v2_spawn_count_and_edge(c):
    """第 2 条：数量取**区间**，且集结点必须贴边。"""
    th = thresholds.load().profile("strict")
    size = th.size_min

    # 数量：区间而不是等于某个数。集结点数量是待定数值，而地图规范曾把它
    # 写成「定为 4」——那是把平衡旋钮当成结构结论。
    doc = _big_doc(size)
    doc["spawns"] = [{"id": i, "pos": [2 + i * 3, 2], "corridor": k}
                     for i, k in enumerate(sorted(mapfile.CORRIDOR_KINDS))]
    mapfile.stamp_content_hash(doc)
    c.true(not validate.check_spawn_count_and_edge(doc, Grid(doc), th),
           f"{len(doc['spawns'])} 个集结点应当落在 "
           f"[{th.spawn_count_min}, {th.spawn_count_max}] 内")

    doc = _big_doc(size)
    doc["spawns"] = [{"id": 0, "pos": [2, 2], "corridor": "open"}]
    mapfile.stamp_content_hash(doc)
    if th.spawn_count_min > 1:
        c.true(validate.check_spawn_count_and_edge(doc, Grid(doc), th),
               "少于下界必须报")

    # 贴边：集结点跑到地图中央 = 攻方在城边上凭空出现。
    doc = _big_doc(size, spawn_at=[size // 2, size // 2 - 4])
    probs = validate.check_spawn_count_and_edge(doc, Grid(doc), th)
    c.true(any("边界" in p for p in probs),
           "集结点离边界太远必须报，且报错要点出「边界」")


def check_v5_ram_march(c):
    """第 5 条：行军占比。**没有墙时自动通过**，同第 10 条的先例。"""
    th = thresholds.load().profile("strict")
    size = th.size_min
    validate.reset_ram_speed_cache()

    doc = _big_doc(size, walls_at=[])
    c.true(not validate.check_ram_march_fraction(doc, Grid(doc), th),
           "没有 initial_walls 时自动通过 —— 没有墙就谈不上「走到城墙外沿」")

    # 墙紧贴集结点 ⇒ 行军时间几乎为 0 ⇒ 占比远低于下界 ⇒ 必须报。
    doc = _big_doc(size, spawn_at=[size // 2, 2],
                   walls_at=[[size // 2, 3]])
    probs = validate.check_ram_march_fraction(doc, Grid(doc), th)
    c.true(probs, "墙贴着集结点必须报：玩家来不及反应，侦查没有时间窗口")

    # 判据是「两个区间有交集」，而不是「完全包含」——因为 episode 长度本身
    # 是一个待定区间。完全包含在当前占位值下只有唯一一个可行步数，
    # 那等于把一条检查变成一个等式。
    speed = thresholds.ram_speed_cells_per_tick()
    steps_ok = int(th.ram_march_fraction_min * speed * th.episode_ticks_min) + 2
    doc = _big_doc(size, spawn_at=[size // 2, 2],
                   walls_at=[[size // 2, 2 + steps_ok]])
    c.true(not validate.check_ram_march_fraction(doc, Grid(doc), th),
           f"行军 {steps_ok} 格时占比区间应与目标区间有交集")


def check_v14_spawn_buildable_distance(c):
    """第 14 条：它是一条**结构约束**的可校验形式，不是一个数值旋钮。

    「任何静态建筑的视野都不得覆盖集结区」——一座永久建筑若能照亮集结区，
    「这波要不要花钱侦查」就被一次性买断，`Scout`、`Wraith` 屏蔽、佯攻诱饵
    会一起失效。所以它不能靠「把视野半径调小一点」解决。
    """
    th = thresholds.load().profile("strict")
    size = th.size_min
    ring = int(th.static_vision_radius_max)

    # 全图可建造 ⇒ 集结点脚下就能建 ⇒ 必须报。
    doc = _big_doc(size)
    c.true(validate.check_spawn_buildable_distance(doc, Grid(doc), th),
           "集结点脚下可建造时必须报")

    # 在集结点周围铺 no_build 环 ⇒ 通过。**这正是生成器采用的手段**：
    # 用禁建区表达那条结构约束，而不是靠调小视野半径。
    spawn = (size // 2, 2)
    nb = [list("0" * size) for _ in range(size)]
    for oy in range(-(ring + 1), ring + 2):
        for ox in range(-(ring + 1), ring + 2):
            x, y = spawn[0] + ox, spawn[1] + oy
            if 0 <= x < size and 0 <= y < size:
                nb[y][x] = "1"
    doc = _big_doc(size, spawn_at=list(spawn))
    doc["layers"]["no_build"]["rows"] = ["".join(r) for r in nb]
    mapfile.stamp_content_hash(doc)
    ok_doc = doc      # 下面几条要复用这张「距离本身没问题」的图
    c.true(not validate.check_spawn_buildable_distance(doc, Grid(doc), th),
           f"集结点周围 {ring + 1} 格禁建后应当通过")

    # ---- 上限必须与数值表一致，否则本条会**绿着失效** ----
    #
    # 这一段是两次破坏性验证的常驻形式。原先 `static_vision_radius_max` 是手写的
    # 8 而表里 `Watch` 是 12，于是本条判定合法的图上一座瞭望塔照旧照亮集结区
    # ——实测生成器默认产出的四个集结点到最近可建造格都是 10 格，`Watch(12)`
    # 全部够得着，16/16。**没有任何东西会红**，因为本条只看它自己那个声明。
    table_max, worst = thresholds.max_building_vision()
    c.true(table_max > 0 and isinstance(worst, str),
           "max_building_vision() 应当返回（最大值, 建筑名）")

    import json as _json
    raw = _json.loads(open(thresholds.DEFAULT_PATH, encoding="utf-8").read())
    c.true(raw["profiles"]["strict"]["static_vision_radius_max"] >= table_max,
           f"thresholds.json 的 strict 声明 "
           f"{raw['profiles']['strict']['static_vision_radius_max']} 小于数值表里"
           f"建筑视野的最大值 {table_max:g}（{worst}）—— 那让本条绿着失效")

    # 破坏 A：声明比表里最大值小 ⇒ 即便距离本身够，也必须报「上限本身失效」。
    low = _json.loads(_json.dumps(raw["profiles"]["strict"]))
    low["static_vision_radius_max"] = int(table_max) - 1
    th_low = thresholds.Profile("strict", low)
    probs = validate.check_spawn_buildable_distance(ok_doc, Grid(ok_doc), th_low)
    c.true(probs, "声明小于表里最大值时必须报——否则那是一次绿着的违反")
    c.true(any("上限本身失效" in p for p in probs),
           f"报的应当是「上限本身失效」而不只是距离不够，实际：{probs}")

    # 破坏 B：声明更**大**是允许的（更保守），但那张原本刚好通过的图会因此不够。
    # 这一条同时钉住「cap 取两者的较大值」，而不是「取声明」。
    high = _json.loads(_json.dumps(raw["profiles"]["strict"]))
    high["static_vision_radius_max"] = ring + 4
    th_high = thresholds.Profile("strict", high)
    probs = validate.check_spawn_buildable_distance(ok_doc, Grid(ok_doc), th_high)
    c.true(probs and not any("上限本身失效" in p for p in probs),
           "声明更大时不该报「上限失效」（更保守是允许的），"
           f"但距离应当不够，实际：{probs}")

    # 负数 = 本档显式弃用本条（只有 fixture 用）。**弃用的档不去核对声明**，
    # 否则 fixture 会因为 -1 < 12 而红，而那条 -1 有它自己的理由
    # （7×5 上「集结点远离一切可建造格」客观上无法满足）。
    off = _json.loads(_json.dumps(raw["profiles"]["strict"]))
    off["static_vision_radius_max"] = -1
    th_off = thresholds.Profile("strict", off)
    c.true(not validate.check_spawn_buildable_distance(
               _big_doc(size), Grid(_big_doc(size)), th_off),
           "负数上限应当让本条恒真（fixture 那一档靠它），且不核对声明")


def check_generator_produces_valid_maps(c):
    """生成器产出的图必须通过**全部**检查（§9：不通过就丢弃重生成）。"""
    all_th = thresholds.load()
    th, cfg = all_th.profile("strict"), all_th.generator

    doc, attempts = generate.generate_valid(1, cfg, th)
    c.true(attempts >= 1, "尝试次数至少 1")
    bad = [(chk.no, ps) for chk, ps in validate.run(doc, Grid(doc), th) if ps]
    c.true(not bad, f"生成的图必须通过全部检查，实际红了：{bad}")

    # 格式层也要过（`mapfile.check_format` 与校验器是两层）。
    mapfile.check_format(doc)
    c.true(mapfile.verify_content_hash(doc), "生成的图 content_hash 必须自洽")

    # §9：`map_id` 编码生成种子，使任何一局训练都可复现。
    c.true("1" in doc["map_id"] and doc["map_id"].startswith("gen_"),
           f"map_id 必须编码种子，实际是 {doc['map_id']!r}")

    # 走廊性质必须**真的体现在地形上**，否则「选集结点」没有可学的信号。
    # 这一条盯的是两个真实 bug：林地走廊一株森林都没撒、`defile` 的夹壁
    # 起点错了一个半径 —— 两者都不会让任何检查变红。
    names = generate._terrain_names(doc)
    counts = {}
    for row in names:
        for cell in row:
            counts[cell] = counts.get(cell, 0) + 1
    c.true(counts.get("Forest", 0) > 0, "生成的图必须有 Forest（森林带 + 林地走廊）")
    c.true(counts.get("Rock", 0) > 0, "生成的图必须有 Rock（城圈岩壁 + 隘口夹壁）")

    corridors = {s["corridor"] for s in doc["spawns"]}
    c.eq(sorted(corridors), sorted(cfg.corridors),
         "生成的集结点必须正好覆盖配置里那几种走廊（§2.2：数量 = 种类数）")

    # 林地走廊附近真的有森林 —— 且**不是连成一条通到墙的**（第 10 条已经查了
    # 后半句，这里查前半句：它不能一株都没有）。
    if "forest" in cfg.corridors:
        fpos = next(tuple(s["pos"]) for s in doc["spawns"]
                    if s["corridor"] == "forest")
        keep = tuple(doc["keep"])
        near = 0
        for y, row in enumerate(names):
            for x, cell in enumerate(row):
                if cell != "Forest":
                    continue
                # 落在集结点与 keep 之间那条带上
                if min(fpos[0], keep[0]) - 4 <= x <= max(fpos[0], keep[0]) + 4 \
                        and min(fpos[1], keep[1]) <= y <= max(fpos[1], keep[1]):
                    near += 1
        c.true(near > 0,
               "林地走廊沿途一株森林都没有 —— §2.2 那条性质就只是个标签了。"
               "这不会让任何检查变红，只有看产出才发现")


def check_generator_is_deterministic(c):
    """同种子同结果，不同种子不同图。§9：`map_id` 编码种子使训练可复现。"""
    all_th = thresholds.load()
    th, cfg = all_th.profile("strict"), all_th.generator

    a, _ = generate.generate_valid(7, cfg, th)
    b, _ = generate.generate_valid(7, cfg, th)
    c.eq(a["content_hash"], b["content_hash"],
         "同一个种子必须生成逐字节相同的图 —— 否则「map_id 编码种子」是假的，"
         "训练不可复现")
    c.eq(mapfile.canonical_dumps(a), mapfile.canonical_dumps(b),
         "规范序列化也必须逐字节相同")

    d, _ = generate.generate_valid(8, cfg, th)
    c.true(d["content_hash"] != a["content_hash"],
           "不同种子必须给出不同的图 —— §9 开头：单张地图训练不够，"
           "策略会记住地图而非学会战术")


def check_generator_discard_reporting_works(c):
    """**丢弃与否决计数这条路径必须被走过一次。**

    生成器现在的策略很保守，丢弃率是 0%（报告自己会说这不一定是好消息）。
    于是「丢弃 → 计数 → 报告」这条路径在正常运行里从不执行，
    而一条从没人见过它红的守卫和不存在没有区别。

    这里把阈值收紧到必然否决，断言：失败**有界**（不挂住）、
    抛的是 `GenerationFailed`、且 `tally` 指向真正否决它的那一条。
    """
    all_th = thresholds.load()
    cfg = all_th.generator

    # 复制一份 strict 并把行军占比收紧到不可能满足（当前占位下真实占比约 17–33%）。
    th = all_th.profile("strict")

    class Tightened:
        pass
    tight = Tightened()
    for k in thresholds.PROFILE_KEYS:
        setattr(tight, k, getattr(th, k))
    tight.name = "tightened(自检用)"
    tight.ram_march_fraction_min = 0.001
    tight.ram_march_fraction_max = 0.002

    from collections import Counter
    tally = Counter()

    class Few:
        pass
    few = Few()
    for k in thresholds.GENERATOR_KEYS:
        setattr(few, k, getattr(cfg, k))
    few.max_attempts = 3          # 有界：自检不该花几十秒

    try:
        generate.generate_valid(99, few, tight, tally)
        c.failures.append("[生成器] 阈值收紧到不可能满足时本该抛 GenerationFailed")
        c.count += 1
    except generate.GenerationFailed as e:
        c.count += 1
        c.eq(sum(e.tally.values()) > 0, True, "否决计数必须非空")
        c.true(5 in e.tally,
               f"收紧的是行军占比，否决计数应当指向第 5 条，实际是 "
               f"{dict(e.tally)} —— 报告指错条目比不报更坏，"
               f"因为它会让人去松错的那一边")


def check_generator_preview_renders(c):
    """ASCII 预览必须能画出来，且**全是 ASCII**。

    记号用 ✓ 之类会在 Windows 中文控制台（cp936）上抛 UnicodeEncodeError，
    而报错完全不指向真正的原因 —— 同 `validate._marks()` 那条，
    只是这里从一开始就不用非 ASCII。
    """
    all_th = thresholds.load()
    doc, _ = generate.generate_valid(3, all_th.generator,
                                     all_th.profile("strict"))
    art = generate.preview(doc)
    c.true(art.count("\n") >= doc["size"][1], "预览的行数至少等于地图高度")
    c.true("K" in art, "预览里必须能看到 keep")
    c.true("X" in art, "预览里必须能看到集结点")
    try:
        art.encode("cp936")
        c.count += 1
    except UnicodeEncodeError as e:
        c.failures.append(f"[生成器] 预览含 cp936 编码不了的字符：{e}")
        c.count += 1


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
    ("第 8 节：注册表覆盖性", check_registry_covers_spec),
    ("第 3 条 corridor", check_v3_corridors),
    ("第 4 条 集结点可达", check_v4_reachable),
    ("第 6 条 inner 三种资源", check_v6_inner_resources),
    ("第 7 条 外部资源点不可围", check_v7_outer_unenclosable),
    ("第 7 与第 10 条的对立是真的", check_v7_v10_tension_is_real),
    ("第 8 条 初始城圈留缺口", check_v8_initial_breach),
    ("第 10 条 森林遮蔽通道", check_v10_forest_corridor),
    ("第 11 条 Plain 孤岛", check_v11_islands),
    ("第 12 条 水切断走廊", check_v12_water_cuts),
    ("第 13 条 桥在水上", check_v13_bridge_on_water),
    ("第 15 条 content_hash", check_v15_hash),
    ("第 16 条 实体落在可建造格", check_v16_entity_cells),
    ("第 17 条 摆放冲突", check_v17_placement_conflicts),
    ("第 18 条 resources 解禁波数按距离单调", check_v18_resource_unlock_wave),
    ("第 19 条 障碍类型不得是地形名", check_v19_obstacle_types),
    ("第 21 条 至少一个 outer 资源点", check_v21_has_outer),
    ("第 22 条 障碍摆放冲突", check_v22_obstacle_placement),
    ("干净地图跑完整 run()", check_validator_on_clean_map),
    ("profile 不是无操作（strict 下夹具必须红）", check_profile_is_not_a_noop),
    ("第 14 条的度量：切比雪夫且形状无关", check_chebyshev_is_conservative),
    ("调色板 ≤ 10 项", check_palette_bound),
    ("CLI：路径写错一半必须红", check_cli_missing_path_is_red),
    # —— 阈值归口与它解锁的四条（第 1、2、5、14 条）——
    ("阈值读取层：手误不得静默落回默认值", check_thresholds_loader),
    ("Ram 速度只有一个真相来源", check_ram_speed_comes_from_stats_table),
    ("第 1 条 size 区间", check_v1_size_range),
    ("第 2 条 集结点数量与贴边", check_v2_spawn_count_and_edge),
    ("第 5 条 Ram 行军占比", check_v5_ram_march),
    ("第 14 条 集结点到可建造格的距离", check_v14_spawn_buildable_distance),
    # —— 生成器（第 9 节）——
    ("生成器产出合法地图", check_generator_produces_valid_maps),
    ("生成器确定：同种子同图、异种子异图", check_generator_is_deterministic),
    ("生成器的丢弃与否决计数真的工作", check_generator_discard_reporting_works),
    ("生成器的 ASCII 预览", check_generator_preview_renders),
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
