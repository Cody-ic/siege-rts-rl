#pragma once
#include <string>
#include "game/defender_macro.hpp"

namespace bindings {
inline std::size_t defender_profile_index(std::uint64_t seed,std::size_t maps,std::size_t profiles) {
    if(maps==0 || profiles==0) throw rts::ContractError("Profile schedule dimensions must be positive");
    // Stateless mixing avoids locking style to entrance/level modulo cycles.
    // All maps in one round share a choice; no game or learner RNG is consumed.
    std::uint64_t value=seed/maps+0x9e3779b97f4a7c15ULL;
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ULL;
    value=(value^(value>>27))*0x94d049bb133111ebULL;
    value^=value>>31;
    return static_cast<std::size_t>(value%profiles);
}
// Training opponents, not balance changes or new unit AI. All retain the real
// defender's breach repair, garrison, counter-composition and worker behavior.
inline game::MacroParams defender_profile(const std::string& name) {
    game::MacroParams params;
    if(name=="balanced") return params;
    if(name=="fortified") {
        params.mix_spear_permille=350;
        params.mix_ranger_permille=100;
        params.keep_guard_towers=4;
        params.mason_target=5;
        params.noncombat_max_permille=400;
        params.gatherers_per_wave=1;
        return params;
    }
    if(name=="mobile") {
        params.mix_spear_permille=250;
        params.mix_ranger_permille=350;
        params.keep_guard_towers=1;
        params.max_towers_per_entry=3;
        return params;
    }
    throw rts::ContractError("Unknown scripted defender profile: "+name);
}
} // namespace bindings
