# -*- coding: utf-8 -*-
"""预算与节奏曲线模型 —— 攻方兵力 / 守方金币 / 出矿与解禁节奏。

`波次预算曲线与堡垒等级曲线.md` 给了公式,但那份文档只能被**读**;
一条曲线改了之后「拐点还在不在」「多余兵力有没有浪费」得手算。
这个脚本把那些公式变成可跑的,并把其中**结构性**的几条做成断言。

用法:
    py tools/balance/budget_curves.py            # 逐波表 + 结构检查
    py tools/balance/budget_curves.py --waves 60 # 看更远

## 先说清楚哪些是结构、哪些是旋钮 —— 这是本文件存在的主要理由

CLAUDE.md「关于数值」要求区分这两类。这一堆量看着都像旋钮,其实不是:

| 量 | 它真正控制什么 | 类别 |
|---|---|---|
| `cost(等级)` 与 `战力(等级)` 的阶 | 「40 杂兵 vs 8 精英」是不是真决策 | **结构**。**2026-09-02 定案并落地**：取 `c ∝ √B(L)`（`World::train_cost_gold`），有意让「凿点用质、铺开用量」（`数值设计与成本产出矩阵.md` §12.5），「只堆精英」由人口上限 8+2K 与兵种等级上限封顶——下文「结论一」的「必须同阶」据此修订，保留作决策史 |
| 编成位有硬顶 | 单波**编队**数落在 RL 可训练区间 | **结构**（CLAUDE.md 明文）,取值 **2026-09-05 定为 27**——依据见 `PLACEHOLDER` 里那段注释（SMAC 最大官方图恰是 27 个 agent / 我们的观测是它的 18-21 倍 / 40 在文献带外）。语义同日改成编队数:27 个 agent 对应约 70 个单位 |
| 资源点分布形态 | 「围哪一簇」是不是真决策 | **结构**（已定,见地图规范） |
| 波间隔 T × 收入率 | 守方每波预算 | **只有乘积进预算** ⇒ 一个自由度 |
| 出矿速率 × 矿点密度 | 总收入 | 同上,只有乘积 |
| 兵力预算指数 α | 难度增长速度 | 数值 |
| 等级战力指数 p+q | 等级值多少战力 | 数值（两侧有界,见 CLAUDE.md §1.4） |
| 解禁波次序列 | 拐点反复出现的节奏 | **主难度旋钮**（CLAUDE.md 原话） |

后面几条是数值,所以本文件里它们**全部是占位值**,集中在 `PLACEHOLDER` 一处。

## 三条结构结论,推导写在这里

### 一、`cost(L)` 必须与 `战力(L)` 同阶,否则宏观层的动作空间塌到角上

> **【2026-09-02 修订】**「必须同阶」被 `数值设计与成本产出矩阵.md` §12 的
> 等成本对拼模拟细化推翻：定案取 `c ∝ √B(L)`（已落地为
> `World::train_cost_gold`），**有意**让堆级质占优（「凿点用质、铺开用量」，
> §12.5），「只堆精英」改由人口上限 `8+2K` 与兵种等级上限 = 堡垒等级封顶。
> 下面原文保留作决策史。

`波次预算曲线与堡垒等级曲线.md` §3 已经摸到这条（「造价也应该近似线性,
否则会出现堆量或堆级其中一个策略碾压另一个」),这里把它说准:

设一个 L 级单位的战力是 `P(L)`、造价是 `C(L)`。宏观层在「多买一个」与
「升一级」之间的取舍,比的是**每单位预算买到的战力** `P(L)/C(L)`。

- `C` 比 `P` 涨得慢 ⇒ 堆级严格占优 ⇒ 永远买少量高级兵
- `C` 比 `P` 涨得快 ⇒ 堆量严格占优 ⇒ 永远买一堆 1 级兵
- `C ∝ P` ⇒ 两者等效,**于是选哪个由战术决定**（集火脆弱性、AOE 吃亏程度、
  目标选择),而那正是 CLAUDE.md 要的:「40 个低级杂兵铺开 / 8 个高级精英
  硬凿一点」是真决策,且是「AI 适应玩家最直观的演示形式之一」

所以这不是平衡数值,是**结构**:它决定那个决策存不存在。

### 二、~~`cost ∝ 战力` 一旦成立,「波次强度曲线」与「兵力预算曲线」是同一条~~

> **【2026-09-02 订正】本条不成立,详见 `攻守实力模型与平衡分析.md` §8 第 1 条。**
> 下面推导里的「总战力」是 ΣB(单位 HP×DPS 之和)——没有一场仗按它打;
> 一波兑现到场上的是 **Σ√B**(同文档 §2:现行曲线第 5 波兑现率 43%、
> 第 20 波 32%),而兑现率随编成形状(人数 × 等级怎么分)变。于是
> 「标定一条曲线就够」不成立:预算曲线之外还有一层随编成变化的兑现映射。
> 按正确货币建的模型在同目录 `siege_race.py`。下面原文保留作决策史。

因为攻方总战力 = Σ 每个单位的战力 = Σ 造价 / 比例常数 = 兵力预算 / 常数。
于是**只需要标定一条曲线**,而不是「预算曲线」加「预算怎么变成战力」两条。
这条在标定时省的功夫比看起来多:少一层非线性映射。

### 三、波间隔与收入率在预算上只有乘积进去

守方每波拿到的金币 = 收入率 × 波间隔。所以这两个数**不是两个自由度**。
推论:波间隔应当由**别的**约束钉死（episode 长度 1200–2400 tick、
建造与训练耗时、演示可读性),然后收入率去解那个目标比值。
出矿速率与矿点密度同理——只有乘积进预算,而**分布形态**是另一回事
（它管「围哪一簇」那个决策,已由地图规范定成「大散居、小聚居、交错杂居」)。

## 已知局限

本模型只算**预算与战力的总量**,不算对局。它答得了「拐点在第几波」,
答不了「那一波实际会不会破城」——后者要 `bindings/` + `train/` 之后跑真实对局。
所以它的用途是**给标定划出可行域**,不是给出终值。

订正（2026-09-02,详见 攻守实力模型与平衡分析.md §8 第 1 条）:
本模型的「战力」全程是 **ΣB**（单位 HP×DPS 之和）的旧货币——攻方、守方两侧
同币种,所以**内部比较（拐点存不存在）仍自洽**;但任何一列的绝对值都不能
读成场上兑现的战力（那是 Σ√B,兑现率随编成形状变),跨模型对比请用
`siege_race.py`。
"""
import argparse
import json
import os
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
STATS_PATH = os.path.join(REPO_ROOT, "game", "data", "stats_placeholder.json")

# ---------------------------------------------------------------------------
# 占位值,全部集中在这里。**一个都没有标定过。**
# 攻守两侧的形状取自 `波次预算曲线与堡垒等级曲线.md` §1/§2（那份文档也写明
# 系数是占位旋钮）,这里只是把它们放到一处好一起改。
# ---------------------------------------------------------------------------
PLACEHOLDER = {
    # 攻方
    "slots_base": 6.0,          # 编成位起步
    "slots_per_wave": 1.2,      # 编成位每波 +
    "slots_cap": 27,            # 硬顶。「有硬顶」是结构（CLAUDE.md）,而取值
                                # **2026-09-05 第一次有依据**（此前是 None,
                                # 因为 40 是随手标的）。三条依据:
                                #   1. SMAC 最大的官方图 27m_vs_30m 恰是 27 个
                                #      agent,官方难度「Super Hard」,默认 QMIX
                                #      只有 56% 胜率、调参后才 100%
                                #   2. 我们每 agent 的观测是 SMAC 的 18-21 倍
                                #      （3166 float vs ~150-176）,所以取文献
                                #      安全带（20-30）的**下沿**
                                #   3. 找不到任何一例「参数共享 PPO + ~3000 维
                                #      观测 + 40 个 agent」收敛的公开工作
                                # **语义同日改成「编队数」**:一队 = 同兵种 1-3
                                # 个,于是 27 个 agent 对应约 70 个单位。
                                # 顶的是 agent 数（RL 可训练规模）,不是单位数。
                                # 订正（2026-09-02）:硬顶改变兑现率（Σ√B,
                                # 攻守实力模型 §2）与 Ram 阈值（同文档 §3）,
                                # **改**难度 —— 那条已兑现:ram_permille
                                # 100 -> 250 重标,因为 Ram 单独成队 ⇒ 台数 =
                                # 队数,旧值在 27 个编队下只给 2 台（旧 7 台）
    "power_base": 6.0,          # 兵力预算系数
    "power_alpha": 1.25,        # 兵力预算指数（超线性 ⇒ > 1,这一条是结构；
                                # 1.6 -> 1.25 见《波次预算曲线与堡垒等级曲线.md》§1,
                                # 低波段 1.6 太陡,而结构只要求 > 1）
    # 等级战力:战力倍率 = (1 + k(L-1))^(p+q)。k 与 p+q 都是数值。
    "level_k": 0.22,
    "level_pq": 1.0,
    # 守方
    "wave_interval_ticks": 1800,   # 波间隔。落在 thresholds.json 的 episode 区间内
    "inner_mine_count": 2,         # 城内金矿数
    "mine_income_per_period": 11,  # 单座金矿每结算周期产金（数值表里的 income_amount）
    "keep_income_per_period": 2,   # 堡垒的兵力地板（同上）
    # 解禁:第几波放出一批城外矿,每批几座。**这是主难度旋钮。**
    "unlock_schedule": {5: 1, 10: 1, 16: 2, 24: 2, 34: 3},
    # 守方拿到城外矿要付代价（要出城守),这里用一个「实际吃到的比例」粗略表达
    "outer_capture_frac": 0.6,
    # 人口上限 = pop_base + pop_per_keep_level * K。**这两个数曾经漂过**：
    # 2026-09-02 定版 8+2K 并落地为 `WorldInit::pop_cap_base` /
    # `pop_cap_per_keep_level`，而这里一直留着被推翻的原占位 20+4K
    # （《波次预算曲线与堡垒等级曲线》§2.1 的结论正是「20+4K 是装饰品」）。
    # 2026-09-03 订正。**真相来源是 `rts/world.hpp` 的那两个默认值**，
    # 这里只是复述——改的时候两处一起改（同 slots/power 那四个的纪律）。
    "pop_base": 8,
    "pop_per_keep_level": 2,
    # 每波结束后守方战力存量留下的比例（战损 + 墙塔被拆 + 维修开销一并折进去）。
    # 粗略占位,真值只能从真实对局测。
    "retention_per_wave": 0.75,
}


def load_stats():
    with open(STATS_PATH, encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# 曲线
# ---------------------------------------------------------------------------

def slots(w, P=PLACEHOLDER):
    """编成位(w):线性;硬顶只在 slots_cap 非 None 时生效（取值待实测,见 PLACEHOLDER）。"""
    s = P["slots_base"] + P["slots_per_wave"] * (w - 1)
    cap = P["slots_cap"]
    return s if cap is None else min(float(cap), s)


def power_budget(w, P=PLACEHOLDER):
    """兵力预算(w):超线性,无上限。"""
    return P["power_base"] * (w ** P["power_alpha"])


def level_power_mult(L, P=PLACEHOLDER):
    """一个 L 级单位的战力倍率 = (1 + k(L-1))^(p+q)。"""
    return (1.0 + P["level_k"] * (L - 1)) ** P["level_pq"]


def nominal_level(w, P=PLACEHOLDER):
    """本波名义等级。

    **由「人均预算 ÷ 人均造价」反解,不是「预算 ÷ 编成位」。** 后者只在
    `cost(L) = base·L` 且 base=1 时才对;这里把攻方侧的人均造价假设写成
    `base·(1 + k(L-1))`,于是 `slots · base · (1 + k(L-1)) = 预算` ⇒ 解出 L。

    订正（2026-09-02,详见 攻守实力模型与平衡分析.md §8 第 3 条）:
    公式**保留**,但理由改——它最初是为了让「总战力 ≡ 兵力预算由构造成立」
    （文件头结构结论二,已被 §8 第 1 条订正为不成立）。现在它的定位是:
    攻方那条「兵力预算」按定义就是战力预算（§12.6:攻方无经济、不是真扣钱),
    本函数只是把人均预算按 p=1 的人均造价假设折成等级;守方造价
    `c ∝ √B(L)` 落地时已定攻方反解**不跟**（§12.6 已决,
    `demo_driver.cpp::wave_level` 旁有同源注释）,两侧差异归标定时校准。
    """
    per_unit = power_budget(w, P) / slots(w, P)
    # per_unit = base·(1 + k(L-1))，取 base = P["power_base"] / P["slots_base"]
    # 使第 1 波恰好 L=1（那是 `波次预算曲线与堡垒等级曲线.md` §1 验的关键点）
    base = power_budget(1, P) / slots(1, P)
    return 1.0 + (per_unit / base - 1.0) / P["level_k"]


def attacker_power(w, P=PLACEHOLDER):
    """攻方本波总战力（**ΣB 口径**,单位 HP×DPS 之和)。

    订正（2026-09-02,详见 攻守实力模型与平衡分析.md §8 第 1 条）:
    ~~结构结论二:`cost ∝ 战力` ⇒ 总战力 ∝ 兵力预算~~ —— 那条已在文件头
    订正为不成立:兑现到场上的是 Σ√B 而非 ΣB(兑现率随波数与编成形状变,
    §2 实测第 5 波 43%、第 20 波 32%)。本函数仍按「编成位 × 单位战力」
    显式算一遍,返回值只在**与守方同币种（ΣB）比较**时有意义
    （`defender_stock_power` 也是 ΣB);别把它的绝对值读成场上战力,
    按 Σ√B 口径的模型在同目录 `siege_race.py`。
    """
    return slots(w, P) * level_power_mult(nominal_level(w, P), P)


def unlocked_outer_mines(w, P=PLACEHOLDER):
    return sum(n for wave, n in P["unlock_schedule"].items() if wave <= w)


def defender_gold_this_wave(w, P=PLACEHOLDER):
    """守方本波拿到的金币。

    城内是线性（矿数固定,收入率固定)+ 堡垒地板;城外按解禁与实际吃到的比例。
    """
    stats = load_stats()
    period = stats["global"]["income_period_ticks"]
    periods = P["wave_interval_ticks"] / period
    inner = P["inner_mine_count"] * P["mine_income_per_period"]
    keep = P["keep_income_per_period"]
    outer = (unlocked_outer_mines(w, P) * P["mine_income_per_period"]
             * P["outer_capture_frac"])
    return (inner + keep + outer) * periods


def defender_buyable_power(w, P=PLACEHOLDER):
    """守方用**本波**金币能买到的战力（上界:全部拿去买兵、不修不建）。

    与攻方同一把尺子（**ΣB 口径**）:比例常数取 `Archer` 的
    `cost_gold ÷ 战力(1级)`（1 级处 c ∝ √B 与原假设一致,因为 √B(1)=1）。
    守方的战力单位因此与攻方可比——但两侧都是旧货币,比较结果只在
    ΣB 口径内自洽,场上兑现要按 Σ√B（见 `attacker_power` 的订正注,
    攻守实力模型与平衡分析.md §8 第 1 条）。
    """
    stats = load_stats()
    archer = stats["units"]["Archer"]
    gold_per_power = archer["cost_gold"]      # 1 级 Archer 战力记为 1
    return defender_gold_this_wave(w, P) / gold_per_power


def defender_stock_power(w, P=PLACEHOLDER):
    """守方到第 w 波累积下来的战力存量。

    **攻方每波的预算是流量,守方是存量 —— 这两个不能直接比,初版就是这么比的。**
    攻方的单位一波打完基本死光（亡灵不在乎伤亡,是消耗品),下一波拿一份新预算;
    守方的墙、塔、活着的兵**留到下一波**。所以守方那一侧要累加。

    `retention` 是每波结束后留下来的比例,一个粗略的占位:它把战损、
    墙塔被拆、维修花掉的钱一并折进去。真值只能从真实对局测,
    所以这里刻意用一个数而不是一套损耗模型——假装算得细并不会让它更准。
    """
    stock = 0.0
    for i in range(1, w + 1):
        stock = stock * P["retention_per_wave"] + defender_buyable_power(i, P)
    return stock


# ---------------------------------------------------------------------------
# 结构检查。**这几条不是「数值合不合适」,是「机制存不存在」。**
# ---------------------------------------------------------------------------

def check_structure(P=PLACEHOLDER, waves=60):
    problems = []

    # 一、造价曲线与游戏实现同阶（2026-09-02 起是 `cost_L ∝ √B(L)`——
    #     `World::train_cost_gold` = base × level_permille(L, k),与血量/伤害
    #     同一条曲线）。§12.7 的教训:这里写自己的假设等于没查,本公式必须
    #     与 `train_cost_gold` 逐字同源,改一边不改另一边就是那次事故的复刻。
    #     在这条曲线下「买 1 个 L 级」严格质占优（战力 B^(p+q)=B^1 vs 同价
    #     堆量只有 √B）——这是**设计意图**（§12.5:凿点用质、铺开用量）,
    #     不是回归;挡住「只堆精英」的是人口上限 8+2K 与兵种等级上限 = 堡垒
    #     等级,不是造价。本条因此反过来断言「倾斜方向不变」:若有人把 k 或
    #     p+q 改到堆量反超,那是结构变了,应当红。
    base_cost = 1.0
    for L in (2, 5, 10, 20):
        cost_L = base_cost * (1.0 + P["level_k"] * (L - 1)) ** 0.5   # 造价 ∝ √B(L)
        power_L = level_power_mult(L, P)                              # 战力 = B(L)^(p+q)
        n_equiv = cost_L / base_cost                                  # 同价能买几个 1 级
        power_stack = n_equiv * level_power_mult(1, P)
        if power_L < power_stack:
            problems.append(
                f"堆级不再占优（L={L}）：买 1 个 {L} 级得战力 {power_L:.2f}，"
                f"同价买 {n_equiv:.2f} 个 1 级得 {power_stack:.2f} —— "
                f"§12.5 定的「凿点用质」结构翻了。若这是有意改设计,"
                f"请同步改 `World::train_cost_gold` 与本检查的断言方向,"
                f"别让两处各存一份公式（§12.7）。")

    # 二、必须存在拐点:只靠城内保底、龟缩到撑不住的那一波
    #     （CLAUDE.md「两条硬性曲线要求」）。
    #     **比的是守方存量 vs 攻方流量**,理由见 `defender_stock_power`。
    inner_only = dict(P)
    inner_only["unlock_schedule"] = {}
    inner_only["outer_capture_frac"] = 0.0
    turning = None
    for w in range(1, waves + 1):
        if defender_stock_power(w, inner_only) < attacker_power(w, P):
            turning = w
            break
    if turning is None:
        problems.append(
            f"{waves} 波之内没有拐点：只靠城内保底就一直买得起足以对等的兵力，"
            f"于是龟缩不被惩罚，野战机制（冲锋助跑 / 枪阵 / 风筝）与"
            f"「攻其必救」全部失效（CLAUDE.md「两条硬性曲线要求」）")

    # 三、编成位必须真的打满,否则「多余兵力只能投等级」这条结构从不生效。
    #     slots_cap 为 None（取值待实测）时挂起——不是检查废了,是它检查的
    #     那个数暂时不存在;填回取值时这条自动恢复。
    if P["slots_cap"] is not None and slots(waves, P) < P["slots_cap"]:
        problems.append(
            f"第 {waves} 波编成位才 {slots(waves, P):.1f}，没到硬顶 "
            f"{P['slots_cap']} —— 「编成位耗尽后多余兵力只能投入等级」"
            f"这条结构在可见波数内从不生效")

    # 四、兵力预算必须超线性（CLAUDE.md:攻方那条无界成长轴)。
    if P["power_alpha"] <= 1.0:
        problems.append(
            f"power_alpha = {P['power_alpha']} ≤ 1，兵力预算不是超线性 —— "
            f"那条无界成长轴没了，无尽模式不再「最终一定会输」")

    return problems, turning


def main():
    ap = argparse.ArgumentParser(description="预算与节奏曲线模型")
    ap.add_argument("--waves", type=int, default=40, help="看到第几波")
    args = ap.parse_args()
    P = PLACEHOLDER
    W = args.waves

    print("=== 预算与节奏曲线（全部数值为占位值，一个都没标定过） ===\n")
    print(f"{'波':>3} {'编成位':>7} {'兵力预算':>9} {'名义等级':>8} {'攻方战力':>9} "
          f"{'城外矿':>6} {'守方金币':>9} {'守方存量':>9} {'守/攻':>7}")
    print("-" * 78)
    shown = [w for w in range(1, W + 1) if w <= 5 or w % 5 == 0]
    for w in shown:
        ap_ = attacker_power(w, P)
        dp = defender_stock_power(w, P)
        print(f"{w:>3} {slots(w, P):>7.1f} {power_budget(w, P):>9.1f} "
              f"{nominal_level(w, P):>8.1f} {ap_:>9.1f} "
              f"{unlocked_outer_mines(w, P):>6} "
              f"{defender_gold_this_wave(w, P):>9.0f} {dp:>9.1f} "
              f"{dp / ap_:>7.2f}")
    print("\n「守方存量」是累积的（墙塔留着、活兵留着，每波按 retention 折旧），")
    print("而「攻方战力」是每波一份新预算 —— 两侧不是同一种量，见 defender_stock_power。")

    problems, turning = check_structure(P, waves=W)
    print()
    if turning:
        print(f"拐点（只靠城内保底就买不起对等兵力）：第 {turning} 波")

    # ---- 诊断:存量为什么会饱和,以及那决定了存活波数 ----
    #
    # 存量递推是 S(w) = r·S(w-1) + 收入(w)。收入若**不随波数增长**,几何级数
    # 收敛到 收入/(1-r) —— 也就是说守方存量有一个**与波数无关的天花板**,
    # 而攻方按 w^α 无界增长。于是存活波数几乎完全由「收入增长 vs α」决定,
    # 跟 retention 只差一个常数倍。
    #
    # 这不是缺陷:CLAUDE.md 明写无尽模式下玩家最终一定会输、要看的是
    # **中位存活波数**。但它说明一件事 —— 想把局拉长,调 retention 或起始收入
    # 都只是平移,**只有让收入随波数增长才改变斜率**,而那条路 CLAUDE.md 也
    # 已经指名了:「主要的难度旋钮是解禁节奏与强度曲线之比」。
    r = P["retention_per_wave"]
    inner_income = defender_gold_this_wave(1, P)
    stats = load_stats()
    ceiling = (inner_income / stats["units"]["Archer"]["cost_gold"]) / (1.0 - r)
    print()
    print("诊断：")
    print(f"  收入不增长时，守方存量的天花板 = 每波收入 ÷ (1 − retention) "
          f"= {ceiling:.1f}（与波数无关）")
    w_cross = next((w for w in range(1, 500) if attacker_power(w, P) > ceiling), None)
    if w_cross:
        print(f"  攻方战力在第 {w_cross} 波超过这个天花板 —— 此后靠城内收入"
              f"**无论攒多久**都追不上")
    print(f"  所以调 retention 或起始收入只是平移，改斜率只能靠让收入随波数增长")
    print(f"  （解禁节奏）或压低兵力预算指数 α（现 {P['power_alpha']}）。")
    print()
    if problems:
        print(f"!! 结构检查 {len(problems)} 条不通过：\n")
        for p in problems:
            print(f"  - {p}\n")
        return 1
    slot_note = ("编成位打满" if P["slots_cap"] is not None
                 else "编成位硬顶未设、该条挂起")
    print(f"结构检查全部通过（堆级质占优方向不变（c ∝ √B，§12.5） / 拐点存在 / {slot_note} / 预算超线性）。")
    print("\n注意这只说明**机制存在**，不说明数值合适——本模型只算总量、不算对局，")
    print("「那一波会不会真的破城」要等 bindings/ + train/ 落地后跑真实对局。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
