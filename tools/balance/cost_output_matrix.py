# -*- coding: utf-8 -*-
"""
成本产出矩阵计算器（占位版，v1）。

CLAUDE.md「平衡工具：成本产出矩阵」要求"每格记录1v1对抗的期望资源盈亏"，
但 bindings/ 与 train/ 都还不存在，没有批量仿真可跑。本工具用
rts_core/include/rts/combat_math.hpp 里逐字复现的战斗公式做闭式1v1交换比
估算，作为批量仿真落地前的最小可行替代——不是长期方案，见
《数值设计与成本产出矩阵.md》第8节「已知局限」。

用法：
    py tools/balance/cost_output_matrix.py

读取 game/data/stats_placeholder.json，对 CLAUDE.md「克制二部图」里
点名的每一对关系跑一次闭式验证，打印结果。新增/修改数值后跑一遍，
先看这份报告再提交。
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from combat_math import UnitStats, GlobalStats, ttk_seconds, apply_permille  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
STATS_PATH = os.path.join(REPO_ROOT, "game", "data", "stats_placeholder.json")

# 花名册里哪些单位会用到冲锋/反冲锋结构（rts_core/src/unit_behavior.cpp 的结构判定，
# 数值表里没有，这里手动登记，改花名册时要同步改这里）
CHARGERS = {"Knight"}
ANTI_CHARGERS = {"Spear"}


def load_units(path):
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    g = doc["global"]
    glob = GlobalStats(
        hp_permille_per_level=g["hp_permille_per_level"],
        dmg_permille_per_level=g["dmg_permille_per_level"],
        high_ground_miss_permille=g["high_ground_miss_permille"],
        high_ground_dmg_permille=g["high_ground_dmg_permille"],
        high_ground_range_bonus=g["high_ground_range_bonus"],
        charge_bonus_permille_per_cell=g["charge_bonus_permille_per_cell"],
        charge_max_cells=g["charge_max_cells"],
        anti_charge_permille=g["anti_charge_permille"],
        income_period_ticks=g["income_period_ticks"],
        repair_hp_per_work_tick=g["repair_hp_per_work_tick"],
        repair_wood_per_1000hp=g["repair_wood_per_1000hp"],
        cancel_refund_permille=g["cancel_refund_permille"],
        garrison_mount_ticks=g["garrison_mount_ticks"],
        mason_work_radius=g["mason_work_radius"],
    )
    units = {}
    for name, u in doc["units"].items():
        units[name] = UnitStats(
            name=name, side="unknown", max_hp=u["max_hp"], damage=u["damage"],
            range_=u["range"], speed=u["speed"], vision=u["vision"],
            windup_ticks=u["windup_ticks"], cooldown_ticks=u["cooldown_ticks"],
            vs_structure_permille=u["vs_structure_permille"], aoe_radius=u["aoe_radius"],
            proj_speed=u["proj_speed"], cost_gold=u["cost_gold"], train_ticks=u["train_ticks"],
            is_charger=name in CHARGERS, counters_charge=name in ANTI_CHARGERS,
            is_combat=u["damage"] > 0 or name not in ("Scout", "Mason", "Wraith"),
        )
    return units, glob


# CLAUDE.md「克制二部图」逐条登记：(施加克制方, 被克方, 场景说明, 是否可用纯TTK验证)
# 猎杀/侦查类关系（Scout/Mason/Wraith 相关）不是纯数值能验证的机制，标 False。
COUNTER_GRAPH = [
    ("Ranger", "Ram", "快速切入、出城强袭", False),      # Ram目标是建筑，1v1单位对拼不是它的场景
    ("Archer", "Ghoul", "拉扯+Tower齐射", False),        # 靠"射程差=免费仇恨窗口"，不是纯DPS，naive TTK测不出
    ("Spear", "Knight", "开阔地克骑（枪阵）", True),
    ("Ranger", "Shade", "冲脸；城墙高度惩罚同样克Shade", True),
    ("Flak", "Phoenix", "唯一克制，纯位置性", False),     # 结构性，不是数值克制
    ("Shade", "Archer", "压制墙头守军（双方射程相同，非风筝场景，可用纯DPS验证）", True),
    ("Shade", "Spear", "慢速近战被风筝", False),          # 靠"射程+速度差=可无限风筝"，naive TTK假设双方已接敌，测不出
    ("Knight", "Ranger", "骑兵对冲，遏制出城", True),
]


def report(units, g):
    print("=== 成本产出矩阵（闭式1v1近似，非批量仿真） ===\n")
    for atk_name, def_name, note, checkable in COUNTER_GRAPH:
        if not checkable:
            print(f"[结构性/非数值] {atk_name} 克 {def_name}（{note}）—— 跳过数值验证")
            continue
        a, d = units[atk_name], units[def_name]
        run = g.charge_max_cells if (a.is_charger or a.counters_charge) else 0.0
        a2d = ttk_seconds(a, d, g, charge_run=run)
        d2a = ttk_seconds(d, a, g, charge_run=run)
        ok = d2a > a2d
        tag = "通过" if ok else "!! 未验证通过，需要调参数"
        print(f"[{tag}] {atk_name} 克 {def_name}（{note}）：{atk_name}杀{def_name}用时"
              f"{a2d:.2f}s, 反过来{d2a:.2f}s")
    print("\n提示：巷战/无助跑场景下 Spear vs Knight 应该反转（Knight占优），"
          "该场景在 combat_math 里通过 charge_run=0 触发，此处矩阵只跑默认场景，"
          "完整两个场景的对比见《数值设计与成本产出矩阵.md》第3节。")


if __name__ == "__main__":
    units, g = load_units(STATS_PATH)
    report(units, g)
