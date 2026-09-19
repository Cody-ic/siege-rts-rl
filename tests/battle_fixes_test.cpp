#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include "game/battle_scene.hpp"
#include "game/demo_driver.hpp"
#include "game/defender_macro.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "game/world_builder.hpp"
#include "rts/world_view.hpp"
#include "rts/utf8_path.hpp"

namespace {
rts::StatsTable stats() {return game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");}
game::MapData pool_map() {return game::MapLoader::from_file(std::string(GAME_DATA_DIR)+"/maps/pool/gen_01001000.json");}
void clear_units(rts::World& w) {
    std::vector<rts::UnitId> ids;
    for(auto side:{rts::Side::Attacker,rts::Side::Defender}) {
        w.enumerate_units(side,ids);for(auto id:ids) w.kill_unit(id);
    }
}
}

TEST_CASE("不死鸟跳过贴脸堡垒但仍能攻击其他建筑", "[battlefix]") {
    rts::WorldInit init;init.width=16;init.height=16;init.terrain.assign(256,rts::Terrain::Plain);
    init.keep={5,5};init.stats=stats();
    init.buildings.push_back({rts::BldType::Keep,{5,5},10000,10000});
    rts::World w(init);
    const auto bird=w.spawn_unit(rts::UnitType::Phoenix,{5.5f,6.0f},1,1000,1000);
    const auto bit=1u<<static_cast<unsigned>(rts::UnitAction::AtkBld);
    REQUIRE((w.action_mask(bird)&bit)==0);
    const auto target=w.place_bld(rts::BldType::Barrack,{6,6},10000,10000);
    REQUIRE((w.action_mask(bird)&bit)!=0);
    const rts::UnitAction attack=rts::UnitAction::AtkBld;
    w.submit_actions(rts::Side::Attacker,&attack,1);w.advance(80);
    REQUIRE(w.bld_hp(w.bld_at({5,5}))==10000);
    REQUIRE(w.bld_hp(target)<10000);
}

TEST_CASE("塔楼前摇与释放进入攻击动画且弹丸保留真实发射原点", "[battlefix]") {
    const auto map=pool_map();auto init=game::make_world_init(map,stats(),3,1);
    init.buildings.clear();init.units.clear();
    init.buildings.push_back({rts::BldType::Keep,map.keep(),10000,10000});
    init.buildings.push_back({rts::BldType::Flak,{85,82},10000,10000});
    rts::World w(init);
    w.spawn_unit(rts::UnitType::Phoenix,{85.5f,76.5f},1,10000,10000);
    w.advance(1);
    auto draw=game::BattleScene::sorted(map,w.view(rts::Side::Defender),w.now());
    auto tower=std::find_if(draw.begin(),draw.end(),[](const auto& item){return item.sprite=="Flak";});
    REQUIRE(tower!=draw.end());REQUIRE(tower->state=="attack");REQUIRE(tower->attack_progress<1.0f);
    const int windup=init.stats.of(rts::BldType::Flak).windup_ticks;
    w.advance(windup);
    auto view=w.view(rts::Side::Defender);
    REQUIRE_FALSE(view.proj_pos().empty());
    REQUIRE(view.proj_origin()[0]==rts::center_of(rts::GridPos{85,82}));
    draw=game::BattleScene::sorted(map,view,w.now());
    tower=std::find_if(draw.begin(),draw.end(),[](const auto& item){return item.sprite=="Flak";});
    REQUIRE(tower->attack_progress>=1.0f);
    const auto bolt=std::find_if(draw.begin(),draw.end(),[](const auto& item){return item.sprite=="Bolt";});
    REQUIRE(bolt!=draw.end());REQUIRE(bolt->projectile_source=="Flak");
    REQUIRE(bolt->flight_progress>0.0f);REQUIRE(bolt->flight_progress<1.0f);
    w.advance(100);
    view=w.view(rts::Side::Defender);
    REQUIRE(view.proj_origin().size()==view.proj_pos().size());
}

TEST_CASE("地图池所有开局塔楼避开城门通道", "[battlefix]") {
    const auto directory=rts::path_from_utf8(std::string(GAME_DATA_DIR)+"/maps/pool");
    int count=0;
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        if(entry.path().extension()!=".json") continue;
        const auto map=game::MapLoader::from_file(rts::utf8_from_path(entry.path()));
        game::DemoBattle battle(map,stats(),1);const auto view=battle.world().view(rts::Side::Defender);
        for(std::size_t k=0;k<view.bld_pos().size();++k) {
            if(view.bld_type()[k]!=rts::BldType::Tower && view.bld_type()[k]!=rts::BldType::Flak) continue;
            CAPTURE(entry.path().filename().string(),view.bld_pos()[k].i,view.bld_pos()[k].j);
            REQUIRE_FALSE(map.gate_approach(view.bld_pos()[k]));
        }
        ++count;
    }
    REQUIRE(count>0);
}

TEST_CASE("已进城步兵不等待仍在破墙的攻城锤", "[battlefix]") {
    const auto map=pool_map();auto st=stats();
    for(auto& unit:st.unit) unit.damage=0;
    for(auto& bld:st.bld) bld.damage=0;
    game::DemoBattle battle(map,st,5);auto& w=const_cast<rts::World&>(battle.world());
    clear_units(w);w.begin_assault();
    const auto gate=w.bld_at({95,85});if(w.alive(gate)) w.destroy_bld(gate);
    const auto ram=w.spawn_unit(rts::UnitType::Ram,{96.2f,86.5f},1,10000,10000);
    const auto ghoul=w.spawn_unit(rts::UnitType::Ghoul,{90.5f,85.5f},1,10000,10000);
    REQUIRE((w.action_mask(ram)&(1u<<static_cast<unsigned>(rts::UnitAction::AtkWall)))!=0);
    battle.update(30);
    REQUIRE(w.unit_pos(ghoul).x<90.0f);
    REQUIRE(w.alive(w.bld_at({95,86})));
}
TEST_CASE("墙头驻军不被地面近战直接选中", "[battlefix]") {
    rts::WorldInit init;init.width=16;init.height=16;init.terrain.assign(256,rts::Terrain::Plain);
    init.keep={8,8};init.stats=stats();init.stats.unit[static_cast<std::size_t>(rts::UnitType::Archer)].damage=0;
    init.buildings.push_back({rts::BldType::Keep,{8,8},10000,10000});
    init.buildings.push_back({rts::BldType::Wall,{9,8},1200,1200});
    rts::World w(init);const auto archer=w.spawn_unit(rts::UnitType::Archer,{8.5f,8.5f},1,240,240);
    const std::uint16_t wish=rts::slot_of({9,8},16);w.submit_garrison_wishes(rts::Side::Defender,&wish,1);w.advance(100);
    REQUIRE(w.view(rts::Side::Defender).unit_garrison()[archer.index()]==wish);
    const auto ghoul=w.spawn_unit(rts::UnitType::Ghoul,{10.0f,8.5f},1,1000,1000);
    const auto bit=1u<<static_cast<unsigned>(rts::UnitAction::AtkNear);
    REQUIRE((w.action_mask(ghoul)&bit)==0);
    const auto shade=w.spawn_unit(rts::UnitType::Shade,{11.5f,8.5f},1,1000,1000);
    REQUIRE((w.action_mask(shade)&bit)!=0);
}
TEST_CASE("开发者指定波次重新生成对应攻方且不受人口约束", "[battlefix]") {
    const auto map=pool_map();game::DemoBattle b(map,stats(),1);
    b.enable_developer();b.developer_wave(37);
    REQUIRE(b.world().wave()==37);REQUIRE(b.world().phase()==rts::WavePhase::Build);
    REQUIRE(b.world().defender_pop_cap()>1000000);
    for(auto r:{rts::Resource::Stone,rts::Resource::Wood,rts::Resource::Gold}) REQUIRE(b.world().stock(r)>=1000000000);
    std::vector<rts::UnitId> ids;b.world().enumerate_units(rts::Side::Attacker,ids);
    REQUIRE_FALSE(ids.empty());REQUIRE(b.wave_plan().units()==static_cast<int>(ids.size()));
    b.developer_wave(70);REQUIRE(b.world().wave()==70);
    REQUIRE(b.build_ticks_left()>0);
}
TEST_CASE("工匠从城内修墙并在敌人靠近时撤离", "[battlefix]") {
    rts::WorldInit init;init.width=16;init.height=16;init.terrain.assign(256,rts::Terrain::Plain);
    init.keep={7,7};init.stats=stats();init.buildings.push_back({rts::BldType::Keep,{7,7},10000,10000});
    for(int i=3;i<=12;++i) for(int j=3;j<=12;++j) if(i==3||i==12||j==3||j==12)
        init.buildings.push_back({i==12&&j==7?rts::BldType::Gate:rts::BldType::Wall,{static_cast<std::int16_t>(i),static_cast<std::int16_t>(j)},1200,1200});
    for(auto& building:init.buildings) if(building.pos==rts::GridPos{12,8}) building.hp=600;
    rts::World w(init);const auto target=w.bld_at({12,8});
    w.set_stock(rts::Resource::Wood,10000);
    rts::Command repair;repair.kind=rts::CommandKind::Repair;repair.slot=rts::slot_of({12,8},16);w.submit(rts::Side::Defender,&repair,1);
    const auto mason=w.spawn_unit(rts::UnitType::Mason,{7.5f,8.5f},1,120,120);
    game::DefenderScript script({},1);std::vector<rts::UnitId> ids;std::vector<rts::UnitAction> acts;std::vector<std::uint16_t> wishes;
    const auto step=[&](){w.enumerate_units(rts::Side::Defender,ids);script.decide(w.view(rts::Side::Defender),ids,acts,wishes);w.submit_actions(rts::Side::Defender,acts.data(),acts.size());w.submit_garrison_wishes(rts::Side::Defender,wishes.data(),wishes.size());w.advance(4);};
    for(int k=0;k<25;++k) {step();REQUIRE(w.unit_pos(mason).x<12.0f);}
    REQUIRE(w.bld_hp(target)>600);
    w.spawn_unit(rts::UnitType::Ghoul,{12.5f,8.5f},1,10000,10000);
    const auto before=w.unit_pos(mason);
    for(int k=0;k<10;++k) step();
    REQUIRE(w.unit_pos(mason).x<before.x);
}
TEST_CASE("建筑升级最低工料随等级增加", "[battlefix]") {
    const auto map=pool_map();rts::World w(game::make_world_init(map,stats(),1,1));
    REQUIRE(w.bld_upgrade_cost_stone(rts::BldType::Tower,1)>=32);
    REQUIRE(w.bld_upgrade_cost_wood(rts::BldType::Tower,1)>=12);
    REQUIRE(w.bld_upgrade_cost_stone(rts::BldType::Tower,8)>w.bld_upgrade_cost_stone(rts::BldType::Tower,1));
}
TEST_CASE("攻方必须由窥使观察才能得到新情报，情报用于下一波", "[battlefix]") {
    game::DemoBattle b(pool_map(),stats(),1);auto& w=const_cast<rts::World&>(b.world());
    std::vector<rts::UnitId> ids;w.enumerate_units(rts::Side::Attacker,ids);for(auto id:ids) w.kill_unit(id);
    rts::GridPos tower{};const auto v=w.view(rts::Side::Attacker);
    for(std::size_t k=0;k<v.bld_pos().size();++k) if(v.bld_alive()[k]&&v.bld_type()[k]==rts::BldType::Tower) {tower=v.bld_pos()[k];break;}
    const auto point=rts::center_of(tower);
    w.spawn_unit(rts::UnitType::Ghoul,point,1,10000,10000);b.update(16);
    REQUIRE_FALSE(b.wave_scouted());
    w.spawn_unit(rts::UnitType::Wraith,point,1,10000,10000);b.update(16);
    REQUIRE_FALSE(b.wave_scouted());
    REQUIRE(b.recon_transmitting());
    b.update(game::DemoBattle::kReconTransmitTicks);
    REQUIRE(b.wave_scouted());
    w.enumerate_units(rts::Side::Attacker,ids);for(auto id:ids) w.kill_unit(id);
    w.begin_assault();b.update(1);
    REQUIRE(b.world().wave()==2);REQUIRE(b.wave_intel().fresh);REQUIRE(b.wave_intel().towers>0);
}

TEST_CASE("攻方沿途优先破坏可见城外采集建筑", "[battlefix]") {
    for(auto type:{rts::UnitType::Ghoul,rts::UnitType::Shade,rts::UnitType::Phoenix}) for(float distance:{1.0f,4.0f}) {
        game::DemoBattle battle(pool_map(),stats(),72);auto& w=const_cast<rts::World&>(battle.world());clear_units(w);
        const auto site=std::find_if(w.resources().begin(),w.resources().end(),[](const auto& s){return s.tier==rts::ResourceTier::Outer;});
        REQUIRE(site!=w.resources().end());
        auto building=w.bld_at(site->pos);
        if(!building.valid()) building=w.place_bld(rts::gatherer_of(site->kind),site->pos,10000,10000);
        REQUIRE(building.valid());
        const auto c=rts::center_of(site->pos);
        const auto enemy=w.spawn_unit(type,{c.x+distance,c.y},1,10000,10000);
        REQUIRE(enemy.valid());
        if(type==rts::UnitType::Phoenix) w.spawn_unit(rts::UnitType::Ghoul,{85.5f,85.5f},1,10000,10000);
        const auto before=w.bld_hp(building);w.begin_assault();
        for(int tick=0;tick<160;++tick) battle.update(1);
        REQUIRE(w.bld_hp(building)<before);
    }
}

TEST_CASE("Killing a transmitting Wraith denies next-wave intelligence", "[battlefix]") {
    game::DemoBattle b(pool_map(),stats(),9);auto& w=const_cast<rts::World&>(b.world());
    clear_units(w);
    const auto point=rts::center_of(w.keep_pos());
    w.spawn_unit(rts::UnitType::Ghoul,point,1,100000,100000);
    const auto spy=w.spawn_unit(rts::UnitType::Wraith,point,1,100000,100000);
    b.update(12);REQUIRE(b.recon_transmitting());REQUIRE_FALSE(b.wave_scouted());
    w.kill_unit(spy);b.update(80);
    REQUIRE_FALSE(b.wave_scouted());REQUIRE_FALSE(b.recon_transmitting());
    clear_units(w);w.begin_assault();b.update(1);
    REQUIRE(w.wave()==2);REQUIRE_FALSE(b.wave_intel().fresh);
}

TEST_CASE("Ground attackers do not wait under tower fire", "[battlefix]") {
    auto st=stats();for(auto& u:st.unit) u.damage=0;
    for(auto& b:st.bld) b.damage=0;
    st.bld[static_cast<std::size_t>(rts::BldType::Tower)].damage=1;
    game::DemoBattle battle(pool_map(),st,5);auto& w=const_cast<rts::World&>(battle.world());
    clear_units(w);w.begin_assault();
    const auto gate=w.bld_at({95,85});if(w.alive(gate)) w.destroy_bld(gate);
    const auto ghoul=w.spawn_unit(rts::UnitType::Ghoul,{90.5f,85.5f},1,100000,100000);
    w.spawn_unit(rts::UnitType::Ram,{110.5f,85.5f},1,100000,100000);
    auto tower=w.bld_at({90,82});if(w.alive(tower)) w.destroy_bld(tower);
    REQUIRE(w.place_bld(rts::BldType::Tower,{90,82},10000,10000).valid());
    battle.update(24);
    REQUIRE(w.unit_pos(ghoul).x<90.0f);
}

TEST_CASE("A reachable barracks is attacked without waiting for the formation", "[battlefix]") {
    auto st=stats();for(auto& b:st.bld) b.damage=0;
    game::DemoBattle battle(pool_map(),st,8);auto& w=const_cast<rts::World&>(battle.world());clear_units(w);
    const auto old=w.bld_at({88,85});if(w.alive(old)) w.destroy_bld(old);
    const auto barrack=w.place_bld(rts::BldType::Barrack,{88,85},10000,10000);REQUIRE(barrack.valid());
    w.spawn_unit(rts::UnitType::Ghoul,{89.4f,85.5f},1,100000,100000);
    w.spawn_unit(rts::UnitType::Ram,{110.5f,85.5f},1,100000,100000);w.begin_assault();
    battle.update(60);REQUIRE(w.bld_hp(barrack)<10000);
}

TEST_CASE("Scout reports merge distinct sightings without counting the same unit twice", "[battlefix]") {
    auto st=stats();for(auto& u:st.unit) {u.damage=0;u.speed=0;}
    for(auto& b:st.bld) b.damage=0;
    game::DefenderSetup setup;setup.scout_death_permille=0;
    game::DemoBattle battle(pool_map(),st,1,{}, {},setup);
    auto& w=const_cast<rts::World&>(battle.world());clear_units(w);
    REQUIRE(w.spawns().size()>=2);
    const auto p=rts::center_of(w.spawns()[0].pos),q=rts::center_of(w.spawns()[1].pos);
    w.spawn_unit(rts::UnitType::Ghoul,p,1,10000,10000);
    w.spawn_unit(rts::UnitType::Shade,q,1,10000,10000);
    auto scout=w.spawn_unit(rts::UnitType::Scout,p,1,10000,10000);battle.update(1);
    REQUIRE(battle.scout_report().size()==1);REQUIRE(battle.scout_report()[0].count==1);
    w.kill_unit(scout);scout=w.spawn_unit(rts::UnitType::Scout,q,1,10000,10000);battle.update(1);
    REQUIRE(battle.scout_report().size()==2);
    w.kill_unit(scout);w.spawn_unit(rts::UnitType::Scout,p,1,10000,10000);battle.update(1);
    REQUIRE(battle.scout_report().size()==2);
    for(const auto& row:battle.scout_report()) REQUIRE(row.count==1);
    REQUIRE(battle.scout_report_tick()==w.now());
}

TEST_CASE("Fast scouts turn through narrow gates without oscillating", "[battlefix][scoutnav]") {
    for (const auto name : {"gen_01008000.json", "gen_01012000.json"}) {
        CAPTURE(name);
        const auto map = game::MapLoader::from_file(std::string(GAME_DATA_DIR)+"/maps/pool/"+name);
        game::DefenderSetup setup; setup.scout_death_permille = 0;
        game::DemoBattle battle(map, stats(), 1, {}, {}, setup);
        game::DefenderMacro macro(map);
        for (int tick = 0; tick < 600 && battle.scout_outcome() != game::DemoBattle::ScoutOutcome::Success; ++tick) {
            if (tick % 20 == 0) {
                std::vector<rts::Command> commands;
                std::vector<game::UnitOrder> orders;
                macro.decide(battle.world(), commands, orders);
                battle.submit_defender(commands.data(), commands.size());
            }
            battle.update(1);
        }
        REQUIRE(battle.scout_outcome() == game::DemoBattle::ScoutOutcome::Success);
    }
}

TEST_CASE("Recon counts explored defenses beyond the starting city and excludes unseen ones", "[battlefix]") {
    const auto map=pool_map();auto init=game::make_world_init(map,stats(),7,1);
    init.units.clear();init.buildings.clear();init.buildings.push_back({rts::BldType::Keep,map.keep(),10000,10000});
    rts::World w(init);
    const auto p=w.spawns()[0].pos;
    const auto tower=w.place_bld(rts::BldType::Tower,p,10000,10000);REQUIRE(tower.valid());
    game::AttackerMacro macro(map);
    w.advance(1);REQUIRE(macro.read_intel(w.view(rts::Side::Attacker),false).towers==0);
    w.spawn_unit(rts::UnitType::Wraith,rts::center_of(p),1,10000,10000);w.advance(1);
    REQUIRE(macro.read_intel(w.view(rts::Side::Attacker),true).towers==1);
}

TEST_CASE("Formation waits briefly in safety but cannot be held forever by a stuck rear unit", "[battlefix]") {
    auto st=stats();for(auto& u:st.unit) u.damage=0;for(auto& b:st.bld) b.damage=0;
    st.unit[static_cast<std::size_t>(rts::UnitType::Ram)].speed=0;
    game::DemoBattle battle(pool_map(),st,3);auto& w=const_cast<rts::World&>(battle.world());clear_units(w);
    const auto v=w.view(rts::Side::Defender);
    for(std::size_t k=0;k<v.bld_alive().size();++k) if(v.bld_alive()[k] && v.bld_type()[k]!=rts::BldType::Keep) w.destroy_bld(w.bld_at(v.bld_pos()[k]));
    w.begin_assault();const auto ghoul=w.spawn_unit(rts::UnitType::Ghoul,{90.5f,85.5f},1,100000,100000);
    w.spawn_unit(rts::UnitType::Ram,{110.5f,85.5f},1,100000,100000);
    battle.update(30);REQUIRE(w.unit_pos(ghoul).x==90.5f);
    battle.update(110);REQUIRE(w.unit_pos(ghoul).x<89.5f);
}
