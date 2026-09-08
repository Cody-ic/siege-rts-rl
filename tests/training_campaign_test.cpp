#include <catch2/catch_test_macros.hpp>
#include <array>
#include "game/training_campaign.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"

TEST_CASE("Campaign training reproduces real scripted decisions and forks state", "[campaigntrain]") {
    const auto map=game::MapLoader::from_file(std::string(GAME_DATA_DIR)+"/maps/pool/gen_01001000.json");
    const auto stats=game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
    game::TrainingCampaign campaign(map,stats,2);
    game::DemoBattle reference(map,stats,2);
    game::DefenderMacro macro(map);
    std::vector<rts::Command> commands;
    std::vector<game::UnitOrder> orders;
    for(int tick=0;tick<1500;++tick) {
        if(tick%20==0) {
            commands.clear();orders.clear();macro.decide(reference.world(),commands,orders);
            reference.submit_defender(commands.data(),commands.size());
        }
        for(const auto& order:orders) {
            if(order.garrison)reference.issue_garrison_order(order.ids,order.target);
            else reference.issue_move_order(order.ids,order.target);
        }
        reference.update(1);
        campaign.advance_scripted(1);
        if(tick%100==0) REQUIRE(campaign.world().state_hash()==reference.world().state_hash());
    }
    auto fork=campaign.fork();
    const auto before=campaign.world().state_hash();
    fork.advance_scripted(17);
    REQUIRE(campaign.world().state_hash()==before);
    campaign.advance_scripted(17);
    REQUIRE(campaign.world().state_hash()==fork.world().state_hash());
    auto transition=campaign.advance_scripted(10000);
    REQUIRE(transition.ticks<10000);
    REQUIRE((transition.wave_advanced || transition.defeated));
    REQUIRE(transition.wave_advanced);
    REQUIRE(campaign.world().wave()==2);
    REQUIRE(campaign.world().now()>1500);
    REQUIRE_THROWS(campaign.advance_scripted(0));
}

TEST_CASE("Campaign branches accept player macro commands independently", "[campaigntrain]") {
    const auto map=game::MapLoader::from_file(std::string(GAME_DATA_DIR)+"/maps/pool/gen_01001000.json");
    const auto stats=game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
    game::TrainingCampaign idle(map,stats,9);
    idle.advance(20,{});
    auto summoned=idle.fork();
    std::array<rts::Command,1> commands;
    commands[0].kind=rts::CommandKind::Summon;
    summoned.advance(1,commands);
    idle.advance(1,{});
    REQUIRE(idle.world().now()==summoned.world().now());
    REQUIRE(idle.world().phase()==rts::WavePhase::Build);
    REQUIRE(summoned.world().phase()==rts::WavePhase::Assault);
    REQUIRE(idle.world().state_hash()!=summoned.world().state_hash());
}
