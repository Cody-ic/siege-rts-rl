#include <cmath>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "game/defender_script.hpp"
#include "game/stats_loader.hpp"
#include "../game/src/battle_snapshot.hpp"

namespace {
rts::WorldInit arena() {
    rts::WorldInit init;
    init.width=40;init.height=28;init.terrain.assign(40*28,rts::Terrain::Plain);
    init.keep={3,12};init.buildings.push_back({rts::BldType::Keep,init.keep,2400,2400});
    init.buildings.push_back({rts::BldType::Tower,{10,12},500,500});
    init.stats=game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
    for(auto& b:init.stats.bld) {b.damage=0;b.vision=1;}
    return init;
}
std::vector<rts::UnitAction> decide(rts::World& w,game::DefenderScript& script) {
    std::vector<rts::UnitId> ids;w.enumerate_units(rts::Side::Defender,ids);
    std::vector<rts::UnitAction> actions;std::vector<std::uint16_t> wishes;
    script.decide(w.view(rts::Side::Defender),ids,actions,wishes);
    return actions;
}
void guard(rts::World& w,game::DefenderScript& script,rts::UnitId unit) {
    REQUIRE(script.issue_guard_order(w.view(rts::Side::Defender),{&unit,1},{10,12}));
}
void start_attack(rts::World& w,rts::UnitId enemy,rts::UnitAction action=rts::UnitAction::AtkNear) {
    w.advance(1); // Refresh fog before the observation.
    REQUIRE((w.action_mask(enemy)&(1u<<static_cast<unsigned>(action)))!=0);
    w.submit_actions(rts::Side::Attacker,&action,1);w.advance(1);
    REQUIRE(w.unit_windup(enemy)>0);
}
void run(rts::World& w,game::DefenderScript& script,int ticks,bool fight=true) {
    std::vector<rts::UnitId> ids;std::vector<rts::UnitAction> actions;std::vector<std::uint16_t> wishes;
    for(int t=0;t<ticks;++t) {
        w.enumerate_units(rts::Side::Defender,ids);
        if(t%8==0) {
            script.decide(w.view(rts::Side::Defender),ids,actions,wishes);
            w.submit_actions(rts::Side::Defender,actions.data(),actions.size());
            w.submit_garrison_wishes(rts::Side::Defender,wishes.data(),wishes.size());
        } else if(script.refresh_scout_navigation(w.view(rts::Side::Defender),ids,actions))
            w.submit_actions(rts::Side::Defender,actions.data(),actions.size());
        w.enumerate_units(rts::Side::Attacker,ids);actions.clear();
        for(auto id:ids) actions.push_back(fight && (w.action_mask(id)&(1u<<static_cast<unsigned>(rts::UnitAction::AtkNear)))?
            rts::UnitAction::AtkNear:rts::UnitAction::Stop);
        w.submit_actions(rts::Side::Attacker,actions.data(),actions.size());w.advance(1);
    }
}
}

TEST_CASE("Post defenders retaliate against a visible caster beyond the ordinary radius", "[script][guard][retaliation]") {
    auto type=rts::UnitType::Ranger;
    SECTION("Ranger") {}
    SECTION("Spear") {type=rts::UnitType::Spear;}
    rts::World w(arena());game::DefenderScript script({},3);
    const auto unit=w.spawn_unit(type,{16.1f,12.5f},1,1000,1000);
    const auto mage=w.spawn_unit(rts::UnitType::Shade,{19.6f,12.5f},1,80,140);
    guard(w,script,unit);
    start_attack(w,mage);
    REQUIRE(w.view(rts::Side::Defender).unit_target_raw()[mage.index()]==unit.raw());
    decide(w,script);
    REQUIRE(script.defender_post(unit)->mode==game::PostMode::Engaging);
    run(w,script,300);
    REQUIRE_FALSE(w.alive(mage));REQUIRE(w.alive(unit));
    const auto p=w.unit_pos(unit);
    REQUIRE(std::hypot(p.x-10.5f,p.y-12.5f)<=1.5f);
    REQUIRE(script.defender_post(unit)->mode==game::PostMode::Holding);
}

TEST_CASE("A caster attacking a post mate alerts all three combat classes", "[script][guard][retaliation]") {
    auto type=rts::UnitType::Spear;
    SECTION("Spear") {}
    SECTION("Ranger") {type=rts::UnitType::Ranger;}
    SECTION("Ground archer") {type=rts::UnitType::Archer;}
    rts::World w(arena());game::DefenderScript script({},3);
    const auto helper=w.spawn_unit(type,{10.5f,11.5f},1,1000,1000);
    const auto victim=w.spawn_unit(rts::UnitType::Ranger,{16.1f,12.5f},1,1000,1000);
    const auto mage=w.spawn_unit(rts::UnitType::Shade,{19.6f,12.5f},1,1000,1000);
    guard(w,script,helper);guard(w,script,victim);
    start_attack(w,mage);
    REQUIRE(w.view(rts::Side::Defender).unit_target_raw()[mage.index()]==victim.raw());
    const auto actions=decide(w,script);
    REQUIRE(rts::is_move(actions[0]));
    REQUIRE(script.defender_post(helper)->mode==game::PostMode::Engaging);
}

TEST_CASE("Killing the nearby decoy does not end the response to the outside shooter", "[script][guard][retaliation]") {
    rts::World w(arena());game::DefenderScript script({},3);
    const auto rider=w.spawn_unit(rts::UnitType::Ranger,{16.1f,12.5f},1,320,320);
    const auto near=w.spawn_unit(rts::UnitType::Shade,{17.1f,12.5f},1,1,140);
    const auto shooter=w.spawn_unit(rts::UnitType::Shade,{19.6f,12.5f},1,100,140);
    guard(w,script,rider);w.advance(1);
    const rts::UnitAction opening[]={rts::UnitAction::Stop,rts::UnitAction::AtkNear};
    w.submit_actions(rts::Side::Attacker,opening,2);w.advance(1);
    REQUIRE(w.view(rts::Side::Defender).unit_target_raw()[shooter.index()]==rider.raw());
    run(w,script,250);
    REQUIRE_FALSE(w.alive(near));REQUIRE_FALSE(w.alive(shooter));REQUIRE(w.alive(rider));
    REQUIRE(script.defender_post(rider)->mode==game::PostMode::Holding);
}

TEST_CASE("A retreating shooter cannot drag its pursuer beyond the post leash", "[script][guard][retaliation]") {
    rts::World w(arena());game::DefenderScript script({},3);
    const auto rider=w.spawn_unit(rts::UnitType::Ranger,{16.1f,12.5f},1,1000,1000);
    const auto mage=w.spawn_unit(rts::UnitType::Shade,{19.6f,12.5f},1,1000,1000);
    guard(w,script,rider);start_attack(w,mage);decide(w,script);
    REQUIRE(script.defender_post(rider)->mode==game::PostMode::Engaging);
    const auto away=rts::UnitAction::MoveSE;
    w.submit_actions(rts::Side::Attacker,&away,1);
    std::vector<rts::UnitId> ids;std::vector<rts::UnitAction> actions;
    for(int tick=0;tick<280;++tick) {
        w.enumerate_units(rts::Side::Defender,ids);
        if(tick%8==0) {
            actions=decide(w,script);w.submit_actions(rts::Side::Defender,actions.data(),actions.size());
        } else if(script.refresh_scout_navigation(w.view(rts::Side::Defender),ids,actions))
            w.submit_actions(rts::Side::Defender,actions.data(),actions.size());
        w.advance(1);
        REQUIRE(std::hypot(w.unit_pos(rider).x-10.5f,w.unit_pos(rider).y-12.5f)<=12.0f);
    }
    REQUIRE(script.defender_post(rider)->mode==game::PostMode::Holding);
}

TEST_CASE("Outpost response respects visibility identity distance and expiration", "[script][guard][retaliation]") {
    auto init=arena();
    bool idle=false, hidden=false, unrelated=false, distant=false, expired=false, reused_enemy=false, reused_victim=false, sealed=false;
    SECTION("A visible idle caster does not enlarge the normal patrol") {idle=true;}
    SECTION("Attacks on unrelated troops do not pull a different post") {unrelated=true;}
    SECTION("Hidden attacks reveal neither identity nor position") {
        hidden=true;for(auto& u:init.stats.unit) u.vision=1;
    }
    SECTION("Even active attackers beyond the absolute leash are ignored") {distant=true;}
    SECTION("An old attack expires after the caster stops firing") {expired=true;}
    SECTION("A replacement enemy cannot inherit the previous threat") {reused_enemy=true;}
    SECTION("A replacement ally cannot inherit the old guard order") {reused_victim=true;}
    SECTION("A sealed route does not provoke a futile chase") {
        sealed=true;
        for(int y=0;y<28;++y) init.buildings.push_back({rts::BldType::Wall,{14,static_cast<std::int16_t>(y)},1200,1200});
    }
    rts::World w(init);game::DefenderScript script({},3);
    const auto helper=w.spawn_unit(rts::UnitType::Spear,{10.5f,11.5f},1,1000,1000);
    auto victim=w.spawn_unit(rts::UnitType::Ranger,{distant?20.1f:16.1f,12.5f},1,1000,1000);
    auto mage=w.spawn_unit(rts::UnitType::Shade,{distant?23.6f:19.6f,12.5f},1,1000,1000);
    guard(w,script,helper);
    if(!unrelated) guard(w,script,victim);
    if(idle) w.advance(1);else start_attack(w,mage);
    decide(w,script);
    if(expired || reused_enemy || reused_victim) {
        REQUIRE(script.defender_post(helper)->mode==game::PostMode::Engaging);
        const rts::UnitAction stop=rts::UnitAction::Stop;
        w.submit_actions(rts::Side::Attacker,&stop,1);
        if(reused_enemy) {
            const auto old=mage;w.kill_unit(mage);
            mage=w.spawn_unit(rts::UnitType::Shade,{19.6f,12.5f},1,1000,1000);
            REQUIRE(mage.index()==old.index());REQUIRE(mage!=old);
        }
        if(reused_victim) {
            const auto old=victim;w.kill_unit(victim);
            victim=w.spawn_unit(rts::UnitType::Ranger,{16.1f,12.5f},1,1000,1000);
            REQUIRE(victim.index()==old.index());REQUIRE(victim!=old);
            guard(w,script,victim);
        }
        w.advance(expired?80:1);
        decide(w,script);
    }
    CAPTURE(idle,hidden,unrelated,distant,expired,reused_enemy,reused_victim,sealed);
    REQUIRE(script.defender_post(helper)->mode==game::PostMode::Holding);
}

TEST_CASE("A visible attack on the guarded building also permits retaliation", "[script][guard][retaliation]") {
    auto init=arena();init.stats.unit[static_cast<std::size_t>(rts::UnitType::Shade)].range=10;
    rts::World w(init);game::DefenderScript script({},3);
    const auto unit=w.spawn_unit(rts::UnitType::Ranger,{15.5f,12.5f},1,1000,1000);
    const auto mage=w.spawn_unit(rts::UnitType::Shade,{19.6f,12.5f},1,1000,1000);
    guard(w,script,unit);
    start_attack(w,mage,rts::UnitAction::AtkBld);
    REQUIRE(w.view(rts::Side::Defender).unit_target_raw()[mage.index()]==w.bld_at({10,12}).raw());
    decide(w,script);REQUIRE(script.defender_post(unit)->mode==game::PostMode::Engaging);
    // Same coordinate, new building identity: the old attack is no longer its threat.
    w.destroy_bld(w.bld_at({10,12}));w.place_bld(rts::BldType::Tower,{10,12},500,500);
    guard(w,script,unit);decide(w,script);
    REQUIRE(script.defender_post(unit)->mode==game::PostMode::Returning);
}

TEST_CASE("Retaliation memory survives cooldown and a snapshot between decisions", "[script][guard][retaliation][save]") {
    rts::World w(arena());game::DefenderScript script({},3);
    const auto helper=w.spawn_unit(rts::UnitType::Spear,{10.5f,11.5f},1,1000,1000);
    const auto victim=w.spawn_unit(rts::UnitType::Ranger,{16.1f,12.5f},1,1000,1000);
    const auto mage=w.spawn_unit(rts::UnitType::Shade,{19.6f,12.5f},1,1000,1000);
    guard(w,script,helper);guard(w,script,victim);
    start_attack(w,mage);
    // Only the per-tick refresh observes this windup, not a normal decision.
    std::vector<rts::UnitId> ids;w.enumerate_units(rts::Side::Defender,ids);
    std::vector<rts::UnitAction> actions;
    script.refresh_scout_navigation(w.view(rts::Side::Defender),ids,actions);
    const rts::UnitAction stop=rts::UnitAction::Stop;
    w.submit_actions(rts::Side::Attacker,&stop,1);w.advance(7);
    REQUIRE(w.unit_windup(mage)==0);
    const auto encoded=game::SnapshotCodec::encode(script);
    game::DefenderScript restored({},99);game::SnapshotCodec::decode(encoded,restored);
    for(int t=0;t<60;++t) {
        const auto a=decide(w,script),b=decide(w,restored);
        REQUIRE(a==b);
        REQUIRE(script.defender_post(helper)->mode==restored.defender_post(helper)->mode);
        if(t==0) REQUIRE(script.defender_post(helper)->mode==game::PostMode::Engaging);
        w.advance(1);
    }
    REQUIRE(script.defender_post(helper)->mode==game::PostMode::Holding);
}
