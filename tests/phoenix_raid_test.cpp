#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <iostream>
#include "game/phoenix_raid.hpp"
#include "game/stats_loader.hpp"
#include "game/player_input.hpp"
#include "rts/combat_math.hpp"
#include "rts/world.hpp"

namespace {
rts::World arena() {
    rts::WorldInit init; init.width=64; init.height=48;
    init.terrain.assign(64*48,rts::Terrain::Plain);init.keep={60,40};
    init.spawns.push_back({{2,22}});
    init.stats=game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
    for(auto& b:init.stats.bld) b.upgrade_ticks=0;
    init.buildings.push_back({rts::BldType::Keep,init.keep,10000,10000});
    return rts::World(init);
}
rts::UnitId bird_at(rts::World& w,int level,rts::Vec2 pos={10.5f,22.5f}) {
    const auto hp=rts::apply_permille(w.stats().of(rts::UnitType::Phoenix).max_hp,
        {rts::level_permille(level,w.stats().global.hp_permille_per_level)});
    return w.spawn_unit(rts::UnitType::Phoenix,pos,level,hp,hp);
}
rts::BldId flak_at(rts::World& w,int level,rts::GridPos pos={18,22},bool damaged=false) {
    w.set_stock(rts::Resource::Stone,10000000);w.set_stock(rts::Resource::Wood,10000000);
    const auto keep=game::upgrade_command(w.keep_pos(),w.width());
    const int required=(level-1)*w.stats().global.building_level_cap_divisor+1;
    for(int k=w.bld_level(w.bld_at(w.keep_pos()));k<required;++k) w.submit(rts::Side::Defender,&keep,1);
    const auto hp=w.stats().of(rts::BldType::Flak).max_hp;
    const auto id=w.place_bld(rts::BldType::Flak,pos,damaged?1:hp,hp);
    const auto upgrade=game::upgrade_command(pos,w.width());
    for(int k=1;k<level;++k) w.submit(rts::Side::Defender,&upgrade,1);
    if(level>1) w.advance(1);
    REQUIRE(w.bld_level(id)==level);
    return id;
}
game::PhoenixRaidEstimate estimate(rts::World& w,rts::UnitId bird,rts::GridPos at={18,22}) {
    game::AttackerKnowledge knowledge;knowledge.observe(w.view(rts::Side::Attacker),true);
    for(const auto& b:knowledge.buildings()) if(b.pos==at)
        return game::estimate_phoenix_raid(w.view(rts::Side::Attacker),knowledge,bird,b,400);
    FAIL("raid target missing");return {};
}
rts::UnitAction fly(rts::Vec2 p,rts::Vec2 to) {
    rts::UnitAction best=rts::UnitAction::Stop;float best_d=1e30f;
    for(int a=1;a<=8;++a) {
        const auto action=static_cast<rts::UnitAction>(a);const auto delta=rts::move_delta(action);
        const float scale=rts::is_grid_diagonal(action)?0.2f/std::sqrt(2.0f):0.2f;
        const float x=p.x+static_cast<float>(delta.di)*scale-to.x;
        const float y=p.y+static_cast<float>(delta.dj)*scale-to.y;
        if(x*x+y*y<best_d) {best_d=x*x+y*y;best=action;}
    }
    return best;
}
}

TEST_CASE("Phoenix raid compares level, damaged AA and overlapping fire", "[demo][phoenixraid]") {
    auto w=arena();const auto bird=bird_at(w,60);const auto flak=flak_at(w,1);
    const auto isolated=estimate(w,bird);
    CAPTURE(isolated.expected_damage,isolated.attack_ticks);
    REQUIRE(isolated.viable);
    SECTION("supporting AA can deny an otherwise winnable duel") {
        flak_at(w,60,{19,22});const auto supported=estimate(w,bird);
        REQUIRE(supported.expected_damage>isolated.expected_damage);
        REQUIRE_FALSE(supported.viable);
    }
    SECTION("same level bird cannot defeat a healthy AA tower") {
        w.kill_unit(bird);const auto weak=bird_at(w,1);
        REQUIRE_FALSE(estimate(w,weak).viable);
    }
    SECTION("damaged high level AA is evaluated using remaining health") {
        w.destroy_bld(flak);flak_at(w,60);
        REQUIRE_FALSE(estimate(w,bird).viable);
        w.destroy_bld(w.bld_at({18,22}));
        flak_at(w,60,{18,22},true);
        REQUIRE(estimate(w,bird).attack_ticks<isolated.attack_ticks);
    }
    SECTION("far away AA does not suppress this local raid") {
        flak_at(w,60,{58,3});
        REQUIRE(estimate(w,bird).expected_damage==isolated.expected_damage);
    }
}

TEST_CASE("Phoenix raids use remembered AA rather than unseen current truth", "[demo][phoenixraid]") {
    auto w=arena();const auto bird=bird_at(w,60);const auto flak=flak_at(w,1,{40,22});
    game::AttackerKnowledge knowledge;knowledge.observe(w.view(rts::Side::Attacker));
    REQUIRE_FALSE(game::choose_phoenix_raid(w.view(rts::Side::Attacker),knowledge,bird,400).known_targets);
    knowledge.observe(w.view(rts::Side::Attacker),true);
    const auto before=game::choose_phoenix_raid(w.view(rts::Side::Attacker),knowledge,bird,400);
    REQUIRE(before.target.has_value());
    w.destroy_bld(flak);flak_at(w,60,{40,22});
    knowledge.observe(w.view(rts::Side::Attacker));
    REQUIRE(game::choose_phoenix_raid(w.view(rts::Side::Attacker),knowledge,bird,400).target==before.target);
    knowledge.observe(w.view(rts::Side::Attacker),true);
    REQUIRE_FALSE(game::choose_phoenix_raid(w.view(rts::Side::Attacker),knowledge,bird,400).target.has_value());
}

TEST_CASE("Phoenix attacks AA before nearby ordinary buildings", "[demo][phoenixraid]") {
    auto w=arena();const auto bird=bird_at(w,60,{16,22.5f});
    const auto flak=flak_at(w,1);
    const auto bait=w.place_bld(rts::BldType::Barrack,{16,23},700,700);
    const rts::UnitAction attack=rts::UnitAction::AtkBld;
    w.submit_actions(rts::Side::Attacker,&attack,1);w.advance(12);
    REQUIRE(w.alive(bird));REQUIRE(w.bld_hp(flak)<450);REQUIRE(w.bld_hp(bait)==700);
}

TEST_CASE("Observed wall archers contribute to the Phoenix raid risk", "[demo][phoenixraid]") {
    auto w=arena();flak_at(w,1);const auto bird=bird_at(w,60);
    const auto before=estimate(w,bird);
    REQUIRE(before.viable);
    w.place_bld(rts::BldType::Wall,{17,24},1200,1200);
    const auto archer=w.spawn_unit(rts::UnitType::Archer,{16.5f,24.5f},60,10000,10000);
    const std::uint16_t wish=rts::slot_of({17,24},w.width());
    w.submit_garrison_wishes(rts::Side::Defender,&wish,1);
    w.advance(w.stats().global.garrison_mount_ticks+1);
    REQUIRE(w.view(rts::Side::Attacker).unit_mount()[archer.index()]==0);
    const auto after=estimate(w,bird);
    REQUIRE(after.expected_damage>before.expected_damage);
    REQUIRE_FALSE(after.viable);
}

TEST_CASE("Combat events include instant attacks and lethal interception with full identities", "[demo][feather]") {
    auto init=rts::WorldInit{};init.width=12;init.height=12;init.keep={1,1};
    init.terrain.assign(144,rts::Terrain::Plain);
    init.buildings.push_back({rts::BldType::Keep,init.keep,10000,10000});
    init.stats=game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
    auto& phoenix=init.stats.unit[static_cast<std::size_t>(rts::UnitType::Phoenix)];
    phoenix.windup_ticks=0;phoenix.cooldown_ticks=0;
    auto& flak=init.stats.bld[static_cast<std::size_t>(rts::BldType::Flak)];
    flak.damage=10000;flak.windup_ticks=0;flak.proj_speed=0;
    rts::World w(init);const auto bird=bird_at(w,1,{5.5f,5.5f});
    const auto barrack=w.place_bld(rts::BldType::Barrack,{6,5},700,700);
    auto action=rts::UnitAction::AtkBld;w.submit_actions(rts::Side::Attacker,&action,1);w.advance(1);
    REQUIRE(w.bld_hp(barrack)<700);
    REQUIRE(std::find(w.combat_events().begin(),w.combat_events().end(),bird)!=w.combat_events().end());
    action=rts::UnitAction::Stop;w.submit_actions(rts::Side::Attacker,&action,1);w.advance(1);
    REQUIRE(w.combat_events().empty());
    w.place_bld(rts::BldType::Flak,{5,6},450,450);w.advance(1);
    REQUIRE_FALSE(w.alive(bird));
    REQUIRE(std::find(w.combat_events().begin(),w.combat_events().end(),bird)!=w.combat_events().end());
    const auto replacement=bird_at(w,1,{1.5f,1.5f});
    REQUIRE(replacement.index()==bird.index());REQUIRE(replacement!=bird);
    REQUIRE(std::find(w.combat_events().begin(),w.combat_events().end(),replacement)==w.combat_events().end());
}

TEST_CASE("A profitable Phoenix raid actually removes AA and follows with a protected building", "[demo][phoenixraid]") {
    auto w=arena();const auto bird=bird_at(w,60);const auto flak=flak_at(w,1);
    const auto tower=w.place_bld(rts::BldType::Tower,{21,22},600,600);
    game::AttackerKnowledge knowledge;
    bool killed_flak=false;
    for(int tick=0;tick<1000 && w.alive(bird) && w.alive(tower);++tick) {
        if(tick%rts::kDecisionPeriodMax==0) {
            knowledge.observe(w.view(rts::Side::Attacker));
            const auto plan=game::choose_phoenix_raid(w.view(rts::Side::Attacker),knowledge,bird,400);
            REQUIRE(plan.target.has_value());
            const auto p=w.unit_pos(bird),goal=rts::center_of(*plan.target);
            const auto action=std::hypot(p.x-goal.x,p.y-goal.y)<=w.stats().of(rts::UnitType::Phoenix).range
                ? rts::UnitAction::AtkBld : fly(p,goal);
            w.submit_actions(rts::Side::Attacker,&action,1);
        }
        w.advance(1);
        if(!w.alive(flak)) killed_flak=true;
        if(!killed_flak) REQUIRE(w.bld_hp(tower)==600);
    }
    REQUIRE(killed_flak);REQUIRE_FALSE(w.alive(tower));REQUIRE(w.alive(bird));
}

TEST_CASE("Phoenix raid level matrix", "[.raidbench]") {
    std::cout<<"bird_level,flak_level,flak_count,estimated_damage,viable,destroyed_target,survived_escape,remaining_hp\n";
    for(const int bird_level:{1,10,30,60,90}) for(const int flak_level:{1,5,15,30})
        for(const int count:{1,2}) {
            auto w=arena();const auto flak=flak_at(w,flak_level);
            if(count==2) flak_at(w,flak_level,{19,22});
            const auto bird=bird_at(w,bird_level);
            const auto e=estimate(w,bird);
            if(e.viable) {
                game::AttackerKnowledge knowledge;
                for(int tick=0;tick<1500 && w.alive(bird);++tick) {
                    const auto p=w.unit_pos(bird);
                    if(!w.alive(flak) && p.x<4) break;
                    if(tick%rts::kDecisionPeriodMax==0) {
                        rts::UnitAction action;
                        if(!w.alive(flak)) action=fly(p,{2.5f,22.5f});
                        else {
                            knowledge.observe(w.view(rts::Side::Attacker));
                            const auto plan=game::choose_phoenix_raid(w.view(rts::Side::Attacker),knowledge,bird,400);
                            if(!plan.target) action=fly(p,{2.5f,22.5f});
                            else {
                                const auto goal=rts::center_of(*plan.target);
                                action=std::hypot(p.x-goal.x,p.y-goal.y)<=3.0f?rts::UnitAction::AtkBld:fly(p,goal);
                            }
                        }
                        w.submit_actions(rts::Side::Attacker,&action,1);
                    }
                    w.advance(1);
                }
                CAPTURE(bird_level,flak_level,count);
                REQUIRE_FALSE(w.alive(flak));REQUIRE(w.alive(bird));
                REQUIRE(w.unit_pos(bird).x<4);
            }
            std::cout<<bird_level<<','<<flak_level<<','<<count<<','<<e.expected_damage<<','<<e.viable
                <<','<<!w.alive(flak)<<','<<(w.alive(bird)&&w.unit_pos(bird).x<4)
                <<','<<(w.alive(bird)?w.unit_hp(bird):0)<<'\n';
        }
    SUCCEED();
}
