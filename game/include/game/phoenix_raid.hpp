#ifndef GAME_PHOENIX_RAID_HPP
#define GAME_PHOENIX_RAID_HPP

#include <optional>
#include "game/attacker_knowledge.hpp"

namespace game {

struct PhoenixRaidEstimate {
    rts::GridPos target{};
    float approach_ticks = 0;
    float attack_ticks = 0;
    float expected_damage = 0;
    bool viable = false;
};

// Buildings come exclusively from shared observations/memory. Only currently
// observed wall archers supplement that snapshot. No hidden cooldowns or repairs.
PhoenixRaidEstimate estimate_phoenix_raid(const rts::WorldView& view,
    const AttackerKnowledge& knowledge, rts::UnitId bird,
    const rts::FlowBuilding& target, int reserve_permille);

struct PhoenixRaidPlan {
    std::optional<rts::GridPos> target;
    bool known_targets = false;
};
PhoenixRaidPlan choose_phoenix_raid(const rts::WorldView& view,
    const AttackerKnowledge& knowledge, rts::UnitId bird, int reserve_permille);

} // namespace game
#endif
