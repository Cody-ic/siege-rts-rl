#include "rts/flow.hpp"

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>

#include "rts/combat_math.hpp"
#include "rts/stats.hpp"
#include "rts/terrain.hpp"
#include "rts/unit_behavior.hpp"
#include "rts/world_view.hpp"

namespace rts {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
// 与 mechanics.cpp 的 kSqrt2 同值：斜向步的几何要与移动机制一致，
// 否则 field 的斜向估价系统性偏离实际行军时间。
constexpr float kSqrt2 = 1.41421356f;

}  // namespace

float estimate_breach_ticks(const StatsTable& stats, UnitType mover,
                            std::int32_t level, std::int64_t hp) {
    const auto& s = stats.of(mover);
    if (s.damage <= 0) return kInf;
    const auto damage = apply_permille(s.damage, {
        level_permille(level, stats.global.dmg_permille_per_level), s.vs_structure_permille});
    const auto hits = std::max<std::int64_t>(1, hp / damage + (hp % damage != 0));
    const auto period = std::max(s.cooldown_ticks, s.windup_ticks + 1);
    const float flight = behavior_of(mover).launches_projectile() && s.proj_speed > 0
        ? std::ceil(1.0f / s.proj_speed) : 0.0f;
    return 1.0f + static_cast<float>(s.windup_ticks) + flight +
        static_cast<float>(hits - 1) * static_cast<float>(period);
}

FlowField FlowField::compute(const WorldView& view, UnitType mover, int tier,
                             std::span<const GridPos> goals,
                             const FlowTiering& tiering) {
    assert(tier >= 0 && tier < kFlowTierCount);
    assert(tiering.mid_from >= 2 && tiering.high_from > tiering.mid_from);

    return compute_impl(view, mover, tier, goals, flow_rep_level(tier, tiering), nullptr, 0);
}

FlowField FlowField::compute_known(const WorldView& view, UnitType mover,
                                   std::int32_t level, std::span<const GridPos> goals,
                                   std::span<const FlowBuilding> buildings, float risk_ticks) {
    assert(level >= 1 && std::isfinite(risk_ticks) && risk_ticks >= 0);
    return compute_impl(view, mover, flow_tier_of(level, {}), goals, level, &buildings, risk_ticks);
}

FlowField FlowField::compute_impl(const WorldView& view, UnitType mover, int tier,
                                  std::span<const GridPos> goals, std::int32_t level,
                                  const std::span<const FlowBuilding>* buildings,
                                  float risk_ticks) {

    FlowField f;
    f.w_ = view.width();
    f.h_ = view.height();
    f.tier_ = tier;
    const std::size_t n =
        static_cast<std::size_t>(f.w_) * static_cast<std::size_t>(f.h_);
    f.cost_.assign(n, kInf);
    f.travel_.assign(n, kInf);
    f.damage_.assign(n, kInf);
    f.dir_.assign(n, kFlowNoDir);

    const StatsTable& stats = view.stats();
    const UnitStats& s = stats.of(mover);
    const UnitBehavior& beh = behavior_of(mover);
    const Side my_side = side_of(mover);

    const auto cell = [&](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(f.w_) +
               static_cast<std::size_t>(x);
    };

    // 进一格的额外代价（tick）：0 = 完全敞开；正数 = 先破坏格上的实体；
    // inf = 此兵种不可入。地形不可通行也折进 inf——于是「完全敞开」恰好是
    // `extra == 0` 一个判据，与 tick_movement 的 cell_open 逐条对应
    // （含城门例外），斜向穿角的两角检查直接用它。
    std::vector<float> extra(n, 0.0f);
    if (!is_aerial(mover)) {   // 空军飞过一切（地形与实体都不挡，机制如此）
        for (std::size_t c = 0; c < n; ++c) {
            const int x = static_cast<int>(c % static_cast<std::size_t>(f.w_));
            const int y = static_cast<int>(c / static_cast<std::size_t>(f.w_));
            if (!view.terrain().passable(x, y)) extra[c] = kInf;
        }

        // 单发结构伤害：与机制同一对函数（combat_math），代表等级 = 档下界。
        // 刻意不带冲锋倍率——动量是瞬态，field 是稳态估计（文件头）。
        const std::int64_t lvl_pm = level_permille(level, stats.global.dmg_permille_per_level);
        const std::int64_t dmg =
            s.damage > 0
                ? apply_permille(s.damage, {lvl_pm, s.vs_structure_permille})
                : 0;
        const float period = static_cast<float>(
            s.windup_ticks + s.cooldown_ticks > 0 ? s.windup_ticks + s.cooldown_ticks
                                                  : 1);
        const auto break_ticks = [&](std::int64_t hp) {
            if (buildings) return estimate_breach_ticks(stats, mover, level, hp);
            return static_cast<float>((hp + dmg - 1) / dmg) * period;
        };

        const auto add_building = [&](BldType type, GridPos pos, std::int64_t hp, bool built) {
            if (!f.in_bounds(pos)) return;
            const std::size_t c = cell(pos.i, pos.j);
            if (extra[c] == kInf) return;
            if (my_side == Side::Defender) {
                // 建筑全属守方：己方建筑不可入，唯一例外是**完工的城门**
                // （与移动机制同一条例外，tests/flow_test.cpp 锁两处同真值）。
                if (!(type == BldType::Gate && built)) {
                    extra[c] = kInf;
                }
            } else {
                // 与 try_bump_attack 同一对谓词：墙 / 门要 can_break_structure，
                // 其余建筑要 combat()。
                const bool is_wall =
                    type == BldType::Wall || type == BldType::Gate;
                const bool can =
                    is_wall ? beh.can_break_structure() : beh.combat();
                extra[c] = (can && dmg > 0) ? break_ticks(hp) : kInf;
            }
        };
        if (buildings) {
            for (const auto& b : *buildings) add_building(b.type, b.pos, b.hp, b.built);
        } else {
            for (std::size_t k = 0; k < view.bld_pos().size(); ++k) {
                if (view.bld_alive()[k]) add_building(view.bld_type()[k], view.bld_pos()[k],
                                                    view.bld_hp()[k], view.bld_built()[k] != 0);
            }
        }

        const auto o_pos = view.obstacle_pos();
        const auto o_hp = view.obstacle_hp();
        const auto o_alive = view.obstacle_alive();
        for (std::size_t k = 0; k < o_pos.size(); ++k) {
            if (!o_alive[k]) continue;
            const std::size_t c = cell(o_pos[k].i, o_pos[k].j);
            if (extra[c] == kInf) continue;
            // 中立障碍双方都可清（try_bump_attack 同一条）。
            extra[c] =
                (beh.can_break_structure() && dmg > 0) ? break_ticks(o_hp[k]) : kInf;
        }
    }

    // Stationary building fire is estimated from last-observed type/level and
    // completion. Repair does not silence a completed tower. Fire goes through
    // terrain in the simulation, so LOS filters discovery, not this coverage.
    std::vector<float> fire(n, 0.0f);
    if (buildings && risk_ticks > 0) {
        for (const auto& b : *buildings) {
            const auto& bs = stats.of(b.type);
            if (!b.built || bs.damage <= 0 || (b.type == BldType::Flak) != is_aerial(mover)) continue;
            const auto hit = apply_permille(bs.damage, {
                level_permille(b.level, stats.global.dmg_permille_per_level)});
            const float rate = static_cast<float>(hit) /
                static_cast<float>(std::max(bs.cooldown_ticks, bs.windup_ticks + 1));
            const int radius = static_cast<int>(std::ceil(bs.range));
            for (int y = std::max(0, b.pos.j-radius); y <= std::min(f.h_-1, b.pos.j+radius); ++y) {
                for (int x = std::max(0, b.pos.i-radius); x <= std::min(f.w_-1, b.pos.i+radius); ++x) {
                    const float dx = static_cast<float>(x-b.pos.i), dy = static_cast<float>(y-b.pos.j);
                    if (dx*dx+dy*dy <= bs.range*bs.range) fire[cell(x,y)] += rate;
                }
            }
        }
    }
    const auto max_hp = apply_permille(s.max_hp, {level_permille(level, stats.global.hp_permille_per_level)});
    const float damage_price = risk_ticks / static_cast<float>(max_hp);

    // （代价, 格下标）全序弹出：代价相等时小下标先出，平局的归属因此规范。
    using QItem = std::pair<float, std::int32_t>;
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> q;

    // 目标格一律视为敞开（理由见头文件）。
    for (const GridPos g : goals) {
        if (!f.in_bounds(g)) continue;
        const std::size_t c = cell(g.i, g.j);
        extra[c] = 0.0f;
        if (f.cost_[c] > 0.0f) {
            f.cost_[c] = 0.0f;
            f.travel_[c] = 0.0f;
            f.damage_[c] = 0.0f;
            q.push({0.0f, static_cast<std::int32_t>(c)});
        }
    }

    if (s.speed <= 0.0f) return f;   // 不会动的兵种：field 只有目标格本身
    const float orth = 1.0f / s.speed;
    const float diag = kSqrt2 / s.speed;

    while (!q.empty()) {
        const auto [d, ci] = q.top();
        q.pop();
        const std::size_t c = static_cast<std::size_t>(ci);
        if (d > f.cost_[c]) continue;   // 陈旧条目
        // 反向松弛：弹出 B、松弛邻居 A，边 A→B 的代价 = 步长 + 「进 B」的
        // 额外项。extra[B] = inf ⇒ B 不能作为路径中转（inf 传染切断传播）。
        const float enter = d + extra[c];
        if (!(enter < kInf)) continue;
        const int bx = static_cast<int>(c % static_cast<std::size_t>(f.w_));
        const int by = static_cast<int>(c / static_cast<std::size_t>(f.w_));
        for (int k = 0; k < kMoveDirCount; ++k) {
            const GridDelta dd = move_delta(move_of(k));
            // A + delta(k) = B ⇒ 存进 dir_[A] 的 k 正是「从 A 迈向 B」那一步。
            const int ax = bx - static_cast<int>(dd.di);
            const int ay = by - static_cast<int>(dd.dj);
            if (ax < 0 || ay < 0 || ax >= f.w_ || ay >= f.h_) continue;
            const bool diag_step = dd.di != 0 && dd.dj != 0;
            if (diag_step && kDiagonalNeedsBothOrthogonal) {
                // 两个正交角要**完全敞开**（extra == 0，含地形与实体，
                // 城门例外已折进 extra）——与移动机制同一条穿角规则。
                if (extra[cell(ax, by)] != 0.0f || extra[cell(bx, ay)] != 0.0f) {
                    continue;
                }
            }
            const std::size_t a = cell(ax, ay);
            const float move_ticks = diag_step ? diag : orth;
            // Breaching happens while still in A, not inside the occupied B.
            const float edge_damage = move_ticks * (fire[a]+fire[c]) * 0.5f + extra[c]*fire[a];
            const float cand = enter + move_ticks + edge_damage*damage_price;
            if (cand < f.cost_[a]) {   // 只有严格更优才改写：平局归先到者（规范序）
                f.cost_[a] = cand;
                f.travel_[a] = f.travel_[c] + extra[c] + move_ticks;
                f.damage_[a] = f.damage_[c] + edge_damage;
                f.dir_[a] = static_cast<std::uint8_t>(k);
                q.push({cand, static_cast<std::int32_t>(a)});
            }
        }
    }
    return f;
}

}  // namespace rts
