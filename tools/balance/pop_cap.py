# -*- coding: utf-8 -*-
"""守方人口上限：把它放到**攻方那条曲线旁边**去判，而不是只判「咬不咬得住」。

## 为什么要重开这个（2026-09-03，试玩反馈「2 级 12 太少了，比敌人还少」）

`波次预算曲线与堡垒等级曲线.md` §2.1 定 `8+2K` 时只用了**一条**判据：

> **上限必须咬得住** —— 即「上限 < 玩家把全部金币买 1 级兵能养的数量」。
> 咬不住时金币是唯一约束，两个输出（人口上限、兵种等级上限）一起变装饰品。

那条判据是对的，但它**只看守方自己的钱包，一次都没有看攻方**。于是它选出的
公式满足「咬得住」，却可能咬到「守方的兵力天花板追不上攻方」——而 §2.1(5)
自己已经描述过这个失效形态：

> 「顶住之后升级的效率只有 1/4.5，守方战力会**撞上一堵墙**而不是继续爬坡，
>  `必败` 会来得又早又突然。」

§2.1(5) 把成因只归给**造价曲线**（那条 2026-09-02 已修：`c ∝ √B(L)`）。
本文补的是：**上限本身也能造出同一堵墙**，而且它现在就是那堵墙。

## 三条判据（第一条是既有的，后两条是本文补的）

- **C1 咬得住**（§2.1 原判据）：`cap(K) < 金币能养的稳态数量`。
  不满足 ⇒ 人口上限与兵种等级上限一起是装饰品。
- **C2 不是天花板**：守方**能买到的最大战力** `cap(K) × √B(等级上限(K))`
  必须跟得上攻方同波的 `Σ√B`，直到目标陷落波。
  不满足 ⇒ 顶住之后既不能加人、也不能加质，取舍消失、只剩天花板。
  **注意 C1 与 C2 不矛盾**：C1 要求**数量**被咬住，C2 要求**数量 × 质量**
  这个乘积别被咬住——两者同时成立，才是「把余钱换成质量」那条设计。
- **C3 天花板要够得着**：`K` 必须真的涨得起来。`K` 由 `Keep` 升级驱动
  （200 石 + 200 木 + 400 tick/级），而那笔钱与墙、塔抢同一份石材。
  实测（校准 runner，12 张池图）**每局只升 1 次**、`K` 停在 2 ⇒
  上限恒 12、`ceil(K/2)=1` ⇒ 建筑等级上限一次都没打开过。
  不满足 ⇒ 公式里的 `K` 是自由变量，而现实里它是常数，曲线的形状无关紧要。

用法:
    py tools/balance/pop_cap.py            # 三条判据 + 候选公式对比
    py tools/balance/pop_cap.py --waves 30
"""
import argparse
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
STATS = os.path.join(HERE, '..', '..', 'game', 'data', 'stats_placeholder.json')

# 攻方曲线：与 `game::WaveCurve` 的默认值、`budget_curves.py` 的 PLACEHOLDER 同源。
SLOTS_BASE, SLOTS_PER_WAVE = 6.0, 1.2
POWER_BASE, POWER_ALPHA = 6.0, 1.25
# 守方现行公式（`WorldInit::pop_cap_base` / `pop_cap_per_keep_level`）。
POP_BASE, POP_PER_K = 8, 2
# 开局编成（`demo_init`）：3 Archer + 2 Spear + 1 Ranger + 1 Mason。
START_POP = 7
# 其中非战斗单位占的人口。**`defender_pop()` 数所有守方单位，工匠与斥候
# 一样吃格子**——这一条是本文里最容易漏的：宏观层要靠工匠施工/维修/升级，
# 所以「有效战斗人口」比上限低，而低多少取决于你想同时开几个工地。
NONCOMBAT_MIN = 1


def load():
    with open(STATS, encoding='utf-8') as f:
        return json.load(f)


def slots_at(w):
    return max(1, int(SLOTS_BASE + SLOTS_PER_WAVE * (w - 1)))


def power_at(w):
    return POWER_BASE * (w ** POWER_ALPHA)


def wave_level(w, k):
    """攻方名义等级。逐字照 `game::wave_level`（含那个第 1 波锚点与四舍五入）。"""
    if k <= 0:
        return 1
    per_unit = power_at(w) / slots_at(w)
    base = POWER_BASE / SLOTS_BASE
    lv = 1.0 + (per_unit / base - 1.0) / k
    return max(1, int(lv + 0.5))


def sq(L, k):
    """√B(L)，与 `rts::level_permille` 同一条曲线（这里用浮点，只作分析）。"""
    return math.sqrt(1.0 + k * (L - 1))


def atk_power(w, k):
    """攻方一波**兑现**的战力：Σ√B（不是 ΣB——见《攻守实力模型与平衡分析》§2）。"""
    return slots_at(w) * sq(wave_level(w, k), k)


def def_ceiling(K, k, pop_base=POP_BASE, pop_per=POP_PER_K, lv_cap=None):
    """守方**买得到的最大** Σ√B：人口上限 × √B(兵种等级上限)。

    兵种等级上限默认 = K（`World::unit_level_cap()`，无除数）。
    """
    cap = pop_base + pop_per * K
    L = K if lv_cap is None else lv_cap(K)
    return cap, cap * sq(L, k)


# 一波多长（tick）与结算周期数：**实测值**，不是占位推算。校准 runner 的
# 12 局 × 4 波平均波长约 2100 tick，而 `income_period_ticks` 从数值表读。
WAVE_TICKS = 2100.0


def gold_sustainable(mines_outer, cost_gold, income_period, mine_income,
                     keep_income, retention=0.75):
    """金币能养的稳态数量（§2.1(2) 的算法，但周期数与产出都从数值表读）。

    留存率 0.75 是那一节的占位；**结论的方向不随它变**（§2.1(7)）。
    """
    periods = WAVE_TICKS / income_period
    per_period = mine_income * (1 + mines_outer) + keep_income   # 城内恒 1 座
    gold_per_wave = per_period * periods
    return gold_per_wave / cost_gold / (1.0 - retention)


def runner_report(path):
    """读校准 runner 的 JSON，回答「三种资源里到底哪一种咬得住」。

    起因是一条试玩反馈：「堡垒陷落时金币还剩 2000 左右」。这个模式存在的理由是
    **那件事在 runner 的输出里此前完全不可见**——逐波记录里一个资源数都没有
    （2026-09-03 补上 `stone/wood/gold_start|end`、`pop`、`pop_cap`、`train_bld`）。
    """
    with open(path, encoding='utf-8') as f:
        d = json.load(f)
    runs = d['runs']
    print('== 三种资源哪一种咬得住（%s，%d 局）==' % (os.path.basename(path), len(runs)))
    print('   判据：一种资源若在**波末**长期堆积，它就没有 sink ⇒ 它那条决策轴是死的')
    print()
    names = (('stone', '石'), ('wood', '木'), ('gold', '金'))
    print('   资源  波末中位  波末最大  末波中位  「涨了」的波占比')
    for key, cn in names:
        ends = [w[key + '_end'] for r in runs for w in r['waves']]
        last = [r['waves'][-1][key + '_end'] for r in runs]
        up = sum(1 for r in runs for w in r['waves']
                 if w[key + '_end'] > w[key + '_start'])
        tot = sum(len(r['waves']) for r in runs)
        ends.sort()
        last.sort()
        print('   %s     %7d  %8d  %8d   %d/%d'
              % (cn, ends[len(ends) // 2], max(ends), last[len(last) // 2], up, tot))
    print()
    at_cap = sum(1 for r in runs for w in r['waves']
                 if w['pop_cap_end'] > 0 and w['pop_end'] >= w['pop_cap_end'])
    tot = sum(len(r['waves']) for r in runs)
    print('   波末人口顶到上限：%d/%d 波' % (at_cap, tot))
    tb = [w['train_bld_end'] for r in runs for w in r['waves']]
    print('   出兵建筑数（Barrack + Keep）：中位 %d，最大 %d'
          % (sorted(tb)[len(tb) // 2], max(tb)))
    print('   ——一座一次只练一名，所以它是补员速率的分母。')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--waves', type=int, default=20)
    ap.add_argument('--runner', help='校准 runner 的 JSON：改判「哪种资源咬得住」')
    a = ap.parse_args()

    if a.runner:
        runner_report(a.runner)
        return

    d = load()
    k = d['global']['hp_permille_per_level'] / 1000.0
    archer_gold = d['units']['Archer']['cost_gold']
    keep_up_s = d['buildings']['Keep']['upgrade_cost_stone']
    keep_up_w = d['buildings']['Keep']['upgrade_cost_wood']
    income_period = d['global']['income_period_ticks']
    mine_income = d['buildings']['Mine']['income_amount']
    keep_income = d['buildings']['Keep']['income_amount']

    print('== 守方人口上限：三条判据 ==')
    print('   现行 cap(K) = %d + %dK，兵种等级上限 = K，k = %.2f' %
          (POP_BASE, POP_PER_K, k))
    print('   攻方 slots(w) = %g + %g(w-1)，兵力预算 = %g·w^%g，'
          '名义等级由造价反解' % (SLOTS_BASE, SLOTS_PER_WAVE, POWER_BASE, POWER_ALPHA))
    print()

    # —— C2：把攻方那条曲线和守方的天花板并排 ——
    print('-- C2 不是天花板：守方买得到的最大 Σ√B vs 攻方兑现的 Σ√B --')
    print('   守方那一列按「K 停在实测值 2」和「K 一路顶到 w/2」两个极端各算一次')
    print()
    print('    波  攻方人数 名义级 攻方Σ√B | K=2: 上限 Σ√B  倍率 | K=w/2: K 上限 Σ√B  倍率')
    for w in range(1, a.waves + 1):
        ap_ = atk_power(w, k)
        cap2, dp2 = def_ceiling(2, k)
        Kf = max(1, w // 2)
        capf, dpf = def_ceiling(Kf, k)
        print('    %2d   %5d  %5d  %8.1f | %4d %8.1f  %5.2fx | %3d %4d %8.1f  %5.2fx'
              % (w, slots_at(w), wave_level(w, k), ap_,
                 cap2, dp2, dp2 / ap_, Kf, capf, dpf, dpf / ap_))
    print()
    print('    「倍率」= 守方天花板 ÷ 攻方兑现。< 1 = **就算把上限用满、等级顶到上限**，')
    print('    守方的兵也堆不出攻方那么多战力（塔与墙不占人口，是另一份，见下）。')

    # —— C1：咬得住 ——
    print()
    print('-- C1 咬得住（§2.1 原判据，重算一遍确认没被 C2 的修法破坏）--')
    print('    城外 Mine  金币稳态可养   cap(1)  cap(2)  cap(4)  cap(8)')
    for mo in (0, 1, 2, 4):
        s = gold_sustainable(mo, archer_gold, income_period, mine_income, keep_income)
        row = '    %8d   %10.1f  ' % (mo, s)
        for K in (1, 2, 4, 8):
            cap = POP_BASE + POP_PER_K * K
            row += '%6s ' % ('%d✓' % cap if cap < s else '%d✗' % cap)
        print(row)

    # —— C3：K 涨得起来吗 ——
    print()
    print('-- C3 天花板够得着：K 由 Keep 升级驱动（%d 石 + %d 木 + %d tick/级）--'
          % (keep_up_s, keep_up_w, d['buildings']['Keep']['upgrade_ticks']))
    print('    石+木流量按 §2.1(4) 的 347/波算；投入比例是玩家的决策')
    for frac in (0.10, 0.20, 0.33, 0.50):
        per_level = (keep_up_s + keep_up_w) / (347.0 * frac)
        print('      投 %2d%% ⇒ 每级 %4.1f 波 ⇒ 第 %d 波 K=%d，上限 %d'
              % (frac * 100, per_level, a.waves,
                 1 + int(a.waves / per_level),
                 POP_BASE + POP_PER_K * (1 + int(a.waves / per_level))))
    print('    ⚠️ 实测（校准 runner，12 张池图 × --macro）：**每局只升 1 次、K 停在 2**。')
    print('       升级的钱与墙塔抢同一份石材，而塔是当波就要用的。')

    # —— 候选公式 ——
    print()
    print('== 候选公式对比 ==')
    cands = [
        ('8+2K   （现行）', 8, 2, None),
        ('12+3K', 12, 3, None),
        ('16+4K', 16, 4, None),
        ('12+2K  仅抬基数', 12, 2, None),
        ('8+2K + 等级上限 2K', 8, 2, lambda K: 2 * K),
        ('12+3K + 等级上限 2K', 12, 3, lambda K: 2 * K),
    ]
    print('   判据：C1 在城外 0 矿时 cap(2) 是否 < 稳态可养（下面那个数）；')
    print('         C2 在 K=2（实测轨迹）时倍率 >= 1 的最后一波')
    print()
    s0 = gold_sustainable(0, archer_gold, income_period, mine_income, keep_income)
    print('   （城外 0 矿的稳态可养 = %.1f 名 1 级弓手）' % s0)
    print('   %-22s cap(1) cap(2) cap(6)  C1@K=2  C2最后守得住的波（K=2）' % '公式')
    for name, pb, pp, lv in cands:
        cap1 = pb + pp * 1
        cap2 = pb + pp * 2
        cap6 = pb + pp * 6
        c1 = '✓咬' if cap2 < s0 else '✗松'
        last = 0
        for w in range(1, 61):
            _c, dp = def_ceiling(2, k, pb, pp, lv)
            if dp / atk_power(w, k) >= 1.0:
                last = w
        print('   %-22s %5d  %5d  %5d   %-6s  %d' %
              (name, cap1, cap2, cap6, c1, last))
    # —— C4：这一轮真正的发现 ——
    print()
    print('-- 比价：200 石 + 200 木，升堡垒 vs 直接建塔 --')
    print('   这是宏观层每一轮真在做的比较，也是「K 为什么不动」的全部答案。')
    a_ = d['units']['Archer']
    t_ = d['buildings']['Tower']
    adps = a_['damage'] * 20.0 / a_['cooldown_ticks']
    tdps = t_['damage'] * 20.0 / t_['cooldown_ticks']
    n = keep_up_s / t_['cost_stone']
    print('     升 1 级堡垒 ⇒ +%d 人口 = %d 名弓手 = %.1f dps / %d hp，'
          '另付 %d 金' % (POP_PER_K, POP_PER_K, POP_PER_K * adps,
                          POP_PER_K * a_['max_hp'], POP_PER_K * a_['cost_gold']))
    print('                  兵种等级上限 +1（**只对以后招的兵生效**——编队移除后')
    print('                  就地升级已删）、建筑等级上限 +0.5')
    print('     同样 %d 石   ⇒ %.1f 座箭楼 = %.1f dps / %d hp，**不占人口、不花金**'
          % (keep_up_s, n, n * tdps, int(n * t_['max_hp'])))
    print('     ⇒ 火力 %.1f 倍、血量 %.1f 倍，还省 %d 金。'
          % (n * tdps / (POP_PER_K * adps),
             n * t_['max_hp'] / (POP_PER_K * a_['max_hp']),
             POP_PER_K * a_['cost_gold']))
    print('     **「一局只升 1 次堡垒」不是脚本写坏了，是这个比价下的正确答案。**')
    print('     于是 cap(K) 里的 K 在实战中是常数 ⇒ 公式的**形状无关紧要**，')
    print('     玩家看到的永远是 cap(1..2) = 10..12。')

    print()
    print('-- 两侧的等级尺度对不上（独立于人口上限的第二个病）--')
    print('   攻方名义等级 = 人均预算反解、除以 k=%.2f ⇒ 每多 1 份人均预算 = %.1f 级'
          % (k, 1.0 / k))
    print('   守方兵种等级上限 = 堡垒等级 K，而 K 是一个「%d 石一级」的计数器'
          % keep_up_s)
    print()
    print('    波  攻方名义级  √B(攻)  守方K=2 √B(守)  单位战力差  克制倍率还剩')
    for w in (1, 2, 4, 8, 12, 20, 30):
        L = wave_level(w, k)
        ratio = (1 + k * (L - 1)) / (1 + k * 1)
        print('    %2d      %5d  %6.2f       2  %6.2f     %5.2fx        %6.2fx'
              % (w, L, sq(L, k), sq(2, k), sq(L, k) / sq(2, k), 1.0 / ratio))
    print('   CLAUDE.md §1.4：「p+q 太大则**克制关系被等级淹没**」，而克制比例')
    print('   随等级差衰减 ∝ B^-(p+q) = B^-1 ⇒ 第 8 波整张克制二部图被稀释 4.8 倍。')
    print('   **这一条与人口上限无关，改人口上限也修不掉它。**')

    # —— 让 K 动起来要多便宜 ——
    print()
    print('-- 若要 K 追上 w/2（那条让 Σ√B 比值稳在 ~0.57 的轨迹）--')
    for frac in (0.20, 0.33):
        need = 347.0 * frac * 2.0    # 每 2 波一级 ⇒ K ≈ w/2
        print('     投 %d%% 石木 ⇒ 每级总价须 <= %.0f（石+木），'
              '即约 %.0f 石 + %.0f 木；现价 %d + %d'
              % (frac * 100, need, need / 2, need / 2, keep_up_s, keep_up_w))

    print()
    print('   注：C2 那一列**不是陷落波**——它只说「守方的兵能不能单独顶住攻方的兵」。')
    print('   真实防线还有塔与墙（不占人口），所以陷落波会更晚；反过来，守方人口里')
    print('   还有工匠（`defender_pop()` 数所有单位），所以有效战斗人口更低。')
    print('   两者方向相反、量级都不小，**所以这一列只能当形状看，不能当预测**。')


if __name__ == '__main__':
    main()
