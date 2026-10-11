#include "game/defender_script.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include "post_navigation.hpp"

#include "rts/unit_behavior.hpp"
#include "rts/visibility.hpp"

namespace game {
namespace {
float distance2(rts::Vec2 a, rts::Vec2 b) {
    const float x = a.x - b.x, y = a.y - b.y;
    return x*x + y*y;
}
bool allowed(std::uint16_t mask, rts::UnitAction action) {
    return (mask & (1u << static_cast<unsigned>(action))) != 0;
}

using detail::PostRoute;
using detail::route_to_any;

}

void DefenderScript::observe_post_threats(const rts::WorldView& v) {
    post_threats_.resize(v.unit_type().size());
    for (std::size_t e=0; e<post_threats_.size(); ++e) {
        auto& threat=post_threats_[e];
        const auto enemy=v.unit_id(static_cast<std::uint16_t>(e));
        if (!enemy.valid() || enemy!=threat.enemy || threat.until<v.now()) threat={};
        if (!enemy.valid() || rts::side_of(v.unit_type()[e])!=rts::Side::Attacker ||
            !rts::is_combat(v.unit_type()[e]) || rts::is_aerial(v.unit_type()[e])) continue;
        const auto pos=rts::grid_of(v.unit_pos()[e]);
        if (v.fog().vis_bytes()[int(pos.j)*v.width()+int(pos.i)]!=
            static_cast<std::uint8_t>(rts::Vis::Visible)) continue;
        const auto kind=v.unit_target_kind()[e];
        if (v.unit_windup()[e]<=0 || (kind!=rts::TgtKind::Unit && kind!=rts::TgtKind::Bld)) continue;
        const auto& stats=v.stats().of(v.unit_type()[e]);
        const auto memory=std::max(2*rts::kDecisionPeriodMax,
            stats.cooldown_ticks+stats.windup_ticks+rts::kDecisionPeriodMax);
        threat={enemy,kind,v.unit_target_raw()[e],v.now()+memory};
    }
}

const DefenderPost* DefenderScript::defender_post(rts::UnitId id) const noexcept {
    if (id.index() >= defender_posts_.size()) return nullptr;
    const auto& p = defender_posts_[id.index()];
    return p.active && p.generation == id.generation() ? &p : nullptr;
}

bool DefenderScript::issue_guard_order(const rts::WorldView& view,
                                        std::span<const rts::UnitId> ids, rts::GridPos target) {
    const auto building = view.bld_at(target);
    if (!building.valid()) return false;
    const auto type = view.bld_type()[building.index()];
    const bool wall = (type == rts::BldType::Wall || type == rts::BldType::Gate) &&
        view.bld_built()[building.index()];
    bool issued = false;
    for (const auto id : ids) {
        if (!view.alive(id) || rts::side_of(view.unit_type()[id.index()]) != rts::Side::Defender ||
            !rts::is_combat(view.unit_type()[id.index()])) continue;
        if (wall && view.unit_type()[id.index()] == rts::UnitType::Archer) {
            issue_garrison_order({&id,1},target);
        } else {
            const auto slot = id.index();
            if (defender_posts_.size() <= slot) defender_posts_.resize(static_cast<std::size_t>(slot)+1);
            auto& post = defender_posts_[slot];
            post = {};
            post.active = true; post.generation = id.generation();
            post.anchor = target; post.player_set = true; post.building = building.raw();
            if (slot < manual_order_.size()) manual_order_[slot] = {};
        }
        issued = true;
    }
    return issued;
}

rts::UnitAction DefenderScript::decide_post(const rts::WorldView& v, rts::UnitId id) {
    const auto slot = id.index();
    if (defender_posts_.size() <= slot) defender_posts_.resize(static_cast<std::size_t>(slot)+1);
    auto& post = defender_posts_[slot];
    if (!post.active || post.generation != id.generation()) {
        post = {};
        post.active = true;
        post.generation = id.generation();
        post.anchor = hold_point_for(slot, v.keep_pos());
        post.anchor.i = static_cast<std::int16_t>(std::clamp(int(post.anchor.i), 0, v.width()-1));
        post.anchor.j = static_cast<std::int16_t>(std::clamp(int(post.anchor.j), 0, v.height()-1));
    }
    if (slot < manual_order_.size() && manual_order_[slot].active) {
        const auto order = manual_order_[slot];
        if (order.generation == id.generation()) {
            post.anchor = order.target;
            post.player_set = true;
            post.building = rts::BldId::kInvalidRaw;
        }
        manual_order_[slot] = {};
    }
    if (post.building != rts::BldId::kInvalidRaw) {
        const auto building = v.bld_at(post.anchor);
        if (!building.valid() || building.raw() != post.building) post.building = rts::BldId::kInvalidRaw;
    }
    if (scout_navigation_.size() <= slot) scout_navigation_.resize(static_cast<std::size_t>(slot)+1);
    scout_navigation_[slot].active = false;
    if (v.unit_garrison()[slot] != rts::kNoSlot) return rts::UnitAction::Stop;
    const auto me = v.unit_pos()[slot];
    const auto here = rts::grid_of(me);
    const auto mask = v.action_mask(id);
    const int w = v.width(), h = v.height(), n = w*h;
    const auto index = [w](rts::GridPos p) {return int(p.j)*w + int(p.i);};
    const auto cell = [w](int c) {return rts::GridPos{static_cast<std::int16_t>(c%w), static_cast<std::int16_t>(c/w)};};
    const auto type = v.unit_type()[slot];
    const float attack_range = v.stats().of(type).range;
    const float engage_cells = type == rts::UnitType::Ranger ? p_.ranger_engage_cells : p_.spear_engage_cells;
    const auto anchor=rts::center_of(post.anchor);
    const float response_cells=std::max(engage_cells,p_.ranger_chase_cells);
    const auto attacks_post = [&](std::size_t e) {
        if (e>=post_threats_.size()) return false;
        const auto& threat=post_threats_[e];
        if (!threat.enemy.valid() || !v.alive(threat.enemy) || threat.until<v.now()) return false;
        if (threat.target_kind==rts::TgtKind::Bld)
            return post.building!=rts::BldId::kInvalidRaw && threat.target==post.building;
        if (threat.target_kind!=rts::TgtKind::Unit) return false;
        if (threat.target==id.raw()) return true;
        const auto victim=static_cast<std::uint16_t>(threat.target>>16);
        if (victim>=defender_posts_.size()) return false;
        const auto& peer=defender_posts_[victim];
        return peer.active && peer.generation==static_cast<std::uint16_t>(threat.target) &&
            peer.anchor==post.anchor && peer.building==post.building;
    };
    std::vector<std::size_t> enemies;
    std::vector<std::uint8_t> pursue(v.unit_type().size(),0);
    float nearest_d2 = std::numeric_limits<float>::infinity();
    for (std::size_t e=0; e<v.unit_type().size(); ++e) {
        if (!v.unit_alive()[e] || rts::side_of(v.unit_type()[e])!=rts::Side::Attacker ||
            rts::is_aerial(v.unit_type()[e])) continue;
        const auto pos = v.unit_pos()[e];
        const float from_post=distance2(anchor,pos);
        const bool response=from_post<=response_cells*response_cells && attacks_post(e);
        pursue[e]=static_cast<std::uint8_t>(from_post<=engage_cells*engage_cells || response);
        const float local_range = std::max(attack_range,p_.kite_trigger_cells);
        if (distance2(me,pos)>local_range*local_range && !pursue[e]) continue;
        if (v.fog().vis_bytes()[index(rts::grid_of(pos))] != static_cast<std::uint8_t>(rts::Vis::Visible)) continue;
        enemies.push_back(e);
        nearest_d2 = std::min(nearest_d2, distance2(me,pos));
    }
    // Ground archers retain their existing kiting behavior while guarding a building.
    if (type == rts::UnitType::Archer) {
        int threat=-1;float closest=p_.kite_trigger_cells*p_.kite_trigger_cells;
        for (const auto e:enemies) {
            const auto& behavior=rts::behavior_of(v.unit_type()[e]);
            if (!behavior.combat() || behavior.engage_range()==rts::EngageRange::Ranged) continue;
            const float d2=distance2(me,v.unit_pos()[e]);
            if (d2<closest) {closest=d2;threat=static_cast<int>(e);}
        }
        if (threat_streak_.size()<=slot) threat_streak_.resize(static_cast<std::size_t>(slot)+1);
        threat_streak_[slot]=threat>=0?threat_streak_[slot]+1:0;
        if (threat>=0 && threat_streak_[slot]>p_.reaction_decisions &&
            static_cast<std::int32_t>(rng_.below(1000))<p_.kite_permille) {
            post.mode=PostMode::Engaging;
            return flee_from(v,slot,v.unit_pos()[static_cast<std::size_t>(threat)],mask);
        }
    }
    // Fight enemies in reach, regardless of health, enemy level or numerical odds.
    if (nearest_d2 <= attack_range*attack_range && allowed(mask,rts::UnitAction::AtkNear)) {
        post.mode = PostMode::Engaging;
        return rts::UnitAction::AtkNear;
    }
    const bool nearby_enemy = std::any_of(enemies.begin(),enemies.end(),[&](std::size_t e) {
            return pursue[e]!=0;
        });
    if (post.player_set && distance2(me,rts::center_of(post.anchor))<=p_.arrive_cells*p_.arrive_cells &&
        !nearby_enemy) {
        post.mode=PostMode::Holding;
        return rts::UnitAction::Stop;
    }

    std::vector<std::uint8_t> open(v.terrain().passable_bytes(), v.terrain().passable_bytes()+n);
    for (std::size_t b=0; b<v.bld_type().size(); ++b) {
        if (v.bld_alive()[b] && !(v.bld_type()[b]==rts::BldType::Gate && v.bld_built()[b]))
            open[static_cast<std::size_t>(index(v.bld_pos()[b]))] = 0;
    }
    for (std::size_t o=0; o<v.obstacle_pos().size(); ++o)
        if (v.obstacle_alive()[o]) open[static_cast<std::size_t>(index(v.obstacle_pos()[o]))] = 0;
    const int start = index(here);
    open[static_cast<std::size_t>(start)] = 1;
    const auto move = [&](PostRoute route,PostMode mode) {
        post.mode=mode;
        if (rts::is_move(route.first)) scout_navigation_[slot]={true,here,cell(route.goal),id.generation()};
        return route.first;
    };
    if (nearby_enemy) {
        // Compare nearby targets using only paths inside the pursuit budget.
        std::vector<int> steps(static_cast<std::size_t>(n),-1), queue{start};
        std::vector<rts::UnitAction> first(static_cast<std::size_t>(n),rts::UnitAction::Stop);
        steps[static_cast<std::size_t>(start)]=0;
        for (std::size_t head=0; head<queue.size(); ++head) {
            const int c=queue[head], d=steps[static_cast<std::size_t>(c)];
            if (d+1>p_.ranger_chase_cells) continue;
            for (int k=0; k<rts::kMoveDirCount; ++k) {
                const auto a=rts::move_of(k);
                if (rts::is_grid_diagonal(a) || (c==start && !allowed(mask,a))) continue;
                const auto delta=rts::move_delta(a);
                const int x=c%w+delta.di, y=c/w+delta.dj;
                if (x<0 || y<0 || x>=w || y>=h) continue;
                const int next=y*w+x;
                const auto u=static_cast<std::size_t>(next);
                if (!open[u] || steps[u]>=0) continue;
                // A per-decision path budget alone would slide with the unit,
                // allowing a kiting enemy to drag it arbitrarily far from home.
                if (distance2(rts::center_of(cell(next)),anchor)>response_cells*response_cells) continue;
                steps[u]=d+1;
                first[u]=c==start?a:first[static_cast<std::size_t>(c)];
                queue.push_back(next);
            }
        }
        int combat_goal=-1;
        float best=std::numeric_limits<float>::infinity();
        for (const auto e:enemies) {
            const auto pos=v.unit_pos()[e];
            if (!pursue[e]) continue;
            // Prefer nearby exposed ranged/siege targets; no survival threshold.
            const float priority=(v.unit_type()[e]==rts::UnitType::Shade || v.unit_type()[e]==rts::UnitType::Ram)?0.8f:1.0f;
            for (int y=std::max(0,int(pos.y-attack_range)); y<std::min(h,int(pos.y+attack_range)+1); ++y)
                for (int x=std::max(0,int(pos.x-attack_range)); x<std::min(w,int(pos.x+attack_range)+1); ++x) {
                    const int c=y*w+x;
                    const auto u=static_cast<std::size_t>(c);
                    if (c==start || steps[u]<0 ||
                        distance2(rts::center_of(cell(c)),pos)>attack_range*attack_range) continue;
                    const float score=static_cast<float>(steps[u])*priority;
                    if (score<best) {best=score;combat_goal=c;}
                }
        }
        if (combat_goal>=0) return move({combat_goal,first[static_cast<std::size_t>(combat_goal)]},PostMode::Engaging);
    }

    // Idle, undeployed riders may help clear nearby player-marked obstacles.
    if (type == rts::UnitType::Ranger && !post.player_set) {
        std::vector<int> clear_goals;
        for (std::size_t o=0; o<v.obstacle_pos().size(); ++o) {
            if (!v.obstacle_alive()[o] || !v.obstacle_clear_ordered()[o] ||
                distance2(rts::center_of(v.obstacle_pos()[o]),rts::center_of(post.anchor))>
                    p_.ranger_chase_cells*p_.ranger_chase_cells) continue;
            for (int k=0; k<rts::kMoveDirCount; ++k) {
                const auto a=rts::move_of(k); if (rts::is_grid_diagonal(a)) continue;
                const auto delta=rts::move_delta(a);
                const int x=v.obstacle_pos()[o].i+delta.di, y=v.obstacle_pos()[o].j+delta.dj;
                if (x<0 || y<0 || x>=w || y>=h) continue;
                const int c=y*w+x;
                if (!open[static_cast<std::size_t>(c)]) continue;
                if (c==start) {
                    for (int d=0; d<rts::kMoveDirCount; ++d) {
                        const auto step=rts::move_of(d);
                        const auto offset=rts::move_delta(step);
                        if (here.i+offset.di==v.obstacle_pos()[o].i && here.j+offset.dj==v.obstacle_pos()[o].j && allowed(mask,step)) {
                            post.mode=PostMode::Moving;
                            return step;
                        }
                    }
                } else clear_goals.push_back(c);
            }
        }
        const auto route=route_to_any(open,w,h,start,clear_goals,mask);
        if (route.goal>=0) return move(route,PostMode::Moving);
    }
    std::vector<int> home_goals;
    const int radius=static_cast<int>(std::ceil(p_.arrive_cells));
    for (int y=std::max(0,int(post.anchor.j)-radius); y<std::min(h,int(post.anchor.j)+radius+1); ++y)
        for (int x=std::max(0,int(post.anchor.i)-radius); x<std::min(w,int(post.anchor.i)+radius+1); ++x) {
            const int c=y*w+x;
            if (open[static_cast<std::size_t>(c)] &&
                distance2(rts::center_of(cell(c)),rts::center_of(post.anchor))<=p_.arrive_cells*p_.arrive_cells)
                home_goals.push_back(c);
        }
    auto route=route_to_any(open,w,h,start,home_goals,mask);
    if (route.goal<0) {post.mode=PostMode::Blocked; return rts::UnitAction::Stop;}
    if (route.goal==start && distance2(me,rts::center_of(post.anchor))>p_.arrive_cells*p_.arrive_cells) {
        // The current tile's center is in the arrival circle, but the continuous
        // position can still be outside it. Finish that last step within the tile
        // rather than oscillating between two adjacent tiles around the building.
        const auto center=rts::center_of(here);
        const float dx=center.x-me.x,dy=center.y-me.y;
        const auto action=std::abs(dx)>=std::abs(dy)?
            (dx>0?rts::UnitAction::MoveSE:rts::UnitAction::MoveNW):
            (dy>0?rts::UnitAction::MoveSW:rts::UnitAction::MoveNE);
        if (allowed(mask,action)) route.first=action;
        return move(route,PostMode::Returning);
    }
    return move(route,route.goal==start?PostMode::Holding:PostMode::Returning);
}
} // namespace game
