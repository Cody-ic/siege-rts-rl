#include "game/demo_driver.hpp"

#include <cstddef>

#include "game/world_builder.hpp"
#include "rts/action.hpp"
#include "rts/combat_math.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
#include "rts/world_view.hpp"

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
    // 两名枪卫、一名游骑与一名工匠。**全员有编队**（1 = 枪卫、2 = 游骑、
    // 3 = 工匠）：玩家的逐格命令按编队下达（MoveForce / Garrison 的形状），
    // 没编队的单位在交互层就是指挥不动的。
    const rts::Vec2 keep = rts::center_of(init.keep);
    add(rts::UnitType::Archer, keep.x + 2.0f, keep.y - 1.0f, 2, 0);
    add(rts::UnitType::Archer, keep.x + 2.0f, keep.y, 2, 0);
    add(rts::UnitType::Archer, keep.x + 2.0f, keep.y + 1.0f, 2, 0);
    add(rts::UnitType::Spear, keep.x + 2.0f, keep.y - 2.0f, 1, 1);
    add(rts::UnitType::Spear, keep.x + 2.0f, keep.y + 2.0f, 1, 1);
    add(rts::UnitType::Ranger, keep.x + 1.0f, keep.y, 1, 2);
    add(rts::UnitType::Mason, keep.x + 1.0f, keep.y + 1.0f, 1, 3);
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

    // 攻方不在这里：波次循环生效后每波在建造阶段结束时生成（spawn_wave）。
    return init;
}

// ——波次循环的占位常量（演示定数，无平衡含义）——
//
// 建造阶段时长与编成曲线都是**占位**：正式形态里前者是待标定数值、
// 后者由攻方宏观层按双预算决定（编成位线性封顶 / 兵力超线性）。
// 这里只求循环的形状对：波数涨、编成随之变厚、等级随波缓涨。
constexpr int kBuildTicksPlaceholder = 160;   // 8 秒 @ 20 Hz

std::int32_t wave_level(int wave) {
    return 1 + (wave - 1) / 3;   // 占位：每三波涨一级
}

}  // namespace

DemoBattle::DemoBattle(const MapData& map, const rts::StatsTable& stats,
                       std::uint64_t seed)
    : w_(demo_init(map, stats, seed)), script_(ScriptParams{}, seed ^ 0x9e3779b9u) {
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
    // 开局资源（**占位数额**，无平衡含义）：交互层要能试建造，
    // 石木全零的话 Build 永远被解算拒绝，「建造放置」就没法演示。
    w_.set_stock(rts::Resource::Stone, 120);
    w_.set_stock(rts::Resource::Wood, 120);
    // 从建造阶段开始（World 的初始 phase 就是 Build）：倒计时走完才生波。
    build_left_ = kBuildTicksPlaceholder;
    issue_actions();
}

// 本波编成（占位曲线）：随波数缓涨的混编，轮流摆在各集结点周围。
// 骑士走开阔走廊一路直线 = 满动量冲锋（第四批）——首击明显重于互殴，正是要看的。
void DemoBattle::spawn_wave() {
    const auto& spawns = w_.spawns();
    if (spawns.empty()) return;
    const int wave = w_.wave();
    const std::int32_t lv = w_.nominal_level();
    const rts::StatsTable& stats = w_.stats();

    std::vector<rts::UnitType> roster;
    const int ghouls = wave + 2 > 8 ? 8 : wave + 2;
    for (int k = 0; k < ghouls; ++k) roster.push_back(rts::UnitType::Ghoul);
    const int shades = 1 + wave / 2 > 4 ? 4 : 1 + wave / 2;
    for (int k = 0; k < shades; ++k) roster.push_back(rts::UnitType::Shade);
    for (int k = 0; k < 1 + wave / 3; ++k) roster.push_back(rts::UnitType::Ram);
    if (wave >= 2) roster.push_back(rts::UnitType::Knight);
    if (wave >= 3) roster.push_back(rts::UnitType::Phoenix);

    // 固定的落位偏移环（不掷点：demo 的确定性不该依赖「生成时的随机散布」）。
    constexpr float kOff[][2] = {{0.0f, 0.0f},  {1.0f, 0.0f},  {-1.0f, 0.0f},
                                 {0.0f, 1.0f},  {0.0f, -1.0f}, {1.0f, 1.0f},
                                 {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}};
    constexpr std::size_t kOffCount = sizeof(kOff) / sizeof(kOff[0]);
    for (std::size_t n = 0; n < roster.size(); ++n) {
        const rts::Vec2 c = rts::center_of(spawns[n % spawns.size()].pos);
        const float* off = kOff[(n / spawns.size()) % kOffCount];
        const std::int64_t hp = hp_at(stats, roster[n], lv);
        w_.spawn_unit(roster[n], rts::Vec2{c.x + off[0], c.y + off[1]}, lv, hp, hp);
    }
}

bool DemoBattle::keep_alive() const {
    const rts::WorldView v = w_.view(rts::Side::Defender);
    for (std::size_t k = 0; k < v.bld_type().size(); ++k) {
        if (v.bld_alive()[k] && v.bld_type()[k] == rts::BldType::Keep) return true;
    }
    return false;
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

rts::UnitAction DemoBattle::flow_step(rts::UnitId id) {
    const rts::UnitType t = w_.unit_type(id);
    const int tier = rts::flow_tier_of(w_.unit_level(id), tiering_);
    auto& slot = flow_[static_cast<std::size_t>(t) *
                           static_cast<std::size_t>(rts::kFlowTierCount) +
                       static_cast<std::size_t>(tier)];
    if (!slot) {
        const rts::GridPos goal[1] = {w_.keep_pos()};
        slot.emplace(rts::FlowField::compute(w_.view(rts::Side::Attacker), t, tier,
                                             goal, tiering_));
    }
    const rts::UnitAction a = slot->step_of(rts::grid_of(w_.unit_pos(id)));
    return a == rts::UnitAction::Stop
               ? greedy_move(id, rts::center_of(w_.keep_pos()))
               : a;
}

void DemoBattle::issue_actions() {
    // field 每个决策拍作废重算：破坏代价读的是当前墙血，决策拍之间它在变。
    for (auto& f : flow_) f.reset();

    // 守方：执行层脚本（拉扯 / 堵口不追 / 避骑士摸攻城锤 / 命令翻译）。
    w_.enumerate_units(rts::Side::Defender, ids_);
    script_.decide(w_.view(rts::Side::Defender), ids_, acts_);
    w_.submit_actions(rts::Side::Defender, acts_.data(), acts_.size());

    // 攻方：占位脚本（正式形态是逐单位 RL）——优先打得着的人，Ram 优先砸墙，
    // 否则按 flow field 向堡垒推进（「绕远走缺口 vs 就近砸墙」由代价模型
    // 自己比较，field 指进墙格的那一步会变成自动破坏）。
    w_.enumerate_units(rts::Side::Attacker, ids_);
    acts_.clear();
    acts_.reserve(ids_.size());
    for (const rts::UnitId id : ids_) {
        const std::uint16_t mask = w_.action_mask(id);
        rts::UnitAction a = rts::UnitAction::Stop;
        if (has(mask, rts::UnitAction::AtkNear)) {
            a = rts::UnitAction::AtkNear;
        } else if (w_.unit_type(id) == rts::UnitType::Ram &&
                   has(mask, rts::UnitAction::AtkWall)) {
            a = rts::UnitAction::AtkWall;
        } else {
            a = flow_step(id);
        }
        acts_.push_back(a);
    }
    w_.submit_actions(rts::Side::Attacker, acts_.data(), acts_.size());
}

void DemoBattle::update(int ticks) {
    for (int k = 0; k < ticks; ++k) {
        if (defeated_) return;   // 败局定格：世界停在最后一帧
        if (w_.phase() == rts::WavePhase::Build && build_left_ > 0) {
            --build_left_;
        } else if (w_.phase() == rts::WavePhase::Build) {
            w_.begin_assault();
        }
        // 生波挂在「进攻阶段且本波还没生」上，而不是「倒计时走完」上——
        // 玩家的 `Summon`（提前召唤）也会把 phase 掰到 Assault，两条路
        // 在这里汇合，不需要各生各的波。
        if (w_.phase() == rts::WavePhase::Assault) {
            if (!wave_spawned_) {
                spawn_wave();
                wave_spawned_ = true;
                issue_actions();   // 新生成的单位当拍拿到动作，不呆等一个决策周期
                since_decision_ = 0;
            } else if (w_.live_unit_count(rts::Side::Attacker) == 0) {
                // 本波打完（消耗殆尽也算，突破与否不改变循环）：进下一波建造。
                w_.begin_next_wave(wave_level(w_.wave() + 1));
                build_left_ = kBuildTicksPlaceholder;
                wave_spawned_ = false;
            }
        }
        if (since_decision_ >= rts::kDecisionPeriodMax) {
            issue_actions();
            since_decision_ = 0;
        }
        w_.advance(1);
        ++since_decision_;
        if (!keep_alive()) defeated_ = true;   // 丢堡即败（设计，不是演示便宜）
    }
}

}  // namespace game
