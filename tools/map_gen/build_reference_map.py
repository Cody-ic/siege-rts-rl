# -*- coding: utf-8 -*-
"""按设计要求构造《边境要塞》参考演示地图（§9 的手写地图，2026-08-31 第二次重设计）。

**不是生成器**（`generate.py` 是随机地图池用的）。这是这一张特定手写地图的
**权威来源**——`game/data/maps/reference_border_keep_01.json` 是它的产出，
不要直接手改那份 JSON；改布局要求就改这个脚本再重新跑一遍
（`python build_reference_map.py`），理由与 `tools/sprite_gen/` 那批脚本是
"素材生成流水线的输入文件"同一条：CLAUDE.md 要求"软件源代码包括输入文件和输出文件"。

设计要求（组长拍板，2026-08-31；2026-09-01 微调 × 2）：
- 堡垒居中，四周距离堡垒 8 格一圈**城墙实体**（不是 Rock 岩壁），
  一对相对方向开城门 + 设计缺口
- 开局玩家在城墙内部拥有 2 座箭塔 + 1 个兵营
- 城墙内部：2 片森林、**恰好 1 处石矿、1 处金矿、1 处木材**（2026-09-01 第二次
  微调——试玩反馈「城墙内资源点太多」，从「2 石 2 金 1 木」收紧到各恰好 1；
  伐木场踩木点、采石场踩石点、金矿场踩金点，三座预置建筑各自还是恰好一座）
- 城墙外的资源（森林/岩壁/矿脉）随机散布、较为分散、**贴近城墙**
  （2026-09-01 第二次微调——试玩反馈「城外资源点太少且离城墙太远」，
  簇数与簇内点数上调、簇距上界改用与地图边长解耦的 `outer_cluster_span`，
  详见下面 `CFG` 与 `generate.place_outer_clusters` 的注释），参考 AoE4 1v1 对战地图；
  野外森林/岩壁按 #117 恢复为「大散居、小聚居」的小撮散布（每簇 3–5/3–6 格、
  彼此分隔），可破坏障碍按同类 1–3 个成团、只在城外
- 与随机地图池**共用同一套生成逻辑**（`generate.paint`），只是种子固定
- **不画水域**：这张 56 格小图上河与湖既摆不下形态、又压城外线空间，
  水域是 144 池图的内容（`water_lakes_range`/`rivers_range` 全 0，
  `place_water` 因此不消费 rng，本图其余部分的复现性不受影响）

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
# 2026-09-01：这次收紧城内资源/收窄城外簇距的过程中，中间几版参数改动让这个
# 种子中途一度不合法（第 11 条：出现一格 keep 走不到的孤立 Plain）——生成器
# 的随机流随参数改动而挪动，同一个种子在不同参数下不保证仍合法。定下最终
# 参数后重新跑过，20260831 仍合法，未改。`max_attempts=1` 是设计（这张图是
# "挑出来的"，不是"跑出来的"）：改参数后**必须**重新跑一遍 `validate.py`，
# 不合法就换种子，不要假定原种子还能用。

# `paint` 消费的 cfg 形状与 `generate._resolve` 的产出相同（区间全钉死 = 定值）。
# 全部数值都是占位/参考图特化，无平衡含义；改动后必须重跑校验器。
CFG = SimpleNamespace(
    size=SIZE,
    city_radius_range=[8, 8],
    spawn_count_range=[2, 2],
    inner_resources_range={"stone": [1, 1], "wood": [1, 1], "gold": [1, 1]},
    outer_clusters_range=[8, 8],
    outer_cluster_span=7,
    outer_cluster_size=[4, 5],
    initial_breaches=[1, 1],
    wall_hp_frac_range=[1.0, 1.0],
    # 2026-08-31 试玩反馈：这张手写演示图开局满血（原先 [0.35, 1.0] 撒残血，
    # 叠加当时偏低的单位血量后开局手感过难）。**2026-09-01 起池图同样满血**
    # （`thresholds.json` 的 `wall_hp_frac_range` 也钉死了，两处一致）——
    # 这里留区间字段是因为 `paint` 的形状要求，不是「本图还可以残血」。
    # 不影响「AI 是否利用已有缺口」评估指标——那条测的是 `initial_breaches`
    # 留出的完整缺口，与 `hp_frac` 是两个独立字段。
    forest_patches=[10, 14],
    forest_patch_size=[3, 5],
    rock_patches_range=[5, 7],
    rock_patch_size=[3, 6],
    water_lakes_range=[0, 0],
    water_lake_size=[0, 0],
    rivers_range=[0, 0],
    towers_range=[2, 2],
    barracks_range=[1, 1],
    obstacles=[8, 14],
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
