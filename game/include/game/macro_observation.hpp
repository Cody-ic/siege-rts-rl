#ifndef GAME_MACRO_OBSERVATION_HPP
#define GAME_MACRO_OBSERVATION_HPP
#include <array>
#include <string>
#include <vector>
#include "rts/world_view.hpp"

namespace game {
inline constexpr int kMacroObsVersion=2;
inline constexpr int kMacroDetailChannels=rts::kBldTypeCount+13;
inline constexpr int kMacroGrid=16;
inline constexpr int kMacroChannels=3*rts::kUnitTypeCount+3*rts::kBldTypeCount+5;
inline constexpr int kMacroGlobals=10;
struct MacroObservation {
    std::vector<float> cells;
    std::array<float,kMacroGlobals> global{};
};
std::vector<std::string> macro_cell_names();
std::vector<std::string> macro_global_names();
std::vector<std::string> macro_detail_names();
// Full-resolution public terrain and own building state, HWC. No enemy units.
std::vector<float> pack_macro_detail(const rts::WorldView& defender);
// Public defender information only. Hidden enemy units never contribute, even
// to total counts, levels or HP. All buildings belong to the defender in this game.
MacroObservation pack_macro_observation(const rts::WorldView& defender);
// Tests one fully specified command, including affordability and target state.
// Evaluate again after each selected command; masks do not reserve resources.
bool macro_command_legal(const rts::WorldView& defender,const rts::Command& command,
                         bool summon_allowed);
// Complete legal command set, in canonical order; no tactical ranking or cap.
// Includes every buildable cell and every affordable recruitment level.
std::vector<rts::Command> macro_candidates(const rts::WorldView& defender,bool summon_allowed);
}
#endif
