# -*- coding: utf-8 -*-
"""把**地图上的资源点分布**接进入口赛跑模型（`siege_race.py` 的经济那一半）。

## 为什么要单开这一份

`siege_race.py` 的收入是三个常数（`Quarry`/`Lumber`/`Mine` 各一座 + `Keep` 地板），
`tier` 倍率、解禁表、簇数、采集建筑的造价与工时**一个都没进去**；分布只以一个
外生标量 `income_growth` 出现，而 §4 由它得出「收入增长对陷落波零影响」。
那个结论成立的前提是「崩盘发生在收入生效之前」，而那个崩盘时刻本身
（第 3 波）是**按一座采石场算出来的**——供给一侧被钉死了。

本文件把供给一侧换成地图的实测分布：读一张池图，按 `unlock_wave` 逐簇解禁、
按 `tier` 乘产出倍率、采集建筑要花石木与工时、工匠数量与人口上限限制铺开速度。
`siege_race` 的入口攻防（`fight_entry`/`fight_inside`）与攻方曲线（`roster`）
原样复用——**只换经济，不换战斗**，两份结论才可比。

## 模型里的经济（可复算）

- 城内：3 个点（1 石 1 木 1 金），开局各预置一座采集建筑，`tier=inner` ⇒ 倍率 1.0
- 城外：逐簇解禁（同簇同波），`tier=outer` ⇒ 倍率 `outer_permille/1000`（现 1.5）
- 一座采集建筑：花 `cost_stone`/`cost_wood`，工时 `build_ticks ÷ 在场工匠数`
- 工匠：速度 0.08 格/tick ⇒ 往返 `2·dist/0.08/20` 秒；一名工匠同时只在一处
- 人口上限 `8+2K` 对**工匠与弓手共同**生效——铺开速度与门边火力争同一批人口
- 攻方袭扰（可选）：城外采集建筑 500 hp 无人防守，按选定兵种的对结构 dps 拆

## 明确没建模的（与 `siege_race` §1.8 并列）

工匠被点杀（偏守方）；采集建筑挡路与提供视野（偏守方）；`Ranger` 护矿（偏守方）；
城外建筑被拆后重建的决策（模型直接重建）；玩家把采集建筑围进城（那会让
袭扰失效，偏守方）；外环簇要穿过集结点环（偏守方——模型只算距离不算风险）。
"""
import argparse
import collections
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import siege_race as sr

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
POOL = os.path.join(REPO, 'game', 'data', 'maps', 'pool')

# `WorldInit::tier_income_permille` 的占位值（2026-09-02 落地，PR #129）。
# **不是从数值表读的**——它是与数值表并列的外生输入，唯一真相在 `world.hpp`
# 的默认值与 `game/src/map_loader.cpp`；这里抄一份并在 --check-tier 下核对。
TIER_PERMILLE = {'inner': 1000, 'outer': 1500}

GATHERER = {'stone': 'Quarry', 'wood': 'Lumber', 'gold': 'Mine'}
MASON_SPEED = sr.U['Mason']['speed']
MASON_GOLD = sr.U['Mason']['cost_gold']
ARCHER_GOLD = sr.U['Archer']['cost_gold']


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def load_map(path):
    d = json.load(open(path, encoding='utf-8'))
    keep = d['keep']
    R = max(cheb(w['pos'], keep) for w in d['initial_walls'])
    inner, outer = [], []
    for r in d['resources']:
        site = dict(type=r['type'], tier=r['tier'], wave=r['unlock_wave'],
                    dist=cheb(r['pos'], keep))
        (inner if r['tier'] == 'inner' else outer).append(site)
    pre = collections.Counter(b['type'] for b in d['buildings'])
    outer.sort(key=lambda s: (s['wave'], s['dist']))
    return dict(name=os.path.basename(path), R=R, size=d['size'][0],
                inner=inner, outer=outer, pre=pre,
                towers_pre=pre.get('Tower', 0), spawns=len(d['spawns']))


def income_of(built):
    """built: [(type, tier)] → 每 `income_period_ticks` 的 (石, 木, 金)。"""
    out = {'stone': 0.0, 'wood': 0.0, 'gold': 0.0}
    for t, tier in built:
        amt = sr.Bd[GATHERER[t]]['income_amount']
        out[t] += amt * TIER_PERMILLE[tier] / 1000.0
    out['gold'] += sr.Bd['Keep']['income_amount']   # 兵力地板
    return out['stone'], out['wood'], out['gold']


def payback_table():
    """一座采集建筑的回本期（秒），内外两档。"""
    rows = []
    for t, b in GATHERER.items():
        s = sr.Bd[b]
        for tier in ('inner', 'outer'):
            per = s['income_amount'] * TIER_PERMILLE[tier] / 1000.0
            # 回本按「它自己产的那种资源」算：石矿产石、回的是石造价那一份
            secs = s['cost_stone'] / per * (sr.G['income_period_ticks'] / 20.0) \
                if t == 'stone' else s['cost_wood'] / per * (sr.G['income_period_ticks'] / 20.0)
            rows.append((b, tier, per, s['cost_stone'], s['cost_wood'], secs))
    return rows


# ---------------------------------------------------------------- 经济状态
class Eco:
    def __init__(self, m, masons=1, eco_first=True, max_dist=None):
        self.m = m
        self.built = [(s['type'], 'inner') for s in m['inner']]   # 开局三座预置
        self.taken = set()
        self.masons = masons
        self.eco_first = eco_first
        self.max_dist = max_dist if max_dist is not None else 10 ** 9
        self.raided = 0
        self.built_outer = []      # [(type, dist)]，袭扰要按距离挑

    def unlocked(self, w):
        return [(i, s) for i, s in enumerate(self.m['outer'])
                if s['wave'] <= w and i not in self.taken and s['dist'] <= self.max_dist]

    def build_cost(self, t):
        b = sr.Bd[GATHERER[t]]
        return b['cost_stone'], b['cost_wood']

    def job_secs(self, dist):
        """一名工匠铺一座城外采集建筑的占用时长（往返 + 工时，单工匠）。"""
        travel = 2.0 * dist / MASON_SPEED / 20.0
        return travel + sr.Bd['Quarry']['build_ticks'] / 20.0

    def expand(self, w, secs, stone, wood):
        """本波用 `secs` 秒、`stone`/`wood` 存量铺城外采集建筑，就近优先。

        返回 (花掉的石, 花掉的木, 新建座数)。工匠并行 ⇒ 总工时预算 = masons × secs。
        """
        budget = self.masons * secs
        ds, dw, n = 0.0, 0.0, 0
        for i, s in self.unlocked(w):
            cs, cw = self.build_cost(s['type'])
            need = self.job_secs(s['dist'])
            if budget < need or stone - ds < cs or wood - dw < cw:
                continue
            budget -= need
            ds += cs
            dw += cw
            n += 1
            self.taken.add(i)
            self.built.append((s['type'], 'outer'))
            self.built_outer.append((s['type'], s['dist']))
        return ds, dw, n

    def raid(self, n):
        """攻方拆掉 n 座城外采集建筑（先拆最近的——顺路，也最容易到）。"""
        if n <= 0 or not self.built_outer:
            return 0
        self.built_outer.sort(key=lambda x: x[1])
        hit = 0
        for _ in range(min(n, len(self.built_outer))):
            t, _d = self.built_outer.pop(0)
            self.built.remove((t, 'outer'))
            hit += 1
        self.raided += hit
        return hit


def raid_capacity(group, L, secs):
    """一支分出去的部队在 `secs` 秒里能拆几座 500 hp 的采集建筑。"""
    hp = sr.Bd['Quarry']['max_hp']
    dps = sum(sr.usdps(t, L) * n for t, n in group.items())
    return int(dps * secs // hp) if hp > 0 else 0


# ---------------------------------------------------------------- 多波循环
def run_eco(m, alloc='current', D=40, seal=True, aoe_mult=2.0, arch_conc=None,
            masons=1, mason_target=1, eco_first=True, max_dist=None,
            raid_frac=0.0, raid_from=3, pop_cap=True, keep_frac=0.33,
            max_waves=40, wave_secs_cap=0.0, verbose=False):
    """`siege_race.run` 的经济替换版：战斗一字不改，收入改由地图给出。"""
    R = m['R']
    dfd = sr.fresh_defender(R, towers_total=max(1, m['towers_pre']))
    eco = Eco(m, masons=masons, eco_first=eco_first, max_dist=max_dist)
    n_spawn = m['spawns']
    rows = []
    for w in range(1, max_waves + 1):
        if w == 1 and seal and dfd['stone'] >= 30:
            dfd['stone'] -= 30
            dfd['breach_open'] = 0
        if dfd['keep_fund'] >= 200 and dfd['wood'] >= 200:
            dfd['keep_fund'] -= 200
            dfd['wood'] -= 200
            dfd['K'] += 1
        cap = 8 + 2 * dfd['K'] if pop_cap else 10 ** 9
        # 招工匠：人口上限内优先补到 mason_target，剩下的口子给弓手
        while (eco.masons < mason_target and dfd['gold'] >= MASON_GOLD
               and len(dfd['archers']) + dfd['spears'] + dfd['rangers'] + eco.masons + 1 <= cap):
            dfd['gold'] -= MASON_GOLD
            eco.masons += 1
        # 铺城外采集建筑（上一波的时长当作本波可用工时的估计；第 1 波只有建造期）
        secs = rows[-1]['dur_total'] if rows else 13.0
        if eco_first:
            ds, dw, nb = eco.expand(w, secs, dfd['stone'], dfd['wood'])
            dfd['stone'] -= ds
            dfd['wood'] -= dw
        else:
            nb = 0
        while dfd['stone'] >= 80 and dfd['wood'] >= 30:
            dfd['stone'] -= 80
            dfd['wood'] -= 30
            g = 0 if dfd['towers_gate'][0] <= dfd['towers_gate'][1] else 1
            dfd['towers_gate'][g] += 1
        if not eco_first:
            ds, dw, nb = eco.expand(w, secs, dfd['stone'], dfd['wood'])
            dfd['stone'] -= ds
            dfd['wood'] -= dw
        while (dfd['gold'] >= ARCHER_GOLD
               and len(dfd['archers']) + dfd['spears'] + dfd['rangers'] + eco.masons + 1 <= cap):
            dfd['gold'] -= ARCHER_GOLD
            dfd['archers'].append(1)
        dfd['bld_level'] = max(1, math.ceil(dfd['K'] / 2))

        if alloc == 'old':
            ro, L = sr.old_roster(w)
        else:
            ro, L, slots, budget = sr.roster(w, alloc)
        # 攻方分兵袭扰经济：按 raid_frac 从非 Ram/Phoenix 里抽一支去拆采集建筑
        raiders = {}
        if raid_frac > 0 and w >= raid_from:
            for t in ('Ghoul', 'Knight'):
                k = int(ro.get(t, 0) * raid_frac)
                if k > 0:
                    raiders[t] = k
                    ro[t] -= k
        main, feint = sr.split(ro)
        face = w % n_spawn
        if face in (0, 2):
            kind_main, g_main = 'gate', face // 2
        elif face == 1:
            kind_main, g_main = ('breach' if dfd['breach_open'] else 'detour'), 0
        else:
            kind_main, g_main = 'detour', 1
        opened, t_open, rem, loss, dur, entered = sr.fight_entry(
            main, L, D, R, dfd, g_main, kind_main, aoe_mult=aoe_mult,
            arch_conc=arch_conc)
        keep_dmg = sr.fight_inside(entered, L, dfd, dfd['keep_hp']) if entered else 0.0
        f_face = (face + 1) % n_spawn
        if f_face in (0, 2):
            kind_f, g_f = 'gate', f_face // 2
        elif f_face == 1:
            kind_f, g_f = ('breach' if dfd['breach_open'] else 'detour'), 0
        else:
            kind_f, g_f = 'detour', 1
        f_conc = None if arch_conc is None else max(0.0, (1 - arch_conc) * 0.5)
        opened_f, _, rem_f, loss_f, dur_f, entered_f = sr.fight_entry(
            feint, L, D, R, dfd, g_f, kind_f, aoe_mult=aoe_mult, arch_conc=f_conc)
        keep_dmg += sr.fight_inside(entered_f, L, dfd, dfd['keep_hp'] - keep_dmg) if entered_f else 0.0
        dfd['keep_hp'] -= keep_dmg
        keep_after = dfd['keep_hp'] / sr.bhp('Keep')

        dur_total = 260 / 20 + max(dur, dur_f)
        hit = eco.raid(raid_capacity(raiders, L, dur_total)) if raiders else 0
        # 波长上限只作用于**入账**：模型里门守住而攻方又死不掉时，`fight_entry`
        # 会跑到 `t_end`（约 460 s），一波白送 90 个入账周期。那种对峙在真机里
        # 会被「攻方持续砸门」终止，所以它是模型 artifact，不是收入。
        # 默认不设限（保持与 `siege_race` 同口径），`--cap` 给出敏感性。
        eff = min(dur_total, wave_secs_cap) if wave_secs_cap else dur_total
        periods = int(eff * 20 // sr.G['income_period_ticks'])
        s_in, w_in, g_in = income_of(eco.built)
        dfd['keep_fund'] += s_in * periods * keep_frac
        dfd['stone'] += s_in * periods * (1 - keep_frac)
        dfd['wood'] += w_in * periods
        dfd['gold'] += g_in * periods
        repair_hp = sum((1 - f) for f in dfd['gate_hp_frac']) * sr.bhp('Gate', dfd['bld_level'])
        repair_hp += max(0.0, sr.bhp('Keep') - dfd['keep_hp'])
        wood_cost = repair_hp * sr.G['repair_wood_per_1000hp'] / 1000
        if dfd['wood'] >= wood_cost:
            dfd['wood'] -= wood_cost
            dfd['gate_hp_frac'] = [1.0, 1.0]
            dfd['keep_hp'] = sr.bhp('Keep')

        rows.append(dict(w=w, L=L, opened=opened, keep=keep_after, dur_total=dur_total,
                         archers=len(dfd['archers']), towers=dfd['towers_gate'][:],
                         K=dfd['K'], masons=eco.masons, gath=len(eco.built),
                         new=nb, raided=hit, s_in=s_in * periods,
                         stone=dfd['stone'], wood=dfd['wood'], gold=dfd['gold']))
        if verbose:
            r = rows[-1]
            print(f"w{w:2d} L={L:2d} 采集={r['gath']:3d}(+{nb}"
                  f"{',-%d' % hit if hit else ''}) 工匠={r['masons']} K={r['K']} "
                  f"石入={r['s_in']:6.0f} 塔={r['towers']} 弓={r['archers']:2d} "
                  f"堡={keep_after:4.0%} 门={'开' if opened else '守'} 用时={dur_total:3.0f}s")
        if keep_after <= 0:
            return w, rows
    return None, rows


# ---------------------------------------------------------------- 模式
def mode_dist():
    """一、资源点分布的实测（12 张池图）。"""
    maps = [load_map(p) for p in sorted(glob.glob(os.path.join(POOL, '*.json')))]
    print('== 池图资源分布（12 张）==')
    print(' 图            边长  R  城内点        城外点  簇数  解禁波  内环簇距  外环簇距')
    for m in maps:
        clus = collections.defaultdict(list)
        for s in m['outer']:
            clus[s['wave']].append(s)
        near = [min(x['dist'] for x in v) for k, v in clus.items() if k <= (len(clus) + 1) // 2]
        far = [min(x['dist'] for x in v) for k, v in clus.items() if k > (len(clus) + 1) // 2]
        ci = collections.Counter(s['type'] for s in m['inner'])
        print(f" {m['name'][:12]}  {m['size']:3d} {m['R']:3d}  "
              f"石{ci['stone']} 木{ci['wood']} 金{ci['gold']}      "
              f"{len(m['outer']):3d}   {len(clus):3d}   2-{max(clus):2d}   "
              f"{min(near):3.0f}-{max(near):3.0f}   {min(far):3.0f}-{max(far):3.0f}")
    print()
    print('== 一座采集建筑的回本期 ==')
    print(' 建筑     tier    每5s产  造价石 造价木  回本')
    for b, tier, per, cs, cw, secs in payback_table():
        print(f' {b:8s} {tier:6s}  {per:5.1f}   {cs:4d}  {cw:4d}   {secs:5.1f}s')
    print()
    m = maps[0]
    print(f"== 供给 vs 需求（{m['name']}，就近全铺、无袭扰的上界）==")
    print(' 波  已解禁城外点  可铺石矿  石入/波  可建塔/波  §3 门前需求(6弓)  差')
    demand = {3: 2, 4: 4, 5: 8, 6: 9, 8: 11, 10: 14, 12: 16}
    for w in (3, 4, 5, 6, 8, 10, 12):
        unl = [s for s in m['outer'] if s['wave'] <= w]
        q = sum(1 for s in unl if s['type'] == 'stone')
        s_in = (9 + q * 9 * 1.5) * 14          # 14 个周期/波（波长约 70 s）
        print(f' {w:2d}    {len(unl):3d}         {q:3d}    {s_in:7.0f}   '
              f'{s_in / 80:6.1f}     {demand[w]:3d}          '
              f'{s_in / 80 / 2 - demand[w]:+6.1f}')
    print(' 注：「可建塔/波」是全部石材建塔的上界；「差」按两座门平摊后与 §3 需求相比。')


def mode_race(args):
    """二、把经济接进赛跑，跑几种守方策略。"""
    maps = [load_map(p) for p in sorted(glob.glob(os.path.join(POOL, '*.json')))]
    print('== 陷落波（12 张池图，中位 / 全部）==')
    cases = [
        ('A 模型原样口径：只有城内三座、不铺城外', dict(masons=1, mason_target=1, max_dist=0)),
        ('B 铺城外、1 名工匠、不招（现状 demo 的守方脚本）', dict(masons=1, mason_target=1)),
        ('C 铺城外、招到 4 名工匠', dict(masons=1, mason_target=4)),
        ('D C + 弓手 60% 集中主攻门', dict(masons=1, mason_target=4, arch_conc=0.6)),
        ('E C + 只铺内环带（距 keep ≤ 40）', dict(masons=1, mason_target=4, max_dist=40)),
        ('F C + 攻方分三成兵去拆采集建筑（第 3 波起）', dict(masons=1, mason_target=4, raid_frac=0.3)),
        ('G C + 攻方分五成', dict(masons=1, mason_target=4, raid_frac=0.5)),
        ('H C + 先建塔后铺矿（eco_first=False）', dict(masons=1, mason_target=4, eco_first=False)),
    ]
    for name, kw in cases:
        res = []
        for m in maps:
            sw, rows = run_eco(m, max_waves=args.waves, wave_secs_cap=args.cap, **kw)
            res.append(sw if sw else args.waves + 1)
        res.sort()
        med = res[len(res) // 2]
        shown = ['守住' if x > args.waves else str(x) for x in res]
        print(f' {name:46s} 中位={("守住" if med > args.waves else med):>4} '
              f'全部={",".join(shown)}')
    print()
    print('== 把「Ram 等级曲线」这个压制项拿掉，再看经济（否则崩盘早于经济生效）==')
    print('   §4 已证「只买人」永不破门；旧曲线守到 20 波以上。经济要在这两个')
    print('   口径下看，才能回答「城外分布会不会让守方过强」。')
    late = [
        ('只买人(L=1) + 不铺城外', dict(alloc='flat', masons=1, mason_target=1, max_dist=0)),
        ('只买人(L=1) + 铺城外、4 工匠', dict(alloc='flat', masons=1, mason_target=4)),
        ('旧曲线 + 不铺城外', dict(alloc='old', masons=1, mason_target=1, max_dist=0)),
        ('旧曲线 + 铺城外、4 工匠', dict(alloc='old', masons=1, mason_target=4)),
        ('旧曲线 + 铺城外 + 攻方分五成拆矿', dict(alloc='old', masons=1, mason_target=4, raid_frac=0.5)),
    ]
    for name, kw in late:
        res, towers, gath = [], [], []
        for m in maps:
            sw, rows = run_eco(m, max_waves=args.waves, wave_secs_cap=args.cap, **kw)
            res.append(sw if sw else args.waves + 1)
            towers.append(sum(rows[-1]['towers']))
            gath.append(rows[-1]['gath'])
        res.sort()
        med = res[len(res) // 2]
        print(f' {name:36s} 中位陷落={("守住" if med > args.waves else med):>4} '
              f'末波塔数中位={sorted(towers)[6]:3d} 采集建筑中位={sorted(gath)[6]:3d}')
    print()
    print(f"== 逐波轨迹（{maps[0]['name']}，旧曲线 + 铺城外 + 4 工匠）==")
    run_eco(maps[0], alloc='old', masons=1, mason_target=4,
            max_waves=min(20, args.waves), wave_secs_cap=args.cap, verbose=True)


def mode_need():
    """三、反解：要让供给贴着需求走，城外该有多少个点。"""
    m = load_map(sorted(glob.glob(os.path.join(POOL, '*.json')))[0])
    print('== 反解：城外石点该有几个 ==')
    print('判据：第 w 波石材收入刚好够把「§3 门前需求」两门都建满（不留余量）。')
    print(' 波  §3 需求(6弓,两门)  累计需石  该有的城外石点  地图实际(已解禁)')
    demand = {3: 2, 4: 4, 5: 8, 6: 9, 8: 11, 10: 14, 12: 16}
    for w in (3, 4, 5, 6, 8, 10, 12):
        need_towers = demand[w] * 2
        need_stone = need_towers * 80
        # 累计石材 = Σ_{i<=w} 14×(9 + 13.5·q)  ⇒ 解出 q
        q = (need_stone / (14 * w) - 9) / 13.5
        act = sum(1 for s in m['outer'] if s['wave'] <= w and s['type'] == 'stone')
        print(f' {w:2d}       {need_towers:3d}          {need_stone:5d}        '
              f'{max(0.0, q):5.1f}            {act:3d}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', nargs='?', default='dist',
                    choices=['dist', 'race', 'need', 'all'])
    ap.add_argument('--waves', type=int, default=25)
    ap.add_argument('--cap', type=float, default=0.0,
                    help='入账用的波长上限（秒）；0 = 不设限，与 siege_race 同口径')
    a = ap.parse_args()
    if a.mode in ('dist', 'all'):
        mode_dist()
        print()
    if a.mode in ('need', 'all'):
        mode_need()
        print()
    if a.mode in ('race', 'all'):
        mode_race(a)


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    main()
