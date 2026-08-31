# -*- coding: utf-8 -*-
"""按设计要求构造《边境要塞》参考演示地图（§9 的手写地图，2026-08-31 第二次重设计）。

**不是生成器**（`generate.py` 是随机地图池用的）。这是这一张特定手写地图的
**权威来源**——`game/data/maps/reference_border_keep_01.json` 是它的产出，
不要直接手改那份 JSON；改布局要求就改这个脚本再重新跑一遍
（`python build_reference_map.py`），理由与 `tools/sprite_gen/` 那批脚本是
"素材生成流水线的输入文件"同一条：CLAUDE.md 要求"软件源代码包括输入文件和输出文件"。

设计要求（组长拍板，2026-08-31）：
- 堡垒居中，四周距离堡垒 8 格一圈**城墙实体**（不是 Rock 岩壁），
  一对相对方向开城门 + 设计缺口
- 开局玩家在城墙内部拥有 2 座箭塔 + 1 个兵营
- 城墙内部：2 片森林、2 处石矿、2 处金矿、1 处木材；伐木场踩木点、采石场踩石点
- 城墙外的资源（森林/岩壁/矿脉）随机散布、较为分散，参考 AoE4 1v1 对战地图
- 与随机地图池**共用同一套生成逻辑**（`generate.paint`），只是种子固定

跑完必须过 `python validate.py ../../game/data/maps/reference_border_keep_01.json`。
"""
import sys
sys.path.insert(0, ".")
import random
from types import SimpleNamespace

import generate
import mapfile
import thresholds as thmod

SIZE = 56
SEED = 20260831   # 固定种子：挑出来的一张合法且布局好看的图（见下方注释）

# `paint` 消费的 cfg 形状与 `generate._resolve` 的产出相同（区间全钉死 = 定值）。
# 全部数值都是占位/参考图特化，无平衡含义；改动后必须重跑校验器。
CFG = SimpleNamespace(
    size=SIZE,
    city_radius_range=[8, 8],
    spawn_count_range=[2, 2],
    inner_resources_range={"stone": [2, 2], "wood": [1, 1], "gold": [2, 2]},
    outer_clusters_range=[4, 4],
    outer_cluster_size=[2, 3],
    initial_breaches=[1, 1],
    wall_hp_frac_range=[1.0, 1.0],
    # 2026-08-31 试玩反馈：这张手写演示图开局满血（原先 [0.35, 1.0] 撒残血，
    # 叠加当时偏低的单位血量后开局手感过难）。**这不是对 2.3「初始墙段允许带
    # 残血」那条通用设计原则的推翻**——`generate.py` 的训练地图集仍走
    # `thresholds.json` 的 `wall_hp_frac_range`（保留残血，服务 RL 训练侧的
    # 「修旧/建新/造兵」三方决策与残血墙多样性）；这里改的只是这一张演示地图
    # 的具体取值。不影响「AI 是否利用已有缺口」评估指标——那条测的是
    # `initial_breaches` 留出的完整缺口，与 `hp_frac` 是两个独立字段。
    forest_patches=[6, 9],
    forest_patch_size=[4, 8],
    rock_patches_range=[3, 5],
    rock_patch_size=[4, 10],
    towers_range=[2, 2],
    barracks_range=[1, 1],
    obstacles=[5, 10],
    max_attempts=1,
)


def build(seed=SEED):
    """按固定种子画整张图。同种子 ⇒ 逐字节可复现（`check_generator_is_deterministic`
    同一条不变量），所以演示时演的和录的永远是同一张。"""
    th = thmod.load().profile("strict")
    rng = random.Random(seed)
    cv = generate.Canvas(CFG.size)
    generate.paint(cv, CFG, th, rng)
    return cv.to_doc(map_id="border_keep_01", name="边境要塞")


if __name__ == "__main__":
    doc = build()
    out = "../../game/data/maps/reference_border_keep_01.json"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(mapfile.canonical_dumps(doc))
    print("written:", out)
