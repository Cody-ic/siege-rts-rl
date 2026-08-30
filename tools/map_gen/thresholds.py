"""阈值与生成器配置的读取层。

**为什么单独一个模块而不是在 validate.py 里 `json.load`：**

这份配置有两个消费者（校验器与生成器）与一个外部依赖（`Ram` 速度在
`game/data/stats_placeholder.json`）。摊在两处读就会出现两套「缺键怎么办」的答案，
而这个仓库里那种分歧的症状一律是「能跑但结果不对」。

**严格性照 `game::StatsLoader` 那条纪律办：手误不得静默落回默认值。**
缺键报错并点名、拼错的键报错并列出可用键。理由与那边逐字相同——JSON 里
`spawn_count_min` 少个 `s` 若静默落回 0，症状是「第 2 条永远通过」，
而那离病因极远。

**`Ram` 速度刻意不在 thresholds.json 里。** CLAUDE.md 写明数值进仿真只有
`game/data/stats_placeholder.json` → `StatsLoader` → `WorldInit::stats` 这一条路；
在这里抄一份就是第二个真相来源，而两份数值迟早分叉——那时第 5 条会拿一个
仿真里根本不存在的速度算行军时间，且没有任何东西会红。
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
    "spawn_count_min", "spawn_count_max", "spawn_edge_distance_max",
    "static_vision_radius_max",
    "episode_ticks_min", "episode_ticks_max",
    "ram_march_fraction_min", "ram_march_fraction_max",
    "front_length_min", "front_length_max",
})

GENERATOR_KEYS = frozenset({
    "size", "city_radius", "corridors", "corridor_mouth_width",
    "inner_resources", "outer_clusters", "outer_cluster_size",
    "initial_breaches", "wall_hp_frac_range", "forest_patches",
    "forest_patch_radius", "obstacles", "max_attempts",
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
        for k in PROFILE_KEYS:
            setattr(self, k, _num(d, k, where))

        # 区间必须非空。写反了（min > max）的症状是「所有地图都红」，
        # 而那看起来像地图坏了，不像配置坏了。
        for lo, hi in (("size_min", "size_max"),
                       ("spawn_count_min", "spawn_count_max"),
                       ("episode_ticks_min", "episode_ticks_max"),
                       ("ram_march_fraction_min", "ram_march_fraction_max"),
                       ("front_length_min", "front_length_max")):
            if getattr(self, lo) > getattr(self, hi):
                raise ThresholdError(
                    f"{where}：{lo} ({getattr(self, lo)}) > {hi} "
                    f"({getattr(self, hi)})，区间是空的")


class Generator:
    """§9 的固定项。"""

    __slots__ = tuple(sorted(GENERATOR_KEYS))

    def __init__(self, raw, strict):
        where = "generator"
        d = _strip_notes(raw, where)
        _require_keys(d, GENERATOR_KEYS, where)

        self.size = _num(d, "size", where, kind=(int,))
        self.city_radius = _num(d, "city_radius", where, kind=(int,))
        self.corridor_mouth_width = _num(d, "corridor_mouth_width", where, kind=(int,))
        self.outer_clusters = _num(d, "outer_clusters", where, kind=(int,))
        self.max_attempts = _num(d, "max_attempts", where, kind=(int,))

        self.corridors = list(d["corridors"])
        if not self.corridors:
            raise ThresholdError(f"{where}.corridors 是空的——没有走廊就没有集结点")
        if len(set(self.corridors)) != len(self.corridors):
            raise ThresholdError(
                f"{where}.corridors 有重复项：{self.corridors}\n"
                f"  §2.2 要求每个集结点对应一条**性质不同**的走廊，"
                f"重复等于让两个集结点等价（第 3 条会红，但在这里就该拦住）")

        self.inner_resources = dict(d["inner_resources"])
        for t in ("stone", "wood", "gold"):
            if self.inner_resources.get(t, 0) < 1:
                raise ThresholdError(
                    f"{where}.inner_resources 的 {t} 少于 1——"
                    f"第 6 条要求 inner 侧三种各 ≥ 1（4.4）")

        for k in ("outer_cluster_size", "initial_breaches", "wall_hp_frac_range",
                  "forest_patches", "forest_patch_radius", "obstacles"):
            v = d[k]
            if (not isinstance(v, list) or len(v) != 2
                    or not all(isinstance(x, (int, float))
                               and not isinstance(x, bool) for x in v)
                    or v[0] > v[1]):
                raise ThresholdError(
                    f"{where}.{k} 应当是 [下界, 上界] 两个数且下界 ≤ 上界，"
                    f"实际是 {v!r}")
            setattr(self, k, list(v))

        # **生成器的 size 必须落在校验器的 size 区间内。**
        # 不查的话，生成器会兴致勃勃地生成一批第 1 条必然否决的图，
        # 而报告只会说「丢弃率 100%」——那是最难查的一种失败：
        # 每一张图都合法，是配置自己互相矛盾。
        if not (strict.size_min <= self.size <= strict.size_max):
            raise ThresholdError(
                f"generator.size = {self.size} 不在 profiles.strict 的 size 区间 "
                f"[{strict.size_min}, {strict.size_max}] 内。\n"
                f"  这两处必须一致，否则生成器会产出第 1 条必然否决的图，"
                f"而症状（丢弃率 100%）指不出根因")

        # 城区必须装进地图，且四周要留得下走廊。留 4 格是最低限度
        # （集结点自己 + 距边界的余量），真实余量由 spawn_edge_distance_max 管。
        if self.city_radius * 2 + 8 > self.size:
            raise ThresholdError(
                f"generator：city_radius {self.city_radius} 对 size {self.size} "
                f"太大，城区外没有走廊的空间")


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
