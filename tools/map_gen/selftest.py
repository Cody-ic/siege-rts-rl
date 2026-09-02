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
import math
import os
import random
import sys
import tempfile
from types import SimpleNamespace

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
             resources=None, walls=None, obstacles=None, buildings=None,
             name="测试图"):
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
        # 2026-08-31：spawns 不再携带 corridor（字段已废除）；
        # 既有用例里残留的 corridor 键被 check_format 静默忽略，无碍。
        "spawns": spawns if spawns is not None else [
            {"id": 0, "pos": [w // 2, 0]},
        ],
        "resources": resources if resources is not None else [],
        "initial_walls": walls if walls is not None else [],
        # 6.2 的 `obstacles` 是**必填**的（缺失与刻意为空不可区分是个洞），
        # 所以默认给空数组而不是省掉这个键 —— 省掉会让第 19、22 条抛 KeyError，
        # 而那表现为「这一组自身抛了异常」，不是一条有用的失败。
        "obstacles": obstacles if obstacles is not None else [],
        # 6.2 的 `buildings`（2026-08-31 新增）同样必填，同一条纪律。
        "buildings": buildings if buildings is not None else [],
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
        {"id": 0, "corridor": "open"}]), "spawn 缺 pos")
    # 2026-08-31：原「corridor 不在候选清单里」case 随字段废除一起删除——
    # spawn 现在只剩 id 与 pos 两个必填项。
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
    """注册表必须正好覆盖第 8 节的全部条目，不重不漏。

    这一条防的是「白名单漏登记」——`tests/CMakeLists.txt` 里 ctest 标签清单
    已经踩过一次：写错抓得到，**没写进清单抓不到**。第 8 节将来加条目，
    这里会立刻红。
    """
    nos = [chk.no for chk in validate.CHECKS]
    moved = sorted(validate.MOVED_TO_GENERATOR)
    removed = sorted(validate.REMOVED)
    # **表内 + 已移出 + 已废除 = 规范全部条目。** 第 3 条随走廊概念废除、
    # 第 7 条随森林带机制移除、第 20 条随第 7 条一起废除（三者编号都留空）——
    # 都**不是被删掉**：直接删会让一条规范条目无声消失，
    # 而那与「白名单漏登记」是同一类错误。
    c.eq(sorted(nos + moved + removed),
         list(range(1, validate.SPEC_CHECK_COUNT + 1)),
         "CHECKS + MOVED_TO_GENERATOR + REMOVED 必须正好覆盖第 8 节的全部条目")
    c.eq(len(set(nos)), len(nos), "条目编号不得重复")
    c.true(not (set(nos) & set(moved)),
           "一条既在表内又标为已移出，说明移出时忘了删表里那一行")
    c.true(not (set(nos) & set(removed)),
           "一条既在表内又标为已废除，说明废除时忘了删表里那一行")
    for no in moved:
        c.true(bool(validate.MOVED_TO_GENERATOR[no]),
               f"第 {no} 条标为已移出，必须写明去哪了")
    for no in removed:
        c.true(bool(validate.REMOVED[no]),
               f"第 {no} 条标为已废除，必须写明为什么")

    for chk in validate.CHECKS:
        if chk.status == validate.IMPLEMENTED:
            c.true(chk.fn is not None,
                   f"第 {chk.no} 条标为「已实现」却没挂函数")
        else:
            c.true(chk.fn is None,
                   f"第 {chk.no} 条标为「{chk.status}」却挂了函数")
            c.true(bool(chk.note),
                   f"第 {chk.no} 条未实现，必须写明原因（否则会慢慢变成默认跳过）")


# check_v3_corridors 已随「走廊」概念一起删除（2026-08-31）：
# 第 3 条废除、编号留空，`validate.REMOVED` 里有据可查。


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


# check_v18_resource_unlock_wave 已随第 18 条一起删除（2026-09-02）：
# 该条废除、并入第 28 条（簇级单调不蕴含点级单调，两条结构性冲突），
# 编号留空，`validate.REMOVED` 里有据可查。同 check_v3_corridors 先例。


def make_clean_doc():
    """一张各条都过得去的地图。**只此一份**，两处用例共用。

    此前是两份拷贝（`check_validator_on_clean_map` 与那条 CLI 用例各一份），
    于是第 21 条落地时**只改了一份**，另一份红在「一张干净地图应当退出 0」
    这句话上 —— 而那条报错完全不指向真正的原因。两份「同一张图」的拷贝就是
    在等这件事发生。

    (5,2) 的 `outer` 资源点是**第 21 条**要求的：21 条要至少有一个 `outer`。
    这张图原先只有 inner 资源点 —— 而它能通过当时的全部检查，**正是第 21 条
    存在的理由**。x=5 那两格 `Forest`**曾经**是第 7 条额外要求的（一条 4 连通
    的森林带通到边界）——第 7 条已随 `地图与场景设计.md` 2.1 的订正废除
    （2026-09-01），这两格森林留着纯粹是历史遗留，不再对应任何检查，
    但留着也无害，就没有顺手删掉重新验证一遍每处依赖它的用例。

    **2026-09-02：这个 outer 点的类型从 wood 改成 gold**——第 29 条（大改
    第 2 步新增）要求「内环至少一簇含金」，而这张图按「集结点环内侧」分，
    这个孤点簇落在内环。改成 gold 之后它同时满足第 21 条（有 outer）与
    第 29 条（内环含金）；配额半边（三类簇数两两差 ≤ 1）在单簇图上恒过。
    第 31 条在 fixture 档被弃用（阈值 -1，7×5 上 16 格客观不可满足）。

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
                  + [{"type": "gold", "pos": [5, 2], "tier": "outer",
                     "unlock_wave": 2}])



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


def check_v24_building_placement(c):
    """第 24 条：`buildings`（玩家开局已拥有的其余建筑）的摆放冲突。

    五类各验一次，前四类与第 22 条对 `obstacles` 的验法逐条对应；多的一类
    （摆在不可建造格上）是因为建筑要真的"盖得上去"，同第 16 条对
    `resources`/`keep` 的验法。
    """
    doc = make_clean_doc()
    c.eq(validate.check_building_placement(doc, Grid(doc)), [],
         "干净地图（buildings 为空）不该报")

    # (1) 摆在不可建造格上（no_build=1）。make_clean_doc 的 no_build 全 0，
    #     所以先造一格出来。
    d = make_clean_doc()
    d["layers"]["no_build"]["rows"][4] = "0100000"    # (1,4) 变不可建造
    d["buildings"] = [{"type": "Tower", "pos": [1, 4]}]
    got = validate.check_building_placement(d, Grid(d))
    c.eq(len(got), 1, "建筑摆在 no_build 格上必须报")

    # (2) 摆在天然屏障上（Water）。
    d = make_clean_doc()
    d["layers"]["terrain"]["rows"][4] = "0003000"     # (3,4) 变 Water
    d["buildings"] = [{"type": "Barrack", "pos": [3, 4]}]
    got = validate.check_building_placement(d, Grid(d))
    c.eq(len(got), 1, "建筑摆在 Water 上必须报")

    # (3) 同格两座建筑。
    d = make_clean_doc()
    d["buildings"] = [{"type": "Tower", "pos": [0, 4]},
                      {"type": "Barrack", "pos": [0, 4]}]
    got = validate.check_building_placement(d, Grid(d))
    c.eq(len(got), 1, "同一格两座建筑必须报")

    # (4) 与别的点位实体同格。四种各验一次。
    d0 = make_clean_doc()
    for what, pos in (("keep", list(d0["keep"])),
                      ("资源点", list(d0["resources"][0]["pos"])),
                      ("集结点", list(d0["spawns"][0]["pos"]))):
        d = make_clean_doc()
        d["buildings"] = [{"type": "Tower", "pos": pos}]
        got = validate.check_building_placement(d, Grid(d))
        c.true(len(got) >= 1, f"建筑与{what}同格必须报，实际：{got}")

    d = make_clean_doc()
    d["initial_walls"] = [{"kind": "Wall", "pos": [0, 4], "hp_frac": 1.0}]
    d["buildings"] = [{"type": "Tower", "pos": [0, 4]}]
    got = validate.check_building_placement(d, Grid(d))
    c.true(len(got) >= 1, f"建筑与墙段同格必须报，实际：{got}")

    d = make_clean_doc()
    d["obstacles"] = [{"type": "Stump", "pos": [0, 4]}]
    d["buildings"] = [{"type": "Tower", "pos": [0, 4]}]
    got = validate.check_building_placement(d, Grid(d))
    c.true(len(got) >= 1, f"建筑与障碍同格必须报，实际：{got}")

    # 合法摆放不该报：空闲的 Plain 格。
    d = make_clean_doc()
    d["buildings"] = [{"type": "Tower", "pos": [0, 4]},
                      {"type": "Barrack", "pos": [1, 4]}]
    c.eq(validate.check_building_placement(d, Grid(d)), [],
         "空闲的可建造格上摆建筑不该报")

    # (5) 采集建筑踩在**匹配**的资源点上——不是冲突，是它唯一的生效摆法。
    d = make_clean_doc()
    stone_pos = list(d["resources"][0]["pos"])   # make_clean_doc 的第 0 个是 stone
    d["buildings"] = [{"type": "Quarry", "pos": stone_pos}]
    c.eq(validate.check_building_placement(d, Grid(d)), [],
         "Quarry 踩在 stone 资源点上不该报——那是它生效的唯一摆法")

    # (6) 采集建筑踩在**不匹配**的资源点上——类型不对，仍要报。
    d = make_clean_doc()
    wood_pos = list(d["resources"][1]["pos"])    # 第 1 个是 wood
    d["buildings"] = [{"type": "Quarry", "pos": wood_pos}]
    got = validate.check_building_placement(d, Grid(d))
    c.eq(len(got), 1, "Quarry 踩在 wood 资源点上（类型不匹配）必须报")

    # (7) 非采集建筑踩在资源点上——同 (4) 的"与资源点同格"分支，仍要报。
    d = make_clean_doc()
    d["buildings"] = [{"type": "Tower", "pos": stone_pos}]
    got = validate.check_building_placement(d, Grid(d))
    c.eq(len(got), 1, "Tower（非采集建筑）踩在资源点上必须报")


def check_v25_outer_gold(c):
    """第 25 条：outer 资源里金矿 ≥ 阈值（2026-08-31 实测查出城外可零金矿）。

    用 strict 档（阈值 2）与「弃用档」（阈值 0，fixture 同款手法）各验一次。
    """
    th = thresholds.load().profile("strict")
    d = make_clean_doc()
    d["resources"] = [
        {"type": "wood", "pos": [0, 0], "tier": "outer", "unlock_wave": 2},
        {"type": "stone", "pos": [1, 0], "tier": "outer", "unlock_wave": 3},
    ]
    c.true(validate.check_outer_gold(d, Grid(d), th),
           "城外零金矿必须报（strict 阈值 2）")

    d["resources"].append(
        {"type": "gold", "pos": [2, 0], "tier": "outer", "unlock_wave": 4})
    c.true(validate.check_outer_gold(d, Grid(d), th),
           "城外 1 个金矿仍低于阈值 2，必须报")

    d["resources"].append(
        {"type": "gold", "pos": [3, 0], "tier": "outer", "unlock_wave": 5})
    c.true(not validate.check_outer_gold(d, Grid(d), th),
           "城外 2 个金矿应当通过")

    off = thresholds.Profile("strict", {"size_min": 4, "size_max": 96,
                                        "spawn_count_min": 1, "spawn_count_max": 6,
                                        "spawn_wall_distance_range": [0, 4096],
                                        "static_vision_radius_max": -1,
                                        "episode_ticks_min": 1200,
                                        "episode_ticks_max": 2400,
                                        "ram_march_fraction_min": 0.0,
                                        "ram_march_fraction_max": 1.0,
                                        "forest_min_component_cells": 1,
                                        "front_length_min": 1,
                                        "front_length_max": 4096,
                                        "outer_gold_min": 0,
                                        "route_cluster_window": -1,
                                        "cluster_spawn_distance_min": -1,
                                        # 2026-09-02 第 3/4 步新键，弃用值同
                                        # fixture 档（恒真 / 下界 < 0）。
                                        "wild_blocked_fraction_min": 0.0,
                                        "wild_blocked_fraction_max": 1.0,
                                        "route_width_range": [-1, -1],
                                        "bridge_width_min": 0,
                                        "river_cut_spawns_max": -1})
    d2 = make_clean_doc()
    d2["resources"] = [
        {"type": "stone", "pos": [0, 0], "tier": "outer", "unlock_wave": 2}]
    c.true(not validate.check_outer_gold(d2, Grid(d2), off),
           "阈值为 0 时本条应恒真（fixture 那一档靠它）")



def _outer_res(t, x, y, wave):
    """一条 outer 资源条目（第 28/29/31 条用例的造图砖）。"""
    return {"type": t, "pos": [x, y], "tier": "outer", "unlock_wave": wave}


def check_v28_unlock_by_cluster(c):
    """第 28 条：解禁**按簇**——同簇同波（一簇是一个决策单元），簇解禁序按
    簇心到 keep 的切比雪夫距离单调。

    `_big_doc(64)` 的 keep 在 (32,32)。近簇 (22,32)/(24,32)（簇心距 9）、
    远簇 (8,32)/(10,32)（簇心距 23）；两簇最近点隔 12 格（> 8，不会并簇）。
    """
    near = [_outer_res("gold", 22, 32, 1), _outer_res("stone", 24, 32, 1)]
    far = [_outer_res("wood", 8, 32, 2), _outer_res("wood", 10, 32, 2)]

    doc = _big_doc(64)
    doc["resources"] += [dict(r) for r in near + far]
    c.eq(validate.check_cluster_unlock_wave(doc, Grid(doc)), [],
         "同簇同波且簇序单调时不该报")

    # 同簇拆波：一簇是一个决策单元，拆波 = 拆成几次半吊子的决策。
    doc = _big_doc(64)
    bad = [dict(r) for r in near + far]
    bad[1]["unlock_wave"] = 2
    doc["resources"] += bad
    probs = validate.check_cluster_unlock_wave(doc, Grid(doc))
    c.true(any("决策单元" in p for p in probs),
           f"同簇两个波号必须报，实际 {probs}")

    # 簇序反了：远的早解禁、近的晚解禁。
    doc = _big_doc(64)
    bad = [dict(r) for r in near + far]
    for r in bad[:2]:
        r["unlock_wave"] = 2
    for r in bad[2:]:
        r["unlock_wave"] = 1
    doc["resources"] += bad
    probs = validate.check_cluster_unlock_wave(doc, Grid(doc))
    c.true(any("更远" in p for p in probs),
           f"远簇比近簇早解禁必须报，实际 {probs}")


def check_v29_type_quota(c):
    """第 29 条：三类簇数两两之差 ≤ 1；簇类型 = 簇内点数的唯一众数；
    内环（集结点环内侧）至少一簇含金。

    集结点摆在 (32,20)（距 keep 12 格），于是内环 = 簇心 cheb < 12：
    近簇在 (24..26, 32..34)（簇心 cheb 7），远簇在 x=9/55 与 y=9 一带
    （簇心 cheb ≥ 23）。各簇内部点距 ≤ 8（一个分量）、跨簇 ≥ 14（不并簇）。
    """
    inner_gold = [_outer_res("gold", 24, 32, 1), _outer_res("gold", 26, 32, 1),
                  _outer_res("stone", 25, 34, 1)]
    out_stone = [_outer_res("stone", 8, 32, 2), _outer_res("stone", 10, 32, 2),
                 _outer_res("gold", 9, 34, 2)]
    out_wood = [_outer_res("wood", 54, 32, 2), _outer_res("wood", 56, 32, 2),
                _outer_res("gold", 55, 34, 2)]
    out_gold = [_outer_res("gold", 32, 8, 2), _outer_res("gold", 32, 10, 2),
                _outer_res("wood", 34, 9, 2)]
    out_gold2 = [_outer_res("gold", 8, 8, 2), _outer_res("gold", 10, 8, 2),
                 _outer_res("stone", 9, 10, 2)]

    def doc_with(clusters):
        doc = _big_doc(64, spawn_at=[32, 20])
        for cl in clusters:
            doc["resources"] += [dict(r) for r in cl]
        return doc

    doc = doc_with([inner_gold, out_stone, out_wood, out_gold])
    c.eq(validate.check_cluster_type_quota(doc, Grid(doc)), [],
         "金2/石1/木1 + 内环含金，不该报")

    # 配额破坏：金3/石1/木0，两两之差 3 > 1。
    doc = doc_with([inner_gold, out_gold, out_gold2, out_stone])
    probs = validate.check_cluster_type_quota(doc, Grid(doc))
    c.true(any("两两之差" in p for p in probs),
           f"三类簇数 3/1/0 必须报配额，实际 {probs}")

    # 并列众数：簇的类型定不出来，配额无从计起。
    tie = [_outer_res("gold", 44, 8, 2), _outer_res("gold", 46, 8, 2),
           _outer_res("stone", 45, 10, 2), _outer_res("stone", 47, 10, 2)]
    doc = doc_with([inner_gold, out_stone, out_wood, tie])
    probs = validate.check_cluster_type_quota(doc, Grid(doc))
    c.true(any("并列" in p for p in probs),
           f"簇内 2 金 2 石并列必须报，实际 {probs}")

    # 内环无金：买活的钱没有早解禁的来源（实力模型 §6.1）。
    inner_stone = [_outer_res("stone", 24, 32, 1), _outer_res("stone", 26, 32, 1),
                   _outer_res("wood", 25, 34, 1)]
    doc = doc_with([inner_stone, out_stone, out_wood, out_gold])
    probs = validate.check_cluster_type_quota(doc, Grid(doc))
    c.true(any("内环" in p for p in probs),
           f"内环簇不含金必须报，实际 {probs}")


def check_v31_passby(c):
    """第 31 条：每条 spawn→keep 最短路 `route_cluster_window` 格内至少一簇；
    簇的点到任一集结点 ≥ `cluster_spawn_distance_min`。fixture 档两键皆 -1
    （弃用，同 `static_vision_radius_max` 的手法）。

    keep (32,32)、集结点 (32,2)：最短路沿 x=32 直下。簇点 (30,20)：到路径
    2 格（≤ 6）、到集结点 18 格（≥ 16）——干净例。
    """
    strict = thresholds.load().profile("strict")
    fixture = thresholds.load().profile("fixture")

    doc = _big_doc(64)
    doc["resources"] += [_outer_res("gold", 30, 20, 1)]
    c.eq(validate.check_route_passby(doc, Grid(doc), strict), [],
         "路径 6 格内有簇、点到集结点 ≥ 16，不该报")

    # 顺路窗口内没有任何簇（点在 (10,20)，离路径 22 格）。
    doc = _big_doc(64)
    doc["resources"] += [_outer_res("gold", 10, 20, 1)]
    probs = validate.check_route_passby(doc, Grid(doc), strict)
    c.true(any("没有任何资源簇" in p for p in probs),
           f"路径窗口内没有簇必须报，实际 {probs}")

    # 簇落进集结区（(30,10) 到集结点 8 格 < 16；路径窗口那半条仍满足，
    # 两条各验一侧）。
    doc = _big_doc(64)
    doc["resources"] += [_outer_res("gold", 30, 10, 1)]
    probs = validate.check_route_passby(doc, Grid(doc), strict)
    c.true(any("集结区" in p for p in probs),
           f"簇点距集结点 8 格（< 16）必须报，实际 {probs}")

    # fixture 档：同一张违规图两半都弃用 ⇒ 自动通过。
    doc = _big_doc(64)
    doc["resources"] += [_outer_res("gold", 30, 10, 1)]
    c.eq(validate.check_route_passby(doc, Grid(doc), fixture), [],
         "fixture 档两个键都是 -1 = 两半都弃用，违规图也该过")


def check_route_features_report(c):
    """路线特征（§3.1 派生量，进报告不进地图文件）：键齐全、`path_len` 等于
    BFS 最短路长、沿途簇数按 6 格窗口数；地形三项 2026-09-02 第 3/4 步落地
    ——`_big_doc(64)` 全 Plain：最窄处 = 图宽 64、直线带无 Forest、不过桥。"""
    doc = _big_doc(64)
    doc["resources"] += [_outer_res("gold", 30, 20, 1)]
    feats = validate.route_features(doc, Grid(doc))
    c.eq(len(feats), 1, "一个集结点一条特征")
    f = feats[0]
    c.eq(set(f), {"spawn", "path_len", "clusters_near_path",
                  "min_width", "forest_cover", "crosses_bridge"},
         f"路线特征的键必须齐全，实际 {sorted(f)}")
    c.eq(f["path_len"], 30,
         f"(32,2) 到 (32,32) 的最短路应是 30 格，实际 {f['path_len']}")
    c.eq(f["clusters_near_path"], 1,
         "簇点 (30,20) 在路径 2 格内，应数到 1 簇")
    c.eq(f["min_width"], 64,
         f"全 Plain 图上最窄处应等于整条可通行段长（64），实际 {f['min_width']}")
    c.eq(f["forest_cover"], 0.0,
         f"直线带里一株 Forest 都没有，遮蔽应为 0.0，实际 {f['forest_cover']}")
    c.eq(f["crosses_bridge"], False, "全 Plain 图不过桥")


def _set_terrain(doc, cells, name):
    """把 doc 里若干格改成指定地形（第 26/27/30 条用例的造图砖）。"""
    idx = mapfile.TERRAIN_PALETTE.index(name)
    rows = [list(r) for r in doc["layers"]["terrain"]["rows"]]
    for x, y in cells:
        rows[y][x] = str(idx)
    doc["layers"]["terrain"]["rows"] = ["".join(r) for r in rows]
    mapfile.stamp_content_hash(doc)


def _ring64(r, keep=(32, 32)):
    """64×64 图上 cheb == r 的整环格（第 9/26/32 条用例的造图砖）。"""
    kx, ky = keep
    return [(x, y) for x in range(64) for y in range(64)
            if max(abs(x - kx), abs(y - ky)) == r]


def check_v9_defended_front(c):
    """第 9 条（2026-09-02 解阻塞，大改第 5 步）：正面 = 城圈周长 8R，R 从
    墙格到 keep 的最大切比雪夫距离反推——不再需要城区。

    keep (32,32)。半径 8 的整环 ⇒ 正面 64 ∈ [64, 80] 过；半径 5 ⇒ 40 < 64
    必报；无墙自动通过（没有要设防的正面，同第 8 条先例）。
    """
    th = thresholds.load().profile("strict")
    doc = _big_doc(64, walls_at=_ring64(8))
    c.eq(validate.check_defended_front(doc, Grid(doc), th), [],
         "半径 8 的整环：正面 64 应落在 [64, 80]")
    doc = _big_doc(64, walls_at=_ring64(5))
    probs = validate.check_defended_front(doc, Grid(doc), th)
    c.true(probs and "40" in probs[0],
           f"半径 5 的环正面只有 40 格（< 64）必须报，实际 {probs}")
    doc = _big_doc(64)
    c.eq(validate.check_defended_front(doc, Grid(doc), th), [],
         "没有墙就没有要设防的正面——自动通过")


def check_v26_wild_blocked_fraction(c):
    """第 26 条：争夺带（城圈外 2 格到集结点环）阻挡率 ∈ 区间。

    keep (32,32)、半径 8 整环、集结点 (32,2)（cheb 30）⇒ 带 = cheb ∈
    [10, 30]。带内 (x+y) % 6 铺 Forest ≈ 16.7% ∈ [10%, 20%] 过；一株不铺
    0%、% 2 铺 50% 都必报；无墙自动通过（同第 8/9 条先例）。
    """
    th = thresholds.load().profile("strict")

    def band_cells():
        return [(x, y) for x in range(64) for y in range(64)
                if 10 <= max(abs(x - 32), abs(y - 32)) <= 30]

    doc = _big_doc(64, walls_at=_ring64(8))
    _set_terrain(doc, [p for p in band_cells() if (p[0] + p[1]) % 6 == 0],
                 "Forest")
    c.eq(validate.check_wild_blocked_fraction(doc, Grid(doc), th), [],
         "带内 ≈16.7% 的 Forest 应落在 [10%, 20%]")
    doc = _big_doc(64, walls_at=_ring64(8))
    probs = validate.check_wild_blocked_fraction(doc, Grid(doc), th)
    c.true(probs and "阻挡率" in probs[0],
           f"一株不铺（0%）低于下界必须报，实际 {probs}")
    doc = _big_doc(64, walls_at=_ring64(8))
    _set_terrain(doc, [p for p in band_cells() if (p[0] + p[1]) % 2 == 0],
                 "Forest")
    probs = validate.check_wild_blocked_fraction(doc, Grid(doc), th)
    c.true(probs and "阻挡率" in probs[0],
           f"铺一半（50%）高于上界必须报，实际 {probs}")
    doc = _big_doc(64)
    c.eq(validate.check_wild_blocked_fraction(doc, Grid(doc), th), [],
         "无墙 ⇒ 没有城圈可言，自动通过")


def check_v27_route_width_classes(c):
    """第 27 条：最窄处 ∈ [2,4] = 隘口、更宽 = 开阔，全图两类都要有。

    keep (32,32)。Rock 墙段 y=20、x∈[0,40] 留 x∈[30,33] 的四格口：
    集结点 (32,2) 的最短路穿口 ⇒ 隘口；集结点 (2,32) 的路线不碰墙段
    ⇒ 开阔——两类齐，过。口收到 1 格 ⇒ 「1 格缝」必报；全开阔图必红
    「两类都要有」；fixture 档 [-1,-1] 弃用 ⇒ 恒过。
    """
    th = thresholds.load().profile("strict")
    fixture = thresholds.load().profile("fixture")
    spawns2 = [{"id": 0, "pos": [32, 2]}, {"id": 1, "pos": [2, 32]}]

    def make(gap):
        doc = _big_doc(64)
        doc["spawns"] = [dict(s) for s in spawns2]
        mapfile.stamp_content_hash(doc)
        _set_terrain(doc, [(x, 20) for x in range(41) if x not in gap], "Rock")
        return doc

    doc = make(set(range(30, 34)))
    c.eq(validate.check_route_width_classes(doc, Grid(doc), th), [],
         "一隘口（口宽 4）一开阔，两类齐，不该报")
    probs = validate.check_route_width_classes(make({32}), Grid(make({32})), th)
    c.true(any("缝" in p for p in probs),
           f"口宽 1 是「1 格缝」不是隘口，必须报，实际 {probs}")
    doc = _big_doc(64)
    doc["spawns"] = [dict(s) for s in spawns2]
    mapfile.stamp_content_hash(doc)
    probs = validate.check_route_width_classes(doc, Grid(doc), th)
    c.true(any("两类都要有" in p for p in probs),
           f"全开阔图没有隘口路线，必须报，实际 {probs}")
    doc = make(set(range(30, 34)))
    c.eq(validate.check_route_width_classes(doc, Grid(doc), fixture), [],
         "fixture 档 [-1,-1] = 弃用，隘口图也自动通过")


def check_v30_river_bridges(c):
    """第 30 条：桥宽 ≥ 2；不踩桥就走不到 keep 的集结点 ≤ 1。

    keep (32,32)，河 = x=20 整列 Water。桥 (20,10)+(20,11)（流向轴 y、跨度
    2）⇒ 过；只留一格 ⇒ 桥宽 1 必报。无桥时：左侧 1 个集结点被切 = 上限
    1，过；左侧 2 个 ⇒ 必报。fixture 档（0 / -1）两半都弃用 ⇒ 恒过。
    """
    th = thresholds.load().profile("strict")
    fixture = thresholds.load().profile("fixture")

    def river_doc(spawns, bridge_cells):
        doc = _big_doc(64)
        doc["spawns"] = [dict(s) for s in spawns]
        mapfile.stamp_content_hash(doc)
        _set_terrain(doc, [(20, y) for y in range(64)], "Water")
        _set_terrain(doc, bridge_cells, "Bridge")
        return doc

    s1 = [{"id": 0, "pos": [2, 32]}]
    doc = river_doc(s1, [(20, 10), (20, 11)])
    c.eq(validate.check_river_bridges(doc, Grid(doc), th), [],
         "桥宽 2 且没有集结点被切（有桥可踩），不该报")
    doc = river_doc(s1, [(20, 10)])
    probs = validate.check_river_bridges(doc, Grid(doc), th)
    c.true(any("宽" in p for p in probs),
           f"桥宽 1（< 2）必须报，实际 {probs}")
    doc = river_doc(s1, [])
    c.eq(validate.check_river_bridges(doc, Grid(doc), th), [],
         "无桥时左侧 1 个集结点被切 = 上限 1，不该报")
    doc = river_doc(s1 + [{"id": 1, "pos": [2, 40]}], [])
    probs = validate.check_river_bridges(doc, Grid(doc), th)
    c.true(any("不踩桥" in p for p in probs),
           f"2 个集结点被切（> 1）必须报，实际 {probs}")
    doc = river_doc(s1 + [{"id": 1, "pos": [2, 40]}], [(20, 10)])
    c.eq(validate.check_river_bridges(doc, Grid(doc), fixture), [],
         "fixture 档 bridge_width_min 0 / cut -1 = 两半都弃用，恒过")


def check_v32_entrance_faces(c):
    """第 32 条：门与缺口不同面、≥ 3 个面有入口；非整环实体图跳过。

    keep (32,32)、半径 8 整环：门北 (32,24) + 门南 (32,40)、缺口东 (40,30)
    ⇒ 过；缺口挪进门面 (30,24) ⇒ 必报；无缺口 ⇒ 只 2 面有入口必报；
    散墙（环上墙格 < 8r−2）⇒ 不是整环实体图，自动通过。
    """
    def ring_doc(gates, breaches, r=8):
        doc = _big_doc(64)
        doc["initial_walls"] = [
            {"kind": "Gate" if p in gates else "Wall",
             "pos": list(p), "hp_frac": 1.0}
            for p in _ring64(r) if p not in breaches]
        mapfile.stamp_content_hash(doc)
        return doc

    doc = ring_doc({(32, 24), (32, 40)}, {(40, 30)})
    c.eq(validate.check_entrance_faces(doc, Grid(doc)), [],
         "门南北 + 缺口东：门面与缺口面不相交、3 面有入口，不该报")
    doc = ring_doc({(32, 24), (32, 40)}, {(30, 24)})
    probs = validate.check_entrance_faces(doc, Grid(doc))
    c.true(any("同一个面" in p for p in probs),
           f"缺口落进门面必须报，实际 {probs}")
    doc = ring_doc({(32, 24), (32, 40)}, set())
    probs = validate.check_entrance_faces(doc, Grid(doc))
    c.true(any("有入口" in p for p in probs),
           f"只 2 个面有入口（< 3）必须报，实际 {probs}")
    doc = _big_doc(64, walls_at=[(10, 10), (11, 10), (12, 10),
                                 (20, 20), (21, 20), (40, 5)])
    c.eq(validate.check_entrance_faces(doc, Grid(doc)), [],
         "散墙不是整环实体图（环上墙格 < 8r−2），本条无适用对象")


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

    **这一组同时钉住这条为什么必须存在**：第 6 条只查 `inner`，所以少了
    本条，一张把所有资源都放在城里的地图能通过全部检查。下面第二段就是
    那张图。**这条要求现在是「部分资源点在墙外」唯一的守门人**——原先还有
    第 7 条的「不可围墙」做后盾，那条已随 `地图与场景设计.md` 2.1 的订正
    废除（2026-09-01），本条因此更不能删。
    """
    inner_only = [{"type": t, "pos": [i, 0], "tier": "inner"}
                  for i, t in enumerate(["stone", "wood", "gold"])]
    doc, g = _gv(OBLONG, keep=[3, 3], resources=inner_only)
    c.true(validate.check_has_outer_resource(doc, g),
           "一个 outer 都没有应当报")
    # 那张图确实能过第 6 条 —— 这才是本条存在的理由。
    c.true(not validate.check_inner_resources(doc, g),
           "前提：全在城里的图过得了第 6 条")

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
    c.eq((n_impl, n_block, n_pend), (28, 0, 0),
         "条目状态计数变了：改动状态时要同步这条断言与 README 的进度表"
         "（2026-09-01：第 7 条废除，已实现数 22 → 21；2026-09-02：大改第 2 步"
         "新增第 28/29/31 条已实现 + 第 26/27/30/32 条阻塞占位 ⇒ 21+3 已实现、"
         "1+4 阻塞；同日第 3/4/5 步：26/27/30/32 落地 + 第 9 条解阻塞"
         " ⇒ 24+5 已实现、0 阻塞；同日第 6 步：第 18 条废除并入第 28 条"
         " ⇒ 28 已实现）")


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

    # 56 格参考图（2026-09-02 大改第 6 步新增的 `reference` 档）走同一类对照：
    # 自己的档必须全过；strict 必须红，且**红在哪几条也是钉住的**——
    # 第 2 条（D=18 ∉ [36,44]）、第 27 条（一格厚的内环带放不下岩脊隘口）、
    # 第 31 条后半（簇到集结点 16 格在 56 格上客观不可达）。红的集合变了
    # 说明几何前提变了，那时该回来重看这一档，而不是悄悄换一组红。
    ref_path = os.path.join(_REPO, "game", "data", "maps",
                            "reference_border_keep_01.json")
    if os.path.exists(ref_path):
        real = mapfile.load(ref_path)
        r_strict = {chk.no for chk, ps in
                    validate.run(real, th=all_th.profile("strict")) if ps}
        r_ref = {chk.no for chk, ps in
                 validate.run(real, th=all_th.profile("reference")) if ps}
        c.eq(r_strict, {2, 27, 31},
             f"参考图在 strict 下必须红、且只红在几何上客观不可达的"
             f"{{2, 27, 31}}，实际红了：{sorted(r_strict)}")
        c.true(not r_ref,
               f"它在 reference 档下必须全过（ctest 就是这么跑的），"
               f"实际红了：{sorted(r_ref)}")


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

    # **推导出的 size 不在校验器的 size 区间内。**
    # 2026-09-02 起 size 由四环公式推导（不再是配置键）；把 strict 的
    # size_max 收到推导值以下，加载时必须红。不拦的话生成器会产出一批
    # 第 1 条必然否决的图，而症状（丢弃率 100%）指不出根因是配置互相矛盾。
    bad = _json.loads(_json.dumps(base))
    bad["profiles"]["strict"]["size_max"] = 100
    reject(bad, "推导出的 size 越出 strict 的 size 区间")

    # 集结点上界：2026-08-31 时是「不得超过方环四条边」（4），2026-09-02 大改
    # 第 1 步改角度采样后变成「不得超过 5」（相邻角差 ≥ 60°，6 个只剩恰好等分）。
    bad = _json.loads(_json.dumps(base))
    bad["generator"]["spawn_count_range"] = [6, 6]
    reject(bad, "spawn_count_range 上界超过 5（60° 间隔的结构上限）")

    # inner 三种缺一（第 6 条要求各 ≥ 1）。
    bad = _json.loads(_json.dumps(base))
    bad["generator"]["inner_resources_range"] = {"stone": [1, 1], "wood": [1, 1],
                                                  "gold": [0, 0]}
    reject(bad, "inner_resources_range 的 gold 下界为 0")

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
        "buildings": [],
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


def check_v2_spawn_count_and_wall_distance(c):
    """第 2 条：数量取**区间**，且每个集结点到最近墙格的距离 ∈
    `spawn_wall_distance_range`（2026-09-02 大改第 1 步，原判据是「贴边」）。"""
    th = thresholds.load().profile("strict")
    size = th.size_min
    d_lo, d_hi = th.spawn_wall_distance_range

    # 数量：区间而不是等于某个数。集结点数量是待定数值，而地图规范曾把它
    # 写成「定为 4」——那是把平衡旋钮当成结构结论。
    doc = _big_doc(size)
    doc["spawns"] = [{"id": i, "pos": [2 + i * 3, 2]}
                     for i in range(th.spawn_count_min)]
    mapfile.stamp_content_hash(doc)
    c.true(not validate.check_spawn_count_and_wall_distance(doc, Grid(doc), th),
           f"{len(doc['spawns'])} 个集结点应当落在 "
           f"[{th.spawn_count_min}, {th.spawn_count_max}] 内"
           f"（没有墙时距离半条自动通过，同第 5/10 条的先例）")

    doc = _big_doc(size)
    doc["spawns"] = [{"id": 0, "pos": [2, 2]}]
    mapfile.stamp_content_hash(doc)
    if th.spawn_count_min > 1:
        c.true(validate.check_spawn_count_and_wall_distance(doc, Grid(doc), th),
               "少于下界必须报")

    # 距离半条：造一排 y = cx−10 的墙，三个集结点放在墙外 d_mid 格（应当过）；
    # 中间那个挪到墙外 10 格（低于 D_lo，必须报）。**不再查「到地图边界的
    # 距离」**——集结点环上的点可以离边界很远，那是设计（§3.1）。
    # 图要比 size_min 大：d_mid=40 的纵深在 56×56 上摆不下。
    size = 128
    cx = size // 2
    ring = [{"kind": "Wall", "pos": [cx - 10 + i, cx - 10], "hp_frac": 1.0}
            for i in range(21)]
    d_mid = (int(d_lo) + int(d_hi)) // 2
    ring_at = [(w["pos"][0], w["pos"][1]) for w in ring]

    doc = _big_doc(size, walls_at=ring_at)
    doc["spawns"] = [{"id": i, "pos": [cx + dx, cx - 10 - d_mid]}
                     for i, dx in enumerate((-20, 0, 20))]
    mapfile.stamp_content_hash(doc)
    c.true(not validate.check_spawn_count_and_wall_distance(doc, Grid(doc), th),
           f"集结点到最近墙格恰好 {d_mid} 格，应落在 [{d_lo}, {d_hi}] 内")

    doc = _big_doc(size, walls_at=ring_at)
    doc["spawns"] = [{"id": 0, "pos": [cx - 20, cx - 10 - d_mid]},
                     {"id": 1, "pos": [cx, cx - 10 - 10]},
                     {"id": 2, "pos": [cx + 20, cx - 10 - d_mid]}]
    mapfile.stamp_content_hash(doc)
    probs = validate.check_spawn_count_and_wall_distance(doc, Grid(doc), th)
    c.true(any("墙" in p for p in probs),
           "集结点离墙太近必须报，且报错要点出「墙」——贴边判据已作废，"
           "报「边界」说明还在用旧判据")


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


def check_v23_forest_cohesion(c):
    """第 23 条：`Forest` 4 连通块不得碎成粉尘。

    它守的是森林的**职责**（§2.2 看不清来了什么 / 把杀伤区压成薄带），
    而两项都依赖连片——同样的总面积，撒成散点与聚成几片完全不同。
    """
    th = thresholds.load().profile("strict")
    thr = int(th.forest_min_component_cells)
    size = th.size_min

    def with_forest(cells):
        doc = _big_doc(size)
        pal = doc["layers"]["terrain"]["palette"]
        fi = str(pal.index("Forest"))
        rows = [list(r) for r in doc["layers"]["terrain"]["rows"]]
        for x, y in cells:
            rows[y][x] = fi
        doc["layers"]["terrain"]["rows"] = ["".join(r) for r in rows]
        mapfile.stamp_content_hash(doc)
        return doc

    # 一块够大的方形 ⇒ 通过。
    big = [(20 + dx, 20 + dy) for dx in range(4) for dy in range(4)]
    doc = with_forest(big)
    c.true(not validate.check_forest_cohesion(doc, Grid(doc), th),
           f"{len(big)} 格的一整片应当通过（阈值 {thr}）")

    # 一格孤立的粉尘 ⇒ 必须报。**这就是本条存在的理由。**
    doc = with_forest(big + [(40, 40)])
    probs = validate.check_forest_cohesion(doc, Grid(doc), th)
    c.true(probs, "一格孤立的 Forest 必须报——粉尘不承担森林的任何职责，"
                  "却照样计入总面积")

    # **1 格宽的长条必须通过。** 城外散布的森林小簇可以窄到 1 格宽的连接处，
    # 所以判据只能看「块格数」，不能看「块厚度」——写成厚度会把合法的形态判红。
    band = [(30, 10 + i) for i in range(max(thr, 3) + 2)]
    doc = with_forest(band)
    c.true(not validate.check_forest_cohesion(doc, Grid(doc), th),
           f"1 格宽、{len(band)} 格长的森林必须通过（判据允许 1 格宽）")

    # 只靠斜向相连的两格：4 连通下算两块 ⇒ 两块都不够 ⇒ 必须报。
    # 取 4 连通落在保守那一侧（切得更碎 ⇒ 报得更多）。
    doc = with_forest([(50, 50), (51, 51)])
    c.true(validate.check_forest_cohesion(doc, Grid(doc), th),
           "只斜向相连的两格在 4 连通下是两块，都不够，必须报")

    # 阈值 ≤1 = 本档弃用本条（`fixture` 靠它）。
    import json as _json
    raw = _json.loads(open(thresholds.DEFAULT_PATH, encoding="utf-8").read())
    off = _json.loads(_json.dumps(raw["profiles"]["strict"]))
    off["forest_min_component_cells"] = 1
    th_off = thresholds.Profile("strict", off)
    doc = with_forest(big + [(40, 40)])
    c.true(not validate.check_forest_cohesion(doc, Grid(doc), th_off),
           "阈值为 1 时本条应当恒真（fixture 那一档靠它）")


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

    # —— 2026-08-31 重构后的 doc 层结构断言（组长拍板的设计语言）——
    # 这些不变量没有任何第 8 节条目在查（校验器无从得知设计半径 R），
    # 所以钉在这里——`check_generator_wall_seals_ring` 那条先例的延续：
    # 结构性质只靠看产出才能发现，而「看产出」被做成断言就不会漏。
    names = generate._terrain_names(doc)
    counts = {}
    for row in names:
        for cell in row:
            counts[cell] = counts.get(cell, 0) + 1
    c.true(counts.get("Forest", 0) > 0, "生成的图必须有 Forest（城内森林 + 城外散布）")
    c.true(counts.get("Rock", 0) > 0, "生成的图必须有 Rock（城外散布团块）")
    # 2026-09-01：留白 1 作废，Water/Bridge 真的进生成图。
    c.true(counts.get("Water", 0) > 0, "生成的图必须有 Water（湖/河，留白 1 已作废）")
    # **Bridge 不能在单张图上钉「必须有」**：§3.4 的河是「落地后逐条复核、
    # 超标整河撤销」——河被撤掉的图只剩湖、没有桥，而它完全合法（2026-09-02
    # 第 18 条废除后，种子基 1 的受理顺位后移，接到的 gen_00001005 正是这种：
    # Water 51 格、Bridge 0）。「河必架桥」的恒有性由 Canvas 层那条
    # （rivers_range=[1,1] ⇒ 有桥）钉住；这里钉的是**桥流水线没有静默失效**——
    # 换一个种子基还没有桥才红（通常零额外成本：第一张图多半带河）。
    has_bridge = counts.get("Bridge", 0) > 0
    if not has_bridge:
        doc2, _ = generate.generate_valid(2, cfg, th)
        has_bridge = any(
            cell == "Bridge" for row in generate._terrain_names(doc2)
            for cell in row)
    c.true(has_bridge,
           "两个种子基接到的图都没有 Bridge —— 河/桥流水线疑似静默失效"
           "（单张图没有 Bridge 是合法的：河可能被整河撤销，§3.4）")

    for s in doc["spawns"]:
        c.true("corridor" not in s,
               f"spawns 不得再携带 corridor 字段（已废除），实际有 {s!r}")

    # 城圈完整性：每一段墙/门都恰在 cheb == R 的环上，数量 = 8R − 2 门 − 缺口。
    # 这个 doc 层的可断言性正是「城圈改城墙实体」带来的——旧 Rock 环时代
    # 只能 Canvas 直测（见已删除的 check_generator_wall_seals_ring）。
    kx, ky = doc["keep"]
    # 环半径从墙的坐标反推（doc 里没有设计半径字段）：环上必有墙，
    # 取墙格到 keep 的最大切比雪夫距离。
    wall_pos = [tuple(w["pos"]) for w in doc["initial_walls"]]
    r_ring = max(max(abs(x - kx), abs(y - ky)) for x, y in wall_pos)
    c.true(r_ring >= 8, f"环半径反推为 {r_ring}，太小，不像城圈")
    gates = [p for w in doc["initial_walls"] if w["kind"] == "Gate"
             for p in [tuple(w["pos"])]]
    c.eq(len(gates), 2, "恰好两座城门（一对相对方向）")
    c.true(gates[0][0] == gates[1][0] or gates[0][1] == gates[1][1],
           f"两座城门必须落在同一行或同一列（相对方向），实际 {gates}")
    c.true(abs(gates[0][0] - gates[1][0]) + abs(gates[0][1] - gates[1][1])
           == 2 * r_ring,
           f"两座城门必须恰为环上一对相对格，实际 {gates}（环半径 {r_ring}）")
    on_ring = [p for p in wall_pos if max(abs(p[0] - kx), abs(p[1] - ky)) == r_ring]
    c.eq(len(on_ring), len(wall_pos),
         f"每一段墙/门都必须落在 cheb == {r_ring} 的环上，"
         f"实际 {len(wall_pos) - len(on_ring)} 段不在")
    n_breach = 8 * r_ring - len(wall_pos)
    c.true(1 <= n_breach <= 2,
           f"缺口数 = 8R − 墙门总数 = {n_breach}，"
           f"应当落在 initial_breaches [1,2] 内")
    # 墙格的 terrain 必须是 Plain（墙是实体、不改地形——第 17 条的互补一半，
    # 也防「城门/缺口被地形盖住」那类糊墙回归）。
    for x, y in wall_pos:
        c.eq(names[y][x], "Plain",
             f"墙/门格 {(x, y)} 的 terrain 应是 Plain，实际 {names[y][x]}")
    # 城门恒满血（结构薄弱点由「木质、破坏速率更高」承担，残破留给 Wall）。
    for w in doc["initial_walls"]:
        if w["kind"] == "Gate":
            c.eq(w["hp_frac"], 1.0, f"城门应恒满血，实际 {w['hp_frac']}")

    # 城外金矿 ≥ 2（thresholds 里 outer_gold_min 同值——生成器侧的结构保证
    # 2026-09-02 起由主类型金→石→木轮转承担（首簇恒金），这里钉 doc 层）。
    outer_gold = sum(1 for r in doc["resources"]
                     if r["tier"] == "outer" and r["type"] == "gold")
    c.true(outer_gold >= 2, f"城外金矿 {outer_gold} 个，必须 ≥ 2（2026-08-31 试玩查出）")
    # 城内构成：恰好 1 石 + 1 金 + 1 木（2026-09-01 从「2 石 2 金 1–2 木」收紧，
    # 试玩反馈「城墙内资源点太多」）。
    inner = sorted(r["type"] for r in doc["resources"] if r["tier"] == "inner")
    c.eq(inner.count("gold"), 1, f"城内金矿必须恰好 1，实际 inner 构成 {inner}")
    c.eq(inner.count("stone"), 1, f"城内石矿必须恰好 1，实际 inner 构成 {inner}")
    c.eq(inner.count("wood"), 1, f"城内木材必须恰好 1，实际 inner 构成 {inner}")

    # 2026-09-01：金矿场恒一座、精确踩城内金点（「金矿不刷新」试玩反馈的落点）。
    gold_inner = sorted(tuple(r["pos"]) for r in doc["resources"]
                        if r["tier"] == "inner" and r["type"] == "gold")
    mines = sorted(tuple(b["pos"]) for b in doc["buildings"] if b["type"] == "Mine")
    c.eq(len(mines), 1, f"预置金矿场必须恰一座，实际 {mines}")
    c.true(mines[0] in gold_inner,
           f"金矿场 {mines[0]} 必须精确踩在城内金点 {gold_inner} 上（第 24 条豁免）")

    # —— 2026-09-02 大改第 1/2 步的 doc 层断言（§3.7 要求）——
    # 集结点环：数量区间、相邻角差 ≥ 60°（含环绕）、到最近墙格 ∈ strict 档
    # 距离区间。校验器第 2 条查数量与距离，角差是生成器侧承诺（§3.1），
    # 没有第 8 节条目在查，钉在这里。
    n_spawn = len(doc["spawns"])
    c.true(th.spawn_count_min <= n_spawn <= th.spawn_count_max,
           f"集结点 {n_spawn} 个，应落在 "
           f"[{th.spawn_count_min}, {th.spawn_count_max}]")
    angs = sorted(math.degrees(math.atan2(s["pos"][1] - ky, s["pos"][0] - kx))
                  % 360.0 for s in doc["spawns"])
    gaps = [b - a for a, b in zip(angs, angs[1:])] + [angs[0] + 360.0 - angs[-1]]
    c.true(min(gaps) >= 60.0 - 1e-9,
           f"相邻集结点角差 {[round(g, 1) for g in sorted(gaps)]} 必须 ≥ 60°")
    d_lo, d_hi = th.spawn_wall_distance_range
    for s in doc["spawns"]:
        d_near = min(max(abs(s["pos"][0] - x), abs(s["pos"][1] - y))
                     for x, y in wall_pos)
        c.true(d_lo <= d_near <= d_hi,
               f"集结点 {s['pos']} 到最近墙格 {d_near} 格，应落在 "
               f"[{d_lo}, {d_hi}]（第 2 条判据）")

    # 簇：同簇同波、簇解禁序按簇心距离单调、三类配额两两差 ≤ 1、内环含金。
    # 校验器第 28/29 条已在 run() 里查过同一张图，这里钉「生成器产出恒如此」
    # 这层（§3.7）——run() 红是单张图的事，这里红是生成器承诺破了。
    clusters = validate._outer_clusters(doc)
    for cl in clusters:
        waves = {m["unlock_wave"] for m in cl["members"]}
        c.eq(len(waves), 1,
             f"簇（簇心 {list(cl['center'])}）必须同波解禁，实际 {sorted(waves)}")
    ordered = sorted(clusters,
                     key=lambda cl: (max(abs(cl["center"][0] - kx),
                                         abs(cl["center"][1] - ky)),
                                     # tie-break 与校验器第 28 条/生成器逐字同构
                                     # （簇心坐标）——等距两簇的波号顺序三处必须
                                     # 是同一个，否则各自「单调」合起来互相红。
                                     tuple(cl["center"])))
    cl_waves = [min(m["unlock_wave"] for m in cl["members"]) for cl in ordered]
    c.eq(cl_waves, sorted(cl_waves),
         f"簇解禁波按簇心距离必须非降，实际 {cl_waves}")
    primaries = {}
    for cl in clusters:
        tally = {}
        for m in cl["members"]:
            tally[m["type"]] = tally.get(m["type"], 0) + 1
        top = max(tally.values())
        winners = [t for t, v in tally.items() if v == top]
        c.eq(len(winners), 1,
             f"簇（簇心 {list(cl['center'])}）必须有唯一众数，实际 {tally}")
        primaries[winners[0]] = primaries.get(winners[0], 0) + 1
    qt = [primaries.get(t, 0) for t in ("gold", "stone", "wood")]
    c.true(max(qt) - min(qt) <= 1,
           f"三类簇数 {qt} 两两之差必须 ≤ 1（金→石→木轮转）")
    spawn_ring_d = min(max(abs(s["pos"][0] - kx), abs(s["pos"][1] - ky))
                       for s in doc["spawns"])
    inner_cls = [cl for cl in clusters
                 if max(abs(cl["center"][0] - kx), abs(cl["center"][1] - ky))
                 < spawn_ring_d]
    c.true(any(m["type"] == "gold" for cl in inner_cls for m in cl["members"]),
           "内环（集结点环内侧）必须至少一簇含金（实力模型 §6.1）")

    # —— 2026-09-02 大改第 3/4/5 步的 doc 层断言（§3.7 要求）——
    # 校验器第 26/27/30/32 条已在 run() 里查过同一张图；这里钉的是「生成器
    # 产出恒如此」这层承诺——run() 红是单张图的事，这里红是生成器策略破了。
    grid = Grid(doc)

    def cheb(p):
        return max(abs(p[0] - kx), abs(p[1] - ky))

    # 争夺带阻挡率 ∈ strict 区间（第 26 条的生成器侧）。
    outer_d = min(cheb(tuple(s["pos"])) for s in doc["spawns"])
    band = [(x, y) for y in range(doc["size"][1]) for x in range(doc["size"][0])
            if r_ring + 2 <= cheb((x, y)) <= outer_d]
    frac = sum(1 for x, y in band
               if names[y][x] in ("Forest", "Rock", "Water")) / len(band)
    c.true(th.wild_blocked_fraction_min <= frac <= th.wild_blocked_fraction_max,
           f"争夺带阻挡率 {frac:.1%} 必须落在 "
           f"[{th.wild_blocked_fraction_min}, {th.wild_blocked_fraction_max}]")

    # 至少一条隘口路线（最窄处 ∈ route_width_range）——「哪条路窄」是地图的
    # 一部分（第 27 条的生成器侧；开阔那一类由校验器查，这里钉隘口存在）。
    feats = validate.route_features(doc, grid)
    wlo, whi = th.route_width_range
    c.true(any(f["min_width"] is not None and wlo <= f["min_width"] <= whi
               for f in feats),
           f"必须至少有一条隘口路线（最窄处 ∈ [{wlo}, {whi}]），实际 "
           f"{[f['min_width'] for f in feats]}")

    # 缺口必须放在没有门的面上（第 32 条的生成器侧，§3.5 决定项 6）。
    def faces_of(p):
        faces = set()
        if p[1] == ky - r_ring:
            faces.add("north")
        if p[1] == ky + r_ring:
            faces.add("south")
        if p[0] == kx - r_ring:
            faces.add("west")
        if p[0] == kx + r_ring:
            faces.add("east")
        return faces
    gate_faces = {f for gpos in gates for f in faces_of(gpos)}
    wall_set = set(wall_pos)
    breach_faces = {f for x in range(doc["size"][0])
                    for y in range(doc["size"][1])
                    if cheb((x, y)) == r_ring and (x, y) not in wall_set
                    for f in faces_of((x, y))}
    c.true(not (gate_faces & breach_faces),
           f"缺口面 {sorted(breach_faces)} 与门面 {sorted(gate_faces)} 必须不相交")
    c.true(len(gate_faces | breach_faces) >= 3,
           f"有入口的面必须 ≥ 3，实际 {sorted(gate_faces | breach_faces)}")

    # 桥宽 2 的结构签名：每个桥格都有正交桥邻（第 30 条的生成器侧）。
    bset = {(x, y) for y in range(doc["size"][1]) for x in range(doc["size"][0])
            if names[y][x] == "Bridge"}
    for bx, by in bset:
        c.true(any((bx + ox, by + oy) in bset
                   for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1))),
               f"桥 {(bx, by)} 没有正交桥邻——桥宽必须 ≥ 2（§3.4）")


def _ref_cfg(**over):
    """一组全钉死的生成配置，给直测生成器中间产物的用例用。区间全取单值
    ⇒ `_resolve` 抽出来就是这个数，rng 消费序列因此可预测。

    **这不是「参考图同款形状」**（本文档曾这么写，已订正）——具体数值是这批
    用例自己钉死的固定夹具，独立于 `build_reference_map.py` 的 `CFG` 与
    `thresholds.json` 的 `generator` 段；三处各自可以改而互不牵连。改这里
    的默认值前先看下面用它的用例是否硬编码了依赖这些具体数字的断言
    （`inner.count("stone") == 2` 这类）。

    **2026-09-02 大改第 1/2 步**：`size` 174 = 四环公式（2×(12+40+14+18+3)）
    ——旧值 72 装不下集结点环（R+D = 52 > 36）；`outer_clusters_range`/
    `outer_cluster_span` 换成 `inner_band_clusters`/`outer_band_clusters`/
    `outer_band_width`/`cluster_unlock_per_wave`；`spawn_wall_distance` 是
    生成器放置用的 D（真实配置里它从 strict 档区间中点推导，见
    `thresholds.Generator`）。"""
    base = SimpleNamespace(
        size=174, city_radius_range=[12, 12],
        spawn_count_range=[3, 3],
        spawn_wall_distance=40,
        inner_resources_range={"stone": [2, 2], "wood": [1, 2], "gold": [2, 2]},
        inner_band_clusters=[4, 4],
        outer_band_clusters=[4, 4],
        outer_band_width=18,
        outer_cluster_size=[3, 4],
        cluster_unlock_per_wave=1,
        initial_breaches=[1, 1],
        wall_hp_frac_range=[0.5, 0.5],
        forest_patches=[12, 12],
        forest_patch_size=[3, 5],
        rock_patches_range=[6, 6],
        rock_patch_size=[3, 6],
        # 2026-09-02 第 3 步（§3.2 三形态）：林子与岩脊。scatter 直接读它们，
        # 缺一个属性 paint 就 AttributeError。
        woods_range=[3, 3],
        woods_size=[14, 20],
        ridges_range=[2, 2],
        ridge_length=[8, 10],
        gap_width_range=[2, 3],
        water_lakes_range=[0, 0],
        water_lake_size=[0, 0],
        rivers_range=[0, 0],
        towers_range=[2, 2],
        barracks_range=[1, 1],
        obstacles=[0, 0],
        max_attempts=1,
    )
    for k, v in over.items():
        setattr(base, k, v)
    return base


def _painted(cfg, seed=1012000):
    """跑一遍 `paint`，返回 (cv, resolved_cfg)。用真实 strict 档（place_spawns
    要读它），所以 no_build 环是真实宽度。"""
    th = thresholds.load().profile("strict")
    cv = generate.Canvas(cfg.size)
    rng = random.Random(seed)
    r = generate.paint(cv, cfg, th, rng)
    return cv, r


def check_generator_scatter_avoids(c):
    """`scatter_wild_terrain` 不得把 Forest/Rock 撒到墙/门与集结点的邻域。

    这是 2026-08-31 试玩那两个真 bug 的守卫在重构后的形态：旧的
    `check_generator_wall_seals_ring`（走廊口两侧免费缺口）随 `carve_corridor`
    一起删除、其职责由 doc 层的城圈完整性断言取代（见
    `check_generator_produces_valid_maps`）；旧的
    `check_generator_forest_avoids_mouth`（森林糊住墙/门）改写为本条——
    城外散布沿用同一条纪律：墙/门一格缓冲、集结点八邻不放。
    """
    cv, _ = _painted(_ref_cfg(rock_patches_range=[5, 5], forest_patches=[8, 8]))
    walls = {(x, y) for _, (x, y), _ in cv.walls}
    spawns = set(cv.spawns)

    def zone(points, r):
        out = set()
        for px, py in points:
            for oy in range(-r, r + 1):
                for ox in range(-r, r + 1):
                    out.add((px + ox, py + oy))
        return out

    forbidden = zone(walls, 1) | zone(spawns, 1)
    for y in range(cv.size):
        for x in range(cv.size):
            if cv.at(x, y) in ("Forest", "Rock") and (x, y) in forbidden:
                c.failures.append(
                    f"[生成器] 散布地形压到了 {(x, y)}（{cv.at(x, y)}）——"
                    f"墙/门一格缓冲与集结点八邻必须干净")
                c.count += 1
                return
    c.count += 1
    # 环内侧（cheb < R）不该有散布的地形——城内内容只有两片 L 森林，
    # 野外散布的域是 cheb ≥ R+2（scatter_wild_terrain 的 valid() 明写）。
    kx, ky = cv.keep
    for y in range(cv.size):
        for x in range(cv.size):
            if cv.at(x, y) == "Rock" and max(abs(x - kx), abs(y - ky)) < 12:
                c.failures.append(
                    f"[生成器] Rock 团块落进了环内 {(x, y)}——散布域不该进城")
                c.count += 1
                return
    c.count += 1


def check_generator_cluster_invariants(c):
    """`place_outer_clusters` 的硬保证（2026-09-02 大改第 2 步，§3.3）：
    环带归属、顺路簇钉在集结点方向、外环簇避开 ±20° 扇区、主类型按
    金→石→木轮转且为簇内唯一众数、内环含金、四类间距、簇心取整与
    校验器（`grid.cluster_center`）逐格一致。

    簇级断言直接吃 `place_outer_clusters` 的**返回值**（簇记录：band /
    primary / points / center）——这是为可测性留的接口，`paint` 消费方
    忽略它。完整校验由 `check_generator_produces_valid_maps` 走 validate
    覆盖。固定种子 424242：掉簇（整簇放不下）会让轮转序列断在这里，
    那正是校验器第 29 条会拒图的情形，值得响。
    """
    cfg = _ref_cfg()
    th = thresholds.load().profile("strict")
    cv = generate.Canvas(cfg.size)
    rng = random.Random(424242)
    r = generate._resolve(cfg, rng)
    generate.place_ring_walls(cv, r, rng)
    generate.place_spawns(cv, r, th, rng)
    generate.place_inner_content(cv, r, rng)
    clusters = generate.place_outer_clusters(cv, r, th, rng)

    n_inner = sum(1 for cl in clusters if cl["band"] == "inner")
    n_outer = sum(1 for cl in clusters if cl["band"] == "outer")
    c.true(n_inner >= r.spawn_count,
           f"内环簇 {n_inner} 个，应 ≥ 集结点数 {r.spawn_count}（每集结点一顺路簇）")
    c.true(n_outer >= n_inner,
           f"外环簇 {n_outer} 应 ≥ 内环簇 {n_inner}（_resolve 抽样后抬齐）")

    # 环带归属。簇心是落点均值（在名义中心半径 4 内偏移），断言留 4 格余量。
    kx, ky = cv.keep
    R, D = r.city_radius, r.spawn_wall_distance
    for cl in clusters:
        rho = max(abs(cl["center"][0] - kx), abs(cl["center"][1] - ky))
        if cl["band"] == "inner":
            c.true(R + 4 - 4 <= rho <= R + D - 14 + 4,
                   f"内环簇心距 {rho} 应在 [R+4, R+D-14]（±4 均值余量）")
        else:
            c.true(R + D + 14 - 4 <= rho <= cv.size // 2 - 3 + 4,
                   f"外环簇心距 {rho} 应在 [R+D+14, size//2-3]（±4 均值余量）")

    def gap(a, b):
        d = abs(a - b) % 360.0
        return min(d, 360.0 - d)

    spawn_angles = [math.degrees(math.atan2(s[1] - ky, s[0] - kx)) % 360.0
                    for s in cv.spawns]
    inner_angles = [math.degrees(math.atan2(cl["center"][1] - ky,
                                            cl["center"][0] - kx)) % 360.0
                    for cl in clusters if cl["band"] == "inner"]
    # 顺路簇：每个集结点方向上都有一簇内环簇。±4° 抖动 + 均值偏移
    # （atan(4/(R+15)) ≈ 8.5°）⇒ 断言 13°。
    for sa in spawn_angles:
        c.true(any(gap(sa, ia) <= 13.0 for ia in inner_angles),
               f"集结点方向 {sa:.1f}° 附近应有一簇顺路内环簇，"
               f"实际内环方向 {sorted(round(a, 1) for a in inner_angles)}")
    # 外环簇避开每个集结点的 ±20° 扇区（均值偏移留余量，断言 > 16°）。
    for cl in clusters:
        if cl["band"] != "outer":
            continue
        a = math.degrees(math.atan2(cl["center"][1] - ky,
                                    cl["center"][0] - kx)) % 360.0
        c.true(all(gap(a, sa) > 16.0 for sa in spawn_angles),
               f"外环簇心方向 {a:.1f}° 落进了某集结点的 ±20° 扇区")

    # 主类型轮转：放置顺序 = 轮转顺序，固定种子下无一掉簇 ⇒ 序列恰为
    # 金→石→木… 循环；首簇恒金 ⇒ 内环至少一簇含金（实力模型 §6.1）。
    cycle = ("gold", "stone", "wood")
    mains = [cl["primary"] for cl in clusters]
    c.eq(mains, [cycle[i % 3] for i in range(len(mains))],
         f"主类型序列必须按金→石→木轮转，实际 {mains}")
    c.true(any(cl["band"] == "inner" and cl["primary"] == "gold"
               for cl in clusters),
           "内环带必须至少一簇金（首簇恒金的落点）")

    # 簇内构成：主类型是唯一众数、每簇 ≥ 2 种、大小落在配置区间。
    from collections import Counter
    for cl in clusters:
        tally = Counter(cl["kinds"])
        top = tally[cl["primary"]]
        c.true(all(v < top for t, v in tally.items() if t != cl["primary"]),
               f"簇主类型 {cl['primary']} 必须是唯一众数，实际 {dict(tally)}")
        c.true(len(tally) >= 2,
               f"簇只有 {sorted(tally)}——单种类簇必须不存在（2026-08-31 试玩）")
        c.true(r.outer_cluster_size[0] <= len(cl["points"]) <= r.outer_cluster_size[1],
               f"簇大小 {len(cl['points'])} 应落在 {r.outer_cluster_size}")

    # 间距与边界：簇内 ≥ 2、跨簇 ≥ 9（校验器按 ≤8 连边反推簇成员的前提）、
    # 点到任一集结点 ≥ 16（第 31 条后半）、点在图内。
    all_pts = [pt for cl in clusters for pt in cl["points"]]
    for cl in clusters:
        pts = set(cl["points"])
        for pt in cl["points"]:
            c.true(min(max(abs(pt[0] - q[0]), abs(pt[1] - q[1]))
                       for q in all_pts if q != pt) >= 2,
                   f"簇内点 {pt} 与邻居贴邻（切比雪夫 < 2）")
            other_clusters = [q for q in all_pts if q not in pts]
            c.true(min(max(abs(pt[0] - q[0]), abs(pt[1] - q[1]))
                       for q in other_clusters) >= 9,
                   f"跨簇点距 {pt} < 9——校验器反推簇成员会并错簇")
            c.true(all(max(abs(pt[0] - s[0]), abs(pt[1] - s[1])) >= 16
                       for s in cv.spawns),
                   f"簇点 {pt} 落进集结区（到某集结点 < 16）")
            c.true(cv.inside(*pt), f"簇点 {pt} 在地图外")
    centers = [cl["center"] for cl in clusters]
    for i in range(len(centers)):
        for j in range(i + 1, len(centers)):
            d = max(abs(centers[i][0] - centers[j][0]),
                    abs(centers[i][1] - centers[j][1]))
            c.true(d >= 4,
                   f"簇心间距 {d}——名义 ≥ 12 减去两侧均值偏移各 ≤ 4，"
                   f"再低就是两簇粘上了")

    # 簇心取整与 grid.cluster_center 一致（校验器第 28 条复算的前提）。
    for cl in clusters:
        c.eq(cl["center"], gridmod.cluster_center(cl["points"]),
             f"簇心必须按落点均值 half-up 取整：记录 {cl['center']} vs "
             f"复算 {gridmod.cluster_center(cl['points'])}")


def check_generator_wild_patches_are_small_and_separate(c):
    """野外 Forest/Rock 三形态（岩脊/林子/小撮，2026-09-02 第 3 步 §3.2）各自
    是小块，不能相邻粘成大团（#117 的纪律在三形态下延续）。

    上界按形态取：Forest = `woods_size` 上界（林子，30）；Rock =
    `ridge_length` 上界（岩脊，15——一道设计隘口的两段脊隔着 ≥ 2 格的口，
    8 连通意义下不相邻）。下界 3：更小的斑块生成器直接不落地（第 23 条的
    生成器侧）。本用例的 Canvas 没有集结点 ⇒ 只出孤脊不出设计隘口
    （`n_narrow` 为 0），设计隘口的口由 `check_generator_produces_valid_maps`
    的路线宽度断言覆盖。
    """
    cfg = SimpleNamespace(
        city_radius=12, spawn_wall_distance=40,
        forest_patches=[12, 12], forest_patch_size=[3, 5],
        rock_patches=6, rock_patch_size=[3, 6],
        woods=4, woods_size=[14, 30],
        ridges=2, ridge_length=[8, 15],
        gap_width_range=[2, 4])
    cv = generate.Canvas(174)
    generate.scatter_wild_terrain(cv, cfg, random.Random(20260902))

    eight = ((1, 0), (-1, 0), (0, 1), (0, -1),
             (1, 1), (1, -1), (-1, 1), (-1, -1))
    limits = {"Forest": 30, "Rock": 15}
    for kind, upper in limits.items():
        remaining = {(x, y) for y in range(cv.size) for x in range(cv.size)
                     if cv.at(x, y) == kind}
        components = []
        while remaining:
            start = min(remaining)
            remaining.remove(start)
            stack = [start]
            comp = {start}
            while stack:
                x, y = stack.pop()
                for dx, dy in eight:
                    nb = (x + dx, y + dy)
                    if nb in remaining:
                        remaining.remove(nb)
                        comp.add(nb)
                        stack.append(nb)
            components.append(comp)
        c.true(bool(components), f"{kind} 至少应生成一块")
        for comp in components:
            c.true(3 <= len(comp) <= upper,
                   f"{kind} 块大小应为 3--{upper}，实际 {sorted(comp)}")


def check_generator_inner_content(c):
    """`place_inner_content`：两片 3 格 L 形森林（4 连通、不贴墙）、
    资源构成恰好 1 石/1 金/1 木、**堡垒周围 3 格净空**（demo_init 在
    keep+(2,·) 撒 7 个开局单位，地图内容不得与它们撞车）。

    净空那条原先还有一半理由是「demo_init 在 keep+(1,±2) 预置 Tower/Flak」，
    2026-09-01 那两座已改（Tower 删掉、Flak 改为从墙线推，理由见
    `generate.py` 该函数的 docstring）——**净空本身保留**，7 个单位仍要地方站。
    """
    cv, r = _painted(_ref_cfg())
    kx, ky = cv.keep

    # 净空区：cheb ≤ 3 内除 keep 自身外，不得有资源/森林/建筑格。
    for y in range(cv.size):
        for x in range(cv.size):
            if max(abs(x - kx), abs(y - ky)) > 3 or (x, y) == (kx, ky):
                continue
            if cv.at(x, y) != "Plain":
                c.failures.append(
                    f"[生成器] 净空区 {(x, y)} 被 {cv.at(x, y)} 占用——"
                    f"demo_init 的预置内容会撞上它")
                c.count += 1
                return
            if (x, y) in cv.occupied():
                c.failures.append(
                    f"[生成器] 净空区 {(x, y)} 被实体占用——同上")
                c.count += 1
                return
    c.count += 1

    # 城内森林：恰好两片 L 形（每片 3 格、4 连通）。
    inner_forest = [(x, y) for y in range(cv.size) for x in range(cv.size)
                    if cv.at(x, y) == "Forest"
                    and max(abs(x - kx), abs(y - ky)) < r.city_radius]
    c.eq(len(inner_forest), 6, f"城内森林应为两片 3 格 L 形，实际 {len(inner_forest)} 格")
    # 4 连通块计数：两片。
    seen = set()
    comps = 0
    for cell in inner_forest:
        if cell in seen:
            continue
        comps += 1
        stack = [cell]
        seen.add(cell)
        while stack:
            x, y = stack.pop()
            for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nb = (x + ox, y + oy)
                if nb in inner_forest and nb not in seen:
                    seen.add(nb)
                    stack.append(nb)
    c.eq(comps, 2, f"城内森林应恰为两片 4 连通块，实际 {comps} 片")

    # 构成：2 石 + 2 金 + 1–2 木。
    inner = sorted(t for t, _, tier in cv.resources if tier == "inner")
    c.eq(inner.count("stone"), 2, f"inner 石应恒 2，实际 {inner}")
    c.eq(inner.count("gold"), 2, f"inner 金应恒 2，实际 {inner}")
    c.true(1 <= inner.count("wood") <= 2, f"inner 木 1–2，实际 {inner}")


def check_generator_buildings_legal(c):
    """预置建筑（微随机化）：塔贴城门/缺口内侧、兵营在净空区外、伐木场/
    采石场/金矿场**精确踩**对应资源点（第 24 条 GATHERER_MATCH 豁免的摆法）。"""
    cv, r = _painted(_ref_cfg())
    bld = {t: [p for bt, p in cv.buildings if bt == t]
           for t in ("Tower", "Barrack", "Lumber", "Quarry", "Mine")}
    c.eq(len(bld["Tower"]), 2, f"塔应恒 2（配置钉死），实际 {len(bld['Tower'])}")
    c.eq(len(bld["Barrack"]), 1, "兵营应恒 1（配置钉死）")
    c.eq(len(bld["Lumber"]), 1, "伐木场应恒 1")
    c.eq(len(bld["Quarry"]), 1, "采石场应恒 1")
    c.eq(len(bld["Mine"]), 1, "金矿场应恒 1（2026-09-01 追加）")

    anchors = set(cv.gates) | set(cv.breaches)
    for p in bld["Tower"]:
        c.true(any(max(abs(p[0] - a[0]), abs(p[1] - a[1])) <= 2 for a in anchors),
               f"箭塔 {p} 不贴任何城门/缺口（切比雪夫 ≤ 2）")
    kx, ky = cv.keep
    for p in bld["Barrack"]:
        c.true(max(abs(p[0] - kx), abs(p[1] - ky)) >= 4,
               f"兵营 {p} 落进了堡垒净空区（cheb < 4）")
    wood = [(x, y) for t, (x, y), tier in cv.resources
            if t == "wood" and tier == "inner"]
    stone = [(x, y) for t, (x, y), tier in cv.resources
             if t == "stone" and tier == "inner"]
    gold = [(x, y) for t, (x, y), tier in cv.resources
            if t == "gold" and tier == "inner"]
    c.true(bld["Lumber"][0] in wood,
           f"伐木场 {bld['Lumber'][0]} 必须精确踩在木点上（第 24 条豁免）")
    c.true(bld["Quarry"][0] in stone,
           f"采石场 {bld['Quarry'][0]} 必须精确踩在石点上（第 24 条豁免）")
    c.true(bld["Mine"][0] in gold,
           f"金矿场 {bld['Mine'][0]} 必须精确踩在金点上（第 24 条豁免）")


def check_generator_obstacles_outside_and_grouped(c):
    """可破坏树石只在城外，并同时覆盖单体与同类小团两种形态（#117）。"""
    cfg = _ref_cfg(obstacles=[16, 16])
    th = thresholds.load().profile("strict")
    cv = generate.Canvas(cfg.size)
    rng = random.Random(90901)
    r = generate._resolve(cfg, rng)
    generate.place_ring_walls(cv, r, rng)
    generate.place_spawns(cv, r, th, rng)
    generate.place_inner_content(cv, r, rng)
    generate.place_outer_clusters(cv, r, th, rng)
    generate.scatter_wild_terrain(cv, r, rng)
    groups = generate.place_obstacles(cv, r, rng)

    c.eq(len(cv.obstacles), 16, "空位充足时应落满配置要求的障碍数量")
    sizes = [len(g["cells"]) for g in groups]
    c.true(1 in sizes, f"障碍组 {sizes} 中必须有单体")
    c.true(any(n > 1 for n in sizes), f"障碍组 {sizes} 中必须有小团")
    kinds = {g["type"] for g in groups}
    c.true("Rubble" in kinds, f"障碍类型 {sorted(kinds)} 中必须有石堆")
    c.true(bool(kinds & {"Stump", "Sapling"}),
           f"障碍类型 {sorted(kinds)} 中必须有树木")

    kx, ky = cv.keep
    for group in groups:
        cells = group["cells"]
        c.true(1 <= len(cells) <= 3, f"障碍组大小应为 1--3，实际 {cells}")
        for x, y in cells:
            c.true(max(abs(x - kx), abs(y - ky)) >= r.city_radius + 2,
                   f"障碍 {(x, y)} 落进城墙或缓冲区")
            c.true(not any(max(abs(x - sx), abs(y - sy)) <= 1
                           for sx, sy in cv.spawns),
                   f"障碍 {(x, y)} 压到集结点邻域")
            c.true((x, y) not in cv.choke_gaps,
                   f"障碍 {(x, y)} 落进了设计隘口的口——清野不应等于拆掉地图"
                   f"给的隘口（§3.6，2026-09-02 第 5 步）")
        if len(cells) > 1:
            seen = {cells[0]}
            stack = [cells[0]]
            while stack:
                x, y = stack.pop()
                for px, py in cells:
                    if ((px, py) not in seen
                            and max(abs(px - x), abs(py - y)) == 1):
                        seen.add((px, py))
                        stack.append((px, py))
            c.eq(len(seen), len(cells), f"同组障碍必须 8 连通，实际 {cells}")


def check_generator_water(c):
    """`place_water`（2026-09-01，留白 1 作废）：湖与河真的落地、河必架桥、
    桥的四邻有水（第 13 条的生成器侧镜像）、任何水体落地后集结点仍连通 keep
    （第 12 条镜像——自校验失败会整片撤销，所以产出恒满足）。

    **2026-09-02 第 4 步（§3.4 河受管理）增补**：桥宽 2（每个桥格都有正交
    桥邻）、河格只落允许域（cheb ≥ R+6、非 `no_build`——湖照旧不受这条管，
    所以这组断言跑在「只开河、不开湖」的另一张画布上）、被河切断（不踩桥
    走不到 keep）的集结点 ≤ 1。

    另钉住**零配置不消费 rng** 这条：参考图靠固定种子逐字节复现，
    `place_water` 在 range 全 0 时若悄悄抽一次数，同一批种子的产出就全漂了。
    """
    cfg = _ref_cfg(water_lakes_range=[2, 2], water_lake_size=[10, 14],
                   rivers_range=[1, 1])
    cv, r = _painted(cfg)

    waters = [(x, y) for y in range(cv.size) for x in range(cv.size)
              if cv.at(x, y) == "Water"]
    bridges = [(x, y) for y in range(cv.size) for x in range(cv.size)
               if cv.at(x, y) == "Bridge"]
    c.true(len(waters) > 0, "开了水域配置就必须有 Water 落地")
    c.true(len(bridges) > 0, "rivers_range=[1,1] 时必须有 Bridge（河必架桥）")

    # 桥的四邻至少一格 Water（第 13 条镜像）。
    for bx, by in bridges:
        has_water = any(cv.inside(bx + ox, by + oy)
                        and cv.at(bx + ox, by + oy) == "Water"
                        for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        c.true(has_water, f"桥 {(bx, by)} 的四邻没有 Water（第 13 条镜像）")

    # 桥宽 2（第 30 条镜像）：每个桥格都有正交桥邻——1 格宽的桥两头孤。
    bset = set(bridges)
    for bx, by in bridges:
        c.true(any((bx + ox, by + oy) in bset
                   for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1))),
               f"桥 {(bx, by)} 没有正交桥邻——桥宽 2 要求每格都有相邻桥格")

    # 连通性（第 12/4/11 条镜像）。
    c.true(generate._connected_to_keep(cv),
           "水域落地后所有集结点必须仍能走到 keep（自校验失败的整片撤销在跑）")

    # 水域不得贴集结点（留白 1）与资源点八邻。
    for wx, wy in waters + bridges:
        c.true(all(max(abs(wx - s[0]), abs(wy - s[1])) >= 4 for s in cv.spawns),
               f"水域 {(wx, wy)} 贴上了集结点（cheb < 4）")
    res = {(x, y) for _, (x, y), _ in cv.resources}
    for wx, wy in waters + bridges:
        c.true((wx, wy) not in res, f"水域 {(wx, wy)} 盖住了资源点")

    # —— 河的限域与切断上限（§3.4）：只开河、不开湖，水格就全是河格 ——
    cfg_r = _ref_cfg(rivers_range=[1, 1])
    cvr, rr = _painted(cfg_r, seed=5150)
    kx, ky = cvr.keep
    river_cells = [(x, y) for y in range(cvr.size) for x in range(cvr.size)
                   if cvr.at(x, y) in ("Water", "Bridge")]
    c.true(bool(river_cells), "rivers_range=[1,1] 时河必须落地")
    for wx, wy in river_cells:
        c.true(max(abs(wx - kx), abs(wy - ky)) >= rr.city_radius + 6,
               f"河格 {(wx, wy)} 贴上了墙（cheb < R+6，§3.4 限域）")
        c.true(cvr.no_build[wy][wx] == 0,
               f"河格 {(wx, wy)} 压进了禁建环（§3.4：不穿禁建环）")
    c.true(len(generate._river_cut_spawns(cvr)) <= 1,
           f"被河切断（不踩桥走不到 keep）的集结点必须 ≤ 1，实际 "
           f"{generate._river_cut_spawns(cvr)}")

    # 零配置 ⇒ 不消费 rng：同种子下两条路径的产出必须逐格相同。
    cv_a, _ = _painted(_ref_cfg(), seed=777)
    cfg_off = _ref_cfg()
    cv_b = generate.Canvas(cfg_off.size)
    th = thresholds.load().profile("strict")
    rng_b = random.Random(777)
    r_b = generate._resolve(cfg_off, rng_b)
    generate.place_ring_walls(cv_b, r_b, rng_b)
    generate.place_spawns(cv_b, r_b, th, rng_b)
    generate.place_inner_content(cv_b, r_b, rng_b)
    generate.place_outer_clusters(cv_b, r_b, th, rng_b)
    generate.scatter_wild_terrain(cv_b, r_b, rng_b)
    generate.topup_wild_terrain(cv_b, r_b, th, rng_b)
    generate.place_obstacles(cv_b, r_b, rng_b)   # 刻意不调 place_water
    c.true(cv_a.terrain == cv_b.terrain and cv_a.resources == cv_b.resources,
           "零配置的 place_water 不得消费 rng——否则参考图的固定种子复现会漂")


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

    # 复制一份 strict 并把行军占比收紧到不可能满足（真实占比远高于此——
    # 2026-09-02 第 1 步把集结点环推到墙外 40 格，行军段更长）。
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
    # 两个推导属性不在 GENERATOR_KEYS 里（thresholds.Generator 的注释有说明），
    # 裸对象要手工补，否则 paint 里 AttributeError。
    few.size = cfg.size
    few.spawn_wall_distance = cfg.spawn_wall_distance

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
    # 第 3 条已废除（走廊概念取缔），其 selftest 组随之一并删除。
    ("第 4 条 集结点可达", check_v4_reachable),
    ("第 6 条 inner 三种资源", check_v6_inner_resources),
    # 第 7 条已废除（2026-09-01，移除森林带机制，见地图与场景设计.md 2.1），
    # 其 selftest 组（含「第 7 与第 10 条的对立是真的」）随之一并删除，
    # 同第 3 条那条先例。
    ("第 8 条 初始城圈留缺口", check_v8_initial_breach),
    ("第 10 条 森林遮蔽通道", check_v10_forest_corridor),
    ("第 11 条 Plain 孤岛", check_v11_islands),
    ("第 12 条 水切断走廊", check_v12_water_cuts),
    ("第 13 条 桥在水上", check_v13_bridge_on_water),
    ("第 15 条 content_hash", check_v15_hash),
    ("第 16 条 实体落在可建造格", check_v16_entity_cells),
    ("第 17 条 摆放冲突", check_v17_placement_conflicts),
    # 第 18 条已废除（2026-09-02，并入第 28 条，见 validate.REMOVED），
    # 其 selftest 组随之一并删除，同第 3/7 条那条先例。
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
    ("第 2 条 集结点数量与到墙距离", check_v2_spawn_count_and_wall_distance),
    ("第 5 条 Ram 行军占比", check_v5_ram_march),
    ("第 14 条 集结点到可建造格的距离", check_v14_spawn_buildable_distance),
    ("第 23 条 Forest 连通块不得碎成粉尘", check_v23_forest_cohesion),
    ("第 24 条 建筑摆放冲突", check_v24_building_placement),
    ("第 25 条 城外金矿下限", check_v25_outer_gold),
    # —— 2026-09-02 大改第 1/2 步新增的三条（§3.3/§3.1）——
    ("第 28 条 解禁按簇（同簇同波/簇序单调）", check_v28_unlock_by_cluster),
    ("第 29 条 簇类型配额与内环含金", check_v29_type_quota),
    ("第 31 条 路线顺路簇与簇到集结点距离", check_v31_passby),
    ("路线特征报告（§3.1 派生量）", check_route_features_report),
    # —— 2026-09-02 大改第 3/4/5 步：第 9 条解阻塞 + 新条目 26/27/30/32 ——
    ("第 9 条 需人工设防的正面总长", check_v9_defended_front),
    ("第 26 条 争夺带阻挡率", check_v26_wild_blocked_fraction),
    ("第 27 条 路线宽度分类（隘口/开阔）", check_v27_route_width_classes),
    ("第 30 条 桥宽与被切集结点", check_v30_river_bridges),
    ("第 32 条 入口分布（门与缺口异面）", check_v32_entrance_faces),
    # —— 生成器（第 9 节）——
    ("生成器产出合法地图 + 城圈完整性", check_generator_produces_valid_maps),
    ("生成器散布不得压墙/门与集结点邻域", check_generator_scatter_avoids),
    ("生成器资源簇不变量（环带/扇区/配额/间距）", check_generator_cluster_invariants),
    # 「生成器资源森林带自然转折且保持连通」随 `_carve_forest_belt` 一并删除
    # （2026-09-01，移除森林带机制）。
    ("生成器野外树石保持小撮且彼此分隔", check_generator_wild_patches_are_small_and_separate),
    ("生成器城内内容（净空区/森林/构成）", check_generator_inner_content),
    ("生成器预置建筑合法", check_generator_buildings_legal),
    ("生成器可破坏树石只在城外且有单体/小团", check_generator_obstacles_outside_and_grouped),
    ("生成器水域与桥（留白 1 作废）", check_generator_water),
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
