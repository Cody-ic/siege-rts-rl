#include "game/demo_driver.hpp"

#include <cstddef>

#include "game/world_builder.hpp"
#include "rts/action.hpp"
#include "rts/combat_math.hpp"
#include "rts/roster.hpp"

namespace game {
namespace {

// 等级缩放后的满血。与 `rts_core` 的伤害走同一对函数（combat_math），
// 不另立一套公式。
std::int64_t hp_at(const rts::StatsTable& t, rts::UnitType u, std::int32_t level) {
    return rts::apply_permille(
        t.of(u).max_hp, {rts::level_permille(level, t.global.hp_permille_per_level)});
}

bool has(std::uint16_t mask, rts::UnitAction a) noexcept {
    return (mask & static_cast<std::uint16_t>(1u << static_cast<unsigned>(a))) != 0;
}

constexpr float kInvSqrt2 = 0.70710678f;

}  // namespace

DemoBattle::DemoBattle(const MapData& map, const rts::StatsTable& stats,
                       std::uint64_t seed)
    : w_(make_world_init(map, stats, seed, /*nominal_level=*/1)) {
    const rts::StatsTable& t = w_.stats();
    const auto spawn = [&](rts::UnitType u, float x, float y, std::int32_t lv) {
        const std::int64_t hp = hp_at(t, u, lv);
        w_.spawn_unit(u, rts::Vec2{x, y}, lv, hp, hp);
    };
    // 守方：三名弓手贴墙内侧、两名枪卫、一名游骑与一名工匠。
    const rts::Vec2 keep = rts::center_of(w_.keep_pos());
    spawn(rts::UnitType::Archer, keep.x + 2.0f, keep.y - 1.0f, 2);
    spawn(rts::UnitType::Archer, keep.x + 2.0f, keep.y, 2);
    spawn(rts::UnitType::Archer, keep.x + 2.0f, keep.y + 1.0f, 2);
    spawn(rts::UnitType::Spear, keep.x + 2.0f, keep.y - 2.0f, 1);
    spawn(rts::UnitType::Spear, keep.x + 2.0f, keep.y + 2.0f, 1);
    spawn(rts::UnitType::Ranger, keep.x + 1.0f, keep.y, 1);
    spawn(rts::UnitType::Mason, keep.x + 1.0f, keep.y + 1.0f, 1);
    // 箭楼与防空各一座（完工状态）。位置：堡垒斜后方两格，覆盖墙线。
    const auto bld = [&](rts::BldType b, int dx, int dy) {
        const std::int64_t hp = t.of(b).max_hp;
        w_.place_bld(b, rts::GridPos{static_cast<std::int16_t>(w_.keep_pos().i + dx),
                                     static_cast<std::int16_t>(w_.keep_pos().j + dy)},
                     hp, hp);
    };
    bld(rts::BldType::Tower, 1, -2);
    bld(rts::BldType::Flak, 1, 2);

    // 攻方：从集结点出发的一波混编（构成是演示定数，不是平衡结论）。
    if (!w_.spawns().empty()) {
        const rts::Vec2 s0 = rts::center_of(w_.spawns()[0].pos);
        spawn(rts::UnitType::Ghoul, s0.x, s0.y - 1.0f, 1);
        spawn(rts::UnitType::Ghoul, s0.x, s0.y, 1);
        spawn(rts::UnitType::Ghoul, s0.x - 1.0f, s0.y, 1);
        spawn(rts::UnitType::Shade, s0.x + 0.5f, s0.y + 1.0f, 1);
        spawn(rts::UnitType::Phoenix, s0.x, s0.y - 2.0f, 1);
        const rts::Vec2 s1 = w_.spawns().size() > 1
                                 ? rts::center_of(w_.spawns()[1].pos)
                                 : s0;
        spawn(rts::UnitType::Ghoul, s1.x, s1.y, 1);
        spawn(rts::UnitType::Shade, s1.x + 0.5f, s1.y - 1.0f, 1);
        spawn(rts::UnitType::Ram, s1.x, s1.y + 1.0f, 1);
    }
    w_.begin_assault();
    issue_actions();
}

rts::UnitAction DemoBattle::greedy_move(rts::UnitId id, rts::Vec2 target) const {
    const rts::Vec2 p = w_.unit_pos(id);
    const std::uint16_t mask = w_.action_mask(id);
    const float dx = target.x - p.x;
    const float dy = target.y - p.y;
    rts::UnitAction best = rts::UnitAction::Stop;
    float best_dot = 0.0f;   // 只接受朝目标的方向；全被挡住就停
    for (int d = 0; d < rts::kMoveDirCount; ++d) {
        const rts::UnitAction a = rts::move_of(d);
        if (!has(mask, a)) continue;
        const rts::GridDelta g = rts::move_delta(a);
        const float norm = rts::is_grid_diagonal(a) ? kInvSqrt2 : 1.0f;
        const float dot = (dx * static_cast<float>(g.di) +
                           dy * static_cast<float>(g.dj)) * norm;
        if (dot > best_dot) {
            best_dot = dot;
            best = a;
        }
    }
    return best;
}

void DemoBattle::issue_actions() {
    const rts::Vec2 keep = rts::center_of(w_.keep_pos());
    for (const rts::Side side : {rts::Side::Defender, rts::Side::Attacker}) {
        w_.enumerate_units(side, ids_);
        acts_.clear();
        acts_.reserve(ids_.size());
        for (const rts::UnitId id : ids_) {
            const std::uint16_t mask = w_.action_mask(id);
            rts::UnitAction a = rts::UnitAction::Stop;
            if (side == rts::Side::Defender) {
                // 守方：有敌就打，没有就守在原地。
                if (has(mask, rts::UnitAction::AtkNear)) a = rts::UnitAction::AtkNear;
            } else {
                // 攻方：优先打得着的人，Ram 优先砸墙，否则向堡垒推进。
                // 撞上墙会自动开始破坏（机制），所以「推进」自己就会变成「啃墙」。
                if (has(mask, rts::UnitAction::AtkNear)) {
                    a = rts::UnitAction::AtkNear;
                } else if (w_.unit_type(id) == rts::UnitType::Ram &&
                           has(mask, rts::UnitAction::AtkWall)) {
                    a = rts::UnitAction::AtkWall;
                } else {
                    a = greedy_move(id, keep);
                }
            }
            acts_.push_back(a);
        }
        w_.submit_actions(side, acts_.data(), acts_.size());
    }
}

void DemoBattle::update(int ticks) {
    for (int k = 0; k < ticks; ++k) {
        if (since_decision_ >= rts::kDecisionPeriodMax) {
            issue_actions();
            since_decision_ = 0;
        }
        w_.advance(1);
        ++since_decision_;
    }
}

}  // namespace game
