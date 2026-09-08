#pragma once

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

} // namespace game
