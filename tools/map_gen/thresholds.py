"""阈值与生成器配置的读取层。

**为什么单独一个模块而不是在 validate.py 里 `json.load`：**

这份配置有两个消费者（校验器与生成器）与**两个**外部依赖，两个都在
`game/data/stats_placeholder.json`：`Ram` 速度（第 5 条）与**建筑视野的最大值**
（第 14 条）。摊在两处读就会出现两套「缺键怎么办」的答案，
而这个仓库里那种分歧的症状一律是「能跑但结果不对」。

**严格性照 `game::StatsLoader` 那条纪律办：手误不得静默落回默认值。**
缺键报错并点名、拼错的键报错并列出可用键。理由与那边逐字相同——JSON 里
`spawn_count_min` 少个 `s` 若静默落回 0，症状是「第 2 条永远通过」，
而那离病因极远。

**`Ram` 速度刻意不在 thresholds.json 里。** CLAUDE.md 写明数值进仿真只有
`game/data/stats_placeholder.json` → `StatsLoader` → `WorldInit::stats` 这一条路；
在这里抄一份就是第二个真相来源，而两份数值迟早分叉——那时第 5 条会拿一个
仿真里根本不存在的速度算行军时间，且没有任何东西会红。

**建筑视野的最大值是同一条理由的第二例，而它已经真的分叉过一次。**
`static_vision_radius_max` 曾是手写的 8，而表里 `Watch` 是 12；于是校验器判定
合法的图上一座瞭望塔仍然照亮集结区，第 14 条那条**结构**约束被违反而全绿。
所以那个键现在只是一条**声明**，权威值由 `max_building_vision()` 从表里推导，
两者不一致时第 14 条自己会红——见 `validate.py` 该条。
"""

from __future__ import annotations

import json
import os

# 这份配置与 stats 表的位置。都按「相对本文件」算，不依赖调用者的工作目录
# ——同 selftest.py 用 sys.path[0] 互相 import 的理由（ctest 的工作目录不可靠）。
_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(os.path.dirname(_HERE))

DEFAULT_PATH = os.path.join(_HERE, "thresholds.json")
STATS_PATH = os.path.join(_REPO, "game", "data", "stats_placeholder.json")

SCHEMA = "map-thresholds/1"

# 每个 profile 必须给齐的键。**白名单双向用**：缺了报「缺少」，多了报「认不出」。
PROFILE_KEYS = frozenset({
    "size_min", "size_max",
    "spawn_count_min", "spawn_count_max", "spawn_wall_distance_range",
    "static_vision_radius_max",
    "episode_ticks_min", "episode_ticks_max",
    "ram_march_fraction_min", "ram_march_fraction_max",
    "front_length_min", "front_length_max",
    "forest_min_component_cells",
    "outer_gold_min",
    "route_cluster_window", "cluster_spawn_distance_min",
    # 2026-09-02 大改第 3/4/5 步（《地图生成器大改方案》§3.7/§3.8）：
    # 第 26 条争夺带阻挡率、第 27 条路线最窄处、第 30 条桥宽与河切集结点。
    "wild_blocked_fraction_min", "wild_blocked_fraction_max",
    "route_width_range",
    "bridge_width_min", "river_cut_spawns_max",
})

GENERATOR_KEYS = frozenset({
    "city_radius_range", "spawn_count_range",
    "inner_resources_range",
    "inner_band_clusters", "outer_band_clusters", "outer_band_width",
    "outer_cluster_size", "cluster_unlock_per_wave",
    "initial_breaches", "wall_hp_frac_range",
    "forest_patches", "forest_patch_size",
    "rock_patches_range", "rock_patch_size",
    # 2026-09-02 大改第 3 步（§3.2）：林子与岩脊/隘口的四组参数。
    "woods_range", "woods_size",
    "ridges_range", "ridge_length", "gap_width_range",
    "water_lakes_range", "water_lake_size", "rivers_range",
    "towers_range", "barracks_range", "obstacles", "max_attempts",
})


class ThresholdError(ValueError):
    """配置本身有问题（缺键、拼错、越界）。与「地图有问题」是两类，别混。"""


def _strip_notes(obj, where):
    """去掉 `_note*` 注释键，并对剩下的键做白名单检查前的预处理。

    注释用键而不用 JSON 之外的机制，是因为 6.1 要求数据文件文本可 diff，
    而 JSON 没有注释语法。前缀统一取 `_`，与 `stats_placeholder.json` 一致。
    """
    if not isinstance(obj, dict):
        raise ThresholdError(f"{where} 应当是一个对象，实际是 {type(obj).__name__}")
    return {k: v for k, v in obj.items() if not k.startswith("_")}


def _require_keys(got, allowed, where):
    missing = sorted(allowed - set(got))
    if missing:
        raise ThresholdError(
            f"{where} 缺少必填键：{missing}\n"
            f"  刻意不给默认值——静默落回默认值的症状是「检查永远通过」，"
            f"那比报错难查得多")
    unknown = sorted(set(got) - allowed)
    if unknown:
        raise ThresholdError(
            f"{where} 有认不出的键：{unknown}\n"
            f"  可用的键：{sorted(allowed)}\n"
            f"  （拼错一个键与漏掉它的后果相同，所以两者都报）")


def _num(d, key, where, kind=(int, float)):
    v = d[key]
    if isinstance(v, bool) or not isinstance(v, kind):
        raise ThresholdError(f"{where} 的 {key} 应当是数字，实际是 {v!r}")
    return v


class Profile:
    """一档阈值。属性访问，拼错属性名会 AttributeError 而不是静默 None。"""

    __slots__ = tuple(sorted(PROFILE_KEYS)) + ("name",)

    def __init__(self, name, raw):
        where = f"profiles.{name}"
        d = _strip_notes(raw, where)
        _require_keys(d, PROFILE_KEYS, where)
        self.name = name
        for k in PROFILE_KEYS - {"spawn_wall_distance_range", "route_width_range"}:
            setattr(self, k, _num(d, k, where))

        # 2026-09-02：第 2 条的判据从「贴边」改为「到最近墙格的距离 ∈ 区间」
        # （《地图生成器大改方案》§3.1），所以这是一个 [D_lo, D_hi] 区间键。
        # 第 27 条的 `route_width_range` 同形（§3.2，第 3 步）——下界 < 0 =
        # 本档弃用该条（只有 fixture 用，同 `static_vision_radius_max: -1`）。
        for rk in ("spawn_wall_distance_range", "route_width_range"):
            v = d[rk]
            if (not isinstance(v, list) or len(v) != 2
                    or not all(isinstance(x, (int, float))
                               and not isinstance(x, bool) for x in v)
                    or v[0] > v[1]):
                raise ThresholdError(
                    f"{where}.{rk} 应当是 [下界, 上界] 两个数"
                    f"且下界 ≤ 上界，实际是 {v!r}")
            setattr(self, rk, list(v))

        # 区间必须非空。写反了（min > max）的症状是「所有地图都红」，
        # 而那看起来像地图坏了，不像配置坏了。
        for lo, hi in (("size_min", "size_max"),
                       ("spawn_count_min", "spawn_count_max"),
                       ("episode_ticks_min", "episode_ticks_max"),
                       ("ram_march_fraction_min", "ram_march_fraction_max"),
                       ("front_length_min", "front_length_max"),
                       ("wild_blocked_fraction_min",
                        "wild_blocked_fraction_max")):
            if getattr(self, lo) > getattr(self, hi):
                raise ThresholdError(
                    f"{where}：{lo} ({getattr(self, lo)}) > {hi} "
                    f"({getattr(self, hi)})，区间是空的")


class Generator:
    """§9 的固定项 + 随机项（区间/候选域）。

    **哪些留固定、哪些松开是一次刻意的取舍，不是"能松就松"**：`size` 依然固定
    ——**但 2026-09-02 起它不再手写，而是由四环公式推导**
    （《地图生成器大改方案》§2/§3.8：`size = 2×(R + D + 14 + W_far + 3)`，
    D 与 W_far 才是设计量，size 是它们的函数；旧手写值的历史见
    `_note_size`/`_FINDING_size_vs_ram_speed`）。
    **2026-08-31 重构（取缔走廊、城圈改城墙实体）**：`corridors` 候选域、
    `corridor_count_range`、`corridor_mouth_width_range` 三个键整个删除——
    走廊概念已废，替代它们的是 `spawn_count_range`（集结点数，与走廊解耦）
    与野外散布的四组参数（`forest_patches`/`forest_patch_size`/
    `rock_patches_range`/`rock_patch_size`）。
    **2026-09-01**：新增水域三组（`water_lakes_range`/`water_lake_size`/
    `rivers_range`），生成器留白 1（不画 Water/Bridge）作废。
    **2026-09-02（大改第 1/2 步）**：`outer_clusters_range`/`outer_cluster_span`
    两个键作废，拆成 `inner_band_clusters`/`outer_band_clusters`/
    `outer_band_width`（资源簇按环分布，§3.3）；新增 `cluster_unlock_per_wave`
    （解禁按簇，每波开几簇——占位值，与实力模型一起标）。
    **2026-09-02（大改第 3 步，§3.2）**：新增 `woods_range`/`woods_size`
    （林子圆团）、`ridges_range`/`ridge_length`/`gap_width_range`
    （岩脊线与设计隘口）；小撮计数收窄（森林 [20,30]、岩石 [10,16]）。
    **同日第 5 步（§3.5）**：`city_radius_range` 收紧到 [8,10]（§9 已定），
    size 推导自动跟着 R_hi 走（170），下面的加载断言负责对齐。
    """

    # 两个推导属性不在 GENERATOR_KEYS 里（它们不是配置键）：`size` 由四环公式
    # 推出（2026-09-02，§2/§3.8），`spawn_wall_distance` 是 strict 档
    # spawn_wall_distance_range 的中点（生成器放置集结点用的 D）。
    __slots__ = tuple(sorted(GENERATOR_KEYS)) + ("size", "spawn_wall_distance")

    def __init__(self, raw, strict):
        where = "generator"
        d = _strip_notes(raw, where)
        _require_keys(d, GENERATOR_KEYS, where)

        self.max_attempts = _num(d, "max_attempts", where, kind=(int,))
        self.outer_band_width = _num(d, "outer_band_width", where, kind=(int,))
        if self.outer_band_width < 1:
            raise ThresholdError(
                f"{where}.outer_band_width = {self.outer_band_width} 应当 ≥ 1"
                f"——它是外环带宽度 W_far（§2.2 取 15–20），同时进边长推导，"
                f"取 0 或负数会让外环带不存在")
        self.cluster_unlock_per_wave = _num(
            d, "cluster_unlock_per_wave", where, kind=(int,))
        if self.cluster_unlock_per_wave < 1:
            raise ThresholdError(
                f"{where}.cluster_unlock_per_wave = "
                f"{self.cluster_unlock_per_wave} 应当 ≥ 1——它控制每波解禁几簇，"
                f"取 0 等于「永远不解禁城外资源」")

        for k in ("city_radius_range", "spawn_count_range",
                  "inner_band_clusters", "outer_band_clusters",
                  "outer_cluster_size", "initial_breaches",
                  "wall_hp_frac_range", "forest_patches", "forest_patch_size",
                  "rock_patches_range", "rock_patch_size",
                  "woods_range", "woods_size",
                  "ridges_range", "ridge_length", "gap_width_range",
                  "water_lakes_range", "water_lake_size", "rivers_range",
                  "towers_range", "barracks_range", "obstacles"):
            v = d[k]
            if (not isinstance(v, list) or len(v) != 2
                    or not all(isinstance(x, (int, float))
                               and not isinstance(x, bool) for x in v)
                    or v[0] > v[1]):
                raise ThresholdError(
                    f"{where}.{k} 应当是 [下界, 上界] 两个数且下界 ≤ 上界，"
                    f"实际是 {v!r}")
            setattr(self, k, list(v))

        # 2026-09-02：集结点环上按角度采样，相邻角差 ≥ 60°（《地图生成器大改
        # 方案》§3.1）。上界 5：6 个就只剩恰好 60° 等分一种摆法，而 §3.1 拍板
        # 的范围就是 3–5。
        if self.spawn_count_range[1] > 5:
            raise ThresholdError(
                f"{where}.spawn_count_range 上界 {self.spawn_count_range[1]} 超过 "
                f"5——相邻角差 ≥ 60° 下 5 个是设计上限（§3.1）")

        # 簇点数下界 ≥ 3（2026-09-02）：主类型必须是簇内**唯一众数**（校验器
        # 第 29 条按众数定簇类型）且每簇 ≥ 2 种 ⇒ 主类型 ≥ 2 点 + 至少 1 个
        # 异类点 = 3 点起步。下界 2 时「2 点 2 种」定不出主类型。
        if self.outer_cluster_size[0] < 3:
            raise ThresholdError(
                f"{where}.outer_cluster_size 下界 {self.outer_cluster_size[0]} "
                f"小于 3——主类型要 ≥ 2 点才是唯一众数、再加至少 1 个异类点，"
                f"一簇至少 3 点（第 29 条按众数定簇类型）")

        # 隘口与岩脊的配置一致性（2026-09-02 大改第 3 步，§3.2）：
        # 生成器留的口必须落进校验器第 27 条的隘口区间（strict 档
        # `route_width_range`），否则每张图都被第 27 条否决——同 size 那道
        # 检查的理由：配置互相矛盾时症状是丢弃率 100%，指不出根因。
        if strict.route_width_range[0] >= 0:
            glo, ghi = self.gap_width_range
            rlo, rhi = strict.route_width_range
            if glo < rlo or ghi > rhi:
                raise ThresholdError(
                    f"{where}.gap_width_range [{glo}, {ghi}] 越出 strict 档 "
                    f"route_width_range [{rlo}, {rhi}]——生成器留的口必须落进"
                    f"第 27 条的隘口区间，否则每张图都被它否决")
        # 一道岩脊线被隘口分成两段，每段 ≥ 3 格才读作「脊」（与「小撮 ≥ 3」
        # 同一条形态纪律）⇒ 线长下界 6。
        if self.ridge_length[0] < 6:
            raise ThresholdError(
                f"{where}.ridge_length 下界 {self.ridge_length[0]} 小于 6——"
                f"隘口两侧各 ≥ 3 格才读作「脊」，线长至少 6")

        raw_inner = d["inner_resources_range"]
        if not isinstance(raw_inner, dict):
            raise ThresholdError(f"{where}.inner_resources_range 应当是一个对象")
        self.inner_resources_range = {}
        for t in ("stone", "wood", "gold"):
            v = raw_inner.get(t)
            if (not isinstance(v, list) or len(v) != 2
                    or not all(isinstance(x, int) and not isinstance(x, bool)
                               for x in v)
                    or v[0] < 1 or v[0] > v[1]):
                raise ThresholdError(
                    f"{where}.inner_resources_range 的 {t} 应当是 [下界, 上界] 两个"
                    f"整数且下界 ≥ 1（第 6 条要求 inner 侧三种各 ≥ 1，见 4.4），"
                    f"实际是 {v!r}")
            self.inner_resources_range[t] = list(v)

        # **2026-09-02 起 size 是推导量，不是配置键**（《地图生成器大改方案》
        # §2/§3.8：「D 与 W_far 才是设计量，size 是它们的函数」）：
        #   size = 2×(R + D + 14 + W_far + 3)
        # R 取 city_radius_range 上界（最坏情形，随机到小半径时外环带只会更宽），
        # D 取 strict 档 spawn_wall_distance_range 的中点（生成器恒按中点放
        # 集结点），14 是集结点环两侧到内/外环带的间隔（对齐禁建环 13 + 1），
        # 3 是外环带到地图边缘的余量。
        d_lo, d_hi = strict.spawn_wall_distance_range
        self.spawn_wall_distance = (d_lo + d_hi) // 2
        self.size = int(2 * (self.city_radius_range[1]
                             + self.spawn_wall_distance + 14
                             + self.outer_band_width + 3))

        # **生成器的 size 必须落在校验器的 size 区间内。**（推导值同此检查——
        # 公式里任何一元改动都可能把 size 推出区间。）不查的话，生成器会
        # 兴致勃勃地生成一批第 1 条必然否决的图，而报告只会说「丢弃率 100%」
        # ——那是最难查的一种失败：每一张图都合法，是配置自己互相矛盾。
        if not (strict.size_min <= self.size <= strict.size_max):
            raise ThresholdError(
                f"推导出的 size = {self.size}（= 2×(R {self.city_radius_range[1]}"
                f" + D {self.spawn_wall_distance} + 14 + W_far "
                f"{self.outer_band_width} + 3)）不在 profiles.strict 的 size 区间 "
                f"[{strict.size_min}, {strict.size_max}] 内。\n"
                f"  这两处必须一致，否则生成器会产出第 1 条必然否决的图，"
                f"而症状（丢弃率 100%）指不出根因")

        # 城区必须装进地图，且四周要留得下城外空间。
        # **按区间的上界检查**（最坏情形）——city_radius 现在是随机的，配置本身
        # 必须在它能抽到的最大值下仍然成立，否则某些种子会在运行时才炸。
        if self.city_radius_range[1] * 2 + 8 > self.size:
            raise ThresholdError(
                f"generator：city_radius_range 上界 {self.city_radius_range[1]} 对 "
                f"size {self.size} 太大，城区外没有空间")


class Thresholds:
    def __init__(self, raw, path):
        self.path = path
        if not isinstance(raw, dict):
            raise ThresholdError(f"{path}：顶层应当是一个对象")
        schema = raw.get("schema")
        if schema != SCHEMA:
            raise ThresholdError(
                f"{path}：schema 是 {schema!r}，本工具只认 {SCHEMA!r}\n"
                f"  刻意不做向后兼容——旧版少的键会被静默补上默认值，"
                f"而那正是本模块要防的形态")
        profiles_raw = _strip_notes(raw.get("profiles", {}), "profiles")
        if not profiles_raw:
            raise ThresholdError(f"{path}：一个 profile 都没有")
        self.profiles = {name: Profile(name, r)
                         for name, r in sorted(profiles_raw.items())}
        if "strict" not in self.profiles:
            raise ThresholdError(
                f"{path}：缺少 profiles.strict——它是默认档，"
                f"也是生成器唯一认的那一档")
        self.generator = Generator(raw.get("generator", {}), self.profiles["strict"])

    def profile(self, name):
        try:
            return self.profiles[name]
        except KeyError:
            raise ThresholdError(
                f"没有名为 {name!r} 的 profile；有的是 "
                f"{sorted(self.profiles)}") from None


def load(path=None):
    path = path or DEFAULT_PATH
    # 显式 UTF-8：Windows 的默认编码是 cp936，而这份文件有中文注释。
    # 同 mapfile.load() 那条。
    with open(path, encoding="utf-8") as f:
        try:
            raw = json.load(f)
        except json.JSONDecodeError as e:
            raise ThresholdError(f"{path}：JSON 语法错误：{e}") from None
    return Thresholds(raw, path)


def ram_speed_cells_per_tick(path=None):
    """从数值表读 `Ram` 的速度。**这里不给默认值，读不到就抛。**

    第 5 条要的是「行军时间占 episode 的比例」，而时间 = 距离 ÷ 速度。
    速度取不到时唯一诚实的行为是让那一条报「读不到速度」，
    而不是拿一个猜的数算出一个看起来合理的比例。
    """
    path = path or STATS_PATH
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    try:
        v = raw["units"]["Ram"]["speed"]
    except (KeyError, TypeError):
        raise ThresholdError(
            f"{path}：读不到 units.Ram.speed。\n"
            f"  第 5 条（`Ram` 行军占比）拿它算行军时间；"
            f"它是数值进仿真的唯一路径，本工具刻意不另存一份") from None
    if not isinstance(v, (int, float)) or isinstance(v, bool) or v <= 0:
        raise ThresholdError(f"{path}：units.Ram.speed = {v!r}，应当是正数")
    return float(v)


def max_building_vision(path=None):
    """从数值表读**所有建筑视野的最大值**。走 `Ram` 速度那条先例，理由逐字相同。

    第 14 条要守「任何静态建筑的视野都不得覆盖集结区」，而它的可校验形式
    （集结点到最近可建造格的距离 > 上限）里那个「上限」**就是这个最大值**。
    此前 `static_vision_radius_max` 是手写的 8，而表里 `Watch` 是 12——
    于是校验器判定合法的图上，一座瞭望塔仍然照亮集结区，**而没有任何东西会红**。

    实测过：用仓库的 `generate.py` 生成的图，四个集结点到最近可建造格都是 10 格，
    `Watch(12)` 全部够得着（16/16）。这不是假想的边界情形，是生成器的默认产出。

    所以这里**不给默认值、读不到就抛**：拿一个猜的上限算出一个「通过」，
    正是第 14 条要防的那种一次性买断。
    """
    path = path or STATS_PATH
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    blds = raw.get("buildings")
    if not isinstance(blds, dict):
        raise ThresholdError(
            f"{path}：读不到 buildings。\n"
            f"  第 14 条拿建筑视野的最大值当上限；"
            f"它是数值进仿真的唯一路径，本工具刻意不另存一份")
    # `_note` 一类的说明键在同一层，按类型筛而不是按前缀——前缀约定改了这里就漏。
    got = {}
    for name, b in blds.items():
        if not isinstance(b, dict):
            continue
        v = b.get("vision")
        if not isinstance(v, (int, float)) or isinstance(v, bool):
            raise ThresholdError(
                f"{path}：buildings.{name}.vision = {v!r}，应当是数")
        got[name] = float(v)
    if not got:
        raise ThresholdError(
            f"{path}：buildings 里一座都没有 vision——第 14 条的上限无从推导")
    return max(got.values()), max(got, key=lambda k: got[k])
