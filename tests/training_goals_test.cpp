#include <catch2/catch_test_macros.hpp>
#include "game/training_goals.hpp"

TEST_CASE("Economy training targets require remembered harvesting buildings", "[traininggoals]") {
    rts::WorldInit init;
    init.width=24;init.height=24;init.terrain.assign(24*24,rts::Terrain::Plain);
    init.keep={2,2};
    init.buildings.push_back({rts::BldType::Keep,{2,2},200,200});
    init.buildings.push_back({rts::BldType::Quarry,{18,18},100,100});
    init.buildings.push_back({rts::BldType::Tower,{18,20},100,100});
    init.resources.push_back({{18,18},rts::Resource::Stone});
    init.resources.push_back({{18,19},rts::Resource::Wood}); // Empty public site.
    init.resources.push_back({{18,20},rts::Resource::Stone}); // Wrong building type.
    init.stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)].vision=5;
    rts::World w(std::move(init));
    std::vector<rts::UnitId> leaders;
    auto goals=game::known_economy_training_goals(w.view(rts::Side::Attacker),leaders);
    REQUIRE(goals.economy.empty());
    w.spawn_unit(rts::UnitType::Ghoul,{19.5f,18.5f},1,100,100,0);
    w.advance(1);
    w.enumerate_squads(rts::Side::Attacker,leaders);
    const auto before=w.state_hash();
    goals=game::known_economy_training_goals(w.view(rts::Side::Attacker),leaders);
    REQUIRE(goals.economy==std::vector<rts::GridPos>{{18,18}});
    REQUIRE(goals.groups==std::vector<std::uint8_t>{1});
    REQUIRE(w.state_hash()==before);
    REQUIRE_THROWS_AS(game::known_economy_training_goals(w.view(rts::Side::Defender),leaders),rts::ContractError);
    w.kill_unit(leaders[0]);
    w.advance(1);
    REQUIRE(w.fog(rts::Side::Attacker).at(18,18)==rts::Vis::Remembered);
    goals=game::known_economy_training_goals(w.view(rts::Side::Attacker),{});
    REQUIRE(goals.economy==std::vector<rts::GridPos>{{18,18}});
    w.destroy_bld(w.bld_at({18,18}));
    // Destruction outside vision must not magically erase remembered intel.
    goals=game::known_economy_training_goals(w.view(rts::Side::Attacker),{});
    REQUIRE(goals.economy==std::vector<rts::GridPos>{{18,18}});
    w.spawn_unit(rts::UnitType::Ghoul,{19.5f,18.5f},1,100,100,0);
    w.advance(1);
    w.enumerate_squads(rts::Side::Attacker,leaders);
    goals=game::known_economy_training_goals(w.view(rts::Side::Attacker),leaders);
    REQUIRE(goals.economy.empty());
    REQUIRE(goals.groups==std::vector<std::uint8_t>{0});
}
