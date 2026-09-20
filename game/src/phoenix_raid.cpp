#include "game/phoenix_raid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include "rts/combat_math.hpp"

namespace game {
namespace {
float distance(rts::Vec2 a, rts::Vec2 b) {
    return std::hypot(a.x-b.x, a.y-b.y);
}
bool attackable(rts::BldType type) {
    return type != rts::BldType::Keep && type != rts::BldType::Wall &&
        type != rts::BldType::Gate && type != rts::BldType::Fence;
}
struct Threat {
    rts::Vec2 pos;
    float range, damage, period;
    bool removed_by_raid;
};
std::vector<Threat> threats(const rts::WorldView& v, const AttackerKnowledge& knowledge,
                            rts::GridPos target) {
    std::vector<Threat> result;
    const auto& stats = v.stats();
    for (const auto& b : knowledge.buildings()) {
        if (b.type != rts::BldType::Flak || !b.built || b.hp <= 0) continue;
        const auto& s = stats.of(b.type);
        if (s.damage <= 0) continue;
        result.push_back({rts::center_of(b.pos), s.range,
            static_cast<float>(rts::apply_permille(s.damage,
                {rts::level_permille(b.level, stats.global.dmg_permille_per_level)})),
            static_cast<float>(std::max(s.cooldown_ticks, s.windup_ticks+1)), b.pos==target});
    }
    for (std::size_t u=0; u<v.unit_alive().size(); ++u) {
        if (!v.unit_alive()[u] || v.unit_type()[u] != rts::UnitType::Archer ||
            v.unit_garrison()[u] == rts::kNoSlot || v.unit_mount()[u] != 0 ||
            !knowledge.visible(rts::grid_of(v.unit_pos()[u]))) continue;
        const auto& s = stats.of(rts::UnitType::Archer);
        if (s.damage <= 0) continue;
        // Treat a mounted archer as retaining the full wall range, even if the
        // wall is damaged. This avoids consulting unseen wall health.
        result.push_back({v.unit_pos()[u], s.range+stats.global.high_ground_range_bonus,
            static_cast<float>(rts::apply_permille(s.damage,
                {rts::level_permille(v.unit_level()[u], stats.global.dmg_permille_per_level)})),
            static_cast<float>(std::max(s.cooldown_ticks, s.windup_ticks+1)), false});
    }
    return result;
}
struct Flight {
    std::vector<rts::Vec2> points;
    float ticks=0;
    bool arrived=false;
};
Flight flight_route(rts::Vec2 start,rts::Vec2 goal,float range,float speed,int width,int height) {
    Flight route;route.points.push_back(start);
    const float step=speed*static_cast<float>(rts::kDecisionPeriodMax);
    const int limit=static_cast<int>(std::ceil(distance(start,goal)*2.0f/step))+8;
    for(int n=0;n<=limit;++n) {
        const auto p=route.points.back();
        if(distance(p,goal)<=range) {route.arrived=true;break;}
        rts::Vec2 direction{};float best_dot=0;
        for(int d=0;d<rts::kMoveDirCount;++d) {
            const auto a=rts::move_of(d);const auto delta=rts::move_delta(a);
            const float norm=rts::is_grid_diagonal(a)?0.70710678118f:1.0f;
            const rts::Vec2 unit{static_cast<float>(delta.di)*norm,static_cast<float>(delta.dj)*norm};
            const float dot=(goal.x-p.x)*unit.x+(goal.y-p.y)*unit.y;
            if(dot>best_dot) {best_dot=dot;direction=unit;}
        }
        route.points.push_back({std::clamp(p.x+direction.x*step,0.0f,static_cast<float>(width)-0.001f),
            std::clamp(p.y+direction.y*step,0.0f,static_cast<float>(height)-0.001f)});
        route.ticks+=static_cast<float>(rts::kDecisionPeriodMax);
    }
    return route;
}
// Exact circle intersection along each eight-direction flight segment. The
// forecast follows the same direction choice and decision period as the script.
float exposure(rts::Vec2 a, rts::Vec2 b, const Threat& threat, float speed) {
    const float len = distance(a,b);
    if (len < 0.001f) return 0;
    const float dx=(b.x-a.x)/len, dy=(b.y-a.y)/len;
    const float ox=a.x-threat.pos.x, oy=a.y-threat.pos.y;
    const float dot=ox*dx+oy*dy;
    const float radius=threat.range+speed; // One tick at the entry/exit boundary.
    const float disc=dot*dot-(ox*ox+oy*oy-radius*radius);
    if (disc <= 0) return 0;
    const float root=std::sqrt(disc);
    const float inside=std::max(0.0f,std::min(len,-dot+root)-std::max(0.0f,-dot-root));
    return inside/speed;
}
float exposure(const Flight& flight,const Threat& threat,float speed) {
    float ticks=0;
    for(std::size_t i=1;i<flight.points.size();++i)
        ticks+=exposure(flight.points[i-1],flight.points[i],threat,speed);
    return ticks;
}
} // namespace

PhoenixRaidEstimate estimate_phoenix_raid(const rts::WorldView& v,
    const AttackerKnowledge& knowledge, rts::UnitId bird,
    const rts::FlowBuilding& target, int reserve_permille) {
    PhoenixRaidEstimate out; out.target=target.pos;
    if (!v.alive(bird) || v.unit_type()[bird.index()] != rts::UnitType::Phoenix ||
        !attackable(target.type) || target.hp <= 0) return out;
    const auto& s=v.stats().of(rts::UnitType::Phoenix);
    if (s.speed <= 0 || s.damage <= 0) return out;
    const auto start=v.unit_pos()[bird.index()];
    const auto center=rts::center_of(target.pos);
    const auto approach=flight_route(start,center,s.range,s.speed,v.width(),v.height());
    if(!approach.arrived) return out;
    const auto firing=approach.points.back();
    out.approach_ticks=approach.ticks;
    out.attack_ticks=rts::estimate_breach_ticks(v.stats(), rts::UnitType::Phoenix,
        v.unit_level()[bird.index()], target.hp);
    const auto index=bird.index();
    if (v.unit_windup()[index]>0 && v.unit_target_kind()[index]==rts::TgtKind::Bld &&
        distance(v.unit_aim()[index],center)<0.001f) {
        // The already committed hit is part of the remaining HP, not an extra
        // attack after a fresh cooldown. Double-counting it made winning raids
        // abort during their own windup.
        const auto damage=rts::apply_permille(s.damage,
            {rts::level_permille(v.unit_level()[index],v.stats().global.dmg_permille_per_level),s.vs_structure_permille});
        const auto hits=(target.hp-1)/damage+1;
        out.attack_ticks=static_cast<float>(v.unit_windup()[index])+
            static_cast<float>(hits-1)*static_cast<float>(std::max(s.cooldown_ticks,s.windup_ticks+1));
    } else {
        out.attack_ticks+=std::max(0.0f,static_cast<float>(v.unit_cooldown()[index]-1)-out.approach_ticks);
    }

    // The core's AtkBld selects Flak first, then nearest ordinary building. Do
    // not value one target and accidentally attack a different nearby target.
    for (const auto& b : knowledge.buildings()) {
        if (b.pos==target.pos || b.hp<=0 || !attackable(b.type)) continue;
        const float other=distance(firing,rts::center_of(b.pos));
        if (other>s.range) continue;
        if ((b.type==rts::BldType::Flak && target.type!=rts::BldType::Flak) ||
            ((b.type==rts::BldType::Flak)==(target.type==rts::BldType::Flak) &&
                other+0.001f<distance(firing,center))) return out;
    }
    rts::Vec2 exit=start;
    float exit_dist=std::numeric_limits<float>::infinity();
    for (const auto& spawn:v.spawns()) {
        const auto pos=rts::center_of(spawn.pos);
        const float d=distance(firing,pos);
        if (d<exit_dist) {exit_dist=d;exit=pos;}
    }
    const auto escape=flight_route(firing,exit,1.0f,s.speed,v.width(),v.height());
    if(!escape.arrived) return out;
    for (const auto& threat:threats(v,knowledge,target.pos)) {
        float ticks=exposure(approach,threat,s.speed);
        const bool covers_firing=distance(firing,threat.pos)<=threat.range+s.speed;
        if (covers_firing)
            ticks+=out.attack_ticks;
        // Destroying the selected Flak removes its future fire. Other Flaks
        // and wall archers continue firing during the escape.
        if (!threat.removed_by_raid) {
            const float escape_ticks=exposure(escape,threat,s.speed);
            // Separate passes through a circle allow the weapon to reload in
            // the gap. Do not combine them into one artificially cheap pass.
            if(!covers_firing && escape_ticks>0)
                out.expected_damage+=(1.0f+std::floor(escape_ticks/threat.period))*threat.damage;
            else ticks+=escape_ticks;
        }
        if (ticks>0) out.expected_damage+=(1.0f+std::floor(ticks/threat.period))*threat.damage;
    }
    const float reserve=static_cast<float>(v.unit_max_hp()[bird.index()])*
        static_cast<float>(std::clamp(reserve_permille,0,1000))/1000.0f;
    out.viable=static_cast<float>(v.unit_hp()[bird.index()])-out.expected_damage>=reserve &&
        static_cast<float>(v.unit_hp()[bird.index()])>out.expected_damage;
    return out;
}

PhoenixRaidPlan choose_phoenix_raid(const rts::WorldView& v,
    const AttackerKnowledge& knowledge, rts::UnitId bird, int reserve_permille) {
    PhoenixRaidPlan plan;
    float best_damage=std::numeric_limits<float>::infinity();
    float best_time=std::numeric_limits<float>::infinity();
    bool best_flak=false;
    bool best_near=false;
    for (const auto& target:knowledge.buildings()) {
        if (!attackable(target.type) || target.hp<=0) continue;
        plan.known_targets=true;
        const auto estimate=estimate_phoenix_raid(v,knowledge,bird,target,reserve_permille);
        if (!estimate.viable) continue;
        const bool flak=target.type==rts::BldType::Flak;
        const bool near=distance(v.unit_pos()[bird.index()],rts::center_of(target.pos))<=
            v.stats().of(rts::UnitType::Phoenix).vision;
        const float time=estimate.approach_ticks+estimate.attack_ticks;
        const bool cheaper=estimate.expected_damage<best_damage ||
            (estimate.expected_damage==best_damage && time<best_time);
        // An unrelated AA tower across the city must not distract a bird from
        // a safe nearby raid. Within the local objectives, clear AA first.
        if (!plan.target || (near && !best_near) || (near==best_near &&
            ((flak && !best_flak) || (flak==best_flak && cheaper)))) {
            best_damage=estimate.expected_damage;best_time=time;best_flak=flak;best_near=near;plan.target=target.pos;
        }
    }
    return plan;
}
} // namespace game
