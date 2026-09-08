#include <catch2/catch_test_macros.hpp>
#include "game/macro_observation.hpp"
#include "game/player_input.hpp"
#include "game/stats_loader.hpp"

namespace {
rts::WorldInit arena() {
    rts::WorldInit init;
    init.width=40;init.height=40;init.terrain.assign(1600,rts::Terrain::Plain);
    init.keep={2,2};init.buildings.push_back({rts::BldType::Keep,{2,2},2400,2400});
    init.stats=game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
    init.seed=3;init.nominal_level=1;init.map_id="macro-observation";
    init.units.push_back({rts::UnitType::Archer,{3.5f,3.5f},1,240,240});
    return init;
}
}
TEST_CASE("Macro observation excludes hidden enemies including their counts and levels", "[macroobs]") {
    rts::World a(arena()),b(arena());
    b.spawn_unit(rts::UnitType::Ghoul,{35.5f,35.5f},99,9000,9000);
    a.advance(1);b.advance(1);
    const auto x=game::pack_macro_observation(a.view(rts::Side::Defender));
    const auto y=game::pack_macro_observation(b.view(rts::Side::Defender));
    REQUIRE(x.cells==y.cells);REQUIRE(x.global==y.global);
    REQUIRE(x.cells.size()==game::kMacroGrid*game::kMacroGrid*game::kMacroChannels);
    REQUIRE(game::macro_cell_names().size()==game::kMacroChannels);
    REQUIRE(game::macro_global_names().size()==game::kMacroGlobals);
    b.spawn_unit(rts::UnitType::Ghoul,{4.5f,4.5f},1,400,400);
    b.advance(1);
    REQUIRE(game::pack_macro_observation(b.view(rts::Side::Defender)).cells!=x.cells);
    REQUIRE_THROWS(game::pack_macro_observation(a.view(rts::Side::Attacker)));
}
TEST_CASE("Macro command mask rejects unaffordable, occupied and wrong-side choices", "[macroobs]") {
    rts::World w(arena());
    const auto view=w.view(rts::Side::Defender);
    const auto build=game::build_command(rts::BldType::Tower,{8,8},40);
    w.set_stock(rts::Resource::Stone,0);w.set_stock(rts::Resource::Wood,0);
    REQUIRE_FALSE(game::macro_command_legal(view,build,false));
    w.set_stock(rts::Resource::Stone,10000);w.set_stock(rts::Resource::Wood,10000);
    REQUIRE(game::macro_command_legal(view,build,false));
    REQUIRE_FALSE(game::macro_command_legal(view,game::build_command(rts::BldType::Tower,{2,2},40),false));
    REQUIRE_FALSE(game::macro_command_legal(view,game::repair_command({2,2},40),false));
    REQUIRE_FALSE(game::macro_command_legal(view,game::train_command(rts::UnitType::Ghoul,1,{2,2},40),false));
    REQUIRE_FALSE(game::macro_command_legal(view,game::train_command(rts::UnitType::Archer,2,{2,2},40),false));
    w.set_stock(rts::Resource::Gold,10000);
    REQUIRE(game::macro_command_legal(view,game::train_command(rts::UnitType::Archer,1,{2,2},40),false));
    rts::Command summon;summon.kind=rts::CommandKind::Summon;
    REQUIRE_FALSE(game::macro_command_legal(view,summon,false));
    REQUIRE(game::macro_command_legal(view,summon,true));
    w.begin_assault();REQUIRE_FALSE(game::macro_command_legal(view,summon,true));
}
