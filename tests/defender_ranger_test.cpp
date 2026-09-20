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
    REQUIRE(script.defender_post(rider)->anchor==rts::GridPos{20,10});
    w.begin_next_wave(2);run(w,script,200);
    REQUIRE(std::abs(w.unit_pos(rider).x-20.5f)<1.6f);
    w.kill_unit(rider);const auto replacement=w.spawn_unit(rts::UnitType::Ranger,{5.5f,10.5f},1,320,320);
    REQUIRE(replacement.index()==rider.index());run(w,script,8);
    REQUIRE(script.defender_post(replacement)->anchor!=rts::GridPos{20,10});
}

TEST_CASE("Ranger intercepts an exposed enemy, then returns instead of pursuing indefinitely", "[script][ranger]") {
    rts::World w(arena());const auto rider=w.spawn_unit(rts::UnitType::Ranger,{12.5f,12.5f},1,320,320);
    const auto target=w.spawn_unit(rts::UnitType::Shade,{16.5f,12.5f},1,30,120);
    game::DefenderScript script({},3);script.issue_move_order({&rider,1},{12,12});run(w,script,260);
    REQUIRE_FALSE(w.alive(target));REQUIRE(w.alive(rider));
    REQUIRE(std::abs(w.unit_pos(rider).x-12.5f)<1.6f);
    const auto distant=w.spawn_unit(rts::UnitType::Shade,{25.5f,12.5f},1,30,120);
    run(w,script,200);REQUIRE(w.unit_hp(distant)==30);
    REQUIRE(std::abs(w.unit_pos(rider).x-12.5f)<1.6f);
}

TEST_CASE("Ranger charges high level knights even when wounded or outnumbered", "[script][ranger]") {
    auto init=arena();
    int hp=320; bool manual=false;
    SECTION("Full health autonomous engagement") {}
    SECTION("Wounded autonomous engagement") {hp=40;}
    SECTION("Manual order is not vetoed by danger") {hp=40;manual=true;}
    rts::World w(init);
    const auto rider=w.spawn_unit(rts::UnitType::Ranger,{10.5f,12.5f},1,hp,320);
    const auto knight=w.spawn_unit(rts::UnitType::Knight,{12.5f,12.5f},40,1800,1800);
    w.spawn_unit(rts::UnitType::Knight,{13.5f,13.5f},40,1800,1800);
    game::DefenderScript script({},3);
    if (manual) script.issue_move_order({&rider,1},{15,12});
    run(w,script,20,false);
    REQUIRE(w.unit_hp(knight)<1800);
    REQUIRE(w.unit_pos(rider).x>10.5f);
    // Enabling retaliation can kill the rider; low HP never grants an escape override.
    run(w,script,200,true);
    REQUIRE_FALSE(w.alive(rider));
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
        REQUIRE(script.defender_post(rider)->mode==game::PostMode::Blocked);
    }
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

TEST_CASE("All building guards leave a deep concave wall instead of oscillating", "[script][guard]") {
    auto type=rts::UnitType::Ranger;
    SECTION("Ranger") {}
    SECTION("Spear") {type=rts::UnitType::Spear;}
    SECTION("Archer") {type=rts::UnitType::Archer;}
    auto init=arena();
    for(int y=7;y<=17;++y) init.buildings.push_back({rts::BldType::Wall,{15,static_cast<std::int16_t>(y)},1200,1200});
    for(int x=8;x<15;++x) {
        init.buildings.push_back({rts::BldType::Wall,{static_cast<std::int16_t>(x),7},1200,1200});
        init.buildings.push_back({rts::BldType::Wall,{static_cast<std::int16_t>(x),17},1200,1200});
    }
    init.buildings.push_back({rts::BldType::Barrack,{22,12},400,400});
    rts::World w(init);
    const auto guard=w.spawn_unit(type,{13.5f,12.5f},1,320,320);
    game::DefenderScript script({},3);
    REQUIRE(script.issue_guard_order(w.view(rts::Side::Defender),{&guard,1},{22,12}));
    run(w,script,1000,false);
    const auto pos=w.unit_pos(guard);
    CAPTURE(type,pos.x,pos.y);
    REQUIRE((pos.x-22.5f)*(pos.x-22.5f)+(pos.y-12.5f)*(pos.y-12.5f)<=2.25f);
    REQUIRE(script.defender_post(guard)->mode==game::PostMode::Holding);
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

TEST_CASE("Barracks never heal resting troops or charge recovery fees", "[econ][ranger]") {
    auto init=arena();init.buildings.push_back({rts::BldType::Barrack,{10,10},400,400});
    rts::World w(init);w.set_stock(rts::Resource::Gold,100);
    const auto rider=w.spawn_unit(rts::UnitType::Ranger,{11.5f,10.5f},1,80,320);
    const auto spear=w.spawn_unit(rts::UnitType::Spear,{11.5f,11.5f},1,100,360);
    const auto archer=w.spawn_unit(rts::UnitType::Archer,{10.5f,11.5f},1,80,240);
    w.advance(1200);
    REQUIRE(w.unit_hp(rider)==80);REQUIRE(w.unit_hp(spear)==100);REQUIRE(w.unit_hp(archer)==80);
    REQUIRE(w.stock(rts::Resource::Gold)==100);
}

TEST_CASE("Wounded ranger keeps its post instead of seeking a barracks", "[script][ranger]") {
    auto init=arena();init.buildings.push_back({rts::BldType::Barrack,{8,12},400,400});
    rts::World w(init);w.set_stock(rts::Resource::Gold,100);
    const auto rider=w.spawn_unit(rts::UnitType::Ranger,{16.5f,12.5f},1,90,320);
    game::DefenderScript script({},3);script.issue_move_order({&rider,1},{16,12});run(w,script,1000);
    REQUIRE(w.unit_hp(rider)==90);
    REQUIRE(w.unit_pos(rider).x==16.5f);REQUIRE(w.unit_pos(rider).y==12.5f);
    REQUIRE(w.stock(rts::Resource::Gold)==100);
    REQUIRE(script.defender_post(rider)->mode==game::PostMode::Holding);
}

TEST_CASE("Combat troops guard an outlying building, intercept and return across waves", "[script][guard]") {
    auto type=rts::UnitType::Ranger;
    SECTION("Ranger") {}
    SECTION("Spear") {type=rts::UnitType::Spear;}
    SECTION("Archer") {type=rts::UnitType::Archer;}
    auto init=arena();init.buildings.push_back({rts::BldType::Barrack,{20,12},400,400});
    rts::World w(init);
    const auto guard=w.spawn_unit(type,{8.5f,12.5f},1,320,320);
    game::DefenderScript script({},3);
    REQUIRE(script.issue_guard_order(w.view(rts::Side::Defender),{&guard,1},{20,12}));
    run(w,script,400,false);
    const auto at_post=[&] {const auto p=w.unit_pos(guard);return (p.x-20.5f)*(p.x-20.5f)+(p.y-12.5f)*(p.y-12.5f)<=2.25f;};
    REQUIRE(at_post());REQUIRE(w.unit_garrison(guard)==rts::kNoSlot);
    const auto enemy=w.spawn_unit(rts::UnitType::Shade,{24.5f,12.5f},1,30,140);
    run(w,script,400,false);
    CAPTURE(type,w.unit_pos(guard).x,w.unit_pos(guard).y);
    REQUIRE_FALSE(w.alive(enemy));REQUIRE(at_post());
    w.begin_next_wave(2);run(w,script,80,false);REQUIRE(at_post());
    REQUIRE(script.defender_post(guard)->building==w.bld_at({20,12}).raw());
    script.issue_move_order({&guard,1},{10,5});run(w,script,400,false);
    if(type==rts::UnitType::Ranger) REQUIRE(script.defender_post(guard)->anchor==rts::GridPos{10,5});
    else REQUIRE_FALSE(script.defender_post(guard));
    REQUIRE_FALSE(at_post());
}

TEST_CASE("Destroyed buildings and dead troops do not transfer guard assignments", "[script][guard]") {
    auto init=arena();init.buildings.push_back({rts::BldType::Barrack,{20,12},400,400});
    rts::World w(init);const auto guard=w.spawn_unit(rts::UnitType::Spear,{19.5f,12.5f},1,360,360);
    game::DefenderScript script({},1);
    REQUIRE(script.issue_guard_order(w.view(rts::Side::Defender),{&guard,1},{20,12}));
    w.destroy_bld(w.bld_at({20,12}));w.place_bld(rts::BldType::Tower,{20,12},300,300);
    run(w,script,16,false);
    REQUIRE(script.defender_post(guard)->building==rts::BldId::kInvalidRaw);
    REQUIRE(script.defender_post(guard)->anchor==rts::GridPos{20,12});
    w.kill_unit(guard);const auto replacement=w.spawn_unit(rts::UnitType::Spear,{19.5f,12.5f},1,360,360);
    REQUIRE(replacement.index()==guard.index());run(w,script,16,false);
    REQUIRE_FALSE(script.defender_post(replacement));
}

TEST_CASE("Building guard preserves archer wall mounting and excludes noncombat units", "[script][guard]") {
    auto init=arena();init.buildings.push_back({rts::BldType::Wall,{14,12},1200,1200});
    rts::World w(init);
    const auto archer=w.spawn_unit(rts::UnitType::Archer,{13.5f,12.5f},1,240,240);
    const auto spear=w.spawn_unit(rts::UnitType::Spear,{13.5f,11.5f},1,360,360);
    const auto worker=w.spawn_unit(rts::UnitType::Mason,{8.5f,12.5f},1,120,120);
    const auto scout=w.spawn_unit(rts::UnitType::Scout,{8.5f,13.5f},1,100,100);
    const rts::UnitId ids[]={archer,spear,worker,scout};
    game::DefenderScript script({},1);
    REQUIRE(script.issue_guard_order(w.view(rts::Side::Defender),ids,{14,12}));
    run(w,script,160,false);
    REQUIRE(w.unit_garrison(archer)!=rts::kNoSlot);
    REQUIRE(w.unit_garrison(spear)==rts::kNoSlot);
    REQUIRE(script.defender_post(spear));
    REQUIRE_FALSE(script.defender_post(worker));REQUIRE_FALSE(script.defender_post(scout));
    REQUIRE_FALSE(script.issue_guard_order(w.view(rts::Side::Defender),{&worker,1},{14,12}));
    REQUIRE_FALSE(script.issue_guard_order(w.view(rts::Side::Defender),{&spear,1},{25,20}));
    w.place_bld(rts::BldType::Barrack,{20,12},1,400,200);
    REQUIRE(script.issue_guard_order(w.view(rts::Side::Defender),{&archer,1},{20,12}));
    run(w,script,300,false);
    REQUIRE(w.unit_garrison(archer)==rts::kNoSlot);
    REQUIRE(script.defender_post(archer)->anchor==rts::GridPos{20,12});
}

TEST_CASE("A building guard archer still kites nearby melee and returns afterwards", "[script][guard]") {
    auto init=arena();init.buildings.push_back({rts::BldType::Barrack,{20,12},400,400});
    rts::World w(init);const auto guard=w.spawn_unit(rts::UnitType::Archer,{21.5f,12.5f},1,240,240);
    const auto enemy=w.spawn_unit(rts::UnitType::Ghoul,{21.5f,13.5f},1,10000,10000);
    game::DefenderScript script({},1);
    REQUIRE(script.issue_guard_order(w.view(rts::Side::Defender),{&guard,1},{20,12}));
    run(w,script,80,false);
    const auto p=w.unit_pos(guard);
    REQUIRE((p.x-21.5f)*(p.x-21.5f)+(p.y-13.5f)*(p.y-13.5f)>2.5f*2.5f);
    REQUIRE(w.unit_hp(enemy)<10000);
    w.kill_unit(enemy);run(w,script,300,false);
    const auto home=w.unit_pos(guard);
    REQUIRE((home.x-20.5f)*(home.x-20.5f)+(home.y-12.5f)*(home.y-12.5f)<=2.25f);
}

// Explicit diagnostic, excluded from normal CTest. No hardware-specific pass threshold.
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
