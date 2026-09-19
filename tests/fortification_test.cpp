#include <catch2/catch_test_macros.hpp>
#include <array>
#include <limits>
#include <string>
#include <vector>
#include "game/player_input.hpp"
#include "game/map_loader.hpp"
#include "game/save_game.hpp"
#include "game/stats_loader.hpp"
#include "rts/combat_math.hpp"
#include "rts/utf8_path.hpp"
#include "rts/world_view.hpp"

namespace {
rts::StatsTable fortification_stats() {
    return game::StatsLoader::from_file(std::string(GAME_DATA_DIR) + "/stats_placeholder.json");
}
rts::WorldInit fortification_arena(bool instant) {
    rts::WorldInit init;
    init.width = 24;
    init.height = 12;
    init.terrain.assign(288, rts::Terrain::Plain);
    init.keep = {1, 1};
    init.seed = 17;
    init.map_id = "fortification-regression";
    init.stats = fortification_stats();
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Keep)].upgrade_ticks = 0;
    if (instant) for (auto& bld : init.stats.bld) bld.upgrade_ticks = 0;
    init.buildings.push_back({rts::BldType::Keep, init.keep, 2400, 2400});
    return init;
}
void unlock(rts::World& w, int level) {
    w.set_stock(rts::Resource::Stone, 1000000);
    w.set_stock(rts::Resource::Wood, 1000000);
    const auto command = game::upgrade_command(w.keep_pos(), w.width());
    while (w.building_level_cap() < level) {
        w.submit(rts::Side::Defender, &command, 1);
        w.advance(1);
    }
}
void upgrade(rts::World& w, rts::GridPos cell) {
    const auto command = game::upgrade_command(cell, w.width());
    w.submit(rts::Side::Defender, &command, 1);
    w.advance(1);
}
}

TEST_CASE("Fortifications retain linear gains then restart a continuous square root", "[econ][fortification]") {
    rts::World w(fortification_arena(true));
    unlock(w, 31);
    const std::array types{rts::BldType::Wall, rts::BldType::Gate, rts::BldType::Fence,
                           rts::BldType::Tower, rts::BldType::Barrack};
    std::vector<rts::BldId> ids;
    for (std::size_t i = 0; i < types.size(); ++i) {
        const auto hp = w.stats().of(types[i]).max_hp;
        ids.push_back(w.place_bld(types[i], {static_cast<std::int16_t>(5 + 3*i), 6}, hp, hp));
    }
    for (int level = 1; level <= 31; ++level) {
        const auto v = w.view(rts::Side::Defender);
        CAPTURE(level);
        for (std::size_t i = 0; i < ids.size(); ++i) {
            REQUIRE(w.bld_level(ids[i]) == level);
            REQUIRE(v.bld_max_hp()[ids[i].index()] == w.stats().building_max_hp(types[i], level));
            REQUIRE(w.bld_hp(ids[i]) == v.bld_max_hp()[ids[i].index()]);
        }
        const auto hp = w.bld_hp(ids[0]);
        if (level <= 20) REQUIRE(hp == 1200 + 120 * (level - 1));
        if (level == 21) REQUIRE(hp == 3600); // First tail increment is 120, not 125.
        if (level == 22) REQUIRE(hp == 3709);
        if (level == 30) REQUIRE(hp == 4392);
        // Existing units and other buildings still use the original square root.
        const auto original = rts::level_permille(level, 220);
        REQUIRE(w.bld_hp(ids[3]) == rts::apply_permille(600, {original}));
        REQUIRE(w.bld_hp(ids[4]) == rts::apply_permille(w.stats().of(types[4]).max_hp, {original}));
        REQUIRE(w.bld_hp(ids[1]) < hp); // The gate remains the weaker opening.
        if (level < 31) for (const auto id : ids) {
            const auto stone = w.stock(rts::Resource::Stone);
            const auto wood = w.stock(rts::Resource::Wood);
            upgrade(w, v.bld_pos()[id.index()]);
            if (id == ids[0] && level >= 29) {
                REQUIRE(stone - w.stock(rts::Resource::Stone) == 81);
                REQUIRE(wood - w.stock(rts::Resource::Wood) == 27);
            }
        }
    }
}

TEST_CASE("Fortification prices slow after 20 and freeze at the cost of reaching 30", "[econ][fortification]") {
    rts::World w(fortification_arena(true));
    struct Price {int from; std::int64_t stone, wood;};
    for (const auto p : {Price{1,12,4}, Price{18,63,21}, Price{19,66,22},
                        Price{20,67,22}, Price{21,69,23}, Price{28,79,26},
                        Price{29,81,27}, Price{30,81,27}, Price{100,81,27},
                        Price{std::numeric_limits<std::int32_t>::max(),81,27}}) {
        CAPTURE(p.from);
        REQUIRE(w.bld_upgrade_cost_stone(rts::BldType::Wall, p.from) == p.stone);
        REQUIRE(w.bld_upgrade_cost_wood(rts::BldType::Wall, p.from) == p.wood);
        REQUIRE(w.bld_upgrade_cost_stone(rts::BldType::Fence, p.from) == 0);
    }
    REQUIRE(w.bld_upgrade_cost_stone(rts::BldType::Gate, 100) == 54);
    REQUIRE(w.bld_upgrade_cost_wood(rts::BldType::Gate, 100) == 81);
    REQUIRE(w.bld_upgrade_cost_stone(rts::BldType::Tower, 100) >
            w.bld_upgrade_cost_stone(rts::BldType::Tower, 30));
    REQUIRE(w.bld_upgrade_cost_stone(rts::BldType::Keep, 100) == 200);
}

TEST_CASE("Budgeted wall upgrades require workers and preserve damage before paid repair", "[input][fortification]") {
    rts::World w(fortification_arena(false));
    unlock(w, 2);
    const auto first = w.place_bld(rts::BldType::Wall, {5, 5}, 600, 1200);
    const auto second = w.place_bld(rts::BldType::Wall, {6, 5}, 1200, 1200);
    w.set_stock(rts::Resource::Stone, 12);
    w.set_stock(rts::Resource::Wood, 4);
    const std::array cells{rts::GridPos{6,5}, rts::GridPos{5,5}, rts::GridPos{5,5}};
    auto batch = game::plan_upgrades(w.view(rts::Side::Defender), cells);
    REQUIRE(batch.commands.size() == 1);
    REQUIRE(batch.stone == 12);
    REQUIRE(batch.wood == 4);
    w.submit(rts::Side::Defender, batch.commands.data(), batch.commands.size());
    w.advance(100);
    REQUIRE(w.bld_level(first) == 1);
    REQUIRE(w.bld_upgrade_left(first) == 50);
    REQUIRE(w.bld_hp(first) == 600);
    REQUIRE(w.view(rts::Side::Defender).stock()[static_cast<std::size_t>(rts::Resource::Stone)] == 0);
    REQUIRE(w.view(rts::Side::Defender).stock()[static_cast<std::size_t>(rts::Resource::Wood)] == 0);
    w.spawn_unit(rts::UnitType::Mason, {5.5f,6.5f}, 1, 120, 120);
    w.advance(50);
    REQUIRE(w.bld_level(first) == 2);
    REQUIRE(w.bld_hp(first) == 660);
    REQUIRE(w.bld_level(second) == 1);
    REQUIRE(w.view(rts::Side::Defender).bld_max_hp()[first.index()] == 1320);
    w.set_stock(rts::Resource::Wood, 10);
    const auto repair = game::repair_command({5,5}, w.width());
    REQUIRE(game::repair_wood_cost(w.view(rts::Side::Defender), {5,5}) == 10);
    w.submit(rts::Side::Defender, &repair, 1);
    w.advance(200);
    REQUIRE(w.bld_hp(first) == 1320);
    REQUIRE(w.view(rts::Side::Defender).stock()[static_cast<std::size_t>(rts::Resource::Wood)] == 0);
}

TEST_CASE("Actual Ram strikes use the upgraded wall HP through both growth stages", "[mech][fortification]") {
    struct Expected {int level, strikes;};
    for (const auto expected : {Expected{1,2}, Expected{2,3}, Expected{5,3},
                               Expected{10,4}, Expected{15,5}, Expected{20,6},
                               Expected{21,6}, Expected{30,7}}) {
        CAPTURE(expected.level);
        rts::World w(fortification_arena(true));
        unlock(w, expected.level);
        const auto wall = w.place_bld(rts::BldType::Wall, {10,6}, 1200, 1200);
        for (int level = 1; level < expected.level; ++level) upgrade(w, {10,6});
        w.spawn_unit(rts::UnitType::Ram, {11.5f,6.5f}, 99, 10000, 10000);
        const auto attack = rts::UnitAction::AtkWall;
        w.submit_actions(rts::Side::Attacker, &attack, 1);
        int strikes = 0;
        int elapsed = 0;
        while (w.alive(wall) && elapsed < 2000) {
            const auto before = w.bld_hp(wall);
            w.advance(1);
            ++elapsed;
            if (!w.alive(wall) || w.bld_hp(wall) < before) ++strikes;
        }
        REQUIRE_FALSE(w.alive(wall));
        REQUIRE(strikes == expected.strikes);
        REQUIRE(elapsed == 31 + 60 * (expected.strikes - 1));
    }
}

TEST_CASE("Fortification parameters are required validated and fingerprinted independently", "[stats][fortification]") {
    const auto file = rts::path_from_utf8(GAME_DATA_DIR) / "stats_placeholder.json";
    const auto text = game::read_save_text(file);
    const auto baseline = fortification_stats();
    const auto mutate = [&](const std::string& old, const std::string& replacement) {
        auto changed = text;
        const auto at = changed.find(old);
        REQUIRE(at != std::string::npos);
        changed.replace(at, old.size(), replacement);
        return changed;
    };
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(mutate("stats/14", "stats/13")), game::StatsFormatError);
    for (const auto value : {-1, 1001}) {
        REQUIRE_THROWS_AS(game::StatsLoader::from_string(mutate(
            "\"fortification_hp_permille_per_level\": 100",
            "\"fortification_hp_permille_per_level\": " + std::to_string(value))), game::StatsFormatError);
    }
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(mutate(
        "\"fortification_linear_until_level\": 20", "\"fortification_linear_until_level\": 1")), game::StatsFormatError);
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(mutate(
        "\"fortification_price_cap_level\": 30", "\"fortification_price_cap_level\": 19")), game::StatsFormatError);
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(mutate(
        "\"fortification_price_step_permille\": 50", "\"fortification_price_step_permille\": 101")), game::StatsFormatError);
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(mutate(
        "    \"fortification_hp_permille_per_level\": 100,", "")), game::StatsFormatError);
    const auto changed = game::StatsLoader::from_string(mutate(
        "\"fortification_hp_permille_per_level\": 100", "\"fortification_hp_permille_per_level\": 98"));
    REQUIRE(changed.fingerprint() != baseline.fingerprint());
    REQUIRE(changed.building_max_hp(rts::BldType::Wall, 5) != baseline.building_max_hp(rts::BldType::Wall, 5));
    REQUIRE(changed.building_max_hp(rts::BldType::Tower, 5) == baseline.building_max_hp(rts::BldType::Tower, 5));
    for (const auto field : {&rts::GlobalStats::fortification_linear_until_level,
                             &rts::GlobalStats::fortification_price_step_permille,
                             &rts::GlobalStats::fortification_price_cap_level}) {
        auto variant = baseline;
        ++(variant.global.*field);
        REQUIRE(variant.fingerprint() != baseline.fingerprint());
    }
}

TEST_CASE("Upgraded walls cross growth and price boundaries identically after snapshot or replay", "[save][fortification]") {
    const auto data = rts::path_from_utf8(GAME_DATA_DIR);
    const auto map_text = game::read_save_text(data / "demo_skirmish.json");
    auto stats_text = game::read_save_text(data / "stats_placeholder.json");
    // Only remove preparation costs/time so the test reaches the boundaries
    // through real player commands, without unrecorded world mutations.
    for (const auto type : {"Keep", "Wall"}) for (const auto key : {
            "upgrade_cost_stone", "upgrade_cost_wood", "upgrade_ticks"}) {
        const auto section = stats_text.find(std::string("\"") + type + "\": {");
        REQUIRE(section != std::string::npos);
        const auto field = stats_text.find(std::string("\"") + key + "\":", section);
        REQUIRE(field != std::string::npos);
        const auto begin = stats_text.find_first_of("0123456789", field + std::string(key).size() + 3);
        const auto end = stats_text.find_first_not_of("0123456789", begin);
        stats_text.replace(begin, end - begin, "0");
    }
    game::GameShell shell(game::MapLoader::from_string(map_text),
                           game::StatsLoader::from_string(stats_text), 17);
    shell.apply(game::MenuAction::StartNew);
    auto& battle = *shell.battle();
    rts::GridPos wall{};
    bool found = false;
    const auto view = battle.world().view(rts::Side::Defender);
    for (std::size_t i = 0; i < view.bld_type().size(); ++i) {
        if (view.bld_alive()[i] && view.bld_type()[i] == rts::BldType::Wall) {
            wall = view.bld_pos()[i]; found = true; break;
        }
    }
    REQUIRE(found);
    const auto keep_up = game::upgrade_command(battle.world().keep_pos(), view.width());
    while (battle.world().building_level_cap() < 31) {
        battle.submit_defender(&keep_up, 1); battle.update(1);
    }
    const auto wall_up = game::upgrade_command(wall, view.width());
    for (int level = 1; level < 19; ++level) {
        battle.submit_defender(&wall_up, 1); battle.update(1);
    }
    battle.submit_defender(&wall_up, 1); // Save with the level-20 upgrade still queued.
    auto archive = game::capture_battle(shell, map_text, stats_text);
    auto snapshot = game::restore_battle(archive);
    archive.snapshot.clear();
    auto replay = game::restore_battle(archive);
    for (int level = 20; level <= 31; ++level) {
        battle.update(1); snapshot->update(1); replay->update(1);
        REQUIRE(battle.world().bld_level(battle.world().bld_at(wall)) == level);
        REQUIRE(battle.world().state_hash() == snapshot->world().state_hash());
        REQUIRE(battle.world().state_hash() == replay->world().state_hash());
        if (level < 31) {
            battle.submit_defender(&wall_up, 1);
            snapshot->submit_defender(&wall_up, 1);
            replay->submit_defender(&wall_up, 1);
        }
    }
}
