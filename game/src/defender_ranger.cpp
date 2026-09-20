#include "game/defender_script.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

#include "rts/combat_math.hpp"
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
float unit_dps(const rts::WorldView& v, std::size_t u) {
    const auto& s = v.stats().of(v.unit_type()[u]);
    if (s.damage <= 0) return 0;
    const auto damage = rts::apply_permille(s.damage,
        {rts::level_permille(v.unit_level()[u], v.stats().global.dmg_permille_per_level)});
    float value = static_cast<float>(damage) / static_cast<float>(std::max({1, s.cooldown_ticks, s.windup_ticks+1}));
    // Approach safety includes the possible charge, not only the knight's base hit.
    if (rts::behavior_of(v.unit_type()[u]).charges())
        value *= 1.0f + v.stats().global.charge_max_cells *
            static_cast<float>(v.stats().global.charge_bonus_permille_per_cell) / 1000.0f;
    return value;
}
}

const RangerPost* DefenderScript::ranger_post(rts::UnitId id) const noexcept {
    if (id.index() >= ranger_posts_.size()) return nullptr;
    const auto& p = ranger_posts_[id.index()];
    return p.active && p.generation == id.generation() ? &p : nullptr;
}

rts::UnitAction DefenderScript::decide_ranger(const rts::WorldView& v, rts::UnitId id) {
    const auto slot = id.index();
    if (ranger_posts_.size() <= slot) ranger_posts_.resize(static_cast<std::size_t>(slot)+1);
    auto& post = ranger_posts_[slot];
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
            // A new destination replaces the post, but cannot override danger.
            post.retreat_until = 0;
        }
        manual_order_[slot] = {};
    }
    if (scout_navigation_.size() <= slot) scout_navigation_.resize(static_cast<std::size_t>(slot)+1);
    scout_navigation_[slot].active = false;
    if (v.unit_garrison()[slot] != rts::kNoSlot) return rts::UnitAction::Stop;
    const auto me = v.unit_pos()[slot];
    const auto here = rts::grid_of(me);
    const auto mask = v.action_mask(id);
    const float hp = static_cast<float>(v.unit_hp()[slot]);
    const float max_hp = static_cast<float>(v.unit_max_hp()[slot]);
    if (hp < max_hp*0.4f) post.recovering = true;
    if (hp >= max_hp*0.8f) post.recovering = false;

    const int w = v.width(), h = v.height(), n = w*h;
    const auto index = [w](rts::GridPos p) {return int(p.j)*w + int(p.i);};
    const auto cell = [w](int c) {return rts::GridPos{static_cast<std::int16_t>(c%w), static_cast<std::int16_t>(c/w)};};
    std::vector<std::uint8_t> open(v.terrain().passable_bytes(), v.terrain().passable_bytes()+n);
    for (std::size_t b=0; b<v.bld_type().size(); ++b) {
        if (v.bld_alive()[b] && !(v.bld_type()[b]==rts::BldType::Gate && v.bld_built()[b]))
            open[static_cast<std::size_t>(index(v.bld_pos()[b]))] = 0;
    }
    for (std::size_t o=0; o<v.obstacle_pos().size(); ++o)
        if (v.obstacle_alive()[o]) open[static_cast<std::size_t>(index(v.obstacle_pos()[o]))] = 0;
    std::vector<std::size_t> enemies;
    std::vector<float> risk(static_cast<std::size_t>(n), 0);
    for (std::size_t e=0; e<v.unit_type().size(); ++e) {
        if (!v.unit_alive()[e] || rts::side_of(v.unit_type()[e])!=rts::Side::Attacker) continue;
        const auto pos = v.unit_pos()[e];
        if (v.fog().vis_bytes()[index(rts::grid_of(pos))] != static_cast<std::uint8_t>(rts::Vis::Visible)) continue;
        enemies.push_back(e);
        if (!rts::is_combat(v.unit_type()[e])) continue;
        const auto& stats = v.stats().of(v.unit_type()[e]);
        const float radius = v.unit_type()[e]==rts::UnitType::Knight ?
            std::max(stats.range+1.0f,p_.avoid_knight_cells)+0.5f : stats.range+1.0f;
        const float dps = unit_dps(v, e);
        for (int y=std::max(0, int(pos.y-radius)); y<std::min(h, int(pos.y+radius)+1); ++y)
            for (int x=std::max(0, int(pos.x-radius)); x<std::min(w, int(pos.x+radius)+1); ++x) {
                const rts::GridPos p{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
                if (distance2(rts::center_of(p),pos) <= radius*radius &&
                    (rts::is_aerial(v.unit_type()[e]) || rts::line_of_sight(v.terrain(),rts::grid_of(pos),p)))
                    risk[static_cast<std::size_t>(y*w+x)] += dps;
            }
    }

    // A single reachable-area search supplies approach, return and escape paths.
    // Buildings/obstacles remain solid; unreachable goals never fall back to ramming a wall.
    const float step_ticks = 1.0f/std::max(0.01f,v.stats().of(rts::UnitType::Ranger).speed);
    const int start = index(here);
    open[static_cast<std::size_t>(start)] = 1;
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> cost(static_cast<std::size_t>(n),inf), exposure(static_cast<std::size_t>(n),0);
    std::vector<int> steps(static_cast<std::size_t>(n),0);
    std::vector<rts::UnitAction> first(static_cast<std::size_t>(n),rts::UnitAction::Stop);
    using Node = std::pair<float,int>;
    std::priority_queue<Node,std::vector<Node>,std::greater<Node>> queue;
    cost[static_cast<std::size_t>(start)]=0;
    queue.push({0.0f,start});
    // Search locally for combat; long player orders and recovery can route farther.
    float radius = std::max(12.0f, std::sqrt(distance2(me,rts::center_of(post.anchor)))+6.0f);
    if (post.recovering) for (std::size_t b=0;b<v.bld_pos().size();++b)
        if (v.bld_alive()[b] && v.bld_type()[b]==rts::BldType::Barrack && v.bld_built()[b])
            radius=std::max(radius,std::sqrt(distance2(me,rts::center_of(v.bld_pos()[b])))+6.0f);
    while (!queue.empty()) {
        const auto [d,c]=queue.top();queue.pop();
        if (d!=cost[static_cast<std::size_t>(c)]) continue;
        for (int k=0;k<rts::kMoveDirCount;++k) {
            const auto a=rts::move_of(k);
            if (rts::is_grid_diagonal(a) || (c==start && !allowed(mask,a))) continue;
            const auto delta=rts::move_delta(a);
            const int x=c%w+delta.di,y=c/w+delta.dj;
            if (x<0 || y<0 || x>=w || y>=h) continue;
            const int next=y*w+x;const auto u=static_cast<std::size_t>(next);
            if (!open[u] || distance2(me,rts::center_of(cell(next)))>radius*radius) continue;
            const float damage=(risk[static_cast<std::size_t>(c)]+risk[u])*0.5f*step_ticks;
            const float nd=d+step_ticks+damage/std::max(1.0f,hp)*200.0f;
            if (nd<cost[u]) {
                cost[u]=nd;exposure[u]=exposure[static_cast<std::size_t>(c)]+damage;
                steps[u]=steps[static_cast<std::size_t>(c)]+1;
                first[u]=c==start?a:first[static_cast<std::size_t>(c)];queue.push({nd,next});
            }
        }
    }
    const auto move = [&](int goal,RangerMode mode) {
        post.mode=mode;
        const auto action=first[static_cast<std::size_t>(goal)];
        if (rts::is_move(action)) scout_navigation_[slot]={true,here,cell(goal),id.generation()};
        return action;
    };
    const auto support_dps = [&](std::size_t target) {
        float sum=unit_dps(v,slot);
        const auto pos=v.unit_pos()[target];
        for (std::size_t f=0;f<v.unit_pos().size();++f) {
            if (f==slot || !v.unit_alive()[f] || rts::side_of(v.unit_type()[f])!=rts::Side::Defender ||
                !rts::is_combat(v.unit_type()[f]) || v.unit_mount()[f]>0) continue;
            const auto& fs=v.stats().of(v.unit_type()[f]);
            // Count only allies already able to fire; distant troops behind walls are not support.
            if (distance2(pos,v.unit_pos()[f])<=fs.range*fs.range &&
                rts::line_of_sight(v.terrain(),rts::grid_of(v.unit_pos()[f]),rts::grid_of(pos))) sum+=unit_dps(v,f);
        }
        for (std::size_t b=0;b<v.bld_pos().size();++b) {
            if (!v.bld_alive()[b] || !v.bld_built()[b] || v.bld_type()[b]==rts::BldType::Flak) continue;
            const auto& bs=v.stats().of(v.bld_type()[b]);
            if (bs.damage>0 && distance2(pos,rts::center_of(v.bld_pos()[b]))<=bs.range*bs.range)
                sum+=static_cast<float>(rts::apply_permille(bs.damage,{rts::level_permille(v.bld_level()[b],v.stats().global.dmg_permille_per_level)})) /
                    static_cast<float>(std::max({1,bs.cooldown_ticks,bs.windup_ticks+1}));
        }
        return std::max(0.001f,sum);
    };
    const float attack_range=v.stats().of(rts::UnitType::Ranger).range;
    int nearest=-1;float nearest_d2=inf;
    for (const auto e:enemies) if (!rts::is_aerial(v.unit_type()[e])) {
        const float d2=distance2(me,v.unit_pos()[e]);
        if (d2<nearest_d2) {nearest_d2=d2;nearest=static_cast<int>(e);}
    }
    int combat_goal=-1;float best=inf;bool threatened=risk[static_cast<std::size_t>(start)]>0;
    for (const auto e:enemies) {
        if (rts::is_aerial(v.unit_type()[e]) || post.recovering || v.now()<post.retreat_until) continue;
        const auto pos=v.unit_pos()[e];
        if (distance2(pos,rts::center_of(post.anchor))>p_.ranger_engage_cells*p_.ranger_engage_cells) continue;
        const float support=support_dps(e);
        int goal=-1;float approach=inf;
        for (int y=std::max(0,int(pos.y-attack_range));y<std::min(h,int(pos.y+attack_range)+1);++y)
            for (int x=std::max(0,int(pos.x-attack_range));x<std::min(w,int(pos.x+attack_range)+1);++x) {
                const int c=y*w+x;const auto u=static_cast<std::size_t>(c);
                // AtkNear must actually select the target whose exchange we evaluated.
                if (c==start && static_cast<int>(e)!=nearest) continue;
                const auto stand=c==start?me:rts::center_of(cell(c));
                if (distance2(stand,pos)>attack_range*attack_range || !open[u] ||
                    steps[u]>p_.ranger_chase_cells || exposure[u]>hp*0.25f) continue;
                const float loss=risk[u]*static_cast<float>(v.unit_hp()[e])/support;
                if (loss+2.0f*exposure[u]>hp*0.7f) continue;
                const float score=cost[u]+loss/std::max(1.0f,hp)*100.0f;
                if (score<approach) {approach=score;goal=c;}
            }
        if (goal<0) continue;
        // Prefer exposed ranged/siege targets when their approach costs are comparable.
        const float priority=(v.unit_type()[e]==rts::UnitType::Shade || v.unit_type()[e]==rts::UnitType::Ram)?0.8f:1.0f;
        if (approach*priority<best) {best=approach*priority;combat_goal=goal;}
    }
    if (combat_goal>=0) {
        if (combat_goal==start && allowed(mask,rts::UnitAction::AtkNear)) {
            post.mode=RangerMode::Engaging;return rts::UnitAction::AtkNear;
        }
        return move(combat_goal,RangerMode::Engaging);
    }
    if (threatened) post.retreat_until=v.now()+40;
    const bool retreat=threatened || v.now()<post.retreat_until || post.recovering;
    // Idle, undeployed riders may still help clear nearby obstacles, along safe paths.
    if (!retreat && !post.player_set) {
        int clear_goal=-1, clear_obstacle=-1;float clear_cost=inf;
        for (std::size_t o=0;o<v.obstacle_pos().size();++o) {
            if (!v.obstacle_alive()[o] || !v.obstacle_clear_ordered()[o] ||
                distance2(rts::center_of(v.obstacle_pos()[o]),rts::center_of(post.anchor))>
                    p_.ranger_chase_cells*p_.ranger_chase_cells) continue;
            for (int k=0;k<rts::kMoveDirCount;++k) {
                const auto a=rts::move_of(k);if (rts::is_grid_diagonal(a)) continue;
                const auto delta=rts::move_delta(a);
                const int x=v.obstacle_pos()[o].i+delta.di,y=v.obstacle_pos()[o].j+delta.dj;
                if (x<0 || y<0 || x>=w || y>=h) continue;
                const int c=y*w+x;const auto u=static_cast<std::size_t>(c);
                if (risk[u]>0 || exposure[u]>0 || !open[u] || !std::isfinite(cost[u])) continue;
                if (cost[u]<clear_cost) {clear_cost=cost[u];clear_goal=c;clear_obstacle=static_cast<int>(o);}
            }
        }
        if (clear_goal>=0) {
            if (clear_goal!=start) return move(clear_goal,RangerMode::Moving);
            const auto target=v.obstacle_pos()[static_cast<std::size_t>(clear_obstacle)];
            for (int k=0;k<rts::kMoveDirCount;++k) {
                const auto a=rts::move_of(k);const auto d=rts::move_delta(a);
                if (here.i+d.di==target.i && here.j+d.dj==target.j && allowed(mask,a)) {
                    post.mode=RangerMode::Moving;return a;
                }
            }
        }
    }
    int goal=-1;best=inf;
    for (int c=0;c<n;++c) {
        const auto u=static_cast<std::size_t>(c);
        if (!std::isfinite(cost[u]) || !open[u]) continue;
        const auto point=c==start?me:rts::center_of(cell(c));
        float home=std::sqrt(distance2(point,rts::center_of(post.anchor)));
        const bool recovery_site=post.recovering && v.stats().global.recovery_hp_permille_per_second>0 &&
            v.recovery_barrack(point).valid();
        float score=cost[u]+home*step_ticks;
        if (retreat) score+=risk[u]/std::max(1.0f,hp)*10000.0f;
        // A safe hospital is not useful if reaching it would kill the rider.
        if (recovery_site && risk[u]==0 && exposure[u]<=hp*0.35f) score-=100000.0f;
        if (retreat && risk[static_cast<std::size_t>(start)]==0 && exposure[u]>hp*0.35f) continue;
        if (!retreat && home>p_.arrive_cells) continue;
        if (!retreat && (risk[u]>0 || exposure[u]>hp*0.25f)) continue;
        if (score<best) {best=score;goal=c;}
    }
    if (goal<0) {post.mode=RangerMode::Blocked;return rts::UnitAction::Stop;}
    auto mode=post.recovering?RangerMode::Recovering:retreat?RangerMode::Evading:
        distance2(me,rts::center_of(post.anchor))>p_.arrive_cells*p_.arrive_cells?RangerMode::Returning:RangerMode::Holding;
    return move(goal,mode);
}
} // namespace game
