#include "rts/utf8_path.hpp"
#include <catch2/catch_test_macros.hpp>
#include "game/save_game.hpp"
#include "game/chronicle_reading.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "game/player_input.hpp"
#include "game/defender_macro.hpp"
#include <chrono>
#include "rts/utf8_path.hpp"
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

TEST_CASE("Release rejects and preserves v3 v4 and v5 branch saves", "[save]") {
    Temp temp; auto active=shell(); active.apply(game::MenuAction::StartNew);
    const auto archive=game::capture_battle(active,map_text(),stats_text());
    const auto file=temp.path/"campaign.json";
    for(const int version : {3,4,5,6,7,8}) {
        game::write_archive(file,archive);
        auto text=game::read_save_text(file);
        const auto key=text.find("\"version\"");
        REQUIRE(key!=std::string::npos);
        const auto begin=text.find_first_of("0123456789",key+9);
        REQUIRE(begin!=std::string::npos);
        const auto end=text.find_first_not_of("0123456789",begin);
        text.replace(begin,end-begin,std::to_string(version));
        { std::ofstream out(file); out << text; }
        const auto original=game::read_save_text(file);
        REQUIRE_THROWS(game::read_archive(file));
        game::preserve_incompatible_archive(file);
        const auto backup=temp.path/("campaign.json.legacy-v"+std::to_string(version)+"-"+std::to_string(archive.hash)+".json");
        REQUIRE(game::read_save_text(backup)==original);
        game::write_archive(file,archive);
        REQUIRE(game::read_archive(file).hash==archive.hash);
        REQUIRE(game::read_save_text(backup)==original);
    }
}

TEST_CASE("Policy save directories isolate controller combinations", "[savepath]") {
    const std::filesystem::path base="saves";
    REQUIRE(game::policy_save_directory(base,"","")==base);
    REQUIRE(game::policy_save_directory(base,"123","")==base/"rl"/"123");
    REQUIRE(game::policy_save_directory(base,"","456")==base/"rl"/"script"/"defender"/"456");
    REQUIRE(game::policy_save_directory(base,"123","456")==base/"rl"/"123"/"defender"/"456");
    REQUIRE(game::policy_save_directory(base,"123","456")!=game::policy_save_directory(base,"123","789"));
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


TEST_CASE("不死鸟花名册与冷却快照恢复后跨波一致", "[save]") {
    auto curve=game::WaveCurve{};curve.phoenix_from_wave=1;curve.phoenix_base=2;curve.phoenix_cap=2;
    curve.phoenix_respawn_waves=2;curve.phoenix_withdraw_hp_permille=777;
    game::WaveTiming timing;timing.first_build_ticks=1;timing.build_ticks=1;timing.assault_max_ticks=5;
    auto original=shell();
    original.adopt_saved_battle(game::DemoBattle(game::MapLoader::from_string(map_text()),
        game::StatsLoader::from_string(stats_text()),7,timing,curve),1,game::ChronicleChoice::None);
    auto& battle=*original.battle();
    std::vector<rts::UnitId> ids;battle.world().enumerate_units(rts::Side::Attacker,ids);
    for(auto id:ids) if(battle.world().unit_type(id)==rts::UnitType::Phoenix) {
        const_cast<rts::World&>(battle.world()).kill_unit(id);break;
    }
    for(int tick=0;tick<30 && battle.world().wave()<2;++tick) battle.update(1);
    REQUIRE(battle.world().wave()==2);
    REQUIRE(battle.phoenix_respawn().size()==1);
    REQUIRE(battle.phoenix_roster().size()==1);
    const auto archive=game::capture_battle(original,map_text(),stats_text());
    auto restored=game::restore_battle(archive);
    for(int tick=0;tick<40;++tick) {
        REQUIRE(restored->world().state_hash()==battle.world().state_hash());
        REQUIRE(restored->phoenix_respawn()==battle.phoenix_respawn());
        REQUIRE(restored->phoenix_roster().size()==battle.phoenix_roster().size());
        for(std::size_t i=0;i<battle.phoenix_roster().size();++i) {
            REQUIRE(restored->phoenix_roster()[i].id==battle.phoenix_roster()[i].id);
            REQUIRE(restored->phoenix_roster()[i].waves_alive==battle.phoenix_roster()[i].waves_alive);
        }
        battle.world().enumerate_units(rts::Side::Attacker,ids);
        for(auto id:ids) REQUIRE(restored->phoenix_identity(id)==battle.phoenix_identity(id));
        battle.update(1);restored->update(1);
    }
}

TEST_CASE("存档 v4 拒绝 v3 并保留独立旧版备份", "[save]") {
    Temp temp;auto active=shell();active.apply(game::MenuAction::StartNew);
    const auto archive=game::capture_battle(active,map_text(),stats_text());
    const auto file=temp.path/"campaign.json";game::write_archive(file,archive);
    auto text=game::read_save_text(file);
    const std::string marker="\"version\":"+std::to_string(game::kSaveVersion);
    const auto at=text.find(marker);REQUIRE(at!=std::string::npos);
    text.replace(at,marker.size(),"\"version\":3");
    {std::ofstream out(file);out<<text;}
    REQUIRE_THROWS(game::read_archive(file));
    game::preserve_incompatible_archive(file);
    const auto backup=temp.path/("campaign.json.legacy-v3-"+std::to_string(archive.hash)+".json");
    REQUIRE(game::read_save_text(backup)==text);
    game::write_archive(file,archive);
    REQUIRE(game::read_archive(file).hash==archive.hash);
    REQUIRE(game::read_save_text(backup)==text);
}

TEST_CASE("附录读权、分支选择和开发者隔离", "[save]") {
    REQUIRE(game::appendix_notice({},{true,false})==game::kAppendixNotices[0]);
    REQUIRE(game::appendix_notice({true,false},{true,true})==game::kAppendixNotices[1]);
    REQUIRE(game::appendix_notice({},{true,true})==game::kAppendixNotices[2]);
    REQUIRE(game::appendix_notice({true,true},{true,true}).empty());
    REQUIRE(game::appendix_notice({true,true},{}).empty());
    game::JournalProgress progress;
    progress.observe(59,true,game::ChronicleChoice::None,false);
    REQUIRE(progress.appendices.white_feather);
    REQUIRE_FALSE(progress.appendices.readable(1,progress.highest_wave));
    progress.observe(60,false,game::ChronicleChoice::None,false);
    REQUIRE(progress.appendices.readable(1,progress.highest_wave));
    progress.observe(89,false,game::ChronicleChoice::Guard,false);
    REQUIRE_FALSE(progress.appendices.loss_list);
    progress.observe(90,false,game::ChronicleChoice::Release,false);
    REQUIRE_FALSE(progress.appendices.loss_list);
    progress.observe(90,false,game::ChronicleChoice::Guard,true);
    REQUIRE_FALSE(progress.appendices.loss_list);
    progress.observe(90,false,game::ChronicleChoice::Guard,false);
    REQUIRE(progress.appendices.readable(2,90));
    progress.observe(1,false,game::ChronicleChoice::None,false);
    REQUIRE(progress.appendices.white_feather);
    REQUIRE(progress.appendices.loss_list);
    REQUIRE_FALSE(game::earns_white_feather(9));
    REQUIRE(game::earns_white_feather(10));
    game::JournalProgress developer;
    developer.observe(999,true,game::ChronicleChoice::Guard,true);
    REQUIRE(developer==game::JournalProgress{});
}

TEST_CASE("日记 v1 迁移不补发彩蛋，v2 合并持久化与备份", "[save]") {
    Temp temp;const auto file=temp.path/"journal.json";
    {std::ofstream out(file);out<<R"({"format":"siege-journal","version":1,"highest_wave":95})";}
    auto old=game::read_journal(file);
    REQUIRE(old.highest_wave==95);
    REQUIRE_FALSE(old.appendices.white_feather);
    REQUIRE_FALSE(old.appendices.loss_list);
    game::write_journal(file,{59,{true,false}});
    auto restored=game::read_journal(file);
    REQUIRE(restored.highest_wave==95);
    REQUIRE(restored.appendices.white_feather);
    game::write_journal(file,{90,{false,true}});
    restored=game::read_journal(file);
    REQUIRE(restored.appendices.white_feather);
    REQUIRE(restored.appendices.loss_list);
    game::write_journal_progress(file,1);
    REQUIRE(game::read_journal(file)==restored);
    game::write_journal(file,{96,{}});
    {std::ofstream out(file);out<<"broken";}
    REQUIRE(game::read_journal(file)==restored);
}

TEST_CASE("白羽判据按同一身份计数，击落重置且获得后存档保留", "[save]") {
    auto curve=game::WaveCurve{};curve.phoenix_from_wave=1;curve.phoenix_base=1;curve.phoenix_cap=1;
    // The small fixture has edge spawns: keep its army within the spawn ring.
    curve.slots_base=3;curve.slots_per_wave=0;curve.slots_cap=3;
    game::WaveTiming timing;timing.first_build_ticks=1;timing.build_ticks=1;timing.assault_max_ticks=5;
    auto original=shell();
    original.adopt_saved_battle(game::DemoBattle(game::MapLoader::from_string(map_text()),
        game::StatsLoader::from_string(stats_text()),7,timing,curve),1,game::ChronicleChoice::None);
    auto& battle=*original.battle();
    const auto until=[&](int wave) {
        for(int tick=0;tick<300 && battle.world().wave()<wave;++tick) battle.update(1);
        REQUIRE(battle.world().wave()==wave);
    };
    const auto kill_bird=[&]() {
        std::vector<rts::UnitId> ids;battle.world().enumerate_units(rts::Side::Attacker,ids);
        for(auto id:ids) if(battle.world().unit_type(id)==rts::UnitType::Phoenix) {
            const_cast<rts::World&>(battle.world()).kill_unit(id);return;
        }
        FAIL("expected living phoenix");
    };
    until(10);REQUIRE_FALSE(battle.earned_white_feather());
    SECTION("连续十波") {until(11);}
    SECTION("九波后击落不累计旧进度") {
        kill_bird();until(14);
        REQUIRE(battle.phoenix_roster()[0].waves_alive==0);
        until(23);REQUIRE_FALSE(battle.earned_white_feather());
        until(24);
    }
    REQUIRE(battle.earned_white_feather());
    kill_bird();battle.update(1);
    auto archive=game::capture_battle(original,map_text(),stats_text());
    auto restored=game::restore_battle(archive);
    REQUIRE(restored->earned_white_feather());
    for(int tick=0;tick<20;++tick) {
        battle.update(1);restored->update(1);
        REQUIRE(restored->world().state_hash()==battle.world().state_hash());
        REQUIRE(restored->earned_white_feather());
    }
}

TEST_CASE("Chronicle access hides unchosen endings and biography branches", "[save]") {
    using C=game::ChronicleChoice;
    REQUIRE_FALSE(game::can_read_ending(C::None,1));
    REQUIRE_FALSE(game::can_read_ending(C::None,2));
    REQUIRE(game::can_read_ending(C::Guard,1));
    REQUIRE_FALSE(game::can_read_ending(C::Guard,2));
    REQUIRE(game::can_read_ending(C::Release,2));
    REQUIRE_FALSE(game::can_read_ending(C::Release,1));
    const auto guard=game::smiler_for_choice(C::Guard);
    const auto release=game::smiler_for_choice(C::Release);
    const auto none=game::smiler_for_choice(C::None);
    REQUIRE(guard.find("他最幸福的时刻的表情，成了他永远的面具。")!=std::string::npos);
    REQUIRE(guard.find("他还得相信")!=std::string::npos);
    REQUIRE(guard.find("他哭了。") == std::string::npos);
    REQUIRE(release.find("他哭了。")!=std::string::npos);
    REQUIRE(release.find("他还得相信") == std::string::npos);
    REQUIRE(none.find("## 八") == std::string::npos);
    REQUIRE(guard.find("### 如果") == std::string::npos);
}

TEST_CASE("In-progress reconnaissance survives snapshot and transmits at the same tick", "[save]") {
    auto original=shell();original.apply(game::MenuAction::StartNew);
    auto& b=*original.battle();auto& w=const_cast<rts::World&>(b.world());
    std::vector<rts::UnitId> ids;w.enumerate_units(rts::Side::Attacker,ids);
    for(auto id:ids) w.kill_unit(id);
    const auto point=rts::center_of(w.keep_pos());
    w.spawn_unit(rts::UnitType::Ghoul,point,1,100000,100000);
    w.spawn_unit(rts::UnitType::Wraith,point,1,100000,100000);
    b.update(12);REQUIRE(b.recon_transmitting());REQUIRE_FALSE(b.wave_scouted());
    auto restored=game::restore_battle(game::capture_battle(original,map_text(),stats_text()));
    REQUIRE(restored->recon_transmitting());
    for(int i=0;i<80;++i) {
        b.update(1);restored->update(1);
        REQUIRE(b.world().state_hash()==restored->world().state_hash());
        REQUIRE(b.wave_scouted()==restored->wave_scouted());
    }
    REQUIRE(b.wave_scouted());
}

TEST_CASE("Scout gate navigation survives snapshots and operation-log recovery", "[save][scoutnav]") {
    const auto map_json = game::read_save_text(rts::path_from_utf8(GAME_DATA_DIR)/"maps/pool/gen_01012000.json");
    game::GameShell original(game::MapLoader::from_string(map_json), game::StatsLoader::from_string(stats_text()), 1);
    original.apply(game::MenuAction::StartNew);
    auto& battle = *original.battle();
    game::DefenderMacro macro(original.map());
    for (int tick = 0; tick < 307; ++tick) {
        if (tick % 20 == 0) {
            std::vector<rts::Command> commands;
            std::vector<game::UnitOrder> orders;
            macro.decide(battle.world(), commands, orders);
            battle.submit_defender(commands.data(), commands.size());
        }
        battle.update(1);
    } // Mid-decision, while the scout is en route.
    std::vector<rts::UnitId> ids;
    battle.world().enumerate_units(rts::Side::Defender, ids);
    REQUIRE(std::any_of(ids.begin(), ids.end(), [&](auto id) {return battle.world().unit_type(id)==rts::UnitType::Scout;}));
    auto archive = game::capture_battle(original, map_json, stats_text());
    int callbacks = 0;
    auto snapshot = game::restore_battle(archive, [&](auto,auto) {++callbacks;return true;});
    REQUIRE(callbacks == 1); // Must restore the snapshot, not silently replay it.
    archive.snapshot.clear();
    auto replay = game::restore_battle(archive);
    for (int tick = 0; tick < 250; ++tick) {
        battle.update(1); snapshot->update(1); replay->update(1);
        REQUIRE(battle.world().state_hash() == snapshot->world().state_hash());
        REQUIRE(battle.world().state_hash() == replay->world().state_hash());
        REQUIRE(battle.scout_outcome() == snapshot->scout_outcome());
    }
}
