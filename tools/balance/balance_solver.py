# -*- coding: utf-8 -*-
"""守方的**联立约束优化**：堡垒等级 K 同时是三件事的价格，配平要在这三者上取极小。

## 为什么必须联立

守方的建筑等级上限由 `Keep` 等级给出，而 `Keep` 升级要花石材。所以「把塔升级」
不是免费的替代方案。更要紧的是 **K 同时买三样东西**，而它们在同一份石材里竞争：

    K  ──►  建筑等级上限 ceil(K / building_level_cap_divisor)   （塔更强）
       ──►  人口上限 8 + 2K                                     （弓手更多）
       ──►  兵种等级上限 = K                                     （弓手更强）

于是「守住第 w 波」是一个三元的可行性问题，不是一条曲线：

    找 (K, L_d, 弓手投入) 使
        石材：(K−1)·keep_up + 入口数 · D(w, L_d, A) · (tower_build + (L_d−1)·tower_up) ≤ S_石(w)
        金币：A_total · cost_gold(L_a) ≤ S_金(w)
        人口：A_total + 4 ≤ 8 + 2K
    其中 D(·) 是门前拦住主攻群所需的塔数，由 `siege_race` 的入口攻防实测。

**余量 M(w) = 1 / min over 可行组合 of max(石材占用率, 金币占用率)**；M 跌破 1 的那一波
就是这个模型下的陷落波。M(w) 的**形状**（而不是某个点）才是配平的对象。

## 两条由 c ∝ √B 直接推出的性质（落地于 #130，本模型据此简化）

- 单位造价 `c ∝ √B(L)` 而战力也 ∝ √B(L) ⇒ **每金买到的战力与等级无关**。
  于是「多招几个」与「招高级的」在金币上等价，只在**人口**上不等价：
  一名 L 级弓手占 1 人口却顶 √B(L) 名 1 级弓手。**人口一咬住，就该升级而不是加人。**
  所以模型里弓手只记「1 级当量」`A_eff`，金币开销 ∝ A_eff、人口开销 = A_eff / √B(K)。
- 建筑造价 `80 + 32(L−1)` 是**线性**的，而塔的火力 ∝ √B(L)。两者不同阶 ⇒
  升级严格劣于多建（§一的实测：第 20 波升到 12 级要多花 3.2 倍石材只省 2.3 倍塔）。
  **这正是 `数值设计` §12 给单位修掉、却没给建筑修的那条。** 见 `--fix` 档。

## 玩家水平进模型的唯一一个量

`conc` = 门边弓手占全部弓手的比例。脚本均匀登墙是 `13/(8R)` ≈ 0.16，
人类按免费方向提示调兵是 0.5–0.8。它是**技术差**在这个模型里的全部体现，
所以「配平得好不好」的一条判据是：`conc` 从 0.16 走到 0.8，陷落波应当拉开
足够的差距（技术要有回报），但不该差到低分段无法游玩。
"""
import argparse
import functools
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import siege_race as sr
import economy_race as er

G, Bd, U = sr.G, sr.Bd, sr.U

TOWER_BUILD_S, TOWER_BUILD_W = Bd['Tower']['cost_stone'], Bd['Tower']['cost_wood']
TOWER_UP_S, TOWER_UP_W = Bd['Tower']['upgrade_cost_stone'], Bd['Tower']['upgrade_cost_wood']
KEEP_UP_S, KEEP_UP_W = Bd['Keep']['upgrade_cost_stone'], Bd['Keep']['upgrade_cost_wood']
CAP_DIV = G['building_level_cap_divisor']
ARCHER_GOLD = U['Archer']['cost_gold']
POP_BASE, POP_PER_K = 8, 2          # WorldInit::pop_cap_base / pop_cap_per_keep_level
POP_OTHER = 4                        # 2 枪 + 1 骑 + 1 工匠，开局就有、不算进弓手


# ============================================================ 需求一侧
@functools.lru_cache(maxsize=None)
def _opens(w, ld, arch_eff, nt, alloc, flak, R, D):
    """给定塔数，门破不破。用于二分——它对 nt 单调。"""
    if alloc == 'old':
        ro, L = sr.old_roster(w)
    else:
        ro, L, _s, _b = sr.roster(w, alloc)
    main, _ = sr.split(ro)
    d = sr.fresh_defender(R, towers_total=0)
    d['towers_gate'] = [nt, nt]
    d['flak_gate'] = [1 if flak else 0, 1 if flak else 0]
    d['archers'] = [1] * int(round(arch_eff))
    d['bld_level'] = ld
    opened, *_ = sr.fight_entry(main, L, D, R, d, 0, 'gate', arch_conc=1.0)
    return opened


@functools.lru_cache(maxsize=None)
def demand(w, ld, arch_eff=0, alloc='current', flak=True, R=10, D=40, cap=80):
    """门前拦住主攻群所需的 `ld` 级塔数（二分；None = 超过 `cap`）。"""
    if not _opens(w, ld, arch_eff, cap, alloc, flak, R, D):
        lo, hi = 0, cap
        while lo < hi:
            mid = (lo + hi) // 2
            if _opens(w, ld, arch_eff, mid, alloc, flak, R, D):
                lo = mid + 1
            else:
                hi = mid
        return lo
    return None


def sq(L):
    return sr.sq(L)


# ============================================================ 价目
class Prices:
    """一处改价、全模型跟着走。`--fix` 档只动这里，不动数值表。"""

    def __init__(self, keep_up=KEEP_UP_S, tower_build=TOWER_BUILD_S,
                 tower_up_mode='linear', tower_up=TOWER_UP_S,
                 cap_div=CAP_DIV, pop_base=POP_BASE, pop_per_k=POP_PER_K,
                 archer_gold=ARCHER_GOLD):
        self.keep_up = keep_up
        self.tower_build = tower_build
        self.tower_up_mode = tower_up_mode
        self.tower_up = tower_up
        self.cap_div = cap_div
        self.pop_base = pop_base
        self.pop_per_k = pop_per_k
        self.archer_gold = archer_gold

    def tower_cost(self, ld):
        """一座 `ld` 级塔的总石材（建 + 升到该级）。"""
        if self.tower_up_mode == 'sqrt':
            # 与单位同构：造价 ∝ √B(L) ⇒ 每石买到的火力与等级无关
            return self.tower_build * sq(ld)
        return self.tower_build + (ld - 1) * self.tower_up

    def bld_cap(self, K):
        return max(1, math.ceil(K / self.cap_div))

    def pop(self, K):
        return self.pop_base + self.pop_per_k * K

    def archer_gold_for(self, a_eff):
        """A_eff 个 1 级当量的金币开销。c ∝ √B ⇒ 与等级无关，只与当量成正比。"""
        return a_eff * self.archer_gold


# ============================================================ 联立优化
def solve_wave(w, s_stone, g_flow, conc, px, gates=2, alloc='current',
               k_max=28, ld_set=None):
    """一波的可行性：石材按**存量**记、金币按**流量**记。

    分开记是实测逼出来的（`资源点分布与经济平衡.md` 的续篇）：在「刚好守住」的塔数上
    跑入口攻防，**塔零损失、弓手全灭**。塔是永久存量，一次买断；弓手每波全额重置，
    是流量。用同一套记法会得出「守方永不缺资源」的错误结论——那正是余量曲线
    单调上升的成因。

    返回 (占用率, K, L_d, 门边弓手当量, 石材存量, 金币流量, 瓶颈)。
    占用率 = max(石材存量/累计石材, 金币流量/每波金入)，在所有可行组合上取最小。
    """
    best = None
    for K in range(1, k_max + 1):
        stone_keep = (K - 1) * px.keep_up
        if stone_keep > s_stone:
            break
        pop_slots = max(0, px.pop(K) - POP_OTHER)
        # 人口一咬住就升级而不是加人（c ∝ √B ⇒ 金币上等价、人口上不等价）
        a_total_eff_max = pop_slots * sq(K)
        lds = ld_set if ld_set is not None else range(1, px.bld_cap(K) + 1)
        for ld in lds:
            if ld > px.bld_cap(K):
                continue
            for step in range(0, 17):
                a_gate = a_total_eff_max * conc * step / 16.0
                d = demand(w, ld, int(round(a_gate)), alloc=alloc)
                if d is None:
                    continue
                stone = stone_keep + gates * d * px.tower_cost(ld)
                gold = px.archer_gold_for(a_gate / conc) if conc > 0 else 9e18
                use = max(stone / s_stone if s_stone > 0 else 9e9,
                          gold / g_flow if g_flow > 0 else 9e9)
                if best is None or use < best[0]:
                    bn = ('石材' if (stone / s_stone) >= (gold / g_flow) else '金币')
                    best = (use, K, ld, a_gate, stone, gold, bn)
                if d == 0:
                    break
    return best if best else (9e9, None, None, None, None, None, '无解')


# ============================================================ 供给一侧
def supply(m, waves, band=40, wave_secs=65.0, keep_frac=1.0):
    """地图给出的累计石 / 金（内环带已解禁的点全铺、扣掉建矿本身、无袭扰）。上界。"""
    per = G['income_period_ticks'] / 20.0
    periods = wave_secs / per
    out, cum_s, cum_g, nq, nm = [], 0.0, 0.0, 0, 0
    for w in range(1, waves + 1):
        q = sum(1 for s in m['outer']
                if s['wave'] <= w and s['dist'] <= band and s['type'] == 'stone')
        g = sum(1 for s in m['outer']
                if s['wave'] <= w and s['dist'] <= band and s['type'] == 'gold')
        cum_s -= (q - nq) * Bd['Quarry']['cost_stone']
        cum_g -= 0
        cum_s -= (g - nm) * Bd['Mine']['cost_stone']
        nq, nm = q, g
        r_s = Bd['Quarry']['income_amount'] * (1 + nq * er.TIER_PERMILLE['outer'] / 1000.0)
        r_g = (Bd['Mine']['income_amount'] * (1 + nm * er.TIER_PERMILLE['outer'] / 1000.0)
               + Bd['Keep']['income_amount'])
        cum_s += r_s * periods
        cum_g += r_g * periods
        # 石材记累计（塔是存量），金币记**每波流量**（弓手每波全灭，实测）
        out.append(dict(w=w, q=nq, m=nm, s=max(1.0, cum_s) * keep_frac,
                        g=max(1.0, r_g * periods)))
    return out




# ============================================================ 参数扫描
def eval_set(m, px, sigma, waves, concs, gates=2, alloc='current', band=40):
    """给定一套价目与供给缩放，返回每个 conc 的 M 曲线与跌破波。"""
    sup = supply(m, max(waves), band=band)
    out = {}
    for conc in concs:
        ms, cross = [], None
        for w in waves:
            row = sup[w - 1]
            use, K, ld, ag, st, gd, bn = solve_wave(
                w, row['s'] * sigma, row['g'] * sigma, conc, px,
                gates=gates, alloc=alloc)
            mm = 1.0 / use if use else 9e9
            ms.append(mm)
            if cross is None and mm < 1.0:
                cross = w
        out[conc] = (ms, cross)
    return out


def shape_score(ms, waves, target_cross):
    """配平判据打分（越小越好）。三项：起点余量、单调下降、跌破位置。"""
    import math as _m
    pen = 0.0
    pen += abs(_m.log(max(ms[0], 1e-6) / 1.3))                 # 起点该在 1.3 左右
    for a, b in zip(ms, ms[1:]):                                # 该单调不升
        if b > a * 1.02:
            pen += 0.5 * _m.log(b / a)
    # 跌破 1 的位置（线性插值）
    cw = None
    for i in range(len(ms) - 1):
        if ms[i] >= 1.0 > ms[i + 1]:
            t = (ms[i] - 1.0) / max(ms[i] - ms[i + 1], 1e-9)
            cw = waves[i] + t * (waves[i + 1] - waves[i])
            break
    if cw is None:
        cw = waves[-1] * (2.0 if ms[-1] >= 1.0 else 0.5)
    pen += abs(_m.log(cw / target_cross))
    return pen, cw


def mode_scan(a):
    maps = [er.load_map(p) for p in sorted(glob.glob(os.path.join(er.POOL, '*.json')))]
    m = maps[a.map_idx]
    waves, concs = a.waves_list, a.conc_list
    cands = []
    for tb in (80, 160, 240, 360):
        for mode in ('linear', 'sqrt'):
            for sigma in (1.0, 0.5, 0.3, 0.2):
                for cd in (2, 1):
                    cands.append((tb, mode, sigma, cd))
    print(f'== 参数扫描（{m["name"]}，目标：conc={a.target_conc} 在第 {a.target} 波跌破 1）==')
    print('   判据 = |ln(M起点/1.3)| + 单调性罚 + |ln(跌破波/目标)|，越小越好')
    print(' 塔造价 升级曲线 供给x 上限除数   M 曲线(conc=%.2f)                    跌破  分' % a.target_conc)
    rows = []
    for tb, mode, sigma, cd in cands:
        px = Prices(tower_build=tb, tower_up_mode=mode, cap_div=cd)
        r = eval_set(m, px, sigma, waves, [a.target_conc], gates=a.gates, alloc=a.alloc)
        ms, _c = r[a.target_conc]
        sc, cw = shape_score(ms, waves, a.target)
        rows.append((sc, tb, mode, sigma, cd, ms, cw))
    rows.sort()
    for sc, tb, mode, sigma, cd, ms, cw in rows[:12]:
        cells = ' '.join(f'{x:5.2f}' for x in ms)
        print(f'  {tb:4d}   {mode:6s}  {sigma:4.2f}    {cd:2d}      {cells}   '
              f'{cw:5.1f}  {sc:5.2f}')
    print('   波次：      ' + '        ' + ' '.join(f'{w:5d}' for w in waves))
    print()
    best = rows[0]
    print(f'== 最优候选：塔 {best[1]} 石、升级 {best[2]}、供给 ×{best[3]}、'
          f'等级上限除数 {best[4]} 的技术梯度 ==')
    px = Prices(tower_build=best[1], tower_up_mode=best[2], cap_div=best[4])
    r = eval_set(m, px, best[3], waves, [0.16, 0.4, 0.6, 0.8], gates=a.gates, alloc=a.alloc)
    for conc in (0.16, 0.4, 0.6, 0.8):
        ms, cw = r[conc]
        print(f'   conc={conc:.2f}  ' + ' '.join(f'{x:5.2f}' for x in ms)
              + f'   跌破：第 {cw} 波' if cw else
              f'   conc={conc:.2f}  ' + ' '.join(f'{x:5.2f}' for x in ms) + '   守住')


# ============================================================ 报告
def margin_curve(m, px, conc, waves, gates=2, alloc='current', band=40):
    sup = supply(m, max(waves), band=band)
    rows = []
    for w in waves:
        row = sup[w - 1]
        use, K, ld, ag, st, gd, bn = solve_wave(w, row['s'], row['g'], conc, px,
                                                gates=gates, alloc=alloc)
        rows.append(dict(w=w, M=(1.0 / use if use else 9e9), K=K, ld=ld, ag=ag,
                         stone=st, gold=gd, need=bn, sup_s=row['s'], sup_g=row['g']))
    return rows


def cross_wave(rows):
    for r in rows:
        if r['M'] < 1.0:
            return r['w']
    return None


def mode_upgrade(a):
    print('== 一、建筑升级在现价下是不是划算 ==')
    print('   塔：建 %d 石，升 %d 石/级（**线性**）；火力 ∝ √B(L)（**开方**）'
          % (TOWER_BUILD_S, TOWER_UP_S))
    print('   判据：同样的石材，买「多座低级塔」还是「少座高级塔」拦得住')
    px = Prices()
    for w in (10, 20):
        print(f'\n   第 {w} 波（门边 6 名弓手当量）：')
        print('    L_d  需塔/门  单座石材  总石材(2门)  相对 L_d=1')
        base = None
        for ld in range(1, 13):
            d = demand(w, ld, 6)
            if d is None:
                continue
            tot = 2 * d * px.tower_cost(ld)
            base = base or tot
            print(f'    {ld:3d}   {d:5d}    {px.tower_cost(ld):7.0f}   {tot:9.0f}'
                  f'      {tot/base:5.2f}x')
    print('\n   ⇒ 单调上升 ⇒ **最优档恒为 1 级，「建筑等级上限」这个输出是装饰品**。')
    print('   成因与 `数值设计与成本产出矩阵.md` §12 给单位修掉的那条同型：')
    print('   造价线性、战力开方 ⇒ 每石买到的火力 ∝ 1/√L，堆量严格占优。')
    print('\n   同一张表，改成 塔造价 ∝ √B(L)（`--fix` 档）：')
    pf = Prices(tower_up_mode='sqrt')
    for w in (10, 20):
        best = min(((ld, demand(w, ld, 6)) for ld in range(1, 13)),
                   key=lambda x: 2 * x[1] * pf.tower_cost(x[0]) if x[1] else 9e9)
        ld, d = best
        b1 = 2 * demand(w, 1, 6) * pf.tower_cost(1)
        print(f'    第 {w} 波：最优 L_d={ld}，需 {d} 座/门，'
              f'总石材 {2*d*pf.tower_cost(ld):.0f}（L_d=1 要 {b1:.0f}）')


def mode_margin(a):
    maps = [er.load_map(p) for p in sorted(glob.glob(os.path.join(er.POOL, '*.json')))]
    m = maps[a.map_idx]
    px = Prices()
    print(f'== 二、余量曲线 M(w)（{m["name"]}，{a.gates} 个入口）==')
    print('   M = 供给 ÷ 最省的一套可行方案；M < 1 = 守不住')
    for conc in a.conc_list:
        rows = margin_curve(m, px, conc, a.waves_list, gates=a.gates)
        cw = cross_wave(rows)
        cells = '  '.join(f'{r["M"]:5.2f}' for r in rows)
        print(f'   conc={conc:.2f}  M: {cells}   跌破 1：第 {cw} 波'
              if cw else f'   conc={conc:.2f}  M: {cells}   全程守住')
    print('   波次：' + '  '.join(f'{w:5d}' for w in a.waves_list))
    print()
    print(f'   逐波明细（conc={a.conc_list[-1]}）：')
    print('    波     M    最优K  L_d  门边弓手当量  石需/石供      金流/金入    瓶颈')
    for r in margin_curve(m, px, a.conc_list[-1], a.waves_list, gates=a.gates):
        if r['K'] is None:
            print(f'    {r["w"]:2d}   无解')
            continue
        print(f'    {r["w"]:2d}  {r["M"]:6.2f}   {r["K"]:3d}  {r["ld"]:3d}    '
              f'{r["ag"]:7.1f}     {r["stone"]:6.0f}/{r["sup_s"]:<7.0f} '
              f'{r["gold"]:7.0f}/{r["sup_g"]:<7.0f} {r["need"]}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', nargs='?', default='all',
                    choices=['upgrade', 'margin', 'scan', 'all'])
    ap.add_argument('--gates', type=int, default=2)
    ap.add_argument('--map-idx', type=int, default=0)
    ap.add_argument('--band', type=int, default=40)
    ap.add_argument('--waves', default='3,5,8,10,12,15,20,25')
    ap.add_argument('--conc', default='0.16,0.4,0.6,0.8')
    ap.add_argument('--alloc', default='current')
    ap.add_argument('--target', type=float, default=20.0, help='目标跌破波')
    ap.add_argument('--target-conc', type=float, default=0.5)
    a = ap.parse_args()
    a.waves_list = [int(x) for x in a.waves.split(',')]
    a.conc_list = [float(x) for x in a.conc.split(',')]
    if a.mode in ('upgrade', 'all'):
        mode_upgrade(a)
        print()
    if a.mode in ('margin', 'all'):
        mode_margin(a)
        print()
    if a.mode in ('scan', 'all'):
        mode_scan(a)


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    main()
