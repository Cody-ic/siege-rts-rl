# -*- coding: utf-8 -*-
"""一波攻城的「赛跑模型」——攻守实力的正确货币（`攻守实力模型与平衡分析.md` §1 的实现）。

用法（都从仓库任意位置运行，路径按本文件定位）：

    py tools/balance/siege_race.py pool      # §2  预算兑现率：Σ√B / ΣB
    py tools/balance/siege_race.py towers    # §3  拦住 Ram 门不破要几座塔（含旧曲线复原对照）
    py tools/balance/siege_race.py grid      # §4  多波单因素敏感性（陷落波）
    py tools/balance/siege_race.py base      # §4  基线与「弓手 60% 集中」的逐波轨迹

## 它替代的是什么

此前 `budget_curves.py` 把「单位 HP×DPS 之和」叫总战力并写成 `总战力 ≡ 兵力预算`。
那是量纲错：等级各开一份平方根之后，一波兑现到场上的**血池与火力都是 Σ√B、不是 ΣB**；
而且攻城不是消耗战、是入口处的赛跑（`Ram` 活着到门下 → 塔火下撑过破门 → 进城砸堡）。
本文件按后者建模。**结论与局限见同名文档，不在这里复述**（同一件事写在两处必然漂移）。

## 模型形状（细节以文档 §1 为准）

* 聚合量、0.25 s 步进、**不是逐单位仿真**；
* 缩放走 `combat_math.level_permille`（整数 isqrt，与引擎逐位一致）；
* 攻方编成、等级、主/佯攻切分照 `game/src/demo_driver.cpp`；行为照 demo 攻方脚本
  （够得着的人先打、`Ram` 砸结构、其余按 flow 走）；
* 守方照 `DefenderScript`（弓手均匀登墙 ⇒ 门边 13/(8R)；`arch_conc` 参数表示人类集中调兵）；
* 入口一律是门或缺口（flow field 代价下从不砸墙）；
* 「堡垒陷落」= 进城者赢下内城混战（2 枪 1 骑），这条假设偏严，文档 §7 的校准清单第一批。

## 校准前的用法边界

绝对波数只作**相对比较**用；最可能偏的三处（`Phoenix` 真实路径、塔的实际 AOE 命中数、
门边弓手比例）都是参数，校准（无渲染 runner）之后回来改默认值。
"""
import json
import math
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
from combat_math import level_permille  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
STATS_PATH = os.path.join(REPO_ROOT, "game", "data", "stats_placeholder.json")

doc = json.load(open(STATS_PATH, encoding='utf-8'))
U, Bd, G = doc['units'], doc['buildings'], doc['global']
K_LVL = G['hp_permille_per_level']


def sq(L):
    return level_permille(int(L), K_LVL) / 1000.0


def cad(u):
    return max(u['windup_ticks'] + u['cooldown_ticks'], 1)


def udps(n, L=1):
    u = U[n]
    return u['damage'] * sq(L) / cad(u) * 20


def usdps(n, L=1):
    u = U[n]
    return u['damage'] * sq(L) * u['vs_structure_permille'] / 1000 / cad(u) * 20


def uhp(n, L=1):
    return U[n]['max_hp'] * sq(L)


def bdps(n, L=1):
    b = Bd[n]
    return b['damage'] * sq(L) / cad(b) * 20


def bhp(n, L=1):
    return Bd[n]['max_hp'] * sq(L)


HG = (1 - G['high_ground_miss_permille'] / 1000) * G['high_ground_dmg_permille'] / 1000   # 0.49
SPEED = {n: U[n]['speed'] for n in U}


# ---------- 攻方曲线（照 demo_driver.cpp；候选甲/乙只用于比较） ----------
def roster(w, alloc='current'):
    if alloc in ('current', 'flat'):
        slots = int(6 + 1.2 * (w - 1))
    else:
        slots = min(40, int(6 + 3 * (w - 1)))
    alpha = 1.15 if alloc == 'yi' else 1.25
    budget = 6 * w ** alpha
    per = budget / slots
    L = 1 if per < 1 else int(1 + (per - 1) / (K_LVL / 1000) + 0.5)
    if alloc == 'flat':
        L = 1   # 对照：只买人不买级（总预算不守恒，只看形状）
    shades = max(1, slots // 5) if w >= 2 else 0
    knights = max(1, slots * 3 // 20) if w >= 2 else 0
    rams = max(1, slots // 10) if w >= 3 else 0
    phx = 1 if w >= 3 else 0
    wr = 1 if w >= 2 else 0
    ghouls = max(1, slots - shades - knights - rams - phx - wr)
    return dict(Ghoul=ghouls, Shade=shades, Knight=knights, Ram=rams, Phoenix=phx), L, slots, budget


def old_roster(w):
    """09-01 之前的占位曲线，按 demo_driver.cpp 注释近似复原（第 12 波封顶 18 个、
    等级每 3 波 +1、Ram 最多 4 台）。只用于定性对照。"""
    L = 1 + (w - 1) // 3
    ghouls = min(8, 3 + w // 2)
    shades = min(4, w // 3)
    knights = min(1, w // 4)
    rams = min(4, (w - 2) // 3) if w >= 3 else 0
    phx = 1 if w >= 3 else 0
    return dict(Ghoul=ghouls, Shade=shades, Knight=knights, Ram=rams, Phoenix=phx), L


def split(ro, main_permille=700):
    main, feint = {}, {}
    for t, n in ro.items():
        if t in ('Ram', 'Phoenix'):
            main[t] = n
            feint[t] = 0
        else:
            m = (n * main_permille + 999) // 1000
            main[t] = m
            feint[t] = n - m
    return main, feint


# ---------- 守方状态 ----------
def fresh_defender(R, towers_total=3, flak=True):
    return dict(R=R, K=1,
                archers=[2, 2, 2], spears=2, rangers=1,          # 开局 3 名 2 级弓手、2 枪、1 骑
                towers_gate=[towers_total / 2.0, towers_total / 2.0],   # 两座门各摊到的塔（预置贴门/缺口）
                flak_gate=[1 if flak else 0, 0],
                bld_level=1, gate_hp_frac=[1.0, 1.0], breach_open=1,    # 1 个设计缺口，未封
                keep_hp=bhp('Keep'), stone=120, wood=120, gold=0, keep_fund=0.0,
                lost_archers=0, lost_towers=0)


# ---------- 一次入口攻防 ----------
def fight_entry(group, L, D, R, dfd, gate_idx, entry_kind, dt=0.25, aoe_mult=2.0, formation=False,
                arch_conc=None, ram_atknear=True):
    """返回 (入口是否被打开, 打开时刻, 攻方剩余血池比例, 守方损失, 用时, 进城的攻方)

    arch_conc：入口附近弓手占全部弓手的比例。None = 脚本均匀登墙（13/8R）；
    人类会按免费方向提示把人调到主攻门，取 0.6–0.8。
    ram_atknear：demo 攻方脚本「AtkNear 优先」——Ram 若够得着门边墙上的弓手，
    先打人（×0.49 高度惩罚）而不砸门。"""
    n_t = dfd['towers_gate'][gate_idx]
    has_flak = dfd['flak_gate'][gate_idx] > 0
    bl = dfd['bld_level']
    frac_near = min(1.0, 13.0 / (8 * R)) if arch_conc is None else arch_conc
    all_arch = sorted(dfd['archers'], reverse=True)
    n_near = int(round(len(all_arch) * frac_near + 1e-9))
    arch_near, arch_far = all_arch[:n_near], all_arch[n_near:]
    arch_pool = [[uhp('Archer', a), a] for a in arch_near]
    tower_hp = [bhp('Tower', bl)] * int(round(n_t))
    flak_hp = bhp('Flak', bl) if has_flak else 0.0
    detour = 0
    if entry_kind == 'breach':
        struct = 0.0
    elif entry_kind == 'gate':
        struct = bhp('Gate', bl) * dfd['gate_hp_frac'][gate_idx]
    else:   # detour: 非门面绕到最近的门 ≈ R 格
        detour = R
        entry_kind = 'gate'
        struct = bhp('Gate', bl) * dfd['gate_hp_frac'][gate_idx]
    units = []
    for t, n in group.items():
        for _ in range(n):
            units.append(dict(t=t, hp=uhp(t, L), arr=(D + detour) / SPEED[t] / 20.0, alive=True, inside=False))
    if not units:
        return False, None, 0.0, dict(archers=0, towers=0, flak=0), 0.0, {}
    if formation:
        slowest = max(u['arr'] for u in units)
        for u in units:
            u['arr'] = slowest
    T = 0.0
    opened_at = None
    entered = {}
    total_hp0 = sum(u['hp'] for u in units)
    t_end = max(u['arr'] for u in units) + 400
    tower_dps = bdps('Tower', bl)
    flak_dps = bdps('Flak', bl)

    def apply(dmg, cands):
        for u in sorted(cands, key=lambda u: (0 if T >= u['arr'] else 1, u['arr'])):
            if dmg <= 0:
                break
            if not u['alive']:
                continue
            take = min(u['hp'], dmg)
            u['hp'] -= take
            dmg -= take
            if u['hp'] <= 0:
                u['alive'] = False

    while T < t_end:
        alive = [u for u in units if u['alive'] and not u['inside']]
        if not alive:
            break
        at_wall = [u for u in alive if T >= u['arr']]
        in_tz = [u for u in alive if u['arr'] - 7.0 / SPEED[u['t']] / 20 <= T < u['arr']] + at_wall
        in_az = [u for u in alive if u['arr'] - 6.5 / SPEED[u['t']] / 20 <= T < u['arr']] + at_wall
        ground_tz = [u for u in in_tz if u['t'] != 'Phoenix']
        arch_alive = [a for a in arch_pool if a[0] > 0]
        clump = len([u for u in at_wall if u['t'] != 'Phoenix'])
        d_tower = sum(1 for h in tower_hp if h > 0) * tower_dps * (aoe_mult if clump >= 2 else 1.0)
        d_arch = sum(udps('Archer', a[1]) for a in arch_alive)
        apply(d_tower * dt, ground_tz)
        phx = [u for u in alive if u['t'] == 'Phoenix' and T >= u['arr'] - 3 / SPEED['Phoenix'] / 20]
        if phx and arch_alive:
            apply(d_arch * dt * 0.5, phx)
            apply(d_arch * dt * 0.5, [u for u in in_az if u['t'] != 'Phoenix'])
        else:
            apply(d_arch * dt, [u for u in in_az if u['t'] != 'Phoenix'])
        if phx and flak_hp > 0:
            apply(flak_dps * dt, phx)
        adj_arch = arch_alive[:2]
        struct_dps = 0.0
        arch_dmg = 0.0
        for u in at_wall:
            if not u['alive']:
                continue
            t = u['t']
            if t == 'Ram':
                if ram_atknear and adj_arch:
                    arch_dmg += udps('Ram', L) * HG * 1.5   # 主目标 + 半价溅射到相邻那名
                else:
                    struct_dps += usdps('Ram', L)
            elif t in ('Ghoul', 'Knight'):
                if adj_arch:
                    arch_dmg += udps(t, L) * HG
                else:
                    struct_dps += usdps(t, L)
            elif t == 'Shade':
                if arch_alive:
                    arch_dmg += udps('Shade', L) * HG
                else:
                    struct_dps += usdps('Shade', L)
            elif t == 'Phoenix':
                if arch_alive:
                    arch_dmg += udps('Phoenix', L)
                elif tower_hp and max(tower_hp) > 0:
                    i = max(range(len(tower_hp)), key=lambda i: tower_hp[i])
                    tower_hp[i] -= usdps('Phoenix', L) * dt
                elif flak_hp > 0:
                    flak_hp -= usdps('Phoenix', L) * dt
        d = arch_dmg * dt
        for a in sorted(arch_alive, key=lambda a: a[0]):
            if d <= 0:
                break
            take = min(a[0], d)
            a[0] -= take
            d -= take
        if struct > 0:
            struct -= struct_dps * dt
            if struct <= 0 and opened_at is None:
                opened_at = T
        elif opened_at is None and at_wall:
            opened_at = T
        if struct <= 0 and at_wall:
            k = 0
            for u in sorted(at_wall, key=lambda u: u['arr']):
                if u['alive'] and not u['inside'] and k < 2:
                    u['inside'] = True
                    entered[u['t']] = entered.get(u['t'], 0) + 1
                    k += 1
        T += dt
    losses = dict(archers=sum(1 for a in arch_pool if a[0] <= 0),
                  towers=sum(1 for h in tower_hp if h <= 0),
                  flak=1 if (has_flak and flak_hp <= 0) else 0)
    survivors_near = [a[1] for a in arch_pool if a[0] > 0]
    dfd['archers'] = sorted(arch_far + survivors_near, reverse=True)
    dfd['towers_gate'][gate_idx] = sum(1 for h in tower_hp if h > 0)
    if has_flak and flak_hp <= 0:
        dfd['flak_gate'][gate_idx] = 0
    if entry_kind == 'gate':
        dfd['gate_hp_frac'][gate_idx] = max(0.0, struct / bhp('Gate', bl))
    rem = sum(u['hp'] for u in units if u['alive'] and not u['inside'])
    inside_hp = sum(u['hp'] for u in units if u['inside'])
    return opened_at is not None, opened_at, (rem + inside_hp) / total_hp0, losses, T, entered


def fight_inside(entered, L, dfd, keep_left):
    """进城后的粗模型：先与枪卫/游骑混战（平方律离散近似；AtkNear 优先 ⇒ 有人在
    射程内就先打人），内城兵光了之后幸存者全力砸堡垒，砸到堡垒归零或自己被
    后续赶来的守方消耗完为止（demo 无时限）。返回堡垒掉血。"""
    if not entered:
        return 0.0
    hp_a0 = sum(uhp(t, L) * n for t, n in entered.items())
    hp_a = hp_a0
    dps_a = sum(udps(t, L) * n for t, n in entered.items())
    sdps_a = sum(usdps(t, L) * n for t, n in entered.items())
    hp_d0 = dfd['spears'] * uhp('Spear') + dfd['rangers'] * uhp('Ranger')
    hp_d = hp_d0
    dps_d = dfd['spears'] * udps('Spear') + dfd['rangers'] * udps('Ranger')
    dt = 0.25
    keep_dmg = 0.0
    T = 0.0
    while hp_a > 0 and T < 600 and keep_dmg < keep_left:
        fa = hp_a / hp_a0
        if hp_d > 0:
            fd = hp_d / hp_d0
            hp_a -= dps_d * fd * dt
            hp_d = max(0.0, hp_d - dps_a * fa * dt)
        else:
            keep_dmg += sdps_a * fa * dt
        T += dt
    if hp_d0 > 0:
        frac = hp_d / hp_d0
        dfd['spears'] = int(round(dfd['spears'] * frac))
        dfd['rangers'] = int(round(dfd['rangers'] * frac))
    return min(keep_dmg, keep_left)


# ---------- 多波循环 ----------
def run(alloc='current', R=13, D=55, seal=True, towers_total=3, aoe_mult=2.0, formation=False,
        income_growth=0.0, keep_frac=0.33, max_waves=40, pop_cap=False, repair_mult=1.0,
        n_spawn=4, arch_conc=None, ram_atknear=True, verbose=False):
    dfd = fresh_defender(R, towers_total)
    inc_stone = Bd['Quarry']['income_amount']
    inc_wood = Bd['Lumber']['income_amount']
    inc_gold = Bd['Mine']['income_amount'] + Bd['Keep']['income_amount']
    rows = []
    for w in range(1, max_waves + 1):
        if w == 1 and seal and dfd['stone'] >= 30:
            dfd['stone'] -= 30
            dfd['breach_open'] = 0
        # 堡垒基金：每波把 keep_frac 的石材拨进去，攒够 200 石（且有 200 木）就升一级
        if dfd['keep_fund'] >= 200 and dfd['wood'] >= 200:
            dfd['keep_fund'] -= 200
            dfd['wood'] -= 200
            dfd['K'] += 1
        while dfd['stone'] >= 80 and dfd['wood'] >= 30:
            dfd['stone'] -= 80
            dfd['wood'] -= 30
            g = 0 if dfd['towers_gate'][0] <= dfd['towers_gate'][1] else 1
            dfd['towers_gate'][g] += 1
        cap = 8 + 2 * dfd['K'] if pop_cap else 10 ** 9
        while dfd['gold'] >= 60 and (len(dfd['archers']) + dfd['spears'] + dfd['rangers'] + 1) < cap:
            dfd['gold'] -= 60
            dfd['archers'].append(1)
        dfd['bld_level'] = max(1, math.ceil(dfd['K'] / 2))
        ro, L, slots, budget = roster(w, alloc)
        main, feint = split(ro)
        face = w % n_spawn
        if face in (0, 2):
            kind_main, g_main = 'gate', face // 2
        elif face == 1:
            kind_main, g_main = ('breach' if dfd['breach_open'] else 'detour'), 0
        else:
            kind_main, g_main = 'detour', 1
        opened, t_open, rem, loss, dur, entered = fight_entry(main, L, D, R, dfd, g_main, kind_main,
                                                              aoe_mult=aoe_mult, formation=formation,
                                                              arch_conc=arch_conc, ram_atknear=ram_atknear)
        keep_dmg = fight_inside(entered, L, dfd, dfd['keep_hp']) if entered else 0.0
        f_face = (face + 1) % n_spawn
        if f_face in (0, 2):
            kind_f, g_f = 'gate', f_face // 2
        elif f_face == 1:
            kind_f, g_f = ('breach' if dfd['breach_open'] else 'detour'), 0
        else:
            kind_f, g_f = 'detour', 1
        f_conc = None if arch_conc is None else max(0.0, (1 - arch_conc) * 0.5)
        opened_f, _, rem_f, loss_f, dur_f, entered_f = fight_entry(feint, L, D, R, dfd, g_f, kind_f,
                                                                  aoe_mult=aoe_mult, formation=formation,
                                                                  arch_conc=f_conc, ram_atknear=ram_atknear)
        keep_dmg += fight_inside(entered_f, L, dfd, dfd['keep_hp'] - keep_dmg) if entered_f else 0.0
        dfd['keep_hp'] -= keep_dmg
        keep_after_wave = dfd['keep_hp'] / bhp('Keep')
        dfd['lost_archers'] += loss['archers'] + loss_f['archers']
        dfd['lost_towers'] += loss['towers'] + loss_f['towers']
        dur_total = 260 / 20 + max(dur, dur_f)
        periods = int(dur_total * 20 // G['income_period_ticks'])
        mult = 1 + income_growth * max(0, w - 2)
        stone_in = inc_stone * periods * mult
        dfd['keep_fund'] += stone_in * keep_frac
        dfd['stone'] += stone_in * (1 - keep_frac)
        dfd['wood'] += inc_wood * periods * mult
        dfd['gold'] += inc_gold * periods * mult
        # 波间维修：门 + 堡垒（15 木 / 1000 血，几乎免费——这本身是文档 §10 的一条发现）
        repair_hp = sum((1 - f) for f in dfd['gate_hp_frac']) * bhp('Gate', dfd['bld_level'])
        repair_hp += max(0.0, bhp('Keep') - dfd['keep_hp'])
        wood_cost = repair_hp * G['repair_wood_per_1000hp'] / 1000 * repair_mult
        if dfd['wood'] >= wood_cost:
            dfd['wood'] -= wood_cost
            dfd['gate_hp_frac'] = [1.0, 1.0]
            dfd['keep_hp'] = bhp('Keep')
        rows.append(dict(w=w, L=L, slots=slots, kind=kind_main, opened=opened, t_open=t_open, rem=rem,
                         entered=sum(entered.values()) + sum(entered_f.values()),
                         keep=keep_after_wave, archers=len(dfd['archers']),
                         towers=dfd['towers_gate'][:], K=dfd['K'], loss=loss, dur=dur))
        if verbose:
            to = ('@%.0fs' % t_open) if t_open is not None else ''
            print(f"w{w:2d} L={L:2d} n={slots:2d} 入口={kind_main:6s} 开={'是' if opened else '否'}{to:>6} "
                  f"进城={rows[-1]['entered']:2d} 攻余={rem:4.0%} 堡={keep_after_wave:4.0%} "
                  f"弓={len(dfd['archers']):2d}(-{loss['archers']+loss_f['archers']}) 塔={dfd['towers_gate']} K={dfd['K']} 用时={dur:4.0f}s")
        if keep_after_wave <= 0:
            return w, rows
    return None, rows


def realized_pool(w, alloc):
    """一波兑现到场上的血池与火力（1 级单位当量）：Σ √B，而不是 Σ B。"""
    ro, L, slots, budget = roster(w, alloc)
    n = sum(ro.values())
    return n, L, budget, n * sq(L), (n * sq(L)) / budget


def towers_needed(group, L, archers_near, flak, R=13, D=55, formation=False):
    """固定一座门：主攻群到门下、门不破，最少要几座塔。"""
    for nt in range(0, 16):
        d = fresh_defender(R, towers_total=0)
        d['towers_gate'] = [nt, nt]
        d['flak_gate'] = [1 if flak else 0, 1 if flak else 0]
        d['archers'] = [1] * archers_near
        opened, *_ = fight_entry(group, L, D, R, d, 0, 'gate', arch_conc=1.0, formation=formation)
        if not opened:
            return nt
    return '>15'


def first_open(rows):
    for r in rows:
        if r['opened']:
            return r['w']
    return None


def summarize(name, **kw):
    sw, rows = run(max_waves=40, **kw)
    fo = first_open(rows)
    last = rows[-1]
    print(f"{name:44s} 陷落波={str(sw):>4s} 首次开门波={str(fo):>4s} "
          f"末波弓手={last['archers']:2d} 塔={last['towers']} K={last['K']}")
    return sw, rows


def mode_pool():
    print("=== 预算兑现率：一波兑现的血池 Σ√B ÷ 纸面预算 ΣB（人数不含 Wraith）===")
    print(" w  分法      人数  L   预算(ΣB)  兑现血池(Σ√B)  兑现率")
    for alloc in ['current', 'jia', 'yi']:
        for w in [2, 3, 5, 8, 10, 15, 20]:
            n, L, b, pool, r = realized_pool(w, alloc)
            print(f'{w:2d}  {alloc:8s} {n:3d}  {L:2d}  {b:7.1f}   {pool:7.1f}       {r:4.0%}')


def mode_towers():
    print('=== 拦住主攻（门不破）所需塔数 —— 列：门边弓手 0 / 3 / 6 名；F = 该门有 Flak ===')
    print(' w   L  Ram数 | 现行 0  3  6 | 0F 3F 6F | 甲(L) 0 3 6 | 乙(L) 0 3 6 | 只有 Ram、无掩护')
    for w in [3, 4, 5, 6, 8, 10, 12, 15, 20]:
        ro, L, s, b = roster(w, 'current')
        roJ, LJ, *_ = roster(w, 'jia')
        roY, LY, *_ = roster(w, 'yi')
        main, _ = split(ro)
        mainJ, _ = split(roJ)
        mainY, _ = split(roY)
        c = [towers_needed(main, L, a, False) for a in (0, 3, 6)]
        cf = [towers_needed(main, L, a, True) for a in (0, 3, 6)]
        j = [towers_needed(mainJ, LJ, a, False) for a in (0, 3, 6)]
        y = [towers_needed(mainY, LY, a, False) for a in (0, 3, 6)]
        solo = towers_needed({'Ram': main['Ram']}, L, 0, False)
        print(f'{w:2d}  {L:2d}   {ro["Ram"]}   |  {c[0]!s:>2} {c[1]!s:>2} {c[2]!s:>2}  | {cf[0]!s:>2} {cf[1]!s:>2} {cf[2]!s:>2} '
              f'| L{LJ:<2d} {j[0]!s:>2} {j[1]!s:>2} {j[2]!s:>2} | L{LY:<2d} {y[0]!s:>2} {y[1]!s:>2} {y[2]!s:>2} | {solo}')
    print('对照：守方一波石材 135 ⇒ 1.7 座塔/波（全部石材建塔、不升堡垒），两座门各分 0.85。')
    print('\n=== 旧曲线（09-01 之前，按注释近似复原）：所需塔数 0 / 3 / 6 弓手 ===')
    for w in [3, 5, 8, 10, 12, 15, 20]:
        ro, L = old_roster(w)
        main, _ = split(ro)
        print(f'w={w:2d} L={L} Ram={ro["Ram"]}  ', [towers_needed(main, L, a, False) for a in (0, 3, 6)])


def mode_grid():
    print("=== 单因素敏感性（基线：现行曲线 R=13 D=55 封缺口 3 塔 AOE×2 弓手均匀 Ram 先打人）===")
    summarize("基线")
    summarize("弓手 60% 集中主攻门", arch_conc=0.6)
    summarize("弓手 80% 集中主攻门", arch_conc=0.8)
    summarize("Ram 不理弓手、只砸门", ram_atknear=False)
    summarize("不封缺口", seal=False)
    summarize("分配甲：人数 6+3(w-1)、封顶 40", alloc='jia')
    summarize("分配乙：甲 + α 1.15", alloc='yi')
    summarize("对照：只买人（等级恒 1）", alloc='flat')
    summarize("集结点距墙 30", D=30)
    summarize("攻方编队同步到达", formation=True)
    summarize("城区半径 9", R=9)
    summarize("塔 AOE 倍率 1（不齐射）", aoe_mult=1.0)
    summarize("塔 AOE 倍率 3", aoe_mult=3.0)
    summarize("收入每波 +15%（解禁一簇/波）", income_growth=0.15)
    summarize("人口上限 8+2K 生效", pop_cap=True)
    summarize("维修木材 ×4", repair_mult=4.0)
    summarize("堡垒不升级（石材全建塔）", keep_frac=0.0)
    summarize("堡垒 50% 石材", keep_frac=0.5)
    print("\n=== 组合 ===")
    summarize("甲 + 集结点 30 + 编队同步", alloc='jia', D=30, formation=True)
    summarize("甲 + 弓手 60% 集中", alloc='jia', arch_conc=0.6)
    summarize("乙 + 弓手 60% + 收入增长 + 人口上限", alloc='yi', arch_conc=0.6, income_growth=0.15, pop_cap=True)
    summarize("现行 + 弓手 60% + 收入增长 + 人口上限", arch_conc=0.6, income_growth=0.15, pop_cap=True)
    summarize("现行 + R9 + D30 + 编队 + 弓手 60%", R=9, D=30, formation=True, arch_conc=0.6)


def mode_base():
    print("== 基线：现行曲线，R=13，D=55，封缺口，3 塔，AOE×2，脚本均匀登墙 ==")
    run(verbose=True, max_waves=25)
    print("\n== 同上，但弓手 60% 集中到主攻门（人类按方向提示调兵） ==")
    run(verbose=True, max_waves=25, arch_conc=0.6)


if __name__ == '__main__':
    # 输出里有 ⇒ 等非 GBK 字符：Windows 中文控制台（cp936）下不转 UTF-8 会直接
    # UnicodeEncodeError 崩在半截（2026-09-02 审 #126 时实测，towers 模式）。
    if sys.stdout.encoding and sys.stdout.encoding.lower() not in ('utf-8', 'utf8'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    mode = sys.argv[1] if len(sys.argv) > 1 else 'grid'
    {'pool': mode_pool, 'towers': mode_towers, 'grid': mode_grid, 'base': mode_base}[mode]()
