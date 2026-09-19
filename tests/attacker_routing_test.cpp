#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <iostream>
#include "game/attacker_knowledge.hpp"
#include "game/demo_driver.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/combat_math.hpp"

namespace {
rts::StatsTable route_stats() {
    return game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
}
rts::WorldInit arena() {
    rts::WorldInit init;
    init.width=32; init.height=16; init.terrain.assign(512,rts::Terrain::Plain);
    init.keep={29,8}; init.stats=route_stats(); init.seed=17;
    init.buildings.push_back({rts::BldType::Keep,init.keep,100000,100000});
    return init;
}
const rts::FlowBuilding* memory_at(const game::AttackerKnowledge& k,rts::GridPos pos) {
    for(const auto& b:k.buildings()) if(b.pos==pos) return &b;
    return nullptr;
}
}

TEST_CASE("Combat squads share persistent building observations but a Wraith cannot leak them", "[battlefix][routing]") {
    rts::World w(arena());
    const auto target=w.place_bld(rts::BldType::Tower,{8,8},400,1000);
    const auto hidden=w.place_bld(rts::BldType::Barrack,{25,3},1000,1000);
    game::AttackerKnowledge known;
    const auto spy=w.spawn_unit(rts::UnitType::Wraith,{8.5f,8.5f},1,1000,1000);
    w.advance(1);known.observe(w.view(rts::Side::Attacker));
    REQUIRE(memory_at(known,{8,8})==nullptr);
    const auto scout=w.spawn_unit(rts::UnitType::Ghoul,{7.5f,8.5f},1,1000,1000,3);
    w.spawn_unit(rts::UnitType::Ghoul,{1.5f,1.5f},1,1000,1000,4);
    known.observe(w.view(rts::Side::Attacker));
    REQUIRE(memory_at(known,{8,8})!=nullptr);
    REQUIRE(memory_at(known,{8,8})->hp==400);
    REQUIRE(memory_at(known,{25,3})==nullptr);
    w.kill_unit(scout);w.kill_unit(spy);
    const auto saved=known.buildings();
    w.destroy_bld(target);
    REQUIRE(w.place_bld(rts::BldType::Flak,{8,8},900,1000).valid());
    w.destroy_bld(hidden);
    known.observe(w.view(rts::Side::Attacker));
    REQUIRE(known.buildings()==saved); // No slot-based or off-screen updates.
    w.spawn_unit(rts::UnitType::Ghoul,{7.5f,8.5f},1,1000,1000,5);
    known.observe(w.view(rts::Side::Attacker));
    REQUIRE(memory_at(known,{8,8})->type==rts::BldType::Flak);
    REQUIRE(memory_at(known,{8,8})->hp==900);
    w.destroy_bld(w.bld_at({8,8}));
    known.observe(w.view(rts::Side::Attacker));
    REQUIRE(memory_at(known,{8,8})==nullptr);
}

TEST_CASE("Global reconnaissance is a snapshot and respects later hidden rebuilding", "[battlefix][routing]") {
    rts::World w(arena());
    const auto wall=w.place_bld(rts::BldType::Wall,{15,8},100,1200);
    game::AttackerKnowledge known;
    known.observe(w.view(rts::Side::Attacker),true);
    REQUIRE(memory_at(known,{15,8})->hp==100);
    w.destroy_bld(wall);
    w.place_bld(rts::BldType::Wall,{15,8},1200,1200);
    w.place_bld(rts::BldType::Tower,{16,7},800,800);
    known.observe(w.view(rts::Side::Attacker));
    REQUIRE(memory_at(known,{15,8})->hp==100);
    REQUIRE(memory_at(known,{16,7})==nullptr);
    known.observe(w.view(rts::Side::Attacker),true);
    REQUIRE(memory_at(known,{15,8})->hp==1200);
    REQUIRE(memory_at(known,{16,7})!=nullptr);
}

TEST_CASE("Shared ground vision obeys the same terrain occlusion as simulation vision", "[battlefix][routing]") {
    auto init=arena();
    init.terrain[8*32+6]=rts::Terrain::Forest;
    rts::World w(init);
    w.place_bld(rts::BldType::Tower,{7,8},1000,1000);
    w.spawn_unit(rts::UnitType::Ghoul,{5.5f,8.5f},1,1000,1000);
    game::AttackerKnowledge known;
    known.observe(w.view(rts::Side::Attacker));
    REQUIRE(memory_at(known,{7,8})==nullptr);
    w.spawn_unit(rts::UnitType::Phoenix,{5.5f,8.5f},1,1000,1000);
    known.observe(w.view(rts::Side::Attacker));
    REQUIRE(memory_at(known,{7,8})!=nullptr);
}

TEST_CASE("Ram breach estimates match actual hit timing at real levels", "[flow][routing]") {
    for(const int level:{1,8,30,99}) for(const int windup:{0,30,70}) {
        auto init=arena();
        auto& ram=init.stats.unit[static_cast<std::size_t>(rts::UnitType::Ram)];
        ram.windup_ticks=windup;ram.cooldown_ticks=60;
        rts::World w(init);
        const auto wall=w.place_bld(rts::BldType::Wall,{8,8},1200,1200);
        w.spawn_unit(rts::UnitType::Ram,{7.6f,8.5f},level,10000,10000);
        const auto attack=rts::UnitAction::AtkWall;
        w.submit_actions(rts::Side::Attacker,&attack,1);
        int elapsed=0;
        while(w.alive(wall) && elapsed<5000) {w.advance(1);++elapsed;}
        CAPTURE(level,windup,elapsed);
        REQUIRE_FALSE(w.alive(wall));
        REQUIRE(rts::estimate_breach_ticks(init.stats,rts::UnitType::Ram,level,1200)==static_cast<float>(elapsed));
    }
}

TEST_CASE("Routing trades exposure along a detour against exposure while breaching", "[flow][routing]") {
    auto init=arena();
    auto& ghoul=init.stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)];
    ghoul.damage=10;ghoul.vs_structure_permille=1000;ghoul.speed=0.25f;
    ghoul.windup_ticks=2;ghoul.cooldown_ticks=8;ghoul.max_hp=100;
    auto& tower=init.stats.bld[static_cast<std::size_t>(rts::BldType::Tower)];
    tower.damage=40;tower.range=4;tower.cooldown_ticks=20;tower.windup_ticks=2;
    rts::World w(init);
    std::vector<rts::FlowBuilding> known;
    // A tall wall with a short northern detour; extremely thick other segments
    // isolate the breach at y=8 and the gap at y=3.
    for(int y=4;y<16;++y) known.push_back({{12,static_cast<std::int16_t>(y)},rts::BldType::Wall,
                                         y==8?80:1000000,1,true,0});
    const rts::GridPos goal[]={{17,8}};const rts::GridPos start{7,8};
    const auto quiet=rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Ghoul,1,goal,known);
    REQUIRE(quiet.step_of(start)!=rts::UnitAction::MoveSE);
    known.push_back({{12,2},rts::BldType::Tower,1000000,1,true,0});
    const auto risky=rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Ghoul,1,goal,known);
    REQUIRE(risky.step_of(start)==rts::UnitAction::MoveSE);
    REQUIRE(risky.travel_ticks_at(start)>quiet.travel_ticks_at(start));
    // Moving the same tower to the breach makes standing there expensive too.
    known.back().pos={12,10};
    const auto covered=rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Ghoul,1,goal,known);
    REQUIRE(covered.step_of(start)!=rts::UnitAction::MoveSE);
    REQUIRE(covered.travel_ticks_at(start)<risky.travel_ticks_at(start));
    std::cout<<"route_probe: quiet="<<quiet.travel_ticks_at(start)
             <<" gap_fire="<<risky.travel_ticks_at(start)<<" breach_fire="<<covered.travel_ticks_at(start)<<'\n';
}

TEST_CASE("Hidden buildings never change a route and observed repair changes breach cost", "[flow][routing]") {
    auto init=arena();rts::World w(init);
    const rts::GridPos goal[]={{25,8}};
    std::vector<rts::FlowBuilding> known={{{12,8},rts::BldType::Wall,100,1,true,0}};
    const auto make=[&](){return rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Ram,99,goal,known);};
    const auto before=make();
    w.place_bld(rts::BldType::Tower,{10,8},1000,1000);
    w.place_bld(rts::BldType::Wall,{12,8},10000,10000);
    const auto unseen=make();
    for(int y=0;y<16;++y) for(int x=0;x<32;++x) {
        const rts::GridPos p{static_cast<std::int16_t>(x),static_cast<std::int16_t>(y)};
        REQUIRE(before.cost_at(p)==unseen.cost_at(p));
        REQUIRE(before.step_of(p)==unseen.step_of(p));
    }
    known[0].hp=10000;
    const auto repaired=make();
    REQUIRE(repaired.cost_at({11,8})>before.cost_at({11,8}));
}

TEST_CASE("Gameplay routing no longer prices a level 99 ram as level 8", "[flow][routing]") {
    rts::World w(arena());
    const rts::GridPos goal[]={{20,8}};
    std::vector<rts::FlowBuilding> known;
    for(int y=0;y<16;++y) known.push_back({{12,static_cast<std::int16_t>(y)},rts::BldType::Wall,1200,1,true,0});
    const auto low=rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Ram,8,goal,known);
    const auto high=rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Ram,99,goal,known);
    const auto expected=rts::estimate_breach_ticks(w.stats(),rts::UnitType::Ram,8,1200)-
        rts::estimate_breach_ticks(w.stats(),rts::UnitType::Ram,99,1200);
    REQUIRE(expected>0);
    REQUIRE(low.travel_ticks_at({7,8})-high.travel_ticks_at({7,8})==Catch::Approx(expected));
}

TEST_CASE("Only completed buildings with matching air-ground targeting contribute route fire", "[flow][routing]") {
    rts::World w(arena());
    const rts::GridPos goal[]={{10,8}};
    std::vector<rts::FlowBuilding> known={{{8,9},rts::BldType::Flak,1000,20,true,0}};
    const auto ground=rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Ram,10,goal,known);
    REQUIRE(ground.expected_damage_at({7,8})==0);
    const auto air=rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Phoenix,10,goal,known);
    REQUIRE(air.expected_damage_at({7,8})>0);
    known[0].built=false;
    const auto unbuilt=rts::FlowField::compute_known(w.view(rts::Side::Attacker),rts::UnitType::Phoenix,10,goal,known);
    REQUIRE(unbuilt.expected_damage_at({7,8})==0);
}

TEST_CASE("Exposure-aware routing reduces actual tower damage on a defended detour", "[flow][routing]") {
    struct Outcome {int ticks;std::int64_t damage;bool breached;};
    const auto run=[](bool aware) {
        auto init=arena();
        auto& ghoul=init.stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)];
        ghoul.damage=10;ghoul.vs_structure_permille=1000;ghoul.speed=0.25f;
        ghoul.windup_ticks=2;ghoul.cooldown_ticks=8;ghoul.max_hp=100;
        auto& tower=init.stats.bld[static_cast<std::size_t>(rts::BldType::Tower)];
        tower.damage=40;tower.range=4;tower.cooldown_ticks=20;tower.windup_ticks=2;
        tower.proj_speed=0; // Isolate routing; no moving-target projectile misses.
        rts::World w(init);
        for(int y=4;y<16;++y) {
            const auto hp=y==8?80:1000000;
            w.place_bld(rts::BldType::Wall,{12,static_cast<std::int16_t>(y)},hp,hp);
        }
        const auto wall=w.bld_at({12,8});
        w.place_bld(rts::BldType::Tower,{12,2},1000000,1000000);
        const auto unit=w.spawn_unit(rts::UnitType::Ghoul,{7.5f,8.5f},1,10000,10000);
        game::AttackerKnowledge knowledge;
        const rts::GridPos goal[]={{17,8}};
        int ticks=0;
        for(;ticks<1500 && w.alive(unit) && rts::grid_of(w.unit_pos(unit))!=goal[0];++ticks) {
            knowledge.observe(w.view(rts::Side::Attacker),ticks==0);
            const auto field=aware ? rts::FlowField::compute_known(w.view(rts::Side::Attacker),
                rts::UnitType::Ghoul,1,goal,knowledge.buildings()) :
                rts::FlowField::compute(w.view(rts::Side::Attacker),rts::UnitType::Ghoul,0,goal);
            const auto action=field.step_of(rts::grid_of(w.unit_pos(unit)));
            w.submit_actions(rts::Side::Attacker,&action,1);w.advance(1);
        }
        REQUIRE(w.alive(unit));
        REQUIRE(rts::grid_of(w.unit_pos(unit))==goal[0]);
        return Outcome{ticks,10000-w.unit_hp(unit),!w.alive(wall)};
    };
    const auto old=run(false), updated=run(true);
    CAPTURE(old.ticks,old.damage,updated.ticks,updated.damage);
    REQUIRE_FALSE(old.breached);
    REQUIRE(updated.breached);
    REQUIRE(updated.damage<old.damage);
    std::cout<<"engine_route_probe: old_ticks="<<old.ticks<<" old_damage="<<old.damage
             <<" new_ticks="<<updated.ticks<<" new_damage="<<updated.damage<<'\n';
}

TEST_CASE("A delivered Wraith report unlocks one global snapshot in the actual battle", "[battlefix][routing]") {
    auto st=route_stats();for(auto& u:st.unit) {u.speed=0;u.damage=0;}
    for(auto& b:st.bld) b.damage=0;
    const auto map=game::MapLoader::from_file(std::string(GAME_DATA_DIR)+"/maps/pool/gen_01001000.json");
    game::DemoBattle battle(map,st,17);
    auto& w=const_cast<rts::World&>(battle.world());
    std::vector<rts::UnitId> ids;w.enumerate_units(rts::Side::Attacker,ids);
    for(auto id:ids) w.kill_unit(id);
    const rts::GridPos far{150,150};
    const auto target=w.place_bld(rts::BldType::Tower,far,800,800);REQUIRE(target.valid());
    const auto spy=w.spawn_unit(rts::UnitType::Wraith,rts::center_of(w.keep_pos()),1,10000,10000);
    battle.update(12);
    REQUIRE(battle.recon_transmitting());
    REQUIRE(memory_at(battle.attacker_knowledge(),far)==nullptr);
    SECTION("interception withholds all remote knowledge") {
        w.kill_unit(spy);battle.update(80);
        REQUIRE_FALSE(battle.wave_scouted());
        REQUIRE(memory_at(battle.attacker_knowledge(),far)==nullptr);
    }
    SECTION("completion delivers once, not continuous map access") {
        battle.update(80);
        REQUIRE(battle.wave_scouted());
        REQUIRE(memory_at(battle.attacker_knowledge(),far)!=nullptr);
        w.destroy_bld(target);
        w.place_bld(rts::BldType::Barrack,far,2000,2000);
        battle.update(24);
        REQUIRE(memory_at(battle.attacker_knowledge(),far)->type==rts::BldType::Tower);
        w.spawn_unit(rts::UnitType::Phoenix,rts::center_of(far),1,10000,10000);
        battle.update(16);
        REQUIRE(memory_at(battle.attacker_knowledge(),far)->type==rts::BldType::Barrack);
    }
}
