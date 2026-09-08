#ifndef GAME_TRAINING_CAMPAIGN_HPP
#define GAME_TRAINING_CAMPAIGN_HPP

#include <span>
#include <vector>
#include "game/demo_driver.hpp"
#include "game/defender_macro.hpp"

namespace game {

struct CampaignTransition {
    int ticks = 0;
    bool wave_advanced = false;
    bool defeated = false;
    rts::World::Tally attacker;
    rts::World::Tally defender;
};

// A complete native campaign for macro training. Single-unit control stays in
// the game's scripts (or a frozen attacker policy); external decisions use only
// player commands and orders. A wave boundary ends a training transition, not
// the physical campaign: resources, construction, fog and survivors persist.
class TrainingCampaign {
public:
    TrainingCampaign(const MapData& map, const rts::StatsTable& stats, std::uint64_t seed,
                     int macro_period = 20, const MacroParams& params = {},
                     std::shared_ptr<TacticalPolicy> attacker = {});
    const rts::World& world() const noexcept { return battle_.world(); }
    bool defeated() const noexcept { return battle_.defeated(); }
    bool summon_allowed() const noexcept { return battle_.summon_accepted_now(); }
    // Offline script label with the same single-command interface as the learner.
    // Copies the teacher; querying never mutates campaign or script history.
    rts::Command teacher_command() const;

    // Deep-copy game and opponent state for paired decisions. No reseeding or
    // reconstruction from visible summaries; any frozen inference model is shared.
    TrainingCampaign fork() const { return *this; }
    CampaignTransition advance_scripted(int max_ticks);
    CampaignTransition advance(int max_ticks, std::span<const rts::Command> commands,
                               std::span<const UnitOrder> orders = {});

private:
    CampaignTransition run(int max_ticks, bool scripted,
                           std::span<const rts::Command> commands,
                           std::span<const UnitOrder> orders);
    DemoBattle battle_;
    DefenderMacro macro_;
    int macro_period_;
    std::vector<rts::Command> commands_;
    std::vector<UnitOrder> orders_;
};
} // namespace game
#endif
