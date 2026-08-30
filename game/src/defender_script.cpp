#include "game/defender_script.hpp"

#include <cstddef>

#include "rts/roster.hpp"
#include "rts/stats.hpp"
#include "rts/unit_behavior.hpp"

namespace game {
namespace {

constexpr float kInvSqrt2 = 0.70710678f;

bool has(std::uint16_t mask, rts::UnitAction a) noexcept {
    return (mask & static_cast<std::uint16_t>(1u << static_cast<unsigned>(a))) != 0;
}

float dist2(rts::Vec2 a, rts::Vec2 b) noexcept {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// 沿 (dx, dy) 方向挑掩码合法、投影最大的一步。两处用它：贪心兜底（朝目标）
// 与后撤（背对威胁）。全被挡住给 Stop——停住永远合法。
rts::UnitAction greedy_dir(std::uint16_t mask, float dx, float dy) noexcept {
    rts::UnitAction best = rts::UnitAction::Stop;
    float best_dot = 0.0f;
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

// 最近的敌方单位，按谓词过滤。返回槽位下标，找不到给 -1。
template <class Pred>
int nearest_enemy(const rts::WorldView& view, rts::Vec2 from, Pred&& want,
                  float* out_d2 = nullptr) {
    const auto type = view.unit_type();
    const auto pos = view.unit_pos();
    const auto alive = view.unit_alive();
    int best = -1;
    float best_d2 = 0.0f;
    for (std::size_t s = 0; s < type.size(); ++s) {
        if (!alive[s]) continue;
        if (rts::side_of(type[s]) != rts::Side::Attacker) continue;
        if (!want(type[s])) continue;
        const float d2 = dist2(from, pos[s]);
        if (best < 0 || d2 < best_d2) {
            best = static_cast<int>(s);
            best_d2 = d2;
        }
    }
    if (out_d2 != nullptr) *out_d2 = best_d2;
    return best;
}

// 弓手该躲的威胁：**近战打击半径的地面战斗单位**（Ghoul / Knight / Ram）。
// 远程（Shade）不躲——射程相等时后撤不产生间距，只白丢输出；空中（Phoenix）
// 更不躲——地面拉不开与飞行单位的距离，那是 Flak 的活（克制表原文）。
bool is_melee_ground_threat(rts::UnitType t) noexcept {
    const rts::UnitBehavior& b = rts::behavior_of(t);
    return b.combat() && !rts::is_aerial(t) &&
           b.engage_range() != rts::EngageRange::Ranged;
}

}  // namespace

void DefenderScript::decide(const rts::WorldView& view,
                            std::span<const rts::UnitId> ids,
                            std::vector<rts::UnitAction>& out) {
    // 一拍内的缓存重建：field 的破坏代价读当前血量，拍间会变。
    fields_.clear();
    garrison_cells_.clear();
    const auto orders = view.garrison_order();
    for (std::size_t c = 0; c < orders.size(); ++c) {
        if (orders[c] != rts::kNoForce) {
            garrison_cells_.emplace_back(static_cast<std::uint16_t>(c), orders[c]);
        }
    }
    if (threat_streak_.size() < view.unit_type().size()) {
        threat_streak_.resize(view.unit_type().size(), 0);
    }
    if (manual_order_.size() < view.unit_type().size()) {
        manual_order_.resize(view.unit_type().size());
    }
    if (no_order_streak_.size() < view.unit_type().size()) {
        no_order_streak_.resize(view.unit_type().size(), 0);
    }

    out.clear();
    out.reserve(ids.size());
    for (const rts::UnitId id : ids) {
        out.push_back(decide_unit(view, id));
    }
}

void DefenderScript::issue_move_order(std::span<const rts::UnitId> ids,
                                      rts::GridPos target) {
    for (const rts::UnitId id : ids) {
        const std::size_t slot = id.index();
        if (manual_order_.size() <= slot) manual_order_.resize(slot + 1);
        manual_order_[slot] = ManualOrder{true, target, id.generation()};
    }
}

rts::UnitAction DefenderScript::decide_unit(const rts::WorldView& view,
                                            rts::UnitId id) {
    const std::size_t slot = id.index();
    const std::uint16_t mask = view.action_mask(id);
    const rts::UnitType t = view.unit_type()[slot];
    const rts::Vec2 me = view.unit_pos()[slot];

    // 驻守中（含在爬）：钉在墙上，打得着就打。掩码在爬墙时只剩 Stop，
    // 这条自动退化成等待。
    if (view.unit_garrison()[slot] != rts::kNoSlot) {
        return has(mask, rts::UnitAction::AtkNear) ? rts::UnitAction::AtkNear
                                                   : rts::UnitAction::Stop;
    }

    switch (t) {
        case rts::UnitType::Archer: {
            // 拉扯：威胁进圈 → 后撤；退出圈 → 回头放箭。震荡即风筝。
            float d2 = 0.0f;
            const int threat = nearest_enemy(view, me, is_melee_ground_threat, &d2);
            const bool close =
                threat >= 0 && d2 < p_.kite_trigger_cells * p_.kite_trigger_cells;
            threat_streak_[slot] = close ? threat_streak_[slot] + 1 : 0;
            if (close && threat_streak_[slot] > p_.reaction_decisions) {
                // 掷点在判定之后、执行之前：次序跟规范单位序走，确定性由此成立。
                if (static_cast<std::int32_t>(rng_.below(1000)) < p_.kite_permille) {
                    return flee_from(view, slot, view.unit_pos()[static_cast<std::size_t>(threat)], mask);
                }
            }
            if (has(mask, rts::UnitAction::AtkNear)) return rts::UnitAction::AtkNear;
            if (const auto order = follow_orders(view, id, mask)) return *order;
            return muster_fallback(view, slot, mask);
        }
        case rts::UnitType::Spear: {
            // 堵缺口不追击：打得着就打，打不着绝不朝敌人挪一步——
            // 被风筝出阵位正是 Shade 克枪卫的机制，脚本不能亲手送。
            if (has(mask, rts::UnitAction::AtkNear)) return rts::UnitAction::AtkNear;
            if (const auto order = follow_orders(view, id, mask)) return *order;
            return muster_fallback(view, slot, mask);
        }
        case rts::UnitType::Ranger: {
            // 不与骑士对冲：保持距离（克制表「Ranger ──► Knight」的另一半）。
            float d2 = 0.0f;
            const int knight = nearest_enemy(
                view, me, [](rts::UnitType u) { return u == rts::UnitType::Knight; },
                &d2);
            if (knight >= 0 && d2 < p_.avoid_knight_cells * p_.avoid_knight_cells) {
                return flee_from(view, slot,
                                 view.unit_pos()[static_cast<std::size_t>(knight)],
                                 mask);
            }
            if (has(mask, rts::UnitAction::AtkNear)) return rts::UnitAction::AtkNear;
            {
                // **不能只看「有没有指令」，还要看指令现在结不结算成 `Stop`**：
                // 到了编队指令的终点也是 `Stop`，而这一档本该继续往下试
                // 「摸攻城锤」——旧代码用 `!= Stop` 判断，就是把「没指令」与
                // 「到终点了」两种 `Stop` 一并放行到这一步，这条要保住。
                // 只有 `muster_fallback` 才需要把两者分开（它专管「没指令」
                // 那一种），所以下面单独判 `!order`。
                const std::optional<rts::UnitAction> order =
                    follow_orders(view, id, mask);
                if (order && *order != rts::UnitAction::Stop) return *order;
                // 没有别的命令，或编队指令已经到终点：主动摸向敌方攻城锤
                // （「出城的执行手段」）。
                const int ram = nearest_enemy(
                    view, me, [](rts::UnitType u) { return u == rts::UnitType::Ram; });
                if (ram >= 0) {
                    return move_towards(
                        view, slot,
                        rts::grid_of(view.unit_pos()[static_cast<std::size_t>(ram)]),
                        mask);
                }
                // 摸锤也没找到：真的没指令才去集结点；到终点了就老实站着，
                // 不该被拉去集结点（那会撤销一条已经在生效的编队指令）。
                if (!order) return muster_fallback(view, slot, mask);
                return rts::UnitAction::Stop;
            }
        }
        case rts::UnitType::Mason: {
            const std::optional<rts::UnitAction> order = follow_orders(view, id, mask);
            if (order && *order != rts::UnitAction::Stop) return *order;
            // 没有命令，或编队指令已经到终点：都试试自动找活——最近的有
            // 工时的建筑（工地或维修点）。半径内工时才会走（机制第二批），
            // 「到了」的判据就是那个半径。**这条是这里的初版就有的行为**
            // （旧代码同样用 `!= Stop` 放行两种 `Stop`），端到端测试
            // 「经济与补员的闭环」钉着它：工匠靠 `MoveForce` 走到工地附近、
            // 到终点后正是靠这一步才会开始盖。
            const auto bp = view.bld_pos();
            const auto bw = view.bld_work_left();
            const auto ba = view.bld_alive();
            int best = -1;
            float best_d2 = 0.0f;
            for (std::size_t s = 0; s < bp.size(); ++s) {
                if (!ba[s] || bw[s] <= 0) continue;
                const float d2 = dist2(me, rts::center_of(bp[s]));
                if (best < 0 || d2 < best_d2) {
                    best = static_cast<int>(s);
                    best_d2 = d2;
                }
            }
            if (best >= 0) {
                const float r = view.stats().global.mason_work_radius;
                if (best_d2 > r * r) {
                    return move_towards(view, slot,
                                        bp[static_cast<std::size_t>(best)], mask);
                }
                return rts::UnitAction::Stop;   // 已在半径内干活
            }
            // 找活也没找到：真的没指令才去集结点，到终点了就老实站着。
            if (!order) return muster_fallback(view, slot, mask);
            return rts::UnitAction::Stop;
        }
        default:
            // Scout 与任何后加的兵种：打得着就打（Scout 无战力，掩码永远
            // 不给它攻击位），否则只听命令。
            if (has(mask, rts::UnitAction::AtkNear)) return rts::UnitAction::AtkNear;
            if (const auto order = follow_orders(view, id, mask)) return *order;
            return muster_fallback(view, slot, mask);
    }
}

std::optional<rts::UnitAction> DefenderScript::follow_orders(
    const rts::WorldView& view, rts::UnitId id, std::uint16_t mask) {
    const std::size_t slot = id.index();
    const std::uint8_t force = view.unit_force()[slot];
    const rts::Vec2 me = view.unit_pos()[slot];
    const rts::GridPos here = rts::grid_of(me);

    // 找到了任何一种指令：把「连续无指令」的计数清零，再把动作递出去。
    // **这条不是装饰**——见 `ScriptParams::muster_delay_decisions` 的理由：
    // 少了它，`muster_fallback` 分不清「这个单位一直没人管」与「命令刚提交、
    // 还卡在 `World` 的入队与生效之间那一拍」，后者会被误判成前者。
    const auto succeed = [&](rts::UnitAction a) -> std::optional<rts::UnitAction> {
        if (slot < no_order_streak_.size()) no_order_streak_[slot] = 0;
        return a;
    };

    // 框选下达的临时指令：优先级高于编队的持久指令（更新、更具体——
    // 「这次点的就是这几个单位」）。世代不匹配说明槎位被复用了，旧指令
    // 是上一个死掉的单位留下的痕迹，直接清掉，不当真。
    if (slot < manual_order_.size() && manual_order_[slot].active) {
        if (manual_order_[slot].generation != id.generation()) {
            manual_order_[slot] = ManualOrder{};
        } else {
            const rts::GridPos goal = manual_order_[slot].target;
            if (dist2(me, rts::center_of(goal)) > p_.arrive_cells * p_.arrive_cells) {
                return succeed(move_towards(view, slot, goal, mask));
            }
            manual_order_[slot] = ManualOrder{};   // 到了：清空，往下走编队级判断
        }
    }

    if (force != rts::kNoForce) {
        // 驻守指令：走向同编队最近的指令格，到墙边就停——登墙本身归 World
        // （tick_garrison 从相邻同编队单位里挑人），脚本只负责把人送到。
        int best = -1;
        float best_d2 = 0.0f;
        for (const auto& [cell, f] : garrison_cells_) {
            if (f != force) continue;
            const float d2 =
                dist2(me, rts::center_of(rts::pos_of_slot(cell, view.width())));
            if (best < 0 || d2 < best_d2) {
                best = static_cast<int>(cell);
                best_d2 = d2;
            }
        }
        if (best >= 0) {
            const rts::GridPos goal =
                rts::pos_of_slot(static_cast<std::uint16_t>(best), view.width());
            const int ci = goal.i - here.i;
            const int cj = goal.j - here.j;
            const bool adjacent = ci >= -1 && ci <= 1 && cj >= -1 && cj <= 1;
            if (adjacent) return succeed(rts::UnitAction::Stop);   // 等 World 拉上墙
            return succeed(move_towards(view, slot, goal, mask));
        }
        // 编队去处（MoveForce）：到了就站住。
        const std::uint16_t ft = view.force_target()[force];
        if (ft != rts::kNoSlot) {
            const rts::GridPos goal = rts::pos_of_slot(ft, view.width());
            if (dist2(me, rts::center_of(goal)) > p_.arrive_cells * p_.arrive_cells) {
                return succeed(move_towards(view, slot, goal, mask));
            }
            return succeed(rts::UnitAction::Stop);
        }
    }

    // 清野（Clear）：有战力破坏结构的、手头没别的事，就去砸挂了旗的障碍。
    // 破坏本身走机制（撞上自动开始），脚本只负责走过去。
    if (rts::behavior_of(view.unit_type()[slot]).can_break_structure()) {
        const auto op = view.obstacle_pos();
        const auto oa = view.obstacle_alive();
        const auto oc = view.obstacle_clear_ordered();
        int best = -1;
        float best_d2 = 0.0f;
        for (std::size_t s = 0; s < op.size(); ++s) {
            if (!oa[s] || oc[s] == 0) continue;
            const float d2 = dist2(me, rts::center_of(op[s]));
            if (best < 0 || d2 < best_d2) {
                best = static_cast<int>(s);
                best_d2 = d2;
            }
        }
        if (best >= 0) {
            return succeed(move_towards(view, slot, op[static_cast<std::size_t>(best)],
                                        mask));
        }
    }
    return std::nullopt;   // 真的什么指令都没有——调用方决定要不要用自己的兜底
}

rts::GridPos DefenderScript::muster_point_for(std::uint8_t force,
                                              rts::GridPos keep) noexcept {
    // 四个方向错开（北/东/南/西），避免所有编队挤在堡垒同一格上。
    // **只有四档**：编队号当前上限就是 1-4 号，够用；哪天编队能有更多，
    // 再扩这张表。偏移量是占位（CLAUDE.md「关于数值」），只求「分得开」。
    constexpr int kOff[4][2] = {{0, -3}, {3, 0}, {0, 3}, {-3, 0}};
    const int idx = force < 4 ? force : force % 4;
    return rts::GridPos{static_cast<std::int16_t>(keep.i + kOff[idx][0]),
                        static_cast<std::int16_t>(keep.j + kOff[idx][1])};
}

rts::UnitAction DefenderScript::muster_fallback(const rts::WorldView& view,
                                                std::size_t slot,
                                                std::uint16_t mask) {
    // 攻方没有编队（结构，见 CLAUDE.md），这一分支本不该被攻方走到——
    // 写这条防御性检查是因为「没有编队」与「编队是 0 号」不可靠地区分，
    // 直接放行会让 muster_point_for(kNoForce, ...) 算出一个没有意义的点。
    const std::uint8_t force = view.unit_force()[slot];
    if (force == rts::kNoForce) return rts::UnitAction::Stop;

    // **容忍最近几拍的假阴性再动手**（`ScriptParams::muster_delay_decisions`
    // 的理由）：命令刚提交、还卡在 `World` 入队与生效之间那一拍，
    // `follow_orders` 也会报「没有指令」，若立刻挪向集结点，这一步可能
    // 真的改变后续「该走哪条路/该奔哪段墙」的判断——已被破坏性验证抓到过。
    if (slot >= no_order_streak_.size()) no_order_streak_.resize(slot + 1, 0);
    ++no_order_streak_[slot];
    if (no_order_streak_[slot] <= p_.muster_delay_decisions) {
        return rts::UnitAction::Stop;
    }

    const rts::Vec2 me = view.unit_pos()[slot];
    const rts::GridPos goal = muster_point_for(force, view.keep_pos());
    if (dist2(me, rts::center_of(goal)) <= p_.arrive_cells * p_.arrive_cells) {
        return rts::UnitAction::Stop;
    }
    return move_towards(view, slot, goal, mask);
}

rts::UnitAction DefenderScript::move_towards(const rts::WorldView& view,
                                             std::size_t slot, rts::GridPos goal,
                                             std::uint16_t mask) {
    const rts::UnitType t = view.unit_type()[slot];
    const rts::FlowTiering tiering{};
    const int tier = rts::flow_tier_of(view.unit_level()[slot], tiering);
    const std::uint16_t g = rts::slot_of(goal, view.width());

    rts::FlowField* field = nullptr;
    for (CachedField& c : fields_) {
        if (c.goal == g && c.type == t && c.tier == tier) {
            field = &c.field;
            break;
        }
    }
    if (field == nullptr) {
        const rts::GridPos goals[1] = {goal};
        fields_.push_back(CachedField{
            g, t, tier, rts::FlowField::compute(view, t, tier, goals, tiering)});
        field = &fields_.back().field;
    }

    const rts::UnitAction a = field->step_of(rts::grid_of(view.unit_pos()[slot]));
    if (a != rts::UnitAction::Stop) return a;
    // 不可达（或已在目标格）：贪心兜底，演示与训练都不该因一格数据卡死。
    const rts::Vec2 me = view.unit_pos()[slot];
    const rts::Vec2 c = rts::center_of(goal);
    return greedy_dir(mask, c.x - me.x, c.y - me.y);
}

rts::UnitAction DefenderScript::flee_from(const rts::WorldView& view,
                                          std::size_t slot, rts::Vec2 threat,
                                          std::uint16_t mask) {
    const rts::Vec2 me = view.unit_pos()[slot];
    return greedy_dir(mask, me.x - threat.x, me.y - threat.y);
}

}  // namespace game
