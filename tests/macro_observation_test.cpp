#include <catch2/catch_test_macros.hpp>
#include "game/macro_observation.hpp"
#include "game/player_input.hpp"
#include "game/stats_loader.hpp"
#include <set>
#include <tuple>

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
TEST_CASE("Macro candidates cover all legal cells and recruitment levels", "[macroobs]") {
    rts::World w(arena());
    w.set_stock(rts::Resource::Stone,10000);w.set_stock(rts::Resource::Wood,10000);
    w.set_stock(rts::Resource::Gold,10000);
    const auto v=w.view(rts::Side::Defender);
    using Key=std::tuple<int,int,int,int>;
    std::set<Key> actual,expected;
    for(const auto& c:game::macro_candidates(v,true)) {
        REQUIRE(game::macro_command_legal(v,c,true));
        REQUIRE(actual.emplace(static_cast<int>(c.kind),c.slot,c.what,c.level).second);
    }
    const auto consider=[&](rts::Command c) {
        if(game::macro_command_legal(v,c,true)) expected.emplace(static_cast<int>(c.kind),c.slot,c.what,c.level);
    };
    consider({});
    rts::Command summon;summon.kind=rts::CommandKind::Summon;consider(summon);
    for(int slot=0;slot<1600;++slot) {
        rts::Command c;c.slot=static_cast<std::uint16_t>(slot);
        for(const auto kind:{rts::CommandKind::Repair,rts::CommandKind::Upgrade,rts::CommandKind::Cancel,
                             rts::CommandKind::Demolish,rts::CommandKind::Clear}) {
            c.kind=kind;consider(c);
        }
        c.kind=rts::CommandKind::Build;
        for(int type=0;type<rts::kBldTypeCount;++type) {c.what=static_cast<std::uint8_t>(type);consider(c);}
        c.kind=rts::CommandKind::Train;
        for(int type=0;type<rts::kUnitTypeCount;++type) for(int level=1;level<=3;++level) {
            c.what=static_cast<std::uint8_t>(type);c.level=static_cast<std::uint8_t>(level);consider(c);
        }
    }
    REQUIRE(actual==expected);
}
}
TEST_CASE("Macro observation excludes hidden enemies including their counts and levels", "[macroobs]") {
    rts::World a(arena()),b(arena());
    b.spawn_unit(rts::UnitType::Ghoul,{35.5f,35.5f},99,9000,9000);
    a.advance(1);b.advance(1);
    const auto x=game::pack_macro_observation(a.view(rts::Side::Defender));
    const auto y=game::pack_macro_observation(b.view(rts::Side::Defender));
    REQUIRE(x.cells==y.cells);REQUIRE(x.global==y.global);
    REQUIRE(game::pack_macro_detail(a.view(rts::Side::Defender))==game::pack_macro_detail(b.view(rts::Side::Defender)));
    REQUIRE(game::macro_detail_names().size()==game::kMacroDetailChannels);
    REQUIRE(x.cells.size()==game::kMacroGrid*game::kMacroGrid*game::kMacroChannels);
    REQUIRE(game::macro_cell_names().size()==game::kMacroChannels);
    REQUIRE(game::macro_global_names().size()==game::kMacroGlobals);
    b.spawn_unit(rts::UnitType::Ghoul,{4.5f,4.5f},1,400,400);
    b.advance(1);
    REQUIRE(game::pack_macro_observation(b.view(rts::Side::Defender)).cells!=x.cells);
    REQUIRE_THROWS(game::pack_macro_observation(a.view(rts::Side::Attacker)));
    REQUIRE_THROWS(game::pack_macro_detail(a.view(rts::Side::Attacker)));
}
TEST_CASE("Fine macro detail distinguishes adjacent buildings within one coarse region", "[macroobs]") {
    auto left=arena(),right=arena();
    left.buildings.push_back({rts::BldType::Wall,{10,10},100,100});
    right.buildings.push_back({rts::BldType::Wall,{11,10},100,100});
    rts::World a(left),b(right);
    const auto av=a.view(rts::Side::Defender),bv=b.view(rts::Side::Defender);
    REQUIRE(game::pack_macro_observation(av).cells==game::pack_macro_observation(bv).cells);
    const auto x=game::pack_macro_detail(av),y=game::pack_macro_detail(bv);
    REQUIRE(x.size()==40*40*game::kMacroDetailChannels);
    REQUIRE(x!=y);
    const auto offset=static_cast<std::size_t>((10*40+10)*game::kMacroDetailChannels+5+static_cast<int>(rts::BldType::Wall));
    REQUIRE(x[offset]==1.f);REQUIRE(y[offset]==0.f);
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

TEST_CASE("Population-full recruitment still offers scouts but not workers or combat troops", "[macroobs]") {
    auto init=arena();init.pop_cap_base=1;init.pop_cap_per_keep_level=0;
    rts::World w(init);w.set_stock(rts::Resource::Gold,1000);
    const auto view=w.view(rts::Side::Defender);
    REQUIRE(view.defender_pop()==view.defender_pop_cap());
    REQUIRE_FALSE(game::train_pop_full(view,rts::UnitType::Scout));
    REQUIRE(game::train_pop_full(view,rts::UnitType::Mason));
    const auto scout=game::train_command(rts::UnitType::Scout,1,w.keep_pos(),w.width());
    const auto mason=game::train_command(rts::UnitType::Mason,1,w.keep_pos(),w.width());
    REQUIRE(game::macro_command_legal(view,scout,false));
    REQUIRE_FALSE(game::macro_command_legal(view,mason,false));
    bool offered=false;
    for(const auto& c:game::macro_candidates(view,false)) if(c.kind==rts::CommandKind::Train) {
        REQUIRE(c.what==static_cast<std::uint8_t>(rts::UnitType::Scout));offered=true;
    }
    REQUIRE(offered);
    w.submit(rts::Side::Defender,&scout,1);w.advance(1);
    REQUIRE(w.defender_pop()==1);
    REQUIRE(w.stock(rts::Resource::Gold)==975);
    REQUIRE(view.bld_train_left()[0]>0);
    w.advance(80);
    REQUIRE(w.defender_pop()==1);
    std::vector<rts::UnitId> ids;w.enumerate_units(rts::Side::Defender,ids);
    REQUIRE(ids.size()==2);
}
