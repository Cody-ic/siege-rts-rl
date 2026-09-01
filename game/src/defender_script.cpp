#include "game/defender_script.hpp"

#include <cstddef>

#include "rts/fog.hpp"
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
                            std::vector<rts::UnitAction>& out,
                            std::vector<std::uint16_t>& garrison_out) {
    // 一拍内的缓存重建：field 的破坏代价读当前血量，拍间会变。
    fields_.clear();
    wall_claimed_.clear();
    if (threat_streak_.size() < view.unit_type().size()) {
        threat_streak_.resize(view.unit_type().size(), 0);
    }
    if (manual_order_.size() < view.unit_type().size()) {
        manual_order_.resize(view.unit_type().size());
    }

    // 已驻守（含在爬）的墙格本拍先视为已占：一格一人，新指派不许撞上去。
    // 驻守者自己稍后会把同一格再写进意愿（留任），那不算「抢」。
    {
        const auto gar = view.unit_garrison();
        const auto alive = view.unit_alive();
        for (std::size_t s = 0; s < gar.size(); ++s) {
            if (alive[s] && gar[s] != rts::kNoSlot) wall_claimed_.push_back(gar[s]);
        }
    }

    out.clear();
    out.reserve(ids.size());
    garrison_out.clear();
    garrison_out.reserve(ids.size());
    for (const rts::UnitId id : ids) {
        std::uint16_t wish = rts::kNoSlot;
        out.push_back(decide_unit(view, id, wish));
        garrison_out.push_back(wish);
        if (wish != rts::kNoSlot) {
            // 本拍指派出去的墙格随即对后面的单位关上（见 find_wall_post）。
            bool seen = false;
            for (const std::uint16_t c : wall_claimed_) {
                if (c == wish) {
                    seen = true;
                    break;
                }
            }
            if (!seen) wall_claimed_.push_back(wish);
        }
    }
}

void DefenderScript::issue_move_order(std::span<const rts::UnitId> ids,
                                      rts::GridPos target) {
    for (const rts::UnitId id : ids) {
        const std::size_t slot = id.index();
        if (manual_order_.size() <= slot) manual_order_.resize(slot + 1);
        manual_order_[slot] =
            ManualOrder{true, target, false, id.generation()};
    }
}

void DefenderScript::issue_garrison_order(std::span<const rts::UnitId> ids,
                                          rts::GridPos wall_cell) {
    for (const rts::UnitId id : ids) {
        const std::size_t slot = id.index();
        if (manual_order_.size() <= slot) manual_order_.resize(slot + 1);
        manual_order_[slot] =
            ManualOrder{true, wall_cell, true, id.generation()};
    }
}

rts::UnitAction DefenderScript::decide_unit(const rts::WorldView& view,
                                            rts::UnitId id,
                                            std::uint16_t& wish) {
    const std::size_t slot = id.index();
    const std::uint16_t mask = view.action_mask(id);
    const rts::UnitType t = view.unit_type()[slot];
    const rts::Vec2 me = view.unit_pos()[slot];
    wish = rts::kNoSlot;

    // ——手动临时指令（辅助性覆盖）：优先于一切自主默认——
    //
    // 世代不匹配说明槽位被复用了，旧指令是上一个死掉的单位留下的痕迹，
    // 直接清掉，不当真。
    if (slot < manual_order_.size() && manual_order_[slot].active) {
        ManualOrder& order = manual_order_[slot];
        if (order.generation != id.generation()) {
            order = ManualOrder{};
        } else if (order.garrison) {
            // 驻进指定墙段：意愿 + 走位一起给，登墙由 tick_garrison 解算。
            // 目标格得仍是**完工的 Wall/Gate**——墙被拆（或从来就不是墙）时
            // 指令作废，不然单位会在废墟边干等到死。
            bool wall_ok = false;
            {
                const auto bt = view.bld_type();
                const auto bp = view.bld_pos();
                const auto bb = view.bld_built();
                const auto ba = view.bld_alive();
                for (std::size_t b = 0; b < bp.size(); ++b) {
                    if (ba[b] && bb[b] && bp[b] == order.target &&
                        (bt[b] == rts::BldType::Wall || bt[b] == rts::BldType::Gate)) {
                        wall_ok = true;
                        break;
                    }
                }
            }
            if (!wall_ok) {
                order = ManualOrder{};
            } else {
                const std::uint16_t cell =
                    rts::slot_of(order.target, view.width());
                if (view.unit_garrison()[slot] == cell &&
                    view.unit_mount()[slot] == 0) {
                    order = ManualOrder{};   // 已登顶：指令完成，回归自主
                    // 不清 wish——交给下面的自主逻辑重写（弓手留任，其余下墙）。
                } else {
                    wish = cell;
                    if (view.unit_garrison()[slot] != rts::kNoSlot) {
                        return rts::UnitAction::Stop;   // 在爬：钉着等落位
                    }
                    const rts::GridPos here = rts::grid_of(me);
                    const int ci = order.target.i - here.i;
                    const int cj = order.target.j - here.j;
                    if (ci >= -1 && ci <= 1 && cj >= -1 && cj <= 1) {
                        return rts::UnitAction::Stop;   // 贴墙站着，等 World 拉上墙
                    }
                    return move_towards(view, slot, order.target, mask);
                }
            }
        } else {
            // 开拔。已上墙的先下来：意愿清空，这一拍钉着等世界放人。
            if (view.unit_garrison()[slot] != rts::kNoSlot) {
                return rts::UnitAction::Stop;
            }
            if (dist2(me, rts::center_of(order.target)) >
                p_.arrive_cells * p_.arrive_cells) {
                return move_towards(view, slot, order.target, mask);
            }
            order = ManualOrder{};   // 到了：清除，往下走自主逻辑
        }
    }

    // 驻守中（含在爬）：钉在墙上，打得着就打。掩码在爬墙时只剩 Stop，
    // 这条自动退化成等待。**弓手留任**：意愿不重写就会被世界当成「撤了」
    // 放下来；其余兵种不该在墙上（近战上墙够不着人），撤意愿即下墙。
    if (view.unit_garrison()[slot] != rts::kNoSlot) {
        if (t == rts::UnitType::Archer) wish = view.unit_garrison()[slot];
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
                    return flee_from(view, slot,
                                     view.unit_pos()[static_cast<std::size_t>(threat)],
                                     mask);
                }
            }
            if (has(mask, rts::UnitAction::AtkNear)) return rts::UnitAction::AtkNear;
            // 没仗打：找最近的空墙段登墙（高度优势：射程加成 + 低处打它有
            // miss 且减伤，机制第三批）。意愿与走位是同一拍给出的——
            // 走到墙边那一拍 tick_garrison 自会接手。
            const int wall = find_wall_post(view, me);
            if (wall >= 0) {
                const rts::GridPos goal =
                    rts::pos_of_slot(static_cast<std::uint16_t>(wall), view.width());
                wish = static_cast<std::uint16_t>(wall);
                const rts::GridPos here = rts::grid_of(me);
                const int ci = goal.i - here.i;
                const int cj = goal.j - here.j;
                if (ci >= -1 && ci <= 1 && cj >= -1 && cj <= 1) {
                    return rts::UnitAction::Stop;   // 贴墙站着，等 World 拉上墙
                }
                return move_towards(view, slot, goal, mask);
            }
            return hold_position(view, slot, mask);
        }
        case rts::UnitType::Spear: {
            // 堵缺口不追击：打得着就打，打不着绝不朝敌人挪一步——
            // 被风筝出阵位正是 Shade 克枪卫的机制，脚本不能亲手送。
            if (has(mask, rts::UnitAction::AtkNear)) return rts::UnitAction::AtkNear;
            return hold_position(view, slot, mask);
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
            // 没有更近的威胁：主动摸向敌方攻城锤（「出城的执行手段」，
            // 「Ram ──► Ranger 快速切入」）。
            const int ram = nearest_enemy(
                view, me, [](rts::UnitType u) { return u == rts::UnitType::Ram; });
            if (ram >= 0) {
                return move_towards(
                    view, slot,
                    rts::grid_of(view.unit_pos()[static_cast<std::size_t>(ram)]),
                    mask);
            }
            return hold_position(view, slot, mask);
        }
        case rts::UnitType::Mason: {
            // 自动找活：最近的有工时的建筑（工地或维修点）。半径内工时才会走
            // （机制第二批），「到了」的判据就是那个半径。
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
            return hold_position(view, slot, mask);
        }
        default: {
            // Scout 与任何后加的兵种：打得着就打（Scout 无战力，掩码永远
            // 不给它攻击位），否则巡逻——最近的当前不可见的集结点。
            // 攻方从集结点来，盯着那里就是盯着威胁的来路；全可见时回驻防环。
            if (has(mask, rts::UnitAction::AtkNear)) return rts::UnitAction::AtkNear;
            const auto& spawns = view.spawns();
            const std::uint8_t* vis = view.fog().vis_bytes();
            const int w = view.width();
            int best = -1;
            float best_d2 = 0.0f;
            for (std::size_t s = 0; s < spawns.size(); ++s) {
                const rts::GridPos p = spawns[s].pos;
                if (vis[static_cast<std::size_t>(p.j) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(p.i)] ==
                    static_cast<std::uint8_t>(rts::Vis::Visible)) {
                    continue;
                }
                const float d2 = dist2(me, rts::center_of(p));
                if (best < 0 || d2 < best_d2) {
                    best = static_cast<int>(s);
                    best_d2 = d2;
                }
            }
            if (best >= 0) {
                return move_towards(view, slot,
                                    spawns[static_cast<std::size_t>(best)].pos, mask);
            }
            return hold_position(view, slot, mask);
        }
    }
}

int DefenderScript::find_wall_post(const rts::WorldView& view,
                                   rts::Vec2 me) const {
    const auto bt = view.bld_type();
    const auto bp = view.bld_pos();
    const auto bb = view.bld_built();
    const auto ba = view.bld_alive();
    int best = -1;
    float best_d2 = 0.0f;
    for (std::size_t k = 0; k < bp.size(); ++k) {
        if (!ba[k] || !bb[k]) continue;
        // 「墙段」的判据是 Wall‖Gate（机制第三批同一条），门楼一样能站人。
        if (bt[k] != rts::BldType::Wall && bt[k] != rts::BldType::Gate) continue;
        const std::uint16_t cell = rts::slot_of(bp[k], view.width());
        bool claimed = false;
        for (const std::uint16_t c : wall_claimed_) {
            if (c == cell) {
                claimed = true;
                break;
            }
        }
        if (claimed) continue;
        const float d2 = dist2(me, rts::center_of(bp[k]));
        if (best < 0 || d2 < best_d2) {
            best = static_cast<int>(cell);
            best_d2 = d2;
        }
    }
    return best;
}

rts::GridPos DefenderScript::hold_point_for(std::size_t slot,
                                            rts::GridPos keep) noexcept {
    // 堡垒周围一圈（半径 3，八向按槽位轮转），避免所有闲人叠在同一格。
    // 偏移量是占位（CLAUDE.md「关于数值」），只求「分得开、贴着内城」。
    constexpr int kOff[8][2] = {{0, -3}, {3, 0},  {0, 3},   {-3, 0},
                                {2, -2}, {2, 2},  {-2, 2},  {-2, -2}};
    const std::size_t idx = slot % 8;
    return rts::GridPos{static_cast<std::int16_t>(keep.i + kOff[idx][0]),
                        static_cast<std::int16_t>(keep.j + kOff[idx][1])};
}

rts::UnitAction DefenderScript::hold_position(const rts::WorldView& view,
                                              std::size_t slot,
                                              std::uint16_t mask) {
    // 进驻防环之前先响应清野旗（`Clear` 命令挂上的）：能破结构、且已经
    // 闲到要回驻防环的单位，走去砸最近那面旗。破坏本身走机制（撞上自动
    // 开始），脚本只负责走过去。**放在这里而不是各兵种分支里**，是因为
    // 它管的是「没更具体的事做」这层——所有兵种的空闲兜底都流经本函数，
    // 一处接上，五个分支不用各抄一遍。
    if (rts::behavior_of(view.unit_type()[slot]).can_break_structure()) {
        const auto op = view.obstacle_pos();
        const auto oa = view.obstacle_alive();
        const auto oc = view.obstacle_clear_ordered();
        const rts::Vec2 me = view.unit_pos()[slot];
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
            return move_towards(view, slot, op[static_cast<std::size_t>(best)], mask);
        }
    }
    const rts::Vec2 me = view.unit_pos()[slot];
    const rts::GridPos goal = hold_point_for(slot, view.keep_pos());
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
