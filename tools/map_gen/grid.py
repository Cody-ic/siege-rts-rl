#!/usr/bin/env python3
"""格子层的解码、地形标志位与连通性原语。

`地图与场景设计.md` 4.2 说 `rts_core` 载入时把地形枚举展开成 `passable` /
`buildable` / `blocks_vision` 三张位图。本模块在 Python 侧做同一件事，
供校验器与生成器使用。

**两处实现同一张表是有风险的**（C++ 一份、Python 一份，改一处忘另一处），
但这里没有更好的选择：校验器要在 `rts_core` 之外独立跑（第 8 节要求手写地图与
生成地图都必须通过，而生成器是纯 Python）。缓解办法是把表写成与 4.1 那张表
逐行对应的形状，改动时两边都对着同一张文档表改。

## 邻接：默认 8 邻接，且不穿角

`CLAUDE.md`「RL 设计决策」把动作定为「**8 方向移动** + 停止」，所以单位是
8 邻接移动的，连通性检查必须与之一致 —— 用 4 邻接会把「斜着能过去」的地方
判成不通，于是校验器报一条根本不存在的问题。

**但斜向是否允许穿角（两格正交邻居都是障碍时能否斜穿）文档没有定义**，
这属于 `rts_core` 的移动模型，接口尚未定稿。这里取最保守的一种：
**两个正交邻格都可通行才允许斜向通过**。保守的方向是「判成不通」，
于是校验器可能多报、不会漏报 —— 对一个用来挡住坏地图的工具，多报是安全的那一侧。

接口定稿后要回来对齐，届时只改 `neighbors()` 一个函数。
"""
from collections import deque

from mapfile import TERRAIN_PALETTE

# 4.1 那张表，逐行对应。改这里之前先改文档，不要反过来。
#
#                 passable  buildable  blocks_vision
TERRAIN_FLAGS = {
    "Plain":     (True,     True,      False),
    "Rock":      (False,    False,     True),
    "Forest":    (True,     False,     True),
    "Water":     (False,    False,     False),
    "Bridge":    (True,     False,     False),
}

# 地面单位的天然屏障：既不可通行、也不是玩家能拆的东西。
# 墙**不在**这里 —— 5.1 明写墙是「高代价可通行」而不是障碍，把墙当障碍会复现
# DiNaO 那个被批评的失效（AI 卡在墙外转圈 / 永远不绕到缺口）。
NATURAL_BLOCKERS = frozenset({"Rock", "Water"})

_ORTHO = ((1, 0), (-1, 0), (0, 1), (0, -1))
_DIAG = ((1, 1), (1, -1), (-1, 1), (-1, -1))


class Grid:
    """解码后的格子层 + 点位实体的索引。

    只做「这一格是什么」的查询，不做任何判断题 —— 第 8 节那 15 条在 validate.py。
    """

    def __init__(self, doc):
        self.doc = doc
        self.width, self.height = doc["size"]

        palette = doc["layers"]["terrain"]["palette"]
        rows = doc["layers"]["terrain"]["rows"]
        # rows[y][x]，见 mapfile 的坐标约定。这里一次性展开成二维列表，
        # 后面所有查询都走它，避免每次都做一遍 int(char) 的转换。
        self.terrain = [[palette[int(c)] for c in row] for row in rows]

        nb = doc["layers"]["no_build"]["rows"]
        self.no_build = [[c == "1" for c in row] for row in nb]

        self.keep = tuple(doc["keep"])
        self.spawns = {tuple(s["pos"]): s for s in doc["spawns"]}
        self.resources = list(doc["resources"])
        # 墙段按坐标索引。同一格重复写两条墙属于地图有问题，但那是校验器的事，
        # 这里后写的覆盖先写的，不静默丢信息（重复本身由 validate 报）。
        self.walls = {tuple(w["pos"]): w for w in doc["initial_walls"]}

    # -- 基本查询 ---------------------------------------------------------

    def in_bounds(self, x, y):
        return 0 <= x < self.width and 0 <= y < self.height

    def terrain_at(self, x, y):
        return self.terrain[y][x]

    def is_passable(self, x, y):
        """地形本身可通行。**不考虑墙** —— 墙是高代价可通行，见 5.1。"""
        return TERRAIN_FLAGS[self.terrain[y][x]][0]

    def is_buildable(self, x, y):
        """4.2 的组合规则：`terrain.buildable AND NOT no_build`。

        文档专门写明这条，是因为有两套机制都能表达「不可建造」，
        不明写规则的话 `terrain=Plain + no_build=1` 与 `terrain=Bridge`
        谁管谁只能靠猜。
        """
        return TERRAIN_FLAGS[self.terrain[y][x]][1] and not self.no_build[y][x]

    def blocks_vision(self, x, y):
        return TERRAIN_FLAGS[self.terrain[y][x]][2]

    def is_natural_blocker(self, x, y):
        return self.terrain[y][x] in NATURAL_BLOCKERS

    def has_wall(self, x, y):
        return (x, y) in self.walls

    def all_cells(self):
        for y in range(self.height):
            for x in range(self.width):
                yield x, y

    def is_edge(self, x, y):
        return x == 0 or y == 0 or x == self.width - 1 or y == self.height - 1

    def edge_cells(self):
        for x, y in self.all_cells():
            if self.is_edge(x, y):
                yield x, y

    # -- 邻接与连通 -------------------------------------------------------

    def neighbors(self, x, y, passable, diagonal=True):
        """产出可从 (x, y) 走到的邻格。

        `passable` 是 `(x, y) -> bool`，由调用方给出 —— 不同的检查用不同的
        通行定义（地面单位、只走森林、把墙当可通行……），把它做成参数
        比在 Grid 上堆一排 `xxx_neighbors` 方法更不容易用错。

        斜向不穿角，理由见模块 docstring。
        """
        for dx, dy in _ORTHO:
            nx, ny = x + dx, y + dy
            if self.in_bounds(nx, ny) and passable(nx, ny):
                yield nx, ny
        if not diagonal:
            return
        for dx, dy in _DIAG:
            nx, ny = x + dx, y + dy
            if not (self.in_bounds(nx, ny) and passable(nx, ny)):
                continue
            # 不穿角：两个正交邻格都得能过。
            if passable(x + dx, y) and passable(x, y + dy):
                yield nx, ny

    def orthogonal_neighbors(self, x, y):
        """纯四邻，不带通行判断。第 13 条（桥的四邻要有水）明确用的是这个。"""
        for dx, dy in _ORTHO:
            nx, ny = x + dx, y + dy
            if self.in_bounds(nx, ny):
                yield nx, ny

    def neighbors8(self, x, y):
        """纯八邻，不带通行判断、也不管穿角。

        与 `neighbors()` 问的是两件事：那个问「走不走得过去」，这个问「挨没挨着」。
        第 10 条判「遮蔽通道有没有触到城墙」用的是后者 —— 单位能否走过去，
        与森林是否连到了墙边，是两回事。
        """
        for dx, dy in _ORTHO + _DIAG:
            nx, ny = x + dx, y + dy
            if self.in_bounds(nx, ny):
                yield nx, ny

    def flood(self, starts, passable, diagonal=True):
        """从 starts 出发的连通域。starts 自身不做 passable 检查。

        不检查起点是有意的：常见的用法是「从 keep 出发」，而 keep 那一格站着
        建筑、按某些通行定义并不 passable。让调用方为起点负责，比在这里
        猜一个规则更清楚。
        """
        seen = set()
        q = deque()
        for s in starts:
            s = tuple(s)
            if s not in seen and self.in_bounds(*s):
                seen.add(s)
                q.append(s)
        while q:
            x, y = q.popleft()
            for n in self.neighbors(x, y, passable, diagonal=diagonal):
                if n not in seen:
                    seen.add(n)
                    q.append(n)
        return seen

    def bfs_steps(self, starts, passable, diagonal=True):
        """返回 {(x,y): 步数}。步数是格数，不是欧氏距离。

        用于第 5 条的行军时间估计。**它给的是步数不是时间** —— 换算成时间要乘
        单位速度，而速度是待定数值，所以换算留给校验器配合 thresholds.json 做，
        本模块不碰任何数值。
        """
        dist = {}
        q = deque()
        for s in starts:
            s = tuple(s)
            if s not in dist and self.in_bounds(*s):
                dist[s] = 0
                q.append(s)
        while q:
            x, y = q.popleft()
            d = dist[(x, y)] + 1
            for n in self.neighbors(x, y, passable, diagonal=diagonal):
                if n not in dist:
                    dist[n] = d
                    q.append(n)
        return dist

    # -- 常用的几种通行定义 -----------------------------------------------

    def ground_passable(self, x, y):
        """地面单位：地形可通行即可。墙不挡路（5.1：墙是高代价可通行）。"""
        return self.is_passable(x, y)

    def blocked_by_walls_too(self, x, y):
        """地形可通行**且**没有墙。

        这不是仿真里的通行定义（仿真里墙是高代价可通行），只用于推导
        「城圈上哪里是缺口」—— 那个问题问的正是「不拆墙的话能不能进来」。
        """
        return self.is_passable(x, y) and not self.has_wall(x, y)


# --------------------------------------------------------------------------
# 城区 / 城圈 / 正面 / 缺口
# --------------------------------------------------------------------------
#
# 【结论：在现有字段下无法可靠推导。见 issue #29 第一条的更正】
#
# 6.2 说「初始缺口不显式存储 —— 缺口就是城圈上没有墙段的位置，由校验器推导」。
# 这句话预设了「城圈」是已知的，但地图文件里并没有这个东西，而 initial_walls
# 本身是带缺口的（第 8 条要求如此），墙段集合不闭合，反推不出闭合轮廓。
#
# 我在 #29 里提过一个 flood-fill 的定义，**实现时发现它不成立**，两种障碍取法都不行：
#
#   - 障碍取 Rock ∪ Water：走廊口本来就是 Plain（2.2「天然岩壁承担大部分周长，
#     只有走廊口需要人工墙」），flood 从走廊口直接漏到城外，城区退化成整张图
#   - 障碍再加上墙：第 8 条强制要求城圈留缺口，缺口处照样漏
#
# 这不是选错了障碍集，是**问题本身**：一条不闭合的曲线不把平面分成内外两部分。
# 任何 flood-fill 变体都绕不过去，除非引入「能填多大缺口」这类形态学参数，
# 而那又是一个待标定的数值 —— 用一个数值旋钮去定义一条结构约束，方向就错了。
#
# 因此城区必须由**地图显式给出**。这一点顺带给 #26 的 A4 多了一条论据：
# 候选乙的城区 mask 不只是封住「设防」那个洞，它还是校验器第 8、9 条
# 能够实现的前提 —— 甲那一侧根本没有可实现的第 8、9 条。
#
# 在城区来源定案（#26 A4/A5 + #29）之前，下面的函数**只在城圈完整时可用**，
# 泄漏即抛 CityAreaUndecidable，不返回一个看起来像样的错答案。校验器第 8、9 条
# 因此暂时挂起（PR 2 里会显式标为 blocked，不是静默跳过）。
#
# 整块推导刻意收在这里：城区来源一旦定下（显式 mask / 随 Keep 升级分级），
# 要改的只有 derive_city_area() 一个函数。


class CityAreaUndecidable(RuntimeError):
    """城区无法从现有地图字段推导出来。

    携带 leak 是为了让报错能指向具体位置 —— 「推导不出来」这种错误若不给出
    是在哪里漏的，收到它的人无从判断是地图画错了还是工具的问题。
    """

    def __init__(self, message, leak=None):
        super().__init__(message)
        self.leak = leak


def derive_city_area(grid, blocked=None):
    """城区：从 keep 出发、以屏障为界的连通域；泄漏到地图边界即判定推导失败。

    `blocked` 是 `(x, y) -> bool`，默认取「天然屏障或墙」—— 这是两种取法里
    更接近「城圈」直觉的一种（墙确实是城圈的一部分）。默认值不重要，
    因为两种取法在有缺口的地图上都会泄漏，见上面那段注释。
    """
    if blocked is None:
        def blocked(x, y):
            return grid.is_natural_blocker(x, y) or grid.has_wall(x, y)

    area = grid.flood([grid.keep], lambda x, y: not blocked(x, y))

    leaks = sorted(c for c in area if grid.is_edge(*c))
    if leaks:
        raise CityAreaUndecidable(
            "城区推导失败：从 keep 出发的连通域触到了地图边界 "
            f"（例如 {leaks[0]}，共 {len(leaks)} 格），说明城圈不闭合。\n"
            "  这是预期内的 —— 第 8 条要求城圈必须留缺口，而不闭合的轮廓"
            "无法把平面分成内外。\n"
            "  城区需要由地图显式给出（候选乙的 city mask），见 issue #29。",
            leak=leaks[0])
    return area


def derive_city_ring(grid, city=None):
    """城圈：城区里「至少有一个八邻格不在城区内」的那些格。

    用八邻接判边界，与移动的邻接保持一致：只按四邻算的话斜向那一圈会被漏掉，
    而单位是能斜着进来的。
    """
    if city is None:
        city = derive_city_area(grid)
    ring = set()
    for x, y in city:
        for dx, dy in _ORTHO + _DIAG:
            nx, ny = x + dx, y + dy
            if not grid.in_bounds(nx, ny) or (nx, ny) not in city:
                ring.add((x, y))
                break
    return ring


def derive_defended_front(grid, ring=None):
    """需人工设防的正面：城圈上不是天然屏障的那些格。

    第 9 条「需人工设防的正面总长」就是它的基数。
    """
    if ring is None:
        ring = derive_city_ring(grid)
    return {(x, y) for x, y in ring if not grid.is_natural_blocker(x, y)}


def derive_breaches(grid, front=None):
    """缺口：正面上没有墙段的格子。第 8 条查它非空。"""
    if front is None:
        front = derive_defended_front(grid)
    return {(x, y) for x, y in front if not grid.has_wall(x, y)}
