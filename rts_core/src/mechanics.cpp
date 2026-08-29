// 机制第一批：承伤与死亡、目标选择与攻击、相邻格移动、建筑攻击、视野。
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
                // 结构判定：无战力打不了任何东西；没有单位能对空。
                if (!beh.can_engage(their)) continue;
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

    const std::int64_t vs_unit = apply_permille(s.damage, {lvl});
    const std::int64_t vs_structure =
        apply_permille(s.damage, {lvl, s.vs_structure_permille});

    if (s.aoe_radius > 0.0f) {
        // AOE 砸**锁定的坐标**。圈内的单位不分敌我（「溅射单位被包夹时会误伤」
        // 是 CLAUDE.md 列的机制性克制，不是 bug），但空中不挨砸（can_engage）。
        const float r2 = s.aoe_radius * s.aoe_radius;
        for (std::size_t t = 0; t < unit_pool_.slot_count(); ++t) {
            if (!unit_pool_.alive_at(static_cast<std::uint16_t>(t))) continue;
            if (t == k) continue;
            if (!beh.can_engage(u_type_[t])) continue;
            if (dist2(u_pos_[t], aim) > r2) continue;
            deal_damage(TgtKind::Unit,
                        unit_pool_.id_at(static_cast<std::uint16_t>(t)).raw(), vs_unit,
                        my_side);
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

    // 单体：目标死了就落空（箭还在飞，人已经没了）。**不追加射程检查**——
    // 承诺时查过一次，之后目标跑出射程照样中，本批没有在途弹丸实体，
    // 这一条近似记在契约 §1.1.1（弹丸做成第四组实体时一并改）。
    switch (kind) {
        case TgtKind::Unit:
            if (unit_pool_.alive(unit_from_raw(raw))) {
                deal_damage(kind, raw, vs_unit, my_side);
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
            u_hp_[t] -= amount;
            if (u_hp_[t] <= 0) kill_unit(id);
            break;
        }
        case TgtKind::Bld: {
            const BldId id = bld_from_raw(raw);
            const std::size_t t = id.index();
            b_hp_[t] -= amount;
            if (b_hp_[t] <= 0) destroy_bld(id);
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
        const UnitAction a = u_action_[k];
        if (a != UnitAction::AtkNear && a != UnitAction::AtkWeak &&
            a != UnitAction::AtkBld && a != UnitAction::AtkWall) {
            continue;
        }
        if (u_cd_[k] > 0) continue;
        const TargetPick t = pick_target(k, a);
        if (!t.found) continue;
        const float range = stats_.of(u_type_[k]).range;
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
        if (u_windup_[k] > 0) continue;   // 前摇钉住脚（弓手被贴脸即废的一半）
        const UnitAction a = u_action_[k];
        if (!is_move(a)) continue;
        const UnitType type = u_type_[k];
        const float speed = stats_.of(type).speed;
        if (speed <= 0.0f) continue;
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
            u_pos_[k] = np;
            continue;
        }

        // 跨格：地形 + 实体占位。**掩码只判地形**（墙是「高代价可通行」，
        // 契约 §5.1.1 明写它不该出现在掩码里）——实体拦路在这里处理，
        // 代价就是下面那一下「撞上去自动开始破坏」。
        const auto cell_open = [&](int x, int y) {
            if (!terrain_.in_bounds(x, y)) return false;
            if (!terrain_.passable(x, y, mob)) return false;
            if (mob == Mobility::Aerial) return true;   // 空军飞过一切
            const std::size_t c = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(width()) +
                                  static_cast<std::size_t>(x);
            return bld_at_[c] == 0 && obstacle_at_[c] == 0;
        };
        bool ok = cell_open(to.i, to.j);
        if (ok && kDiagonalNeedsBothOrthogonal && to.i != from.i && to.j != from.j) {
            ok = cell_open(to.i, from.j) && cell_open(from.i, to.j);
        }
        if (ok) {
            u_pos_[k] = np;
            continue;
        }

        // 拦住了。若目的格地形本可通行、是实体占着 ⇒ 自动承诺破坏它。
        // 这是「墙 = 高代价可通行」的机制那一半：代价就是站在这儿把它砸开。
        if (mob != Mobility::Ground) continue;
        if (!terrain_.in_bounds(to.i, to.j)) continue;
        if (!terrain_.passable(to.i, to.j, Mobility::Ground)) continue;
        if (u_cd_[k] > 0) continue;

        const std::size_t c = static_cast<std::size_t>(to.j) *
                                  static_cast<std::size_t>(width()) +
                              static_cast<std::size_t>(to.i);
        const UnitBehavior& beh = behavior_of(type);
        TargetPick t;
        if (bld_at_[c] != 0) {
            // 建筑全属守方：守方单位被自家建筑拦住只能站着，不打自己人。
            if (side_of(type) != Side::Attacker) continue;
            const std::size_t s = static_cast<std::size_t>(bld_at_[c] - 1);
            const BldType bt = b_type_[s];
            const bool is_wall = (bt == BldType::Wall || bt == BldType::Gate);
            if (is_wall ? !beh.can_break_structure() : !beh.combat()) continue;
            t.found = true;
            t.kind = TgtKind::Bld;
            t.raw = bld_pool_.id_at(static_cast<std::uint16_t>(s)).raw();
        } else if (obstacle_at_[c] != 0) {
            // 中立障碍双方都可清（守方清野得产出，攻方纯开路）。
            if (!beh.can_break_structure()) continue;
            const std::size_t s = static_cast<std::size_t>(obstacle_at_[c] - 1);
            t.found = true;
            t.kind = TgtKind::Obstacle;
            t.raw = obstacle_pool_.id_at(static_cast<std::uint16_t>(s)).raw();
        } else {
            continue;
        }
        t.pos = center_of(to);
        t.dist2 = dist2(u_pos_[k], t.pos);
        commit_attack(k, t);
    }
}

// ——阶段 4：建筑战斗 + 施工——

void World::tick_bld_combat() {
    for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
        if (!bld_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        if (b_work_[k] > 0) {
            --b_work_[k];   // 施工 / 维修中：不开火、无视野（见 tick_vision）
            continue;
        }
        if (b_cd_[k] > 0) --b_cd_[k];
        const BldStats& s = stats_.of(b_type_[k]);
        if (b_windup_[k] > 0) {
            --b_windup_[k];
            if (b_windup_[k] == 0 && b_tgt_raw_[k] != UnitId::kInvalidRaw) {
                const UnitId id = unit_from_raw(b_tgt_raw_[k]);
                if (unit_pool_.alive(id)) {
                    deal_damage(TgtKind::Unit, b_tgt_raw_[k],
                                apply_permille(s.damage, {}), Side::Defender);
                }
                b_tgt_raw_[k] = UnitId::kInvalidRaw;
            }
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
        b_cd_[k] = s.cooldown_ticks;
        b_windup_[k] = s.windup_ticks;
        if (s.windup_ticks == 0) {
            const UnitId id = unit_from_raw(best_raw);
            if (unit_pool_.alive(id)) {
                deal_damage(TgtKind::Unit, best_raw, apply_permille(s.damage, {}),
                            Side::Defender);
            }
            b_tgt_raw_[k] = UnitId::kInvalidRaw;
        }
    }
}

// ——阶段 5：视野——

void World::tick_vision() {
    for (int s = 0; s < kSideCount; ++s) {
        fog_[static_cast<std::size_t>(s)].begin_tick();
    }
    // 单位视野。空中单位不吃视线遮挡——`Phoenix` 高悬于遮挡物之上；
    // `Wraith` 是**地面**（结构性决定），照常被林与岩挡。
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        const UnitType t = u_type_[k];
        FogLayer& f = fog_[static_cast<std::size_t>(index_of(side_of(t)))];
        mark_vision_circle(f, terrain_, u_pos_[k], stats_.of(t).vision, is_aerial(t),
                           tick_);
    }
    // 建筑视野（建筑全属守方）。完工才有——施工中的骨架没有人在上面瞭望。
    FogLayer& df = fog_[static_cast<std::size_t>(index_of(Side::Defender))];
    for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
        if (!bld_pool_.alive_at(static_cast<std::uint16_t>(k))) continue;
        if (b_work_[k] > 0) continue;
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
