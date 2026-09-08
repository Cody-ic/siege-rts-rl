#include <iostream>
#include <stdexcept>
#include "game/save_game.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"

namespace {
void require(bool condition) {if(!condition) throw std::runtime_error("Macro game save check failed");}
template<class F> void rejects(F action) {
    bool failed=false;try {action();}catch(const std::exception&) {failed=true;}
    require(failed);
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=5) throw std::runtime_error("Usage: macro_game_check MAP STATS MODEL_DIRECTORY SAVE_PATH");
        const auto map_text=game::read_save_text(argv[1]),stats_text=game::read_save_text(argv[2]);
        auto model=std::make_shared<game::MacroPolicy>(argv[3],argv[2]);
        game::GameShell shell(game::MapLoader::from_string(map_text),game::StatsLoader::from_string(stats_text),77,{},model);
        shell.apply(game::MenuAction::StartNew);
        shell.battle()->update(1000);
        require(shell.battle()->world().now()==1000);
        require(shell.battle()->player_events().empty());
        auto archive=game::capture_battle(shell,map_text,stats_text);
        require(archive.defender_policy_identity==model->identity());
        game::write_archive(argv[4],archive);
        auto disk=game::read_archive(argv[4]);
        // Reload model too: no hidden mutable state from the original instance.
        auto reloaded=std::make_shared<game::MacroPolicy>(argv[3],argv[2]);
        auto snapshot=game::restore_battle(disk,{},{},reloaded);
        disk.snapshot.clear();
        auto replay=game::restore_battle(disk,{},{},reloaded);
        rejects([&]{game::restore_battle(disk);});
        auto wrong=disk;wrong.defender_policy_identity+="wrong";
        rejects([&]{game::restore_battle(wrong,{},{},reloaded);});
        wrong=disk;wrong.defender_policy_rng[0]^=1;
        rejects([&]{game::restore_battle(wrong,{},{},reloaded);});
        for(int i=0;i<40;++i) {
            shell.battle()->update(20);snapshot->update(20);replay->update(20);
            require(shell.battle()->world().state_hash()==snapshot->world().state_hash());
            require(shell.battle()->world().state_hash()==replay->world().state_hash());
            require(shell.battle()->defender_policy_rng()==snapshot->defender_policy_rng());
            require(shell.battle()->defender_policy_rng()==replay->defender_policy_rng());
        }
        rejects([&]{shell.battle()->set_defender_policy({});});
        std::cout<<"{\"save_version\":5,\"snapshot_and_replay_exact\":true,\"continuation_checks\":40,"
                    "\"wrong_model_and_rng_rejected\":true,\"tick\":"<<shell.battle()->world().now()<<"}\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
