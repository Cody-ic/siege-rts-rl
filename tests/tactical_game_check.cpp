#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "game/save_game.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"

int main(int argc,char** argv) {
    try {
        if(argc!=6) throw std::runtime_error("Usage: tactical_game_check MAP STATS ATTACKER DEFENDER SAVE");
        const auto require=[](bool ok){if(!ok) throw std::runtime_error("Tactical game check failed");};
        const auto map=game::read_save_text(argv[1]),stats_text=game::read_save_text(argv[2]);
        const auto stats=game::StatsLoader::from_string(stats_text);
        auto attack=std::make_shared<game::TacticalPolicy>(argv[3],stats.fingerprint());
        auto defense=std::make_shared<game::MacroPolicy>(argv[4],argv[2]);
        require(attack->supports_macro_goals());
        game::GameShell shell(game::MapLoader::from_string(map),stats,77,attack,defense);
        shell.apply(game::MenuAction::StartNew);
        shell.battle()->update(1200);
        require(!shell.battle()->defeated() && shell.battle()->learned_squads()>0);
        nlohmann::json checkpoints=nlohmann::json::array();
        const auto check_restore=[&](const std::string& path) {
            const auto saved_tick=shell.battle()->world().now();
            const auto saved_goals=shell.battle()->tactical_goal_diagnostics();
            game::write_archive(path,game::capture_battle(shell,map,stats_text));
            auto disk=game::read_archive(path);
            auto reloaded=std::make_shared<game::TacticalPolicy>(argv[3],stats.fingerprint());
            auto defender=std::make_shared<game::MacroPolicy>(argv[4],argv[2]);
            auto snapshot=game::restore_battle(disk,{},reloaded,defender);
            disk.snapshot.clear();
            auto replay=game::restore_battle(disk,{},reloaded,defender);
            for(auto* battle:{snapshot.get(),replay.get()}) {
                require(shell.battle()->world().state_hash()==battle->world().state_hash());
                require(saved_goals==battle->tactical_goal_diagnostics());
                require(shell.battle()->defender_policy_rng()==battle->defender_policy_rng());
            }
            for(int i=0;i<30;++i) {
                for(auto* battle:{shell.battle(),snapshot.get(),replay.get()}) battle->update(6);
                require(shell.battle()->world().state_hash()==snapshot->world().state_hash());
                require(shell.battle()->world().state_hash()==replay->world().state_hash());
                require(shell.battle()->tactical_goal_diagnostics()==snapshot->tactical_goal_diagnostics());
                require(shell.battle()->tactical_goal_diagnostics()==replay->tactical_goal_diagnostics());
                require(shell.battle()->defender_policy_rng()==snapshot->defender_policy_rng());
                require(shell.battle()->defender_policy_rng()==replay->defender_policy_rng());
            }
            require(shell.battle()->world().now()==saved_tick+180);
            checkpoints.push_back({{"saved_tick",saved_tick},{"saved_goal_counts",saved_goals},
                {"continuation_checks",30},{"final_tick",shell.battle()->world().now()}});
        };
        check_restore(argv[5]);
        bool economy_restore_checked=false;
        std::size_t peak_targets=0,peak_economy=0,peak_supported=0;
        while(!shell.battle()->defeated() && shell.battle()->world().wave()<=6 && shell.battle()->world().now()<30000) {
            shell.battle()->update(6);
            const auto counts=shell.battle()->tactical_goal_diagnostics();
            peak_targets=std::max(peak_targets,counts[0]);peak_economy=std::max(peak_economy,counts[1]);
            peak_supported=std::max(peak_supported,counts[2]);
            if(counts[1]>0 && !economy_restore_checked) {
                check_restore(std::string(argv[5])+".economy.json");
                economy_restore_checked=true;
            }
        }
        nlohmann::json result={{"snapshot_and_replay_exact",true},{"checkpoints",checkpoints},
            {"economy_restore_checked",economy_restore_checked},
            {"attacker_identity",attack->identity()},{"defender_identity",defense->identity()},
            {"peak_known_economy_targets",peak_targets},{"peak_supported_economy_squads",peak_economy},
            {"peak_supported_squads",peak_supported},{"economy_goal_exercised",peak_economy>0},
            {"wave",shell.battle()->world().wave()},{"tick",shell.battle()->world().now()},
            {"defeated",shell.battle()->defeated()}};
        std::cout<<result.dump()<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
