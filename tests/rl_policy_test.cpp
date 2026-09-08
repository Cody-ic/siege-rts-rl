#include <catch2/catch_test_macros.hpp>
#include <array>
#include <chrono>
#include <limits>
#include "game/rl_policy.hpp"
#include "game/save_game.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/batched_env.hpp"

TEST_CASE("Policy argmax obeys legal masks and rejects nonfinite outputs", "[rlpolicy]") {
    std::array<float,rts::kUnitActionCount*2> logits{};
    logits[12]=100;logits[1]=2;logits[2]=2;
    std::array<std::uint16_t,2> masks{7,1};
    const auto actions=game::TacticalPolicy::argmax(logits,masks);
    REQUIRE(static_cast<int>(actions[0])==1); // Stable tie break; illegal max excluded.
    REQUIRE(actions[1]==rts::UnitAction::Stop);
    masks[0]=0;
    REQUIRE_THROWS(game::TacticalPolicy::argmax(logits,masks));
    masks[0]=1;logits[12]=std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS(game::TacticalPolicy::argmax(logits,masks));
}

TEST_CASE("Policy loading checks metadata and batch sizes", "[rlpolicy]") {
    if(!game::TacticalPolicy::runtime_available()) {
        REQUIRE_THROWS(game::TacticalPolicy("missing.onnx",0));return;
    }
    const auto stats=game::StatsLoader::from_file(std::string(GAME_DATA_DIR)+"/stats_placeholder.json");
    const auto path=std::string(GAME_TESTDATA_DIR)+"/rl_constant.onnx";
    REQUIRE_THROWS(game::TacticalPolicy(path,stats.fingerprint()+1));
    game::TacticalPolicy policy(path,stats.fingerprint());
    REQUIRE(policy.ticks_per_step()==6);
    REQUIRE(policy.supports(rts::UnitType::Ghoul,1));
    REQUIRE_FALSE(policy.supports(rts::UnitType::Mason,1));
    REQUIRE_FALSE(policy.supports(rts::UnitType::Ghoul,1001));
    REQUIRE(policy.logits(0,{},{},{}).empty());
    REQUIRE_THROWS(policy.logits(1,{},{},{}));
    REQUIRE_THROWS(policy.logits(33,{},{},{}));
}

TEST_CASE("RL battle saves retain the same model for snapshot and replay recovery", "[rlpolicy]") {
    if(!game::TacticalPolicy::runtime_available()) return;
    const auto map_text=game::read_save_text(std::filesystem::path(GAME_DATA_DIR)/"demo_skirmish.json");
    const auto stats_text=game::read_save_text(std::filesystem::path(GAME_DATA_DIR)/"stats_placeholder.json");
    const auto stats=game::StatsLoader::from_string(stats_text);
    auto policy=std::make_shared<game::TacticalPolicy>(std::string(GAME_TESTDATA_DIR)+"/rl_constant.onnx",stats.fingerprint());
    game::GameShell shell(game::MapLoader::from_string(map_text),stats,77,policy);
    shell.apply(game::MenuAction::StartNew);
    shell.battle()->update(1000);
    REQUIRE(shell.battle()->learned_squads()>0);
    auto archive=game::capture_battle(shell,map_text,stats_text);
    REQUIRE(archive.tactical_policy_identity==policy->identity());
    const auto file=std::filesystem::temp_directory_path()/
        ("siege-rl-save-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
    struct Cleanup {
        std::filesystem::path file;
        ~Cleanup() {std::error_code ec;std::filesystem::remove(file,ec);}
    } cleanup{file};
    game::write_archive(file,archive);
    archive=game::read_archive(file);
    REQUIRE(archive.tactical_policy_identity==policy->identity());
    REQUIRE_THROWS(game::restore_battle(archive));
    auto restored=game::restore_battle(archive,{},policy);
    archive.snapshot.clear();
    auto replayed=game::restore_battle(archive,{},policy);
    for(int step=0;step<30;++step) {
        shell.battle()->update(6);restored->update(6);replayed->update(6);
        REQUIRE(shell.battle()->world().state_hash()==restored->world().state_hash());
        REQUIRE(shell.battle()->world().state_hash()==replayed->world().state_hash());
    }
    REQUIRE_THROWS(shell.battle()->set_tactical_policy({}));
}
