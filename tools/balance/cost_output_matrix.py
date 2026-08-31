# -*- coding: utf-8 -*-
"""
成本产出矩阵计算器（占位版，v1）。

CLAUDE.md「平衡工具：成本产出矩阵」要求"每格记录1v1对抗的期望资源盈亏"，
但 bindings/ 与 train/ 都还不存在，没有批量仿真可跑。本工具用
rts_core/include/rts/combat_math.hpp 里逐字复现的战斗公式做闭式1v1交换比
估算，作为批量仿真落地前的最小可行替代——不是长期方案，见
《数值设计与成本产出矩阵.md》第8节「已知局限」。

用法：
    py tools/balance/cost_output_matrix.py                # 克制二部图逐条
    py tools/balance/cost_output_matrix.py --scenarios    # 另加多场景对照表

读取 game/data/stats_placeholder.json，对 CLAUDE.md「克制二部图」里
点名的每一对关系跑一次闭式验证，打印结果。新增/修改数值后跑一遍，
先看这份报告再提交。

## 判据分三档，而不是「谁胜」一档

**这一版之前的判据是 `对方用时 > 自己用时`，那与 CLAUDE.md 明写的要求相反。**
那一句是：

  > 每格记录 1v1 对抗的期望资源盈亏（**而非谁胜**）

以及克制的定义：

  > 克制单位应能以**显著更少**的资源投入击败目标

一个严格不等号表达不了「显著」。实际后果是具体的：`Shade` 克 `Archer` 以
10.71s vs 11.25s（差 4.8%）被记成「通过」，而那个幅度下两个 `Archer`
就能翻盘，「压制墙头守军」并没有兑现。

所以改成三档，**照 `tools/map_gen/validate.py` 那条纪律**（已实现 / 阻塞 /
待阈值，三种都出现在报告里，不静默跳过）：

| 档 | 含义 |
|---|---|
| 通过 | 方向对，且交换比达到 `SIGNIFICANT_RATIO` |
| **幅度薄** | 方向对但幅度不够——**不算失败，但必须出现在报告里** |
| 未通过 | 方向就是反的 |

`SIGNIFICANT_RATIO` **是占位值**，见它自己那条注释。

## 为什么仍然不是「资源盈亏」：缺一个字段，不是缺算法

真要算盈亏，得知道双方各投入多少资源。守方那侧有 `cost_gold`；
**攻方那侧的货币是「编成位占用」，而那个字段整个仓库都不存在**
（`rts/stats.hpp` 没有，数值表里也没有）。`combat_math.cost_of()` 正是为它
留的位置，所以它至今没有调用方。

于是攻守之间无法折算，本工具只能停在时间维度上。这一条不是"以后优化"，
是**下一步的前置条件**：编成位落地之前，成本产出矩阵不可能真的是成本产出矩阵。
"""
import argparse
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

# 交换比达到多少才算「显著」。**这是占位值，不是标定过的判据。**
#
# CLAUDE.md 只说「显著更少的资源投入」，没有给数。取 1.3 的唯一理由是它把
# 现有那几对里明显成立的（2.2x / 2.19x / 1.63x）与明显勉强的（1.05x）分开，
# **而不是因为 1.3 有什么依据**。
#
# 它最终应当被「资源盈亏」取代而不是被标定成另一个时间比——见模块 docstring
# 末尾那段：那要等编成位占用这个字段存在。所以这里刻意不把它写进
# thresholds 一类的归口，免得它看起来像个已定的旋钮。
SIGNIFICANT_RATIO = 1.3


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
    thin, bad = [], []
    for atk_name, def_name, note, checkable in COUNTER_GRAPH:
        if not checkable:
            print(f"[结构性/非数值] {atk_name} 克 {def_name}（{note}）—— 跳过数值验证")
            continue
        a, d = units[atk_name], units[def_name]
        run = g.charge_max_cells if (a.is_charger or a.counters_charge) else 0.0
        a2d = ttk_seconds(a, d, g, charge_run=run)
        d2a = ttk_seconds(d, a, g, charge_run=run)
        ratio = (d2a / a2d) if a2d > 0 else float("inf")
        if ratio <= 1.0:
            tag, note_tail = "!! 未通过", "——方向就是反的"
            bad.append((atk_name, def_name, ratio))
        elif ratio < SIGNIFICANT_RATIO:
            tag, note_tail = "幅度薄", f"——方向对但只有 {ratio:.2f}x，够不上「显著」"
            thin.append((atk_name, def_name, ratio))
        else:
            tag, note_tail = "通过", ""
        print(f"[{tag}] {atk_name} 克 {def_name}（{note}）：{atk_name}杀{def_name}用时"
              f"{a2d:.2f}s, 反过来{d2a:.2f}s（交换比 {ratio:.2f}x）{note_tail}")

    print("\n提示：巷战/无助跑场景下 Spear vs Knight 应该反转（Knight占优）。"
          "本表只跑默认场景，多场景对照跑 `--scenarios`。")
    if thin:
        print(f"\n幅度薄 {len(thin)} 条（交换比 < {SIGNIFICANT_RATIO}x，"
              f"**不算失败但要记在报告里**）：")
        for a, d, r in thin:
            print(f"  {a} 克 {d}：只有 {r:.2f}x —— CLAUDE.md 要的是「显著更少的"
                  f"资源投入」，这个幅度下被克方靠数量就能翻盘")
        print("  判据本身是占位的（SIGNIFICANT_RATIO），最终应当换成真的资源盈亏，"
              "而那要等「编成位占用」这个字段存在——见模块 docstring。")
    if bad:
        print(f"\n!! 方向反了 {len(bad)} 条，需要调数值：")
        for a, d, r in bad:
            print(f"  {a} 克 {d}：交换比 {r:.2f}x")
    return len(bad)


# 多场景对照。**这些场景此前只以数字的形式活在文档里**，而文档里的数不会随
# 数值表更新——`Shade` 那条注释就是这么过期的（它引的是 damage=11 时的秒数，
# 而表里早已是 14）。放进工具，数就跟着表走。
# 每条给的是 (甲, 乙, 说明, 甲打乙的 kw, 乙打甲的 kw)。
#
# **两个方向必须分别给，不能共用一份 kw。** 初版共用，于是「`Archer` 站在完好
# 城墙上」这一行把高度惩罚同时套在两个方向上——连墙上的弓手朝下射也被罚，
# 算出「Shade 胜」，正好把这一行要说明的事说反了。高度优势是**不对称**的：
# 罚的只有从低处往高处打的那一方。
SCENARIOS = [
    ("Spear", "Knight", "开阔地满助跑",
     dict(charge_run="max"), dict(charge_run="max")),
    ("Spear", "Knight", "巷战/无助跑",
     dict(charge_run=0.0), dict(charge_run=0.0)),
    ("Shade", "Archer", "平地对射", {}, {}),
    # Archer 在墙上：Shade 从低处打它要吃 miss + 伤害惩罚；
    # 反过来 Archer 朝下打，Shade 不在墙上，所以一项惩罚都没有。
    ("Shade", "Archer", "Archer 站在完好城墙上",
     dict(target_on_high_wall=True), dict(attacker_high=True)),
]


def scenarios(units, g):
    print("\n=== 多场景对照 ===\n")
    for a_name, d_name, label, a_kw, d_kw in SCENARIOS:
        a, d = units[a_name], units[d_name]

        def resolve(kw, who):
            kw = dict(kw)
            if kw.get("charge_run") == "max":
                kw["charge_run"] = g.charge_max_cells
            elif "charge_run" not in kw:
                kw["charge_run"] = (g.charge_max_cells
                                    if (who.is_charger or who.counters_charge)
                                    else 0.0)
            return kw

        a2d = ttk_seconds(a, d, g, **resolve(a_kw, a))
        d2a = ttk_seconds(d, a, g, **resolve(d_kw, d))
        winner = a_name if a2d < d2a else d_name
        print(f"  {label:22s}: {a_name}杀{d_name} {a2d:6.2f}s / "
              f"{d_name}杀{a_name} {d2a:6.2f}s → {winner}胜")
    print("\n注意最后两行是同一场景的两个阶段，不是矛盾：`Shade` 在平地压制得住"
          "`Archer`，而 `Archer` 站上完好城墙就翻盘（高度优势）。墙掉血过半后"
          "那三项一起失效（局部破损），于是回到上一行——这正是「压制墙头 → 破墙"
          " → 缺口攻防」三阶段。")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="成本产出矩阵（闭式 1v1 近似）")
    ap.add_argument("--scenarios", action="store_true",
                    help="另打印多场景对照表（开阔地/巷战、平地/墙上）")
    args = ap.parse_args()
    units, g = load_units(STATS_PATH)
    n_bad = report(units, g)
    if args.scenarios:
        scenarios(units, g)
    # 方向反了才非零退出。**「幅度薄」刻意不非零**：那个阈值是占位的，
    # 拿一个占位阈值去让别人的构建红，是把待定当已定。
    sys.exit(1 if n_bad else 0)
