#pragma once

#include <algorithm>
#include "rts/batched_env.hpp"
#include "rts/world_view.hpp"

namespace game {

// Training-only target sampler. Resource locations are public terrain data;
// the existence of a harvesting building must come from the attacker's memory.
// Stateless: resets and repeated observations cannot retain another city's intel.
inline rts::BatchedGoals known_economy_training_goals(
    const rts::WorldView& view, std::span<const rts::UnitId> leaders) {
    if(view.side()!=rts::Side::Attacker)
        throw rts::ContractError("Economy tactical goals require attacker observations");
    rts::BatchedGoals out;
    const auto& fog=view.fog();
    for(const auto& site:view.resources()) {
        const auto c=site.pos;
        if(!fog.in_bounds(c.i,c.j) || fog.at(c.i,c.j)==rts::Vis::Unseen) continue;
        rts::RememberedBld building;
        if(!fog.remembered_bld(c.i,c.j,&building)) continue;
        if(building.type==rts::BldType::Quarry || building.type==rts::BldType::Lumber ||
           building.type==rts::BldType::Mine) out.economy.push_back(c);
    }
    out.groups.assign(leaders.size(),out.economy.empty()?std::uint8_t{0}:std::uint8_t{1});
    return out;
}

// Fixed episode assignment: a fallen raider does not recruit replacement squads.
// All original members are retained so a surviving follower inherits the task.
// reset() reads only the attacker's own roster; targets still use fog memory.
class SplitEconomyTrainingGoals {
public:
    void reset(const rts::World& world) {
        members_.clear();
        std::vector<rts::UnitId> leaders,units,eligible;
        world.enumerate_squads(rts::Side::Attacker,leaders);
        world.enumerate_units(rts::Side::Attacker,units);
        for(auto id:leaders) {
            const auto type=world.unit_type(id);
            if(type==rts::UnitType::Ghoul || type==rts::UnitType::Knight) eligible.push_back(id);
        }
        const auto count=std::min(eligible.size(),std::max(std::size_t{1},eligible.size()/4));
        for(std::size_t i=0;i<count;++i) {
            const auto leader=eligible[i];
            for(auto id:units)
                if(id==leader || (world.unit_squad(leader)!=rts::kNoSquad &&
                                 world.unit_squad(id)==world.unit_squad(leader))) members_.push_back(id);
        }
    }
    rts::BatchedGoals operator()(const rts::WorldView& view,std::span<const rts::UnitId> leaders) const {
        auto goals=known_economy_training_goals(view,leaders);
        for(std::size_t i=0;i<leaders.size();++i)
            if(std::find(members_.begin(),members_.end(),leaders[i])==members_.end()) goals.groups[i]=0;
        return goals;
    }
private:
    std::vector<rts::UnitId> members_;
};

} // namespace game
