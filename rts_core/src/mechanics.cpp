// 机制第一批：承伤与死亡、目标选择与攻击、相邻格移动、建筑攻击、视野。
// 第二批：经济闭环与命令解算。第三批：驻守与高度优势（tick_garrison 一族）。
// 第四批：冲锋与齐射。第五批：在途弹丸（tick_projectiles 一族）。
//
// 与 `world.cpp` 分开放：那边是数据布局与输入校验（1b 的产物），这边是
// 逐 tick 的解算（1c）。阶段顺序的规范写在 `World::advance()` 的注释里，
// **本文件只是它的实现，顺序以那份注释为准**。
//
// 三条实现要求是从设计文档直接搬下来的，改动前先读原文：
//
//   * **AOE 落点在前摇开始那一刻锁定成坐标**（CLAUDE.md「结构破坏规则」）。
//     跟着目标句柄走的话「散开克溅射」整条机制作废，而那不会让任何测试变红
//     ——它是机制正确性，不是确定性。所以锁定发生在 `commit_attack`，
//     `land_attack` 只读 `u_aim_`
//   * **打不到就空转**（契约 §1.1.1 乙）：`Atk*` 不隐含移动，选中的目标不在
//     射程内就什么都不做。掩码的攻击 4 位据同一份目标选择升级为精确
//   * **伤害只走 `apply_permille`**（决定 ⑫）：一次除法、四舍五入、钳到 >= 1。
//     真要禁止某类打击用结构（`can_engage` / `can_break_structure`），
//     不要指望乘出一个 0

#include <cmath>
#include <cstdint>

#include "rts/combat_math.hpp"
#include "rts/unit_behavior.hpp"
#include "rts/world.hpp"

namespace rts {
namespace {

constexpr float kSqrt2 = 1.41421356f;

inline float dist2(Vec2 a, Vec2 b) noexcept {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

inline UnitId unit_from_raw(std::uint32_t raw) noexcept {
    return UnitId::make(static_cast<std::uint16_t>(raw >> 16),
                        static_cast<std::uint16_t>(raw & 0xFFFFu));
}
inline BldId bld_from_raw(std::uint32_t raw) noexcept {
    return BldId::make(static_cast<std::uint16_t>(raw >> 16),
                       static_cast<std::uint16_t>(raw & 0xFFFFu));
}
inline ObstacleId obstacle_from_raw(std::uint32_t raw) noexcept {
    return ObstacleId::make(static_cast<std::uint16_t>(raw >> 16),
                            static_cast<std::uint16_t>(raw & 0xFFFFu));
}

// 建筑的对空 / 对地是**结构**（CLAUDE.md「防空的要害：AA 只对空」——
// 否则 AA 就是「更好的塔」，两难消失）。所以写成谓词，不放进数值表。
constexpr bool bld_targets_air(BldType b) noexcept { return b == BldType::Flak; }

// 视线：两格之间的整数 Bresenham，检查**中间格**的 blocks_vision
// （两个端点不算——站在林子里的单位看得见自己与紧邻格）。
bool line_of_sight(const TerrainMasks& t, GridPos from, GridPos to) noexcept {
    int x0 = from.i, y0 = from.j;
    const int x1 = to.i, y1 = to.j;
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        if (!(x0 == from.i && y0 == from.j) && !(x0 == x1 && y0 == y1)) {
            if (t.blocks_vision(x0, y0)) return false;
        }
        if (x0 == x1 && y0 == y1) return true;
        const int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;   // 是 += dx 不是 += dy：写错的症状是**垂直**视线歪着走
                         // （纯竖直的射线在第二步开始横移），水平与对角反而全对，
                         // 于是只有「隔两格的正上/正下方」这种用例才抓得到
            y0 += sy;
        }
    }
}

// 以 `origin`（连续坐标）为圆心把半径内的格标成可见。
// 空中单位不吃视线遮挡（高悬于林与岩之上）；地面走 Bresenham。
void mark_vision_circle(FogLayer& fog, const TerrainMasks& t, Vec2 origin,
                        float radius, bool aerial, Tick now) {
    const GridPos at = grid_of(origin);
    if (fog.in_bounds(at.i, at.j)) fog.mark_visible(at.i, at.j, now);
    if (radius <= 0.0f) return;
    const float r2 = radius * radius;
    const int lo_x = static_cast<int>(std::floor(origin.x - radius));
    const int hi_x = static_cast<int>(std::floor(origin.x + radius));
    const int lo_y = static_cast<int>(std::floor(origin.y - radius));
    const int hi_y = static_cast<int>(std::floor(origin.y + radius));
    for (int y = lo_y; y <= hi_y; ++y) {
        for (int x = lo_x; x <= hi_x; ++x) {
            if (!fog.in_bounds(x, y)) continue;
            const Vec2 c = center_of(GridPos{static_cast<std::int16_t>(x),
                                             static_cast<std::int16_t>(y)});
            if (dist2(origin, c) > r2) continue;
            if (!aerial &&
                !line_of_sight(t, at, GridPos{static_cast<std::int16_t>(x),
                                              static_cast<std::int16_t>(y)})) {
                continue;
            }
            fog.mark_visible(x, y, now);
        }
    }
}

}  // namespace

// ——目标选择——
//
// 「最近」是平方距离比较、「最弱」是血量绝对值比较（`rts/action.hpp` 定义死了），
// 并列取**槽位下标更小**的——严格小于的扫描天然给出这条，且与规范顺序一致。
World::TargetPick World::pick_target(std::size_t k, UnitAction a) const {
    TargetPick best;
    const UnitType my_type = u_type_[k];
    const UnitBehavior& beh = behavior_of(my_type);
    const Side my_side = side_of(my_type);
    const Vec2 my_pos = u_pos_[k];

    switch (a) {
        case UnitAction::AtkNear:
        case UnitAction::AtkWeak: {
            for (std::size_t s = 0; s < unit_pool_.slot_count(); ++s) {
                if (!unit_pool_.alive_at(static_cast<std::uint16_t>(s))) continue;
                if (s == k) continue;
                const UnitType their = u_type_[s];
                if (side_of(their) == my_side) continue;
                // 通则由 behavior 给；驻守上下文的唯一例外是已登上
                // 墙段的 Archer 可以对空。
                if (!unit_can_engage(k, their)) continue;
                // Ground melee cannot reach a defender who has finished mounting.
                if (!is_aerial(my_type) && beh.engage_range() != EngageRange::Ranged &&
                    u_garrison_[s] != kNoSlot && u_mount_[s] == 0) continue;
                const float d2 = dist2(my_pos, u_pos_[s]);
                const bool better =
                    !best.found ||
                    (a == UnitAction::AtkNear
                         ? d2 < best.dist2
                         : u_hp_[s] < u_hp_[static_cast<std::size_t>(best.raw >> 16)]);
                if (better) {
                    best.found = true;
                    best.kind = TgtKind::Unit;
                    best.raw = unit_pool_.id_at(static_cast<std::uint16_t>(s)).raw();
                    best.pos = u_pos_[s];
                    best.dist2 = d2;
                }
            }
            break;
        }
        case UnitAction::AtkBld:
        case UnitAction::AtkWall: {
            // 建筑全属守方，所以只有攻方单位有目标可选——这不是特判，
            // 是「攻方没有建筑」这个结构事实的自然结果。
            if (my_side != Side::Attacker) break;
            const bool wants_wall = (a == UnitAction::AtkWall);
            // `AtkWall` 打墙与门 ⇒ 需要能破坏结构（`Phoenix` 在这里被结构挡住，
            // 它对墙恒 0 不是数值）。`AtkBld` 打其余建筑 ⇒ 有战力即可
            // （`Phoenix` 点杀防御塔是设计明写的用途）。
            if (wants_wall ? !beh.can_break_structure() : !beh.combat()) break;
            for (std::size_t s = 0; s < bld_pool_.slot_count(); ++s) {
                if (!bld_pool_.alive_at(static_cast<std::uint16_t>(s))) continue;
                const BldType bt = b_type_[s];
                const bool is_wall = (bt == BldType::Wall || bt == BldType::Gate);
                if (is_wall != wants_wall) continue;
                if (bt == BldType::Keep && !beh.can_break_structure()) continue;
                const Vec2 c = center_of(b_pos_[s]);
                const float d2 = dist2(my_pos, c);
                if (!best.found || d2 < best.dist2) {
                    best.found = true;
                    best.kind = TgtKind::Bld;
                    best.raw = bld_pool_.id_at(static_cast<std::uint16_t>(s)).raw();
                    best.pos = c;
                    best.dist2 = d2;
                }
            }
            break;
        }
        case UnitAction::Stop:
        case UnitAction::MoveN:
        case UnitAction::MoveNE:
        case UnitAction::MoveE:
        case UnitAction::MoveSE:
        case UnitAction::MoveS:
        case UnitAction::MoveSW:
        case UnitAction::MoveW:
        case UnitAction::MoveNW:
            break;
    }
    return best;
}

// ——承诺与落地——

void World::commit_attack(std::size_t k, const TargetPick& t) {
    u_tgt_kind_[k] = t.kind;
    u_tgt_raw_[k] = t.raw;
    // **锁定就发生在这一行。** 之后目标怎么走都与落点无关。
    u_aim_[k] = t.pos;
    u_cd_[k] = stats_.of(u_type_[k]).cooldown_ticks;
    const std::int32_t w = stats_.of(u_type_[k]).windup_ticks;
    u_windup_[k] = w;
    if (w == 0) land_attack(k);
}

void World::land_attack(std::size_t k) {
    const UnitType my_type = u_type_[k];
    const UnitStats& s = stats_.of(my_type);
    const UnitBehavior& beh = behavior_of(my_type);
    const Side my_side = side_of(my_type);
    const std::int64_t lvl =
        level_permille(u_level_[k], stats_.global.dmg_permille_per_level);
    const TgtKind kind = u_tgt_kind_[k];
    const std::uint32_t raw = u_tgt_raw_[k];
    const Vec2 aim = u_aim_[k];
    u_tgt_kind_[k] = TgtKind::None;
    u_tgt_raw_[k] = 0;

    // ——冲锋（第四批）：动量在这一击结算并耗尽（无论命中与否）——
    //
    // 「冲完就是冲完了」：miss 或目标已死也照样清零，动量是物理不是意图。
    // 非 Charge 兵种恒 1000（恒等倍率不改变 apply_permille 的舍入结果，
    // 分子分母同乘一个 1000）。
    std::int64_t charge_pm = kPermilleOne;
    if (beh.charges()) {
        charge_pm = charge_permille(u_charge_[k], stats_.global.charge_max_cells,
                                    stats_.global.charge_bonus_permille_per_cell);
        u_charge_[k] = 0.0f;
    }

    const std::int64_t vs_structure =
        apply_permille(s.damage, {lvl, s.vs_structure_permille, charge_pm});

    // 「居高」是攻击方的属性：空中单位从上方来，驻守且墙高完好的自己也在高处，
    // 两者都不吃下面的高度惩罚（放箭的还把它定格进弹丸随弹携带）。
    const bool atk_high = is_aerial(my_type) || on_high_wall(k);

    // ——在途弹丸（第五批）：远程的伤害由弹丸送达——
    //
    // 谁放弹丸是**结构**（`launches_projectile()` = Ranged × 非空中，恰好
    // Archer / Shade；`Ram` 照旧前摇撞击、`Phoenix` 俯冲直击）。发射方的状态
    // （等级倍率、居高与否）在这一刻定格进弹丸；逐目标的倍率（高度 miss /
    // 减伤）等**命中那一刻**按目标当时的状态结算。结构目标的伤害与目标状态
    // 无关，在这里一次乘完（截断只发生一次，决定 ⑫）。
    // 冲锋与反冲锋都不进弹丸路径：没有兵种既放箭又冲锋 / 又架枪阵——
    // 轴组合的现状，tests/unit_behavior_test.cpp 锁住；破了先回来补字段。
    if (beh.launches_projectile()) {
        ProjSpec p;
        p.pos = u_pos_[k];
        p.speed = s.proj_speed;
        p.lvl_pm = lvl;
        p.from_high = atk_high ? 1 : 0;
        p.side = my_side;
        if (s.aoe_radius > 0.0f) {
            // 花名册里没有「远程 + 溅射」的兵种，但表配得出来。语义取齐射：
            // AOE 砸锁定落点、只打地面单位（与建筑齐射同一条命中路径）。
            p.kind = TgtKind::None;
            p.aim = aim;
            p.dmg = s.damage;
            p.aoe = s.aoe_radius;
            launch_projectile(p);
            return;
        }
        p.kind = kind;
        p.raw = raw;
        // 追踪弹的初始目的地 = 目标此刻的位置（此后逐 tick 刷新）。
        // 目标在前摇里就死了 ⇒ 没有箭可放（与旧路径「落空」同义，只是提前）。
        switch (kind) {
            case TgtKind::Unit:
                if (!unit_pool_.alive(unit_from_raw(raw))) return;
                p.aim = u_pos_[static_cast<std::size_t>(raw >> 16)];
                p.dmg = s.damage;
                break;
            case TgtKind::Bld:
                if (!bld_pool_.alive(bld_from_raw(raw))) return;
                p.aim = center_of(b_pos_[static_cast<std::size_t>(raw >> 16)]);
                p.dmg = vs_structure;
                break;
            case TgtKind::Obstacle:
                if (!obstacle_pool_.alive(obstacle_from_raw(raw))) return;
                p.aim = center_of(o_pos_[static_cast<std::size_t>(raw >> 16)]);
                p.dmg = vs_structure;
                break;
            case TgtKind::None:
                return;
        }
        launch_projectile(p);
        return;
    }

    // ——高度优势（第三批）：从低处打墙上单位，有 miss 且伤害打折——
    //
    // **只对单位目标生效**——打墙本身没有「墙居高临下」一说。
    // miss 掷的是本世界的 RNG（状态进哈希，回放里同一发必然同判）。
    const std::int64_t hg_dmg = stats_.global.high_ground_dmg_permille;
    const std::int64_t hg_miss = stats_.global.high_ground_miss_permille;
    const auto misses_high = [&](std::size_t t) {
        if (atk_high || !on_high_wall(t)) return false;
        return static_cast<std::int64_t>(rng_.below(1000)) < hg_miss;
    };
    // AOE 溅射折扣的「主目标」= 承诺时锁定的那个**单位**目标（若仍存活）。
    // 只分主副单位——建筑/障碍侧「全额命中」不分主副（CLAUDE.md「结构破坏
    // 规则」原文对建筑就是这么写的），下面建筑/障碍那两段循环不碰。
    // 承诺目标是墙/门/障碍时没有主目标单位：圈内单位全部按溅射算，
    // 因为对单位而言它们全部是「被波及的」，没有谁是「被瞄准的」。
    const bool has_primary_unit =
        kind == TgtKind::Unit && unit_pool_.alive(unit_from_raw(raw));
    const std::size_t primary_unit_slot =
        has_primary_unit ? static_cast<std::size_t>(raw >> 16) : 0;

    // 全部倍率在**同一次** apply_permille 里乘（截断只能发生一次，决定 ⑫）。
    // 逐目标的三条：高度减伤（第三批）、反冲锋（第四批——关系由轴推导
    // `counters_charge`，幅度随**目标的**动量线性放大，见 combat_math.hpp）、
    // AOE 溅射折扣（非主目标才吃，见上）。
    const auto dmg_vs_unit = [&](std::size_t t) {
        std::int64_t mods[5] = {lvl, charge_pm, 0, 0, 0};
        std::size_t n = 2;
        if (!atk_high && on_high_wall(t)) mods[n++] = hg_dmg;
        if (beh.counters_charge() && behavior_of(u_type_[t]).charges() &&
            u_charge_[t] > 0.0f) {
            mods[n++] = anti_charge_permille(u_charge_[t],
                                             stats_.global.charge_max_cells,
                                             stats_.global.anti_charge_permille);
        }
        if (s.aoe_radius > 0.0f &&
            !(has_primary_unit && t == primary_unit_slot)) {
            mods[n++] = s.splash_dmg_permille;
        }
        return apply_permille(s.damage, mods, n);
    };

    if (s.aoe_radius > 0.0f) {
        // AOE 砸**锁定的坐标**。圈内的单位不分敌我（「溅射单位被包夹时会误伤」
        // 是 CLAUDE.md 列的机制性克制，不是 bug），但分主副——主目标满伤、
        // 其余按 `splash_dmg_permille` 打折（`dmg_vs_unit` 已经把折扣揉进去），
        // 空中不挨砸（can_engage）。
        const float r2 = s.aoe_radius * s.aoe_radius;
        for (std::size_t t = 0; t < unit_pool_.slot_count(); ++t) {
            if (!unit_pool_.alive_at(static_cast<std::uint16_t>(t))) continue;
            if (t == k) continue;
            if (!unit_can_engage(k, u_type_[t])) continue;
            if (dist2(u_pos_[t], aim) > r2) continue;
            if (misses_high(t)) continue;
            deal_damage(TgtKind::Unit,
                        unit_pool_.id_at(static_cast<std::uint16_t>(t)).raw(),
                        dmg_vs_unit(t), my_side);
        }
        for (std::size_t t = 0; t < bld_pool_.slot_count(); ++t) {
            if (!bld_pool_.alive_at(static_cast<std::uint16_t>(t))) continue;
            const BldType bt = b_type_[t];
            const bool is_wall = (bt == BldType::Wall || bt == BldType::Gate);
            if (is_wall ? !beh.can_break_structure() : !beh.combat()) continue;
            if (dist2(center_of(b_pos_[t]), aim) > r2) continue;
            deal_damage(TgtKind::Bld,
                        bld_pool_.id_at(static_cast<std::uint16_t>(t)).raw(),
                        vs_structure, my_side);
        }
        for (std::size_t t = 0; t < obstacle_pool_.slot_count(); ++t) {
            if (!obstacle_pool_.alive_at(static_cast<std::uint16_t>(t))) continue;
            if (!beh.can_break_structure()) continue;
            if (dist2(center_of(o_pos_[t]), aim) > r2) continue;
            deal_damage(TgtKind::Obstacle,
                        obstacle_pool_.id_at(static_cast<std::uint16_t>(t)).raw(),
                        vs_structure, my_side);
        }
        return;
    }

    // 单体直击（近战 / `Ram` / `Phoenix` 俯冲——放箭的在上面已经走弹丸路径）。
    // 目标死了就落空。**不追加射程检查**——承诺时查过一次，前摇里目标挪出
    // 半步照样中：那半步是「弓手被贴脸即废」的镜像，挥出去的刀不检查卷尺。
    switch (kind) {
        case TgtKind::Unit:
            if (unit_pool_.alive(unit_from_raw(raw))) {
                const std::size_t t = static_cast<std::size_t>(raw >> 16);
                if (!is_aerial(my_type) && beh.engage_range()!=EngageRange::Ranged && u_garrison_[t]!=kNoSlot && u_mount_[t]==0) break;
                if (!misses_high(t)) {
                    deal_damage(kind, raw, dmg_vs_unit(t), my_side);
                }
            }
            break;
        case TgtKind::Bld:
            if (bld_pool_.alive(bld_from_raw(raw))) {
                deal_damage(kind, raw, vs_structure, my_side);
            }
            break;
        case TgtKind::Obstacle:
            if (obstacle_pool_.alive(obstacle_from_raw(raw))) {
                deal_damage(kind, raw, vs_structure, my_side);
            }
            break;
        case TgtKind::None:
            break;
    }
}

void World::deal_damage(TgtKind kind, std::uint32_t raw, std::int64_t amount,
                        Side dealer_side) {
    switch (kind) {
        case TgtKind::Unit: {
            const UnitId id = unit_from_raw(raw);
            const std::size_t t = id.index();
            // ——RL 战果计数（见 `take_tally()`）。**只在这一处埋点**：
            // 所有伤害都经 `deal_damage`，所以这里数一次就够，不必去每个
            // 机制里各加一笔（那种散落的记账迟早漏一处）。
            // 伤害按**实际扣掉的血**记，不按 `amount`——超杀的那部分不是战果。
            Tally& tl = tally_[static_cast<std::size_t>(dealer_side)];
            const bool enemy = side_of(u_type_[t]) != dealer_side;
            const auto damage = amount < u_hp_[t] ? amount : u_hp_[t];
            if (enemy) tl.dmg_to_units += damage;
            else tl.friendly_unit_damage += damage;
            u_hp_[t] -= amount;
            if (u_hp_[t] <= 0) {
                if (enemy) {
                    ++tl.units_killed;
                    tl.enemy_unit_levels[static_cast<std::size_t>(u_type_[t])] += u_level_[t];
                    // Preserve noncombat categories, but only for enemy victims.
                    if (!is_combat(u_type_[t])) ++tl.scouts_killed;
                    if (u_type_[t] == UnitType::Scout || u_type_[t] == UnitType::Wraith)
                        ++tl.scout_units_killed;
                    if (u_type_[t] == UnitType::Mason) ++tl.masons_killed;
                    if (side_of(u_type_[t]) == Side::Defender)
                        tl.enemy_unit_gold += train_cost_gold(u_type_[t], u_level_[t]);
                } else {
                    ++tl.friendly_units_killed;
                }
                if (u_type_[t] == UnitType::Phoenix)
                    ++tally_[static_cast<std::size_t>(side_of(u_type_[t]))].phoenix_losses;
                // 自身损失记在**被打的那一方**头上，用满血而不是当前血：
                // 「损失」是这个单位值多少，不是它死时还剩多少。
                tally_[static_cast<std::size_t>(side_of(u_type_[t]))].losses +=
                    u_max_hp_[t];
                tally_[static_cast<std::size_t>(side_of(u_type_[t]))].own_unit_levels[static_cast<std::size_t>(u_type_[t])] += u_level_[t];
                kill_unit(id);
            }
            break;
        }
        case TgtKind::Bld: {
            const BldId id = bld_from_raw(raw);
            const std::size_t t = id.index();
            Tally& tl = tally_[static_cast<std::size_t>(dealer_side)];
            tl.dmg_to_blds += amount < b_hp_[t] ? amount : b_hp_[t];
            b_hp_[t] -= amount;
            if (b_hp_[t] <= 0) {
                ++tl.blds_destroyed;
                // **重建成本**（石 + 木的基础造价），`CLAUDE.md` 明写这个
                // 权重「等于给玩家造成的实际资源损失，是有原则的推导而非
                // 手工试凑，且经济建筑与防御建筑共用同一公式」。
                const BldStats& bs = stats_.of(b_type_[t]);
                tl.bld_value += bs.cost_stone + bs.cost_wood;
                // Keep is paid once by the terminal reward. An unfinished site
                // has its base investment, but no completed upgrade or income.
                if (b_type_[t] != BldType::Keep) {
                    tl.destroyed_stone += bs.cost_stone;
                    tl.destroyed_wood += bs.cost_wood;
                    if (b_built_[t]) {
                        for (int level = 1; level < b_level_[t]; ++level) {
                            tl.destroyed_stone += bld_upgrade_cost_stone(b_type_[t], level);
                            tl.destroyed_wood += bld_upgrade_cost_wood(b_type_[t], level);
                        }
                        if (is_gatherer(b_type_[t]) && stats_.global.income_period_ticks > 0) {
                            for (const auto& site : resources_) {
                                if (site.pos != b_pos_[t] || site.kind != resource_of(b_type_[t]) || wave_ < site.unlock_wave) continue;
                                const auto pm = site.tier == ResourceTier::Outer ? tier_income_permille_.outer : tier_income_permille_.inner;
                                const auto lost_income = (bs.income_amount * pm + kPermilleOne / 2) / kPermilleOne;
                                if (site.kind == Resource::Stone) tl.destroyed_income_stone += lost_income;
                                else if (site.kind == Resource::Wood) tl.destroyed_income_wood += lost_income;
                                else tl.destroyed_income_gold += lost_income;
                                break;
                            }
                        }
                    }
                }
                destroy_bld(id);
            }
            break;
        }
        case TgtKind::Obstacle: {
            const ObstacleId id = obstacle_from_raw(raw);
            const std::size_t t = id.index();
            o_hp_[t] -= amount;
            if (o_hp_[t] <= 0) {
                // 产出归最后一击方；攻方没有经济，所以只有守方拿得到
                // （`无尽模式与地形分层.md` 6.6 的结构决定之一）。
                // 种类是结构（`harvest_of`），数额查表。
                if (dealer_side == Side::Defender) {
                    const Resource r = harvest_of(o_type_[t]);
                    stock_[static_cast<std::size_t>(r)] +=
                        stats_.of(o_type_[t]).yield_amount;
                }
                destroy_obstacle(id);
            }
            break;
        }
        case TgtKind::None:
            break;
    }
}

// ——阶段 2：单位战斗——

void World::tick_unit_combat() {
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        if (u_cd_[k] > 0) --u_cd_[k];
        if (u_windup_[k] > 0) {
            --u_windup_[k];
            if (u_windup_[k] == 0 && u_tgt_kind_[k] != TgtKind::None) {
                land_attack(k);
            }
            continue;   // 前摇中：不承诺新的出手
        }
        // 在爬墙：上墙延迟期间不承诺出手（那段不设防就是延迟的代价）。
        if (u_garrison_[k] != kNoSlot && u_mount_[k] > 0) continue;
        const UnitAction a = u_action_[k];
        if (a != UnitAction::AtkNear && a != UnitAction::AtkWeak &&
            a != UnitAction::AtkBld && a != UnitAction::AtkWall) {
            continue;
        }
        if (u_cd_[k] > 0) continue;
        const TargetPick t = pick_target(k, a);
        if (!t.found) continue;
        // 有效射程 = 基础值 + 远程驻守的高度加成（掩码用同一个函数）。
        const float range = effective_range(k);
        if (t.dist2 > range * range) continue;   // 打不到就空转（乙）
        commit_attack(k, t);
    }
}

// ——阶段 3：移动——

void World::tick_movement() {
    const float w_limit = static_cast<float>(width());
    const float h_limit = static_cast<float>(height());
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        // 前摇钉住脚（弓手被贴脸即废的一半）。冲锋动量在此**冻结**——
        // 承诺出手的那一击正带着它，落地时结算并耗尽（land_attack）。
        if (u_windup_[k] > 0) continue;
        // 驻守钉住（含在爬的）：「上墙的代价是机动性」。想走？先撤登墙意愿（下墙）。
        if (u_garrison_[k] != kNoSlot) continue;
        const UnitType type = u_type_[k];
        const bool charger = behavior_of(type).charges();
        const UnitAction a = u_action_[k];
        if (!is_move(a)) {
            // 站住（Stop，或 Atk* 的空转）就没有助跑可言。
            if (charger) u_charge_[k] = 0.0f;
            continue;
        }
        const float speed = stats_.of(type).speed;
        if (speed <= 0.0f) {
            if (charger) u_charge_[k] = 0.0f;
            continue;
        }
        const Mobility mob = is_aerial(type) ? Mobility::Aerial : Mobility::Ground;

        const GridDelta d = move_delta(a);
        const float step = is_grid_diagonal(a) ? speed / kSqrt2 : speed;
        Vec2 np{u_pos_[k].x + static_cast<float>(d.di) * step,
                u_pos_[k].y + static_cast<float>(d.dj) * step};
        // 边界夹紧：贴边走是合法的，出界不是。
        if (np.x < 0.0f) np.x = 0.0f;
        if (np.y < 0.0f) np.y = 0.0f;
        if (np.x >= w_limit) np.x = w_limit - 0.001f;
        if (np.y >= h_limit) np.y = h_limit - 0.001f;

        const GridPos from = grid_of(u_pos_[k]);
        const GridPos to = grid_of(np);
        if (to == from) {
            // 动量累计**实际位移**（夹紧可能吃掉一部分），不是名义步长。
            if (charger) u_charge_[k] += std::sqrt(dist2(u_pos_[k], np));
            u_pos_[k] = np;
            continue;
        }

        // 跨格：地形 + 实体占位。**掩码只判地形**（墙是「高代价可通行」，
        // 契约 §5.1.1 明写它不该出现在掩码里）——实体拦路在这里处理，
        // 代价就是下面那一下「撞上去自动开始破坏」。
        const Side my_side = side_of(type);
        const auto cell_open = [&](int x, int y) {
            if (!terrain_.in_bounds(x, y)) return false;
            if (!terrain_.passable(x, y, mob)) return false;
            if (mob == Mobility::Aerial) return true;   // 空军飞过一切
            const std::size_t c = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(width()) +
                                  static_cast<std::size_t>(x);
            if (obstacle_at_[c] != 0) return false;
            if (bld_at_[c] == 0) return true;
            // 城门对自己人是通的（机制第六批）：守方地面单位可穿行**完工**的
            // `Gate`。没有这条，守方被自己的墙圈死——「被迫出城争夺外部资源」
            // 在结构上不可能发生，而那是 CLAUDE.md 两个不能砍的机制之一。
            // 攻方照旧要砸开它（try_bump_attack）；工地状态的门不通（还没有
            // 门洞）。flow field 的通行规则抄的是这一条（rts/flow.cpp），
            // 两处必须同真值，tests/flow_test.cpp 锁。
            const std::size_t s = static_cast<std::size_t>(bld_at_[c] - 1);
            return my_side == Side::Defender && b_type_[s] == BldType::Gate &&
                   b_built_[s] != 0;
        };
        bool ok = cell_open(to.i, to.j);
        if (ok && kDiagonalNeedsBothOrthogonal && to.i != from.i && to.j != from.j) {
            ok = cell_open(to.i, from.j) && cell_open(from.i, to.j);
        }
        if (ok) {
            if (charger) u_charge_[k] += std::sqrt(dist2(u_pos_[k], np));
            u_pos_[k] = np;
            continue;
        }

        // 拦住了：先试自动破坏——**骑士撞门是真撞**，承诺成了动量就被那一击
        // 带走（前摇冻结、落地耗尽）；没承诺成才算撞停，动量归零。
        const bool committed = try_bump_attack(k, to);
        if (!committed && charger) u_charge_[k] = 0.0f;
    }
}

// 移动被实体拦住时的自动破坏。若目的格地形本可通行、是实体占着 ⇒ 自动承诺
// 破坏它——「墙 = 高代价可通行」的机制那一半：代价就是站在这儿把它砸开。
bool World::try_bump_attack(std::size_t k, GridPos to) {
    const UnitType type = u_type_[k];
    if (is_aerial(type)) return false;   // 空军飞过一切，不会被实体拦住
    if (!terrain_.in_bounds(to.i, to.j)) return false;
    if (!terrain_.passable(to.i, to.j, Mobility::Ground)) return false;
    if (u_cd_[k] > 0) return false;

    const std::size_t c = static_cast<std::size_t>(to.j) *
                              static_cast<std::size_t>(width()) +
                          static_cast<std::size_t>(to.i);
    const UnitBehavior& beh = behavior_of(type);
    TargetPick t;
    if (bld_at_[c] != 0) {
        // 建筑全属守方：守方单位被自家建筑拦住只能站着，不打自己人。
        if (side_of(type) != Side::Attacker) return false;
        const std::size_t s = static_cast<std::size_t>(bld_at_[c] - 1);
        const BldType bt = b_type_[s];
        const bool is_wall = (bt == BldType::Wall || bt == BldType::Gate);
        if (is_wall ? !beh.can_break_structure() : !beh.combat()) return false;
        t.found = true;
        t.kind = TgtKind::Bld;
        t.raw = bld_pool_.id_at(static_cast<std::uint16_t>(s)).raw();
    } else if (obstacle_at_[c] != 0) {
        // 中立障碍双方都可清（守方清野得产出，攻方纯开路）。
        if (!beh.can_break_structure()) return false;
        const std::size_t s = static_cast<std::size_t>(obstacle_at_[c] - 1);
        t.found = true;
        t.kind = TgtKind::Obstacle;
        t.raw = obstacle_pool_.id_at(static_cast<std::uint16_t>(s)).raw();
    } else {
        return false;
    }
    t.pos = center_of(to);
    t.dist2 = dist2(u_pos_[k], t.pos);
    commit_attack(k, t);
    return true;
}

// ——阶段 4：驻守（第三批）——

// 槽位 `k` 上的单位是否站在有高度的墙上。三条高度优势共用这一个判据；
// 「墙血 < 一半 ⇒ 高度没了」是局部破损那条 Stronghold 规则的二值实现，
// 判据是比例（hp*2 >= max_hp），不引入新数值。
bool World::on_high_wall(std::size_t k) const {
    if (u_garrison_[k] == kNoSlot || u_mount_[k] > 0) return false;
    const std::size_t c = static_cast<std::size_t>(u_garrison_[k]);
    // 防御式：墙被拆时 destroy_bld 会强制下墙，这里理应总能查到建筑。
    if (bld_at_[c] == 0) return false;
    const std::size_t b = static_cast<std::size_t>(bld_at_[c] - 1);
    return b_hp_[b] * 2 >= b_max_hp_[b];
}

// 有效射程：基础值 + 远程驻守的高度加成。**只有远程吃加成**（CLAUDE.md 原文
// 「远程单位在墙上获得射程加成」），判据是三轴里的交战距离——给近战加射程
// 会让枪卫隔空扎人，那不是高度优势，是改机制。
float World::effective_range(std::size_t k) const {
    float r = stats_.of(u_type_[k]).range;
    if (on_high_wall(k) &&
        behavior_of(u_type_[k]).engage_range() == EngageRange::Ranged) {
        r += stats_.global.high_ground_range_bonus;
    }
    return r;
}

bool World::unit_can_engage(std::size_t k, UnitType target) const {
    if (behavior_of(u_type_[k]).can_engage(target)) return true;
    return u_type_[k] == UnitType::Archer && is_aerial(target) &&
           u_garrison_[k] != kNoSlot && u_mount_[k] == 0;
}

// 让槽位 `k` 上的单位离开墙。在爬的直接解除（人本来就还在地面原位）；
// 已登顶的落到第一个空邻格。邻格全被占则**保持驻守**（确定性无操作）——
// 墙格几乎总有空邻格（地图校验器要求墙有内外两侧），不值得一个重试态。
void World::dismount_unit(std::size_t k) {
    if (u_garrison_[k] == kNoSlot) return;
    if (u_mount_[k] > 0) {
        u_garrison_[k] = kNoSlot;
        u_mount_[k] = 0;
        return;
    }
    GridPos out;
    if (!find_free_ground_cell(pos_of_slot(u_garrison_[k], width()), out)) return;
    u_pos_[k] = center_of(out);
    u_garrison_[k] = kNoSlot;
}

void World::tick_garrison() {
    // 先推进在爬的，再受理登墙意愿——顺序写死是规范的一部分。
    // 到 0 的那一刻人**落位墙心**：驻守单位的位置就是墙格中心，射程、视野、
    // 挨打（含 AOE 波及）全按这个位置算，不需要任何「在墙上」的特殊坐标系。
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        if (u_garrison_[k] == kNoSlot || u_mount_[k] <= 0) continue;
        --u_mount_[k];
        if (u_mount_[k] == 0) {
            u_pos_[k] = center_of(pos_of_slot(u_garrison_[k], width()));
        }
    }

    // 受理意愿：按槽位下标升序遍历单位（与 `enumerate_units` 同一个规范顺序）。
    // 意愿是逐单位的（`submit_garrison_wishes`，脚本每个决策拍重发一次），
    // 「驻守者阵亡后谁补位」因此是脚本下一决策拍的事，世界不再持常设指令。
    //
    // 意愿与现状不一致即纠偏：指着别段墙 = 先下来；意愿清空 = 下墙。
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        const std::uint16_t wish = u_garrison_target_[k];
        if (u_garrison_[k] != kNoSlot && u_garrison_[k] != wish) {
            // 已上墙（含在爬）但意愿改了或撤了：下墙。邻格全满时 dismount_unit
            // 保持驻守（确定性无操作），脚本下一拍可再试。
            dismount_unit(k);
        }
        if (wish == kNoSlot) continue;
        if (u_garrison_[k] != kNoSlot) continue;   // 已在目标墙上 / 下墙未成
        if (u_windup_[k] > 0) continue;            // 已承诺的出手先走完
        // 攻方没有登墙手段（CLAUDE.md，结构）。攻方单位的意愿通道本就进不来
        // （submit_garrison_wishes 拒收），这一条是把结构写明。
        if (side_of(u_type_[k]) != Side::Defender) continue;
        // 意愿逐 tick 重验：指着的那格必须是**完工**的 Wall/Gate。脚本读的是
        // 迷雾下的记忆，它记得的墙可能已被拆（destroy_bld 会清意愿）或从未
        // 盖完——这里再挡一道，不把「记忆的时效」推给脚本去赌。
        const std::size_t cell = static_cast<std::size_t>(wish);
        if (bld_at_[cell] == 0) continue;
        const std::size_t b = static_cast<std::size_t>(bld_at_[cell] - 1);
        if (b_type_[b] != BldType::Wall && b_type_[b] != BldType::Gate) continue;
        if (!b_built_[b]) continue;
        // 一格一人：有人在上面（或在爬）就轮不到本单位。
        bool taken = false;
        for (std::size_t u = 0; u < unit_pool_.slot_count(); ++u) {
            if (!unit_pool_.alive_at(static_cast<std::uint16_t>(u))) continue;
            if (u_garrison_[u] == wish) {
                taken = true;
                break;
            }
        }
        if (taken) continue;
        // 「原地登上」：站在墙的八邻才登得上（切比雪夫距离恰为 1）。
        // 走到墙边是脚本的事——它给单位下 Move* 动作与登墙意愿是同一拍。
        const GridPos wall = pos_of_slot(wish, width());
        const GridPos at = grid_of(u_pos_[k]);
        const int dx = at.i > wall.i ? at.i - wall.i : wall.i - at.i;
        const int dy = at.j > wall.j ? at.j - wall.j : wall.j - at.j;
        if (dx > 1 || dy > 1 || (dx == 0 && dy == 0)) continue;
        u_garrison_[k] = wish;
        const std::int32_t d = stats_.global.garrison_mount_ticks;
        u_mount_[k] = d;
        if (d == 0) u_pos_[k] = center_of(wall);   // 零延迟：当场登顶
    }
}

// ——阶段 5：建筑战斗——

void World::tick_bld_combat() {
    for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
        if (!bld_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        // 判据是**完工位**而不是 `b_work_ == 0`：维修复用 `b_work_`，
        // 在修的塔照常开火——抢修残血结构是波次中的正当操作，不是自废武功。
        if (!b_built_[k]) continue;
        if (b_cd_[k] > 0) --b_cd_[k];
        const BldStats& s = stats_.of(b_type_[k]);
        if (b_windup_[k] > 0) {
            --b_windup_[k];
            if (b_windup_[k] == 0) land_bld_attack(k);
            continue;
        }
        if (s.damage <= 0 || b_cd_[k] > 0) continue;

        // 选最近的攻方单位。对空 / 对地按结构谓词过滤：`Flak` 仅对空、
        // 其余仅对地——写反任何一边，「AA 的机会成本」那一整节论证就塌了。
        const bool wants_air = bld_targets_air(b_type_[k]);
        const Vec2 my_pos = center_of(b_pos_[k]);
        bool found = false;
        std::uint32_t best_raw = 0;
        float best_d2 = 0.0f;
        for (std::size_t t = 0; t < unit_pool_.slot_count(); ++t) {
            if (!unit_pool_.alive_at(static_cast<std::uint16_t>(t))) continue;
            if (side_of(u_type_[t]) != Side::Attacker) continue;
            if (is_aerial(u_type_[t]) != wants_air) continue;
            const float d2 = dist2(my_pos, u_pos_[t]);
            if (!found || d2 < best_d2) {
                found = true;
                best_raw = unit_pool_.id_at(static_cast<std::uint16_t>(t)).raw();
                best_d2 = d2;
            }
        }
        if (!found || best_d2 > s.range * s.range) continue;
        b_tgt_raw_[k] = best_raw;
        // 齐射的落点在**承诺那一刻**锁定（与 `Ram` 同一条规则）：
        // 目标散开，箭雨还是下在原地。
        b_aim_[k] = u_pos_[static_cast<std::size_t>(best_raw >> 16)];
        b_cd_[k] = s.cooldown_ticks;
        b_windup_[k] = s.windup_ticks;
        if (s.windup_ticks == 0) land_bld_attack(k);
    }
}

// 建筑出手落地 = **放箭**（第五批起）：建筑不会近战，开火即弹丸。
// `Tower` 的齐射箭雨飞向锁定落点，`Flak` 的狙击弩矢追踪目标。
void World::land_bld_attack(std::size_t k) {
    const BldStats& s = stats_.of(b_type_[k]);
    const std::uint32_t raw = b_tgt_raw_[k];
    b_tgt_raw_[k] = UnitId::kInvalidRaw;

    ProjSpec p;
    p.pos = center_of(b_pos_[k]);
    p.speed = s.proj_speed;
    p.side = Side::Defender;
    p.src_bld = static_cast<std::uint8_t>(b_type_[k]);
    p.from_high = 1;   // 塔上放箭，居高是结构（这一位只管**免掉**高度惩罚）
    p.dmg = s.damage;
    // 等级倍率（建筑等级上限落地后）：与单位同一条规则——基础值不动，
    // 倍率随弹丸走、命中那一刻在 `impact_projectile` 里与高度惩罚一次乘完
    // （截断只发生一次，决定 ⑫）。1 级恒为 1000（no-op），旧行为不变。
    p.lvl_pm = level_permille(b_level_[k], stats_.global.dmg_permille_per_level);

    // 齐射：AOE 砸锁定落点，圈内不分敌我（「溅射误伤」是机制不是 bug——
    // 别站在自家箭楼的齐射区里）、空中不挨砸（箭雨对地，展开在命中路径）。
    // **只对对地建筑生效**：「AA 只做单体狙击型」是结构，表里给 `Flak`
    // 配了半径也不齐射——同「表不能把瞭望塔配成印钞机」的先例。
    if (s.aoe_radius > 0.0f && !bld_targets_air(b_type_[k])) {
        p.kind = TgtKind::None;
        p.aim = b_aim_[k];
        p.aoe = s.aoe_radius;
        launch_projectile(p);
        return;
    }
    // 单体：追踪目标。目标在前摇里就死了 ⇒ 没有箭可放。
    if (raw == UnitId::kInvalidRaw || !unit_pool_.alive(unit_from_raw(raw))) return;
    p.kind = TgtKind::Unit;
    p.raw = raw;
    p.aim = u_pos_[static_cast<std::size_t>(raw >> 16)];
    launch_projectile(p);
}

// ——阶段 6：弹丸（第五批）——

// 字段数组 ↔ ProjSpec 的唯一互换点（与 launch_projectile 成对）。
// 两处若各写一遍字段清单，加字段时只改一边不会报错——症状是命中结算
// 读到默认值。
World::ProjSpec World::proj_at(std::size_t i) const {
    ProjSpec p;
    p.pos = p_pos_[i];
    p.aim = p_aim_[i];
    p.speed = p_speed_[i];
    p.kind = p_kind_[i];
    p.raw = p_raw_[i];
    p.dmg = p_dmg_[i];
    p.lvl_pm = p_lvl_pm_[i];
    p.from_high = p_from_high_[i];
    p.aoe = p_aoe_[i];
    p.side = p_side_[i];
    p.src_bld = p_src_bld_[i];
    return p;
}

void World::launch_projectile(const ProjSpec& p) {
    if (p.speed <= 0.0f) {
        // 瞬时命中：§1.1.1 那条「落地帧瞬时结算」书面近似的退化形态。
        // 没配速度的表行为一字不变——第五批之前的每条测试因此不用动。
        impact_projectile(p);
        return;
    }
    p_pos_.push_back(p.pos);
    p_origin_.push_back(p.pos);
    p_aim_.push_back(p.aim);
    p_speed_.push_back(p.speed);
    p_kind_.push_back(p.kind);
    p_raw_.push_back(p.raw);
    p_dmg_.push_back(p.dmg);
    p_lvl_pm_.push_back(p.lvl_pm);
    p_from_high_.push_back(p.from_high);
    p_aoe_.push_back(p.aoe);
    p_side_.push_back(p.side);
    p_src_bld_.push_back(p.src_bld);
}

// 命中结算。高度 miss / 减伤在**这一刻**掷与乘（箭到的时候人在不在墙上，
// 与放箭那一刻无关——躲上墙是真的能让飞来的箭打折）；发射方那一侧的免除
// 用的是随弹携带的 from_high（放箭时定格，发射方此刻可能已经死了）。
void World::impact_projectile(const ProjSpec& p) {
    const std::int64_t hg_dmg = stats_.global.high_ground_dmg_permille;
    const std::int64_t hg_miss = stats_.global.high_ground_miss_permille;
    const auto misses_high = [&](std::size_t t) {
        if (p.from_high != 0 || !on_high_wall(t)) return false;
        return static_cast<std::int64_t>(rng_.below(1000)) < hg_miss;
    };
    const auto dmg_vs_unit = [&](std::size_t t) {
        std::int64_t mods[2] = {p.lvl_pm, 0};
        std::size_t n = 1;
        if (p.from_high == 0 && on_high_wall(t)) mods[n++] = hg_dmg;
        // 反冲锋不在这里：没有兵种既放箭又架枪阵（轴组合，测试锁住）。
        return apply_permille(p.dmg, mods, n);
    };
    switch (p.kind) {
        case TgtKind::Unit: {
            // 箭还在飞，人已经没了 ⇒ 落空（飞行阶段也查，这里再查一遍是
            // 因为瞬时命中不经飞行阶段，且同 tick 前面的命中可能刚杀了它）。
            if (!unit_pool_.alive(unit_from_raw(p.raw))) return;
            const std::size_t t = static_cast<std::size_t>(p.raw >> 16);
            if (misses_high(t)) return;
            deal_damage(TgtKind::Unit, p.raw, dmg_vs_unit(t), p.side);
            return;
        }
        case TgtKind::Bld:
            if (bld_pool_.alive(bld_from_raw(p.raw))) {
                deal_damage(TgtKind::Bld, p.raw, p.dmg, p.side);
            }
            return;
        case TgtKind::Obstacle:
            if (obstacle_pool_.alive(obstacle_from_raw(p.raw))) {
                deal_damage(TgtKind::Obstacle, p.raw, p.dmg, p.side);
            }
            return;
        case TgtKind::None: {
            // 建筑箭雨不伤己方；单位溅射仍保留原有规则。来源随弹丸保存，
            // 发射建筑在命中前被拆除也不影响阵营判定。
            const float r2 = p.aoe * p.aoe;
            // 校准诊断（2026-09-02）：记这发的实际命中数——`volley_hits_`
            // 的消费者是 §7 runner（见 `World::volley_hits()` 那段注释）。
            std::int32_t hits = 0;
            for (std::size_t t = 0; t < unit_pool_.slot_count(); ++t) {
                if (!unit_pool_.alive_at(static_cast<std::uint16_t>(t))) continue;
                if (p.src_bld && side_of(u_type_[t]) == p.side) continue;
                if (is_aerial(u_type_[t])) continue;
                if (dist2(u_pos_[t], p.aim) > r2) continue;
                if (misses_high(t)) continue;
                deal_damage(TgtKind::Unit,
                            unit_pool_.id_at(static_cast<std::uint16_t>(t)).raw(),
                            dmg_vs_unit(t), p.side);
                ++hits;
            }
            volley_hits_.push_back(hits);
            return;
        }
    }
}

void World::tick_projectiles() {
    if (p_pos_.empty()) return;
    // 逐枚：刷新目的地 → 推进 → 到达即结算；结束后一次性稳定压实（保序左移）。
    // **结算顺序 = 数组序 = 放箭序**，与三组实体的槽位序同族——都是规范序。
    // 同 tick 内前面那枚的命中可能杀死后面那枚的追踪目标：后者轮到自己时
    // **现查**，目标已死即落空——与「箭还在飞，人已经没了」同一条语义。
    std::size_t out = 0;
    for (std::size_t i = 0; i < p_pos_.size(); ++i) {
        bool gone = false;
        Vec2 dest = p_aim_[i];
        switch (p_kind_[i]) {
            case TgtKind::Unit:
                if (!unit_pool_.alive(unit_from_raw(p_raw_[i]))) gone = true;
                else dest = u_pos_[static_cast<std::size_t>(p_raw_[i] >> 16)];
                break;
            case TgtKind::Bld:
                if (!bld_pool_.alive(bld_from_raw(p_raw_[i]))) gone = true;
                else dest = center_of(b_pos_[static_cast<std::size_t>(p_raw_[i] >> 16)]);
                break;
            case TgtKind::Obstacle:
                if (!obstacle_pool_.alive(obstacle_from_raw(p_raw_[i]))) gone = true;
                else dest = center_of(o_pos_[static_cast<std::size_t>(p_raw_[i] >> 16)]);
                break;
            case TgtKind::None:
                break;   // 齐射飞向锁定落点，目的地不刷新
        }
        if (!gone) {
            p_aim_[i] = dest;
            const float d = std::sqrt(dist2(p_pos_[i], dest));
            if (d <= p_speed_[i]) {
                impact_projectile(proj_at(i));
                gone = true;
            } else {
                const float t = p_speed_[i] / d;
                p_pos_[i] = Vec2{p_pos_[i].x + (dest.x - p_pos_[i].x) * t,
                                 p_pos_[i].y + (dest.y - p_pos_[i].y) * t};
            }
        }
        if (gone) continue;
        if (out != i) {
            p_pos_[out] = p_pos_[i];
            p_origin_[out] = p_origin_[i];
            p_aim_[out] = p_aim_[i];
            p_speed_[out] = p_speed_[i];
            p_kind_[out] = p_kind_[i];
            p_raw_[out] = p_raw_[i];
            p_dmg_[out] = p_dmg_[i];
            p_lvl_pm_[out] = p_lvl_pm_[i];
            p_from_high_[out] = p_from_high_[i];
            p_aoe_[out] = p_aoe_[i];
            p_side_[out] = p_side_[i];
            p_src_bld_[out] = p_src_bld_[i];
        }
        ++out;
    }
    p_pos_.resize(out);
    p_origin_.resize(out);
    p_aim_.resize(out);
    p_speed_.resize(out);
    p_kind_.resize(out);
    p_raw_.resize(out);
    p_dmg_.resize(out);
    p_lvl_pm_.resize(out);
    p_from_high_.resize(out);
    p_aoe_.resize(out);
    p_side_.resize(out);
    p_src_bld_.resize(out);
}

// ——阶段 7：经济（第二批）——

// 守方 `Mason` 在 `pos` 施工半径内的人数。施工/维修/升级按人数**线性加速**
// （2026-09-01 起，试玩拍板：工匠优先各管一个任务——脚本侧认领；任务不够
// 分时多人同任务，同任务必须真的更快，否则「都在干活」与「一个人在干活」
// 无法区分）。半径查全局表。
std::int32_t World::mason_count(GridPos pos) const {
    const float r = stats_.global.mason_work_radius;
    if (r <= 0.0f) return 0;
    const float r2 = r * r;
    const Vec2 c = center_of(pos);
    std::int32_t n = 0;
    for (std::size_t s = 0; s < unit_pool_.slot_count(); ++s) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(s))) continue;
        if (u_type_[s] != UnitType::Mason) continue;
        if (dist2(u_pos_[s], c) <= r2) ++n;
    }
    return n;
}

bool World::mason_near(GridPos pos) const {
    return mason_count(pos) > 0;
}

// 八邻按行主序（dj 外层、di 内层）扫，取第一个地形可通行、无建筑无障碍、
// 无地面单位站着的格。**顺序即规范**，换一种扫法就是换一份回放。
// 征兵出兵与驻守下墙共用它——两处各扫一套就是两个会分叉的规范。
bool World::find_free_ground_cell(GridPos at, GridPos& out) const {
    for (int dj = -1; dj <= 1; ++dj) {
        for (int di = -1; di <= 1; ++di) {
            if (di == 0 && dj == 0) continue;
            const int x = at.i + di;
            const int y = at.j + dj;
            if (!terrain_.in_bounds(x, y)) continue;
            if (!terrain_.passable(x, y, Mobility::Ground)) continue;
            const std::size_t c = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(width()) +
                                  static_cast<std::size_t>(x);
            if (bld_at_[c] != 0 || obstacle_at_[c] != 0) continue;
            bool taken = false;
            for (std::size_t s = 0; s < unit_pool_.slot_count(); ++s) {
                if (!unit_pool_.alive_at(static_cast<std::uint16_t>(s))) continue;
                if (is_aerial(u_type_[s])) continue;
                const GridPos g = grid_of(u_pos_[s]);
                if (g.i == x && g.j == y) {
                    taken = true;
                    break;
                }
            }
            if (taken) continue;
            out = GridPos{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
            return true;
        }
    }
    return false;
}

// 给槽位 `k` 上的建筑找出兵格并出兵。找不到返回 false（下 tick 再试）。
bool World::try_train_spawn(std::size_t k) {
    GridPos cell;
    if (!find_free_ground_cell(b_pos_[k], cell)) return false;

    const UnitType ut = static_cast<UnitType>(b_train_type_[k]);
    const UnitStats& s = stats_.of(ut);
    // 出兵等级是 `apply_one` 的 `Train` 分支选定、存在 `b_train_level_[k]`
    // 里的那个（兵种等级上限落地——此前这里恒用 `kMinUnitLevel`）。
    // 等级缩放照走公式，1 级时它就是基础值，所以这次落地这一行才只改了
    // 「读哪个变量」，没改公式本身。
    const std::int32_t lvl = b_train_level_[k];
    const std::int64_t hp = apply_permille(
        s.max_hp, {level_permille(lvl, stats_.global.hp_permille_per_level)});
    spawn_unit(ut, center_of(cell), lvl, hp, hp);
    b_train_type_[k] = kNoTrain;
    b_train_left_[k] = 0;
    b_train_level_[k] = kMinUnitLevel;
    return true;
}

void World::tick_economy() {
    // 施工 / 维修：工匠在场才走工时，**按在场人数线性加速**（2026-09-01 起，
    // 见 `mason_count` 注释——此前是「在场与否」二值占位）。血量按「剩余缺口
    // ÷ 剩余工时」向上取整逐工时补上——无人打扰时恰好在工时归零那一刻到满；
    // 挨了打则往后的每工时多补一点、**总工期不变**（工期是买定的，血量是工期
    // 的产出）。这条自我修正意味着工地挨打不延长工期，只压低它全程的血量
    // 下限——要打断它得打死它，或者点杀工匠（那才是设计给 AI 的目标）。
    // 占位决定，标定时若要「挨打延工」再改。多人同任务时最后几拍一次补
    // 多份工时，血量由 clamp 兜底到满，不会溢出。
    for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
        if (!bld_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        if (b_work_[k] <= 0) continue;
        const std::int32_t crew = mason_count(b_pos_[k]);
        if (crew <= 0) continue;
        const std::int64_t gap = b_max_hp_[k] - b_hp_[k];
        if (gap > 0) {
            const std::int64_t share =
                (gap + b_work_[k] - 1) / b_work_[k] * crew;
            b_hp_[k] = (share >= gap) ? b_max_hp_[k] : b_hp_[k] + share;
        }
        b_work_[k] = (b_work_[k] > crew) ? b_work_[k] - crew : 0;
        if (b_work_[k] == 0) b_built_[k] = 1;
    }

    // 建筑升级：与施工/维修同一条纪律——工匠在场才推进（人数同样线性加速），
    // 点杀维匠一样能拖慢它。倒计时归零即完工，逻辑收在 `finish_upgrade`
    // （与 `apply_one` 的「工期 <= 0 当场完工」共用同一个实现）。
    for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
        if (!bld_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        if (b_upgrade_left_[k] <= 0) continue;
        const std::int32_t crew = mason_count(b_pos_[k]);
        if (crew <= 0) continue;
        b_upgrade_left_[k] =
            (b_upgrade_left_[k] > crew) ? b_upgrade_left_[k] - crew : 0;
        if (b_upgrade_left_[k] == 0) finish_upgrade(k);
    }

    // 征兵：倒计时归零后出兵。邻格全被占就滞留（不消单、不退钱），
    // 下 tick 再试——征兵出口被自己人堵住是玩家该解的局面，不是机制该变的魔法。
    for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
        if (!bld_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        if (b_train_type_[k] == kNoTrain) continue;
        if (b_train_left_[k] > 0) {
            --b_train_left_[k];
            continue;
        }
        try_train_spawn(k);
    }

    // 入账：每 `income_period_ticks` 一次（首次在第 period-1 tick 末，
    // 即恰好推进一个周期之后）。采集建筑要**踩在对应资源点上**才产出——
    // 建造时查过一遍，这里再按同一条件付账，付账条件是唯一真相
    // （初始建筑不经 Build 命令，只有这里能拦住摆错位置的表）。
    // **且那个点必须已经解禁**（`wave_ >= ResourceSite::unlock_wave`）——
    // 两条都在同一处判，理由相同：付账条件只能有一份。
    const std::int32_t period = stats_.global.income_period_ticks;
    if (period > 0 && (tick_ + 1) % period == 0) {
        for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
            if (!bld_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
            if (!b_built_[k]) continue;
            const BldType bt = b_type_[k];
            const std::int64_t amount = stats_.of(bt).income_amount;
            if (amount <= 0) continue;
            if (is_gatherer(bt)) {
                bool unlocked = false;
                ResourceTier tier = ResourceTier::Inner;
                for (const ResourceSite& r : resources_) {
                    if (r.pos == b_pos_[k] && r.kind == resource_of(bt)) {
                        // 资源点随波数解禁（CLAUDE.md 同名一节）：还没到解禁波，
                        // 建筑盖着也不入账——玩家可以早建，只是白等，不是白建
                        // （占位不冲突，采集建筑的其余用途，比如挡路，仍然成立）。
                        unlocked = wave_ >= r.unlock_wave;
                        tier = r.tier;
                        break;
                    }
                }
                if (!unlocked) continue;
                // 城外资源点产出倍率（2026-09-02，`地图生成器大改方案.md` §4
                // 第四条）：按踩着的点的 tier 乘 `tier_income_permille_` 对应档。
                // 记账不走 `apply_permille`——「钳到 >= 1」是伤害规则（见
                // `world.cpp` 维修那条）；舍入同 `train_ticks_at`：
                // 加半个分母再除，唯一一次除法、四舍五入。
                const std::int64_t pm = tier == ResourceTier::Outer
                                            ? tier_income_permille_.outer
                                            : tier_income_permille_.inner;
                stock_[static_cast<std::size_t>(resource_of(bt))] +=
                    (amount * pm + kPermilleOne / 2) / kPermilleOne;
            } else if (bt == BldType::Keep) {
                // 兵力地板：`Keep` 恒产极少量金币（CLAUDE.md 单列一节的护栏，
                // 「三种资源都来自资源点」的唯一有意例外）。数额查表、待标定，
                // 但**是金币**这一点是结构——它兜的是「金矿被点掉后连补兵
                // 都做不到」那种死亡螺旋。
                stock_[static_cast<std::size_t>(Resource::Gold)] += amount;
            }
            // 其余建筑填了 income_amount 也不产出：种类映射是结构，
            // 表不能把瞭望塔配成印钞机。
        }
    }
}

// 建筑升级到账：等级 +1、按新等级从**表里的基础值**重算 `max_hp`
// （不是从当前 `b_max_hp_` 累乘——那会把上一级的缩放误差复利，累乘出来的数
// 与「直接从 1 级基础值算 N 级」不再是同一个数），完工即满血。
// `apply_one`（工期 <= 0，当场完工）与 `tick_economy`（倒计时归零）
// 共用这一个实现，理由同 `repair_wood_cost` 那条「两处算法分叉是绿框骗人
// 的来源」的纪律——升级公式也只该有一处。
void World::finish_upgrade(std::size_t k) {
    const std::int32_t new_level = b_level_[k] + 1;
    const std::int64_t new_max = apply_permille(
        stats_.of(b_type_[k]).max_hp,
        {level_permille(new_level, stats_.global.hp_permille_per_level)});
    b_hp_[k] = std::max<std::int64_t>(1, b_hp_[k] * new_max / b_max_hp_[k]);
    b_level_[k] = new_level;
    b_max_hp_[k] = new_max; // Upgrading preserves the damage fraction; repairs remain necessary.
    b_upgrade_left_[k] = 0;
}

// ——阶段 8：视野——

void World::tick_vision() {
    for (int s = 0; s < kSideCount; ++s) {
        fog_[static_cast<std::size_t>(s)].begin_tick();
    }
    // 单位视野。空中单位不吃视线遮挡——`Phoenix` 与 `Wraith`（2026-09-03 起
    // 同为空军）高悬于遮挡物之上；地面单位照常被林与岩挡。
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        const UnitType t = u_type_[k];
        FogLayer& f = fog_[static_cast<std::size_t>(index_of(side_of(t)))];
        mark_vision_circle(f, terrain_, u_pos_[k], stats_.of(t).vision, is_aerial(t),
                           tick_);
    }
    // 建筑视野（建筑全属守方）。完工才有——施工中的骨架没有人在上面瞭望；
    // 在修的（`b_built_` 且 `b_work_ > 0`）照常有，同 tick_bld_combat 那条判据。
    FogLayer& df = fog_[static_cast<std::size_t>(index_of(Side::Defender))];
    for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
        if (!bld_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        if (!b_built_[k]) continue;
        mark_vision_circle(df, terrain_, center_of(b_pos_[k]),
                           stats_.of(b_type_[k]).vision, false, tick_);
    }
    // 记忆图：可见格上的建筑写进去，可见的空格记「这里没有建筑」。
    // 后者就是「缺口」——三态设计的全部意义（`rts/fog.hpp`）。
    for (int s = 0; s < kSideCount; ++s) {
        FogLayer& f = fog_[static_cast<std::size_t>(s)];
        for (int y = 0; y < height(); ++y) {
            for (int x = 0; x < width(); ++x) {
                if (f.at(x, y) != Vis::Visible) continue;
                const std::size_t c = static_cast<std::size_t>(y) *
                                          static_cast<std::size_t>(width()) +
                                      static_cast<std::size_t>(x);
                if (bld_at_[c] != 0) {
                    const std::size_t b = static_cast<std::size_t>(bld_at_[c] - 1);
                    std::int64_t pm = 0;
                    if (b_max_hp_[b] > 0) {
                        pm = b_hp_[b] * 1000 / b_max_hp_[b];
                        if (pm < 0) pm = 0;
                        if (pm > 1000) pm = 1000;
                    }
                    f.remember_bld(x, y, b_type_[b], static_cast<std::uint16_t>(pm));
                } else {
                    f.remember_no_bld(x, y);
                }
            }
        }
    }
}

}  // namespace rts
