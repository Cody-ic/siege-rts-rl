#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include "game/battle_scene.hpp"
#include "game/demo_driver.hpp"
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
