"""
逐字复现 rts_core/include/rts/combat_math.hpp 与 mechanics.cpp 的相关公式。
用途：在没有 C++ 构建环境的情况下，用真实引擎公式验证/迭代数值设计。
所有函数签名与舍入行为都对照 agent 报告里摘录的原始代码逐行核对。
"""
import math
from dataclasses import dataclass, field
from typing import List

PERMILLE_ONE = 1000


def apply_permille(base: int, permilles: List[int]) -> int:
    """combat_math.hpp:37-52 —— 唯一乘除入口，四舍五入，钳到 >=1。"""
    num = base
    den = 1
    for p in permilles:
        num *= p
        den *= PERMILLE_ONE
    v = (num + den // 2) // den
    return max(v, 1)


def level_permille(level: int, per_level: int) -> int:
    """combat_math.hpp 的 level_permille —— 血量与伤害**各开一份平方根**。

    `√(1 + k(L-1))`，1 级恒为 1000。`per_level` 是**开方前**的线性系数 k，
    血量与伤害共用同一个值（`p - q = 0` 是结构约束，StatsLoader 强制相等）。

    这里原先是线性（`1000 + k(L-1)`），随 §1.4 一起改。**用 math.isqrt 而不是
    `sqrt` 再取整**：C++ 侧走整数牛顿法（浮点开方的最低位会在两套工具链之间
    飘，而那个值进 state_hash），两边必须逐位相同，否则这个工具算出来的交换比
    与仿真里真实发生的不是一回事。
    """
    linear = PERMILLE_ONE + per_level * (level - 1)
    return math.isqrt(PERMILLE_ONE * linear)


def charge_permille(run_cells: float, max_cells: float, per_cell_permille: int) -> int:
    """combat_math.hpp:71-79 —— 倍率=1000+per_cell*min(动量,封顶)。"""
    if max_cells <= 0 or per_cell_permille <= 0 or run_cells <= 0:
        return PERMILLE_ONE
    m = min(run_cells, max_cells)
    return PERMILLE_ONE + int(per_cell_permille * m + 0.5)


def anti_charge_permille(target_run_cells: float, max_cells: float, full_permille: int) -> int:
    """combat_math.hpp:85-97 —— 目标动量越满，反冲锋倍率越接近 full_permille。"""
    if max_cells <= 0:
        return PERMILLE_ONE
    m = min(target_run_cells, max_cells)
    return PERMILLE_ONE + int((full_permille - PERMILLE_ONE) * (m / max_cells) + 0.5)


@dataclass
class UnitStats:
    name: str
    side: str  # "def" or "atk"
    max_hp: int
    damage: int
    range_: float
    speed: float  # 格/tick
    vision: float
    windup_ticks: int
    cooldown_ticks: int
    vs_structure_permille: int
    aoe_radius: float = 0.0
    proj_speed: float = 0.0
    cost_gold: int = 0
    train_ticks: int = 0
    is_charger: bool = False
    counters_charge: bool = False  # Melee x HeavySlow (Spear)
    is_combat: bool = True


@dataclass
class GlobalStats:
    hp_permille_per_level: int
    dmg_permille_per_level: int
    high_ground_miss_permille: int
    high_ground_dmg_permille: int
    high_ground_range_bonus: float
    charge_bonus_permille_per_cell: int
    charge_max_cells: float
    anti_charge_permille: int
    income_period_ticks: int = 100
    repair_hp_per_work_tick: int = 4
    repair_wood_per_1000hp: int = 15
    demolish_refund_permille: int = 800
    garrison_mount_ticks: int = 30
    mason_work_radius: float = 1.6


def cadence(u: UnitStats) -> int:
    """一次完整出手周期 = 前摇+冷却（tick）。"""
    return max(u.windup_ticks + u.cooldown_ticks, 1)


def dps(u: UnitStats, g: GlobalStats, level: int = 1, charge_run: float = 0.0,
        vs_structure: bool = False, target_on_high_wall: bool = False,
        attacker_high: bool = False) -> float:
    """稳态 DPS（tick^-1 × 20 = 每秒），不含 miss（miss 在期望值里单独乘）。"""
    lvl = level_permille(level, g.dmg_permille_per_level)
    mods = [lvl]
    if vs_structure:
        mods.append(u.vs_structure_permille)
    if u.is_charger and charge_run > 0:
        mods.append(charge_permille(charge_run, g.charge_max_cells, g.charge_bonus_permille_per_cell))
    if u.counters_charge and charge_run > 0:
        mods.append(anti_charge_permille(charge_run, g.charge_max_cells, g.anti_charge_permille))
    if (not attacker_high) and target_on_high_wall:
        mods.append(g.high_ground_dmg_permille)
    dmg = apply_permille(u.damage, mods)
    hit_chance = 1.0
    if (not attacker_high) and target_on_high_wall:
        hit_chance = 1.0 - g.high_ground_miss_permille / 1000.0
    return dmg * hit_chance / cadence(u) * 20.0  # per second


def effective_hp(u: UnitStats, g: GlobalStats, level: int = 1) -> int:
    return apply_permille(u.max_hp, [level_permille(level, g.hp_permille_per_level)])


def ttk_seconds(attacker: UnitStats, defender: UnitStats, g: GlobalStats,
                 atk_level: int = 1, def_level: int = 1, **kw) -> float:
    """attacker 单挑 defender，忽略移动/接敌时间，只看纯出手交换。"""
    d = dps(attacker, g, level=atk_level, **kw)
    hp = effective_hp(defender, g, level=def_level)
    return hp / d if d > 0 else float("inf")


def cost_of(u: UnitStats, resources: dict) -> float:
    """把资源折算成统一"资源当量"，用于跨阵营比较（攻方无经济，用编成位等价代价）。"""
    return resources.get(u.name, 0.0)
