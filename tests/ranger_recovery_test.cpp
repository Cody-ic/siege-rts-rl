#include <cmath>
#include <chrono>
#include <cstdio>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "game/defender_script.hpp"
#include "game/stats_loader.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

namespace {
rts::WorldInit arena() {
    rts::WorldInit init;
    init.width=32;init.height=24;init.terrain.assign(32*24,rts::Terrain::Plain);
    init.keep={5,12};init.buildings.push_back({rts::BldType::Keep,init.keep,1200,1200});
    init.stats=game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Keep)].damage=0;
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Keep)].income_amount=0;
    return init;
}
void run(rts::World& w,game::DefenderScript& script,int ticks,bool fight=true) {
    std::vector<rts::UnitId> ids;std::vector<rts::UnitAction> actions;std::vector<std::uint16_t> wishes;
    for (int t=0;t<ticks;++t) {
        w.enumerate_units(rts::Side::Defender,ids);
        if (t%8==0) {
            script.decide(w.view(rts::Side::Defender),ids,actions,wishes);
            w.submit_actions(rts::Side::Defender,actions.data(),actions.size());
            w.submit_garrison_wishes(rts::Side::Defender,wishes.data(),wishes.size());
        } else if (script.refresh_scout_navigation(w.view(rts::Side::Defender),ids,actions))
            w.submit_actions(rts::Side::Defender,actions.data(),actions.size());
        w.enumerate_units(rts::Side::Attacker,ids);actions.clear();
        for (auto id:ids) actions.push_back(fight && (w.action_mask(id)&(1u<<static_cast<unsigned>(rts::UnitAction::AtkNear)))?
            rts::UnitAction::AtkNear:rts::UnitAction::Stop);
        w.submit_actions(rts::Side::Attacker,actions.data(),actions.size());w.advance(1);
    }
}
}

TEST_CASE("Ranger holds a player post across waves and does not transfer it to a reused slot", "[script][ranger]") {
    rts::World w(arena());const auto rider=w.spawn_unit(rts::UnitType::Ranger,{5.5f,10.5f},1,320,320);
    game::DefenderScript script({},3);script.issue_move_order({&rider,1},{20,10});
    run(w,script,400);
    REQUIRE(std::abs(w.unit_pos(rider).x-20.5f)<1.6f);
    REQUIRE(script.ranger_post(rider)->anchor==rts::GridPos{20,10});
    w.begin_next_wave(2);run(w,script,200);
    REQUIRE(std::abs(w.unit_pos(rider).x-20.5f)<1.6f);
    w.kill_unit(rider);const auto replacement=w.spawn_unit(rts::UnitType::Ranger,{5.5f,10.5f},1,320,320);
    REQUIRE(replacement.index()==rider.index());run(w,script,8);
    REQUIRE(script.ranger_post(replacement)->anchor!=rts::GridPos{20,10});
}

TEST_CASE("Ranger intercepts a weak exposed enemy, then returns instead of pursuing indefinitely", "[script][ranger]") {
    rts::World w(arena());const auto rider=w.spawn_unit(rts::UnitType::Ranger,{12.5f,12.5f},1,320,320);
    const auto target=w.spawn_unit(rts::UnitType::Shade,{16.5f,12.5f},1,30,120);
    game::DefenderScript script({},3);script.issue_move_order({&rider,1},{12,12});run(w,script,260);
    REQUIRE_FALSE(w.alive(target));REQUIRE(w.alive(rider));
    REQUIRE(std::abs(w.unit_pos(rider).x-12.5f)<1.6f);
    const auto distant=w.spawn_unit(rts::UnitType::Shade,{25.5f,12.5f},1,30,120);
    run(w,script,200);REQUIRE(w.unit_hp(distant)==30);
    REQUIRE(std::abs(w.unit_pos(rider).x-12.5f)<1.6f);
}

TEST_CASE("Manual ranger deployment cannot force a charge into high level knights", "[script][ranger]") {
    auto init=arena();rts::World w(init);
    const auto rider=w.spawn_unit(rts::UnitType::Ranger,{10.5f,12.5f},1,320,320);
    w.spawn_unit(rts::UnitType::Knight,{15.5f,12.5f},40,1800,1800);
    game::DefenderScript script({},3);script.issue_move_order({&rider,1},{15,12});run(w,script,160);
    REQUIRE(w.alive(rider));REQUIRE(w.unit_hp(rider)==320);
    REQUIRE(script.ranger_post(rider)->anchor==rts::GridPos{15,12});
}

TEST_CASE("Ranger routes through narrow gates and reports sealed destinations", "[script][ranger]") {
    auto init=arena();
    for (int y=0;y<24;++y) init.buildings.push_back({y==5?rts::BldType::Gate:rts::BldType::Wall,{15,static_cast<std::int16_t>(y)},1200,1200});
    SECTION("Offset gate remains traversable with eight tick decisions") {
        rts::World w(init);const auto rider=w.spawn_unit(rts::UnitType::Ranger,{12.5f,12.5f},1,320,320);
        game::DefenderScript script({},3);script.issue_move_order({&rider,1},{19,12});run(w,script,400);
        REQUIRE(w.unit_pos(rider).x>18.0f);
    }
    SECTION("Sealed wall is not treated as a straight-line path") {
        init.buildings[6].type=rts::BldType::Wall;
        rts::World w(init);const auto rider=w.spawn_unit(rts::UnitType::Ranger,{12.5f,12.5f},1,320,320);
        game::DefenderScript script({},3);script.issue_move_order({&rider,1},{19,12});run(w,script,80);
        REQUIRE(w.unit_pos(rider).x<15.0f);
        REQUIRE(script.ranger_post(rider)->mode==game::RangerMode::Blocked);
    }
}

TEST_CASE("Retreat avoids enemies between the rider and keep", "[script][ranger]") {
    rts::World w(arena());const auto rider=w.spawn_unit(rts::UnitType::Ranger,{13.5f,12.5f},1,80,320);
    w.spawn_unit(rts::UnitType::Knight,{11.5f,12.5f},30,1200,1200);
    game::DefenderScript script({},3);run(w,script,64,false);
    REQUIRE(w.alive(rider));
    const auto p=w.unit_pos(rider);
    REQUIRE((p.x-11.5f)*(p.x-11.5f)+(p.y-12.5f)*(p.y-12.5f)>9.0f);
}

TEST_CASE("Hidden attackers cannot change ranger decisions", "[script][ranger]") {
    auto init=arena();
    for (auto& s:init.stats.unit) s.vision=1;
    for (auto& s:init.stats.bld) s.vision=1;
    rts::World a(init),b(init);
    const auto ra=a.spawn_unit(rts::UnitType::Ranger,{12.5f,12.5f},1,320,320);
    const auto rb=b.spawn_unit(rts::UnitType::Ranger,{12.5f,12.5f},1,320,320);
    b.spawn_unit(rts::UnitType::Knight,{16.5f,12.5f},50,2000,2000);
    game::DefenderScript sa({},4),sb({},4);sa.issue_move_order({&ra,1},{12,12});sb.issue_move_order({&rb,1},{12,12});
    run(a,sa,16,false);run(b,sb,16,false);
    REQUIRE(a.unit_pos(ra).x==b.unit_pos(rb).x);REQUIRE(a.unit_pos(ra).y==b.unit_pos(rb).y);
}

TEST_CASE("City lanes allow an intercept without requiring an open charge runway", "[script][ranger]") {
    auto init=arena();
    for (int x=9;x<=20;++x) {
        init.buildings.push_back({rts::BldType::Wall,{static_cast<std::int16_t>(x),10},1200,1200});
        init.buildings.push_back({rts::BldType::Wall,{static_cast<std::int16_t>(x),14},1200,1200});
    }
    rts::World w(init);const auto rider=w.spawn_unit(rts::UnitType::Ranger,{11.5f,12.5f},1,320,320);
    const auto enemy=w.spawn_unit(rts::UnitType::Shade,{16.5f,12.5f},1,70,120);
    game::DefenderScript script({},7);script.issue_move_order({&rider,1},{11,12});run(w,script,300);
    REQUIRE(w.alive(rider));REQUIRE_FALSE(w.alive(enemy));
    REQUIRE(std::abs(w.unit_pos(rider).x-11.5f)<1.6f);
}

TEST_CASE("Paid recovery requires quiet, a usable barracks and gold", "[econ][recovery]") {
    auto init=arena();init.buildings.push_back({rts::BldType::Barrack,{10,10},400,400});
    const auto prepare=[&]() {rts::World w(init);w.set_stock(rts::Resource::Gold,100);return w;};
    SECTION("Fractional charges accumulate, healing stops at max HP, multiple barracks do not multiply it") {
        auto w=prepare();const auto rider=w.spawn_unit(rts::UnitType::Ranger,{11.5f,10.5f},1,160,320);
        w.place_bld(rts::BldType::Barrack,{11,9},400,400);
        w.advance(100);REQUIRE(w.unit_hp(rider)==160);w.advance(1);
        REQUIRE(w.unit_hp(rider)==166);REQUIRE(w.stock(rts::Resource::Gold)==99);
        w.advance(600);REQUIRE(w.unit_hp(rider)==320);
        REQUIRE(w.stock(rts::Resource::Gold)==77); // ceil(160/320 * 45) = 23 gold, not one gold every tick.
        w.advance(100);REQUIRE(w.stock(rts::Resource::Gold)==77);
    }
    SECTION("No gold does not heal") {
        auto w=prepare();w.set_stock(rts::Resource::Gold,0);
        const auto r=w.spawn_unit(rts::UnitType::Ranger,{11.5f,10.5f},1,100,320);w.advance(200);
        REQUIRE(w.unit_hp(r)==100);
    }
    SECTION("Walls block treatment") {
        auto w=prepare();w.place_bld(rts::BldType::Wall,{11,10},1200,1200);
        const auto r=w.spawn_unit(rts::UnitType::Ranger,{12.5f,10.5f},1,100,320);w.advance(200);
        REQUIRE(w.unit_hp(r)==100);
    }
    SECTION("Visible threats suppress healing even while standing still") {
        auto w=prepare();const auto r=w.spawn_unit(rts::UnitType::Ranger,{11.5f,10.5f},1,100,320);
        w.spawn_unit(rts::UnitType::Knight,{13.5f,10.5f},1,200,200);w.advance(200);
        REQUIRE(w.unit_hp(r)==100);
    }
    SECTION("A construction site cannot act as a hospital") {
        auto w=prepare();w.destroy_bld(w.bld_at({10,10}));
        w.place_bld(rts::BldType::Barrack,{10,10},1,400,500);
        const auto r=w.spawn_unit(rts::UnitType::Ranger,{11.5f,10.5f},1,100,320);w.advance(200);
        REQUIRE(w.unit_hp(r)==100);
    }
    SECTION("A melee attack resets the quiet period even if the target does not retaliate") {
        auto w=prepare();const auto r=w.spawn_unit(rts::UnitType::Ranger,{11.5f,10.5f},1,100,320);
        w.spawn_unit(rts::UnitType::Ghoul,{12.5f,10.5f},1,10000,10000);
        const rts::UnitAction action=rts::UnitAction::AtkNear;
        w.submit_actions(rts::Side::Defender,&action,1);w.advance(140);
        REQUIRE(w.unit_hp(r)==100);
        std::vector<rts::UnitId> enemies;w.enumerate_units(rts::Side::Attacker,enemies);w.kill_unit(enemies[0]);
        const rts::UnitAction stop=rts::UnitAction::Stop;w.submit_actions(rts::Side::Defender,&stop,1);
        w.advance(60);REQUIRE(w.unit_hp(r)==100);
        w.advance(80);REQUIRE(w.unit_hp(r)>100);
    }
}

TEST_CASE("Wounded ranger reaches a safe barracks, recovers and resumes its post", "[script][ranger][recovery]") {
    auto init=arena();init.buildings.push_back({rts::BldType::Barrack,{8,12},400,400});
    rts::World w(init);w.set_stock(rts::Resource::Gold,100);
    const auto rider=w.spawn_unit(rts::UnitType::Ranger,{16.5f,12.5f},1,90,320);
    game::DefenderScript script({},3);script.issue_move_order({&rider,1},{16,12});run(w,script,1000);
    REQUIRE(w.unit_hp(rider)>=256);
    REQUIRE(std::abs(w.unit_pos(rider).x-16.5f)<1.6f);
    REQUIRE(w.stock(rts::Resource::Gold)<100);
}

TEST_CASE("Wounded ranger never crosses a lethal choke to reach a safe barracks", "[script][ranger]") {
    auto init=arena();
    for (int y=0;y<24;++y) for (int x=0;x<32;++x)
        if (y!=12) init.terrain[static_cast<std::size_t>(y*32+x)]=rts::Terrain::Rock;
    init.buildings.push_back({rts::BldType::Barrack,{8,12},400,400});
    rts::World w(init);w.set_stock(rts::Resource::Gold,100);
    const auto rider=w.spawn_unit(rts::UnitType::Ranger,{17.5f,12.5f},1,80,320);
    w.spawn_unit(rts::UnitType::Knight,{13.5f,12.5f},40,1800,1800);
    game::DefenderScript script({},3);script.issue_move_order({&rider,1},{17,12});run(w,script,160);
    REQUIRE(w.alive(rider));REQUIRE(w.unit_hp(rider)==80);REQUIRE(w.unit_pos(rider).x>16.0f);
}

// Explicit diagnostic, excluded from normal CTest. Reports Release wall time;
// no hardware-dependent speed threshold masquerades as a gameplay assertion.
TEST_CASE("Sixty riders on a 170 cell map", "[.rangerbench]") {
    auto init=arena();init.width=170;init.height=170;init.terrain.assign(170*170,rts::Terrain::Plain);
    init.keep={85,85};init.buildings[0].pos=init.keep;
    rts::World w(init);game::DefenderScript script({},7);
    for (int i=0;i<60;++i) {
        const auto id=w.spawn_unit(rts::UnitType::Ranger,{float(75+i%10)+0.5f,float(75+i/10)+0.5f},20,1000,1000);
        const rts::GridPos post{static_cast<std::int16_t>(85+i%10),static_cast<std::int16_t>(90+i/10)};
        script.issue_move_order({&id,1},post);
    }
    const auto start=std::chrono::steady_clock::now();run(w,script,200,false);
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("RANGER_BENCH riders=60 map=170 ticks=200 seconds=%.3f ms_per_tick=%.3f\n",seconds,seconds*5);
    REQUIRE(w.live_unit_count(rts::Side::Defender)==60);
}
