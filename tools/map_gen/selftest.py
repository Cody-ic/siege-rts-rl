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
    c.eq(sorted(nos), list(range(1, validate.SPEC_CHECK_COUNT + 1)),
         "CHECKS 必须正好覆盖第 8 节的 1..21（含 8.1 那批）")
    c.eq(len(set(nos)), len(nos), "条目编号不得重复")

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
        resources=[{"type": t, "pos": [i + 1, 2], "tier": "inner"}
                   for i, t in enumerate(["stone", "wood", "gold"])]
                  + [{"type": "wood", "pos": [5, 2], "tier": "outer"}])


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

    failures = [(chk.no, problems) for chk, problems in validate.run(doc)
                if problems]
    c.true(not failures, f"干净地图不该有任何失败，实际：{failures}")

    # 顺带钉住三类计数，避免有人「顺手」把某条改成跳过。
    n_impl = sum(1 for chk in validate.CHECKS
                 if chk.status == validate.IMPLEMENTED)
    n_block = sum(1 for chk in validate.CHECKS
                  if chk.status == validate.BLOCKED)
    n_pend = sum(1 for chk in validate.CHECKS
                 if chk.status == validate.PENDING)
    c.eq((n_impl, n_block, n_pend), (13, 4, 4),
         "条目状态计数变了：改动状态时要同步这条断言与 README 的进度表")


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

        rc, _ = run_cli(good)
        c.eq(rc, 0, "一张干净地图应当退出 0")

        # 同一张干净地图 + 一个不存在的路径 ⇒ **必须** 非 0。
        rc, text = run_cli(good, os.path.join(d, "typo_dir"))
        c.eq(rc, 1,
             "读到了一些、另一些路径不存在时必须红 —— "
             "否则目录改名后覆盖面减半而没有任何东西提示")
        c.true("路径不存在" in text or "不存在" in text,
               "报告里要说清是哪条路径不存在，否则收到失败的人无从下手")


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
    ("第 21 条 至少一个 outer 资源点", check_v21_has_outer),
    ("干净地图跑完整 run()", check_validator_on_clean_map),
    ("第 14 条的度量：切比雪夫且形状无关", check_chebyshev_is_conservative),
    ("调色板 ≤ 10 项", check_palette_bound),
    ("CLI：路径写错一半必须红", check_cli_missing_path_is_red),
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
