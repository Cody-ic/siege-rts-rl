#pragma once
#include <string>
#include "game/defender_macro.hpp"

namespace bindings {
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
