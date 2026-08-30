#include "rts/flow.hpp"

#include <cstddef>
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

FlowField FlowField::compute(const WorldView& view, UnitType mover, int tier,
                             std::span<const GridPos> goals,
                             const FlowTiering& tiering) {
    assert(tier >= 0 && tier < kFlowTierCount);
    assert(tiering.mid_from >= 2 && tiering.high_from > tiering.mid_from);

    FlowField f;
    f.w_ = view.width();
    f.h_ = view.height();
    f.tier_ = tier;
    const std::size_t n =
        static_cast<std::size_t>(f.w_) * static_cast<std::size_t>(f.h_);
    f.cost_.assign(n, kInf);
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
        const std::int64_t lvl_pm = level_permille(
            flow_rep_level(tier, tiering), stats.global.dmg_permille_per_level);
        const std::int64_t dmg =
            s.damage > 0
                ? apply_permille(s.damage, {lvl_pm, s.vs_structure_permille})
                : 0;
        const float period = static_cast<float>(
            s.windup_ticks + s.cooldown_ticks > 0 ? s.windup_ticks + s.cooldown_ticks
                                                  : 1);
        const auto break_ticks = [&](std::int64_t hp) {
            return static_cast<float>((hp + dmg - 1) / dmg) * period;
        };

        const auto b_type = view.bld_type();
        const auto b_pos = view.bld_pos();
        const auto b_hp = view.bld_hp();
        const auto b_built = view.bld_built();
        const auto b_alive = view.bld_alive();
        for (std::size_t k = 0; k < b_pos.size(); ++k) {
            if (!b_alive[k]) continue;
            const std::size_t c = cell(b_pos[k].i, b_pos[k].j);
            if (extra[c] == kInf) continue;
            if (my_side == Side::Defender) {
                // 建筑全属守方：己方建筑不可入，唯一例外是**完工的城门**
                // （与移动机制同一条例外，tests/flow_test.cpp 锁两处同真值）。
                if (!(b_type[k] == BldType::Gate && b_built[k] != 0)) {
                    extra[c] = kInf;
                }
            } else {
                // 与 try_bump_attack 同一对谓词：墙 / 门要 can_break_structure，
                // 其余建筑要 combat()。
                const bool is_wall =
                    b_type[k] == BldType::Wall || b_type[k] == BldType::Gate;
                const bool can =
                    is_wall ? beh.can_break_structure() : beh.combat();
                extra[c] = (can && dmg > 0) ? break_ticks(b_hp[k]) : kInf;
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
            const float cand = enter + (diag_step ? diag : orth);
            const std::size_t a = cell(ax, ay);
            if (cand < f.cost_[a]) {   // 只有严格更优才改写：平局归先到者（规范序）
                f.cost_[a] = cand;
                f.dir_[a] = static_cast<std::uint8_t>(k);
                q.push({cand, static_cast<std::int32_t>(a)});
            }
        }
    }
    return f;
}

}  // namespace rts
