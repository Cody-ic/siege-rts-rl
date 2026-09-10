#include "rts/utf8_path.hpp"
#include <catch2/catch_test_macros.hpp>
#include "game/save_game.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "game/player_input.hpp"
#include "game/defender_macro.hpp"
#include <chrono>
#include <fstream>
namespace {
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("siege-save-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){std::filesystem::create_directories(path);}
    ~Temp(){std::error_code ec;for(const auto& entry:std::filesystem::directory_iterator(path))std::filesystem::remove(entry.path(),ec);std::filesystem::remove(path,ec);}
};
std::string map_text(){return game::read_save_text(rts::path_from_utf8(GAME_DATA_DIR)/"demo_skirmish.json");}
std::string stats_text(){return game::read_save_text(rts::path_from_utf8(GAME_DATA_DIR)/"stats_placeholder.json");}
game::GameShell shell(){return {game::MapLoader::from_string(map_text()),game::StatsLoader::from_string(stats_text()),7};}
}
TEST_CASE("Forced work survives snapshots and operation-log recovery", "[save]") {
    auto original=shell(); original.apply(game::MenuAction::StartNew);
    auto& battle=*original.battle();
    const auto view=battle.world().view(rts::Side::Defender);
    rts::GridPos target{};
    bool found=false;
    for(int y=0;y<view.height() && !found;++y) for(int x=0;x<view.width() && !found;++x) {
        const rts::GridPos p{static_cast<std::int16_t>(x),static_cast<std::int16_t>(y)};
        if(game::can_place_hint(view,rts::BldType::Wall,p)) {target=p;found=true;}
    }
    REQUIRE(found);
    const auto command=game::build_command(rts::BldType::Wall,target,view.width());
    battle.submit_defender(&command,1); battle.update(1);
    std::vector<rts::UnitId> ids; battle.world().enumerate_units(rts::Side::Defender,ids);
    REQUIRE(battle.issue_forced_work(ids,target));
    auto archive=game::capture_battle(original,map_text(),stats_text());
    auto snapshot=game::restore_battle(archive);
    archive.snapshot.clear();
    auto replay=game::restore_battle(archive);
    bool has_forced=false;
    for(const auto id:ids) if(battle.forced_work_active(id)) {
        has_forced=true;
        REQUIRE(snapshot->forced_work_active(id));
        REQUIRE(replay->forced_work_active(id));
    }
    REQUIRE(has_forced);
    for(int i=0;i<20;++i) {
        battle.update(10);snapshot->update(10);replay->update(10);
        REQUIRE(battle.world().state_hash()==snapshot->world().state_hash());
        REQUIRE(battle.world().state_hash()==replay->world().state_hash());
    }
}

TEST_CASE("存档还原临时指令、待执行命令、战斗及后续确定性", "[save]") {
    Temp temp;auto original=shell();original.apply(game::MenuAction::StartNew);
    auto& battle=*original.battle();battle.update(2);
    std::vector<rts::UnitId> ids;battle.world().enumerate_units(rts::Side::Defender,ids);
    REQUIRE_FALSE(ids.empty());
    battle.issue_move_order(ids,battle.world().keep_pos());
    battle.update(30);
    battle.issue_garrison_order(ids,{3,3});
    rts::Command summon;summon.kind=rts::CommandKind::Summon;
    battle.submit_defender(&summon,1);battle.update(20);
    // 刻意在 advance 之前保存命令队列，读档不能漏掉这一帧刚点击的操作。
    const auto train=game::train_command(rts::UnitType::Archer,1,battle.world().keep_pos(),battle.world().width());
    battle.submit_defender(&train,1);
    const auto archive=game::capture_battle(original,map_text(),stats_text());
    const auto file=temp.path/"campaign.json";game::write_archive(file,archive);
    auto restored=game::restore_battle(game::read_archive(file));
    REQUIRE(restored->world().state_hash()==battle.world().state_hash());
    REQUIRE(restored->player_events().size()==battle.player_events().size());
    for(int step=0;step<40;++step) {
        battle.update(20);restored->update(20);
        REQUIRE(restored->world().state_hash()==battle.world().state_hash());
        REQUIRE(restored->build_ticks_left()==battle.build_ticks_left());
        REQUIRE(restored->scout_outcome()==battle.scout_outcome());
    }
    auto resumed=shell();resumed.adopt_saved_battle(std::move(*restored),archive.attempt,archive.choice);
    REQUIRE(resumed.screen()==game::Screen::Main);
    resumed.apply(game::MenuAction::Resume);REQUIRE(resumed.should_advance());
}
TEST_CASE("日记跨进程式重读不倒退且有备份", "[save]") {
    Temp temp;const auto file=temp.path/"journal.json";
    REQUIRE(game::read_journal_progress(file)==1);
    game::write_journal_progress(file,40);
    REQUIRE(game::read_journal_progress(file)==40);
    game::write_journal_progress(file,10);
    REQUIRE(game::read_journal_progress(file)==40);
    game::write_journal_progress(file,70);
    REQUIRE(game::read_journal_progress(file)==70);
    std::ofstream(file)<<"broken";
    REQUIRE(game::read_journal_progress(file)==40);
    game::write_journal_progress(file,70);
    REQUIRE(game::read_journal_progress(file)==70);
    REQUIRE(game::read_journal_progress(temp.path/"journal.json.bak")==40);
    std::filesystem::remove(file);
    REQUIRE(game::read_journal_progress(file)==40);
}
TEST_CASE("跨多波战斗与宏观操作恢复后仍继续一致", "[save]") {
    Temp temp;
    const auto text=game::read_save_text(rts::path_from_utf8(GAME_DATA_DIR)/"maps/pool/gen_01001000.json");
    const auto map=game::MapLoader::from_string(text);
    game::GameShell original(map,game::StatsLoader::from_string(stats_text()),1);
    original.apply(game::MenuAction::StartNew);
    auto& battle=*original.battle();game::DefenderMacro macro(map);
    std::vector<rts::Command> commands;std::vector<game::UnitOrder> orders;
    for(int tick=0;tick<10000 && !battle.defeated();++tick) {
        if(tick%20==0) {commands.clear();orders.clear();macro.decide(battle.world(),commands,orders);battle.submit_defender(commands.data(),commands.size());}
        for(const auto& order:orders) {
            if(order.garrison) battle.issue_garrison_order(order.ids,order.target);
            else battle.issue_move_order(order.ids,order.target);
        }
        battle.update(1);
    }
    INFO("tick="<<battle.world().now()<<" defeated="<<battle.defeated());
    REQUIRE(battle.world().wave()>=4);
    const auto file=temp.path/"campaign.json";
    game::write_archive(file,game::capture_battle(original,text,stats_text()));
    auto saved=game::read_archive(file);
    const auto begin=std::chrono::steady_clock::now();
    int snapshot_steps=0;
    auto restored=game::restore_battle(saved,[&](auto current,auto total){++snapshot_steps;CHECK(current==total);return true;});
    REQUIRE(snapshot_steps==1);
    const auto snapshot_done=std::chrono::steady_clock::now();
    auto replay_archive=saved;replay_archive.snapshot.clear();
    auto replayed=game::restore_battle(replay_archive);
    const auto replay_done=std::chrono::steady_clock::now();
    std::printf("SAVE_BENCH ticks=%d wave=%d snapshot_ms=%.2f replay_ms=%.2f bytes=%zu\n",int(saved.tick),saved.wave,
        std::chrono::duration<double,std::milli>(snapshot_done-begin).count(),
        std::chrono::duration<double,std::milli>(replay_done-snapshot_done).count(),saved.snapshot.size());
    REQUIRE(replayed->world().state_hash()==restored->world().state_hash());

    for(int step=0;step<20;++step) {
        battle.update(20);restored->update(20);
        REQUIRE(restored->world().state_hash()==battle.world().state_hash());
        REQUIRE(restored->build_ticks_left()==battle.build_ticks_left());
    }
}
TEST_CASE("损坏与取消读档不能替换当前对局", "[save]") {
    Temp temp;auto active=shell();active.apply(game::MenuAction::StartNew);active.battle()->update(300);
    auto archive=game::capture_battle(active,map_text(),stats_text());
    const auto hash=active.battle()->world().state_hash();
    SECTION("取消恢复") { REQUIRE_THROWS(game::restore_battle(archive,[](auto,auto){return false;})); }
    SECTION("末态哈希错误") {++archive.hash;REQUIRE_THROWS(game::restore_battle(archive));}
    SECTION("乱序事件") {
        archive.events.push_back({archive.tick+1,0,{},{},{}});
        REQUIRE_THROWS(game::restore_battle(archive));
    }
    SECTION("不存在的父目录是文件") {
        const auto blocked=temp.path/"blocked";std::ofstream(blocked)<<"x";
        REQUIRE_THROWS(game::write_archive(blocked/"campaign.json",archive));
    }
    SECTION("截断文件") {
        const auto file=temp.path/"campaign.json";game::write_archive(file,archive);
        game::write_archive(file,archive);std::ofstream(file)<<"{";
        REQUIRE_THROWS(game::read_archive(file));
        REQUIRE(game::restore_battle(game::read_archive(temp.path/"campaign.json.bak"))->world().state_hash()==hash);
    }
    SECTION("版本备份不随新存档轮换而丢失") {
        const auto file=temp.path/"campaign.json";
        game::write_archive(file,archive);
        game::preserve_incompatible_archive(file);
        game::preserve_incompatible_archive(file);
        const auto backup=temp.path/("campaign.json.legacy-v"+std::to_string(game::kSaveVersion)+"-"+std::to_string(hash)+".json");
        auto newer=shell();newer.apply(game::MenuAction::StartNew);newer.battle()->update(310);
        game::write_archive(file,game::capture_battle(newer,map_text(),stats_text()));
        REQUIRE(game::read_archive(backup).hash==hash);
        REQUIRE(game::read_archive(file).hash!=hash);
    }
    REQUIRE(active.battle()->world().state_hash()==hash);
}
TEST_CASE("完整快照直接恢复而非从头重演，损坏快照退回操作日志", "[save]") {
    auto active=shell();active.apply(game::MenuAction::StartNew);active.battle()->update(600);
    auto archive=game::capture_battle(active,map_text(),stats_text());
    REQUIRE_FALSE(archive.snapshot.empty());int callbacks=0;
    auto restored=game::restore_battle(archive,[&](auto current,auto total){++callbacks;CHECK(current==total);return true;});
    REQUIRE(callbacks==1);
    REQUIRE(restored->world().state_hash()==active.battle()->world().state_hash());
    archive.snapshot="broken";callbacks=0;
    restored=game::restore_battle(archive,[&](auto,auto){++callbacks;return true;});
    REQUIRE(callbacks>1);REQUIRE(restored->world().state_hash()==active.battle()->world().state_hash());
    active.battle()->enable_developer();REQUIRE_THROWS(game::capture_battle(active,map_text(),stats_text()));
}

TEST_CASE("剧情按本局十波里程碑展示，开发者模式不触发", "[save]") {
    REQUIRE(game::chronicle_to_present(0,1,false)==0);
    REQUIRE(game::chronicle_to_present(9,10,false)==1);
    REQUIRE(game::chronicle_to_present(10,10,false)==-1);
    REQUIRE(game::chronicle_to_present(69,70,false)==7);
    REQUIRE(game::chronicle_to_present(0,70,true)==-1);
    auto active=shell();active.apply(game::MenuAction::StartNew);active.battle()->enable_developer();active.battle()->developer_wave(70);
    REQUIRE(active.should_advance());
}
