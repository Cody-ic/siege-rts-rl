#include <charconv>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include "game/demo_driver.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"

template<class T> T number(const char* text) {
    T result{};const std::string value=text;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) throw std::runtime_error("Invalid numeric argument");
    return result;
}
int main(int argc,char** argv) {
    try {
        if(argc!=8) throw std::runtime_error("Usage: macro_game_evaluate MAP STATS ATTACKER_ONNX DEFENDER_DIR SEED MAX_WAVE MAX_TICKS");
        const auto seed=number<std::uint64_t>(argv[5]);
        const int max_wave=number<int>(argv[6]),max_ticks=number<int>(argv[7]);
        if(max_wave<1 || max_ticks<1) throw std::runtime_error("Positive evaluation limits required");
        const auto stats=game::StatsLoader::from_file(argv[2]);
        auto attacker=std::make_shared<game::TacticalPolicy>(argv[3],stats.fingerprint());
        auto defender=std::make_shared<game::MacroPolicy>(argv[4],argv[2]);
        game::DemoBattle battle(game::MapLoader::from_file(argv[1]),stats,seed);
        battle.set_tactical_policy(attacker);battle.set_defender_policy(defender);
        const auto started=std::chrono::steady_clock::now();
        while(!battle.defeated() && battle.world().wave()<=max_wave && battle.world().now()<max_ticks)
            battle.update(1);
        const bool completed=battle.world().wave()>max_wave;
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        nlohmann::json result={{"map",argv[1]},{"seed",seed},{"tick",battle.world().now()},
            {"wave",battle.world().wave()},{"waves_survived",battle.world().wave()-1},
            {"defeated",battle.defeated()},{"completed",completed},{"timeout",!completed&&!battle.defeated()},
            {"state_hash",battle.world().state_hash()},{"policy_rng",battle.defender_policy_rng()},
            {"attacker_identity",attacker->identity()},{"defender_identity",defender->identity()},
            {"policy_period",defender->period()},{"runtime_seconds",seconds}};
        std::cout<<result.dump()<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
