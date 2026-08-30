#include "game/demo_driver.hpp"

#include <cstddef>

#include "game/world_builder.hpp"
#include "rts/action.hpp"
#include "rts/combat_math.hpp"
#include "rts/command.hpp"
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

// 开局兵力全部经 `WorldInit` 进场（不再是构造后逐个 spawn）。改成这样有两个
// 原因：初始局面因此完整地由建局参数表达（回放 = 建局参数 + 输入流，这正是
// `WorldInit::units` 存在的理由）；而且**只有这条路能给单位编队**——三名弓手
// 要编入 0 号编队才能受命驻守（机制第三批）。
rts::WorldInit demo_init(const MapData& map, const rts::StatsTable& stats,
                         std::uint64_t seed) {
    rts::WorldInit init = make_world_init(map, stats, seed, /*nominal_level=*/1);
    const auto add = [&](rts::UnitType u, float x, float y, std::int32_t lv,
                         std::uint8_t force) {
        const std::int64_t hp = hp_at(stats, u, lv);
        init.units.push_back(rts::UnitInit{u, rts::Vec2{x, y}, lv, hp, hp, force});
    };
    // 守方：三名弓手贴墙内侧（0 号编队，开局即受命驻守墙线，见构造函数）、
    // 两名枪卫、一名游骑与一名工匠。
    const rts::Vec2 keep = rts::center_of(init.keep);
    add(rts::UnitType::Archer, keep.x + 2.0f, keep.y - 1.0f, 2, 0);
    add(rts::UnitType::Archer, keep.x + 2.0f, keep.y, 2, 0);
    add(rts::UnitType::Archer, keep.x + 2.0f, keep.y + 1.0f, 2, 0);
    add(rts::UnitType::Spear, keep.x + 2.0f, keep.y - 2.0f, 1, rts::kNoForce);
    add(rts::UnitType::Spear, keep.x + 2.0f, keep.y + 2.0f, 1, rts::kNoForce);
    add(rts::UnitType::Ranger, keep.x + 1.0f, keep.y, 1, rts::kNoForce);
    add(rts::UnitType::Mason, keep.x + 1.0f, keep.y + 1.0f, 1, rts::kNoForce);
    // 箭楼与防空各一座（完工状态）。位置：堡垒斜后方两格，覆盖墙线。
    const auto bld = [&](rts::BldType b, int dx, int dy) {
        const std::int64_t hp = stats.of(b).max_hp;
        init.buildings.push_back(rts::BldInit{
            b,
            rts::GridPos{static_cast<std::int16_t>(init.keep.i + dx),
                         static_cast<std::int16_t>(init.keep.j + dy)},
            hp, hp});
    };
    bld(rts::BldType::Tower, 1, -2);
    bld(rts::BldType::Flak, 1, 2);

    // 攻方：从集结点出发的一波混编（构成是演示定数，不是平衡结论）。
    if (!init.spawns.empty()) {
        const rts::Vec2 s0 = rts::center_of(init.spawns[0].pos);
        add(rts::UnitType::Ghoul, s0.x, s0.y - 1.0f, 1, rts::kNoForce);
        add(rts::UnitType::Ghoul, s0.x, s0.y, 1, rts::kNoForce);
        add(rts::UnitType::Ghoul, s0.x - 1.0f, s0.y, 1, rts::kNoForce);
        add(rts::UnitType::Shade, s0.x + 0.5f, s0.y + 1.0f, 1, rts::kNoForce);
        add(rts::UnitType::Phoenix, s0.x, s0.y - 2.0f, 1, rts::kNoForce);
        const rts::Vec2 s1 = init.spawns.size() > 1
                                 ? rts::center_of(init.spawns[1].pos)
                                 : s0;
        add(rts::UnitType::Ghoul, s1.x, s1.y, 1, rts::kNoForce);
        add(rts::UnitType::Shade, s1.x + 0.5f, s1.y - 1.0f, 1, rts::kNoForce);
        add(rts::UnitType::Ram, s1.x, s1.y + 1.0f, 1, rts::kNoForce);
    }
    return init;
}

}  // namespace

DemoBattle::DemoBattle(const MapData& map, const rts::StatsTable& stats,
                       std::uint64_t seed)
    : w_(demo_init(map, stats, seed)) {
    // 0 号编队受命驻守堡垒正东的三段墙（含门楼——「墙段」的判据是 Wall‖Gate）。
    // 弓手爬上去之后吃高度优势：射程加成、被低处打有 miss 且减伤（第三批），
    // Shade 压制墙头 vs 墙头反压制的画面由此涌现，脚本仍然一行战术没写。
    const rts::GridPos keep = w_.keep_pos();
    rts::Command cmds[3];
    for (int k = 0; k < 3; ++k) {
        cmds[k].kind = rts::CommandKind::Garrison;
        cmds[k].side = rts::Side::Defender;
        cmds[k].force = 0;
        cmds[k].slot = rts::slot_of(
            rts::GridPos{static_cast<std::int16_t>(keep.i + 3),
                         static_cast<std::int16_t>(keep.j + k - 1)},
            w_.width());
    }
    w_.submit(rts::Side::Defender, cmds, 3);
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
                // 守方：有敌就打，没有就守在原地（驻守的在墙上照打）。
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
