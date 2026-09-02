# -*- coding: utf-8 -*-
"""按设计要求构造《边境要塞》参考演示地图（§9 的手写地图，2026-08-31 第二次重设计；
**2026-09-02 第三次重装**：《地图生成器大改方案》§3.9，第 6 步）。

**不是生成器**（`generate.py` 是随机地图池用的）。这是这一张特定手写地图的
**权威来源**——`game/data/maps/reference_border_keep_01.json` 是它的产出，
不要直接手改那份 JSON；改布局要求就改这个脚本再重新跑一遍
（`python build_reference_map.py`），理由与 `tools/sprite_gen/` 那批脚本是
"素材生成流水线的输入文件"同一条：CLAUDE.md 要求"软件源代码包括输入文件和输出文件"。

设计要求（组长拍板，2026-08-31；2026-09-01 微调 × 2；2026-09-02 按大改方案重装）：
- 堡垒居中，四周距离堡垒 8 格一圈**城墙实体**（不是 Rock 岩壁），
  一对相对方向开城门 + 设计缺口
- 开局玩家在城墙内部拥有 2 座箭塔 + 1 个兵营
- 城墙内部：2 片森林、**恰好 1 处石矿、1 处金矿、1 处木材**（2026-09-01 第二次
  微调——试玩反馈「城墙内资源点太多」，从「2 石 2 金 1 木」收紧到各恰好 1；
  伐木场踩木点、采石场踩石点、金矿场踩金点，三座预置建筑各自还是恰好一座）
- 城墙外的资源随机散布、较为分散、**贴近城墙**（参考 AoE4 1v1 对战地图）；
  可破坏障碍按同类 1–3 个成团、只在城外
- 与随机地图池**共用同一套生成逻辑**（`generate.paint`），只是种子固定
- **不画水域**：这张 56 格小图上河与湖既摆不下形态、又压城外线空间
  （`water_lakes_range`/`rivers_range` 全 0，`place_water` 因此不消费 rng，
  本图其余部分的复现性不受影响）

**2026-09-02 大改第 6 步的几何现实（§3.9 预见了其一，其二是落地时实测出来的）：**

1. **没有外环**（方案明说允许，注明于此）：外环带 cheb ∈ [R+D+14, 半宽−3]
   = [62, 25] 为空集——R=8、D=40 的完整四环根本放不下 56 格。
   `outer_band_clusters = 0`，城外资源全部在内环带。
2. **D 收成 18**：集结点环 cheb R+D 必须 ≤ 半宽 28（keep 居中）⇒ D ≤ 20；
   内环带 [R+4, R+D−14] 非空 ⇒ D ≥ 18。取 18（带恰好一格厚 cheb=12，
   顺路簇从 R+15 钳进带里）。校验一侧是 thresholds.json 新增的
   `reference` 档（`spawn_wall_distance_range` [16,20]）——strict 档的
   [36,44] 在本图上客观不可满足，**本图不过 strict 是设计内的**，
   由 ctest `map_gen_validate_reference_strict_is_red` 钉住（防 profile
   退化成后门的同一手法，同 fixture 档）。
3. **没有设计隘口**（第 27 条在 `reference` 档弃用）：岩脊要求脊段每格落在
   内环带内（`generate.try_ridge`），脊长 ≥ 6 ⇒ 带厚至少约 7 格，
   而本图内环带厚 = D−18 ≤ 2。56 格上「两类路线」客观做不出来。
4. **簇到集结点的最小距离 16 → 10**（`reference` 档
   `cluster_spawn_distance_min`）：内环带最外点 cheb 12、簇半径 4、集结点
   cheb 26 ⇒ 同向射向间距恒 ≈ 10，16 客观不可满足。「簇不进集结区」的
   意图由禁建环（no_build 13，第 14 条）承担。

跑完必须过
`python validate.py --profile reference ../../game/data/maps/reference_border_keep_01.json`。
"""
import sys
sys.path.insert(0, ".")
import random
from types import SimpleNamespace

import generate
import mapfile
import thresholds as thmod

SIZE = 56
SEED = 6   # 固定种子：挑出来的一张合法且布局好看的图（见下方注释）
# 2026-09-01：那次收紧城内资源/收窄城外簇距的过程中，中间几版参数改动让原种子
# 20260831 中途一度不合法（第 11 条：出现一格 keep 走不到的孤立 Plain）——生成器
# 的随机流随参数改动而挪动，同一个种子在不同参数下不保证仍合法。定下最终
# 参数后重新跑过，20260831 仍合法，未改。`max_attempts=1` 是设计（这张图是
# "挑出来的"，不是"跑出来的"）：改参数后**必须**重新跑一遍 `validate.py`
# （`--profile reference`），不合法就换种子，不要假定原种子还能用。
# 2026-09-02 大改第 6 步：生成器整套换骨（四环 + 三形态），同一种子再复核——
# 这次 20260831 不再合法（第 25/31 条红：顺路金簇没落下 + 簇贴集结区），
# 顺号扫描 1..59（约 1/3 的种子合法，否决面是第 25/29/31 条——一格厚的内环带
# 放不下全部计划簇时整簇放弃，属预期），眼看前几张合法图的 ASCII 预览后取了 6：
# 城门东西相对、缺口在无门的北面（第 32 条）、三簇金石木分落东北/东南/西南、
# 塔贴门内侧。扫描脚本就是本文件 `build(seed)` + `validate.run()`，无他。

# `paint` 消费的 cfg 形状与 `generate._resolve` 的产出相同（区间全钉死 = 定值）。
# 全部数值都是占位/参考图特化，无平衡含义；改动后必须重跑校验器。
CFG = SimpleNamespace(
    size=SIZE,
    city_radius_range=[8, 8],
    spawn_count_range=[2, 2],
    spawn_wall_distance=18,   # D——为什么是 18，见模块 docstring 第 2 条
    inner_resources_range={"stone": [1, 1], "wood": [1, 1], "gold": [1, 1]},
    # 城外资源簇全部在内环带（每集结点一簇顺路 + 补到 3 簇，金/石/木轮转
    # 恰好各一簇——第 29 条配额 1/1/1、内环含金、城外金点 ≥ 2 同时成立）。
    # **外环 = 0 是设计内的**（放不下，见模块 docstring 第 1 条）。
    inner_band_clusters=[3, 3],
    outer_band_clusters=[0, 0],
    outer_cluster_size=[4, 5],
    cluster_unlock_per_wave=1,
    initial_breaches=[1, 1],
    wall_hp_frac_range=[1.0, 1.0],
    # 2026-08-31 试玩反馈：这张手写演示图开局满血（原先 [0.35, 1.0] 撒残血，
    # 叠加当时偏低的单位血量后开局手感过难）。**2026-09-01 起池图同样满血**
    # （`thresholds.json` 的 `wall_hp_frac_range` 也钉死了，两处一致）——
    # 这里留区间字段是因为 `paint` 的形状要求，不是「本图还可以残血」。
    # 不影响「AI 是否利用已有缺口」评估指标——那条测的是 `initial_breaches`
    # 留出的完整缺口，与 `hp_frac` 是两个独立字段。
    forest_patches=[8, 12],
    forest_patch_size=[3, 5],
    rock_patches_range=[4, 6],
    rock_patch_size=[3, 6],
    # 林子与岩脊都是 0：林子只放内/外环带而本图内环带只有一格厚（长出来的
    # 只能是贴城的一圈环段，读不出「林子」）；岩脊放不下（模块 docstring
    # 第 3 条）。阻挡率由小撮 + `topup_wild_terrain` 补到 `reference` 档
    # 的 [0.10, 0.20]（与 strict 同值）。
    woods_range=[0, 0],
    woods_size=[0, 0],
    ridges_range=[0, 0],
    ridge_length=[6, 6],      # 形状要求（下界 6），ridges=0 时不消费
    gap_width_range=[2, 4],   # 同上
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
    th = thmod.load().profile("reference")
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
