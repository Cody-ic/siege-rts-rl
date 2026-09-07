#include "game/save_game.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/replay.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace game {
namespace {
using Json=nlohmann::json;
constexpr rts::Tick kMaxSaveTicks=2000000; // 27.7 小时仿真，限制损坏文件的恢复成本
constexpr std::size_t kMaxEvents=200000;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void atomic_json(const std::filesystem::path& file,const Json& data) {
    if(!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path());
    auto tmp=file;tmp+=".tmp";
    {
        std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
        require(static_cast<bool>(out),"无法写入存档目录");
        out<<data.dump();out.flush();require(static_cast<bool>(out),"存档写入失败，旧档未替换");
    }
    // 保留上一次完整文件；临时文件绝不作为可读存档。
    if(std::filesystem::exists(file) && !Json::parse(read_save_text(file),nullptr,false).is_discarded()) {
        auto backup=file;backup+=".bak";
        std::filesystem::copy_file(file,backup,std::filesystem::copy_options::overwrite_existing);
    }
#ifdef _WIN32
    require(MoveFileExW(tmp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,
            "替换存档失败，旧档保留");
#else
    std::filesystem::rename(tmp,file);
#endif
}
Json load_json(const std::filesystem::path& file) {return Json::parse(read_save_text(file));}
}
std::string read_save_text(const std::filesystem::path& file) {
    require(std::filesystem::file_size(file)<=64u*1024u*1024u,"存档文件过大");
    std::ifstream input(file,std::ios::binary);
    require(static_cast<bool>(input),"无法读取存档");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
std::filesystem::path default_save_directory() {
#ifdef _WIN32
    const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",nullptr,0);
    if(length>1) {
        std::wstring base(length,L'\0');
        const DWORD copied=GetEnvironmentVariableW(L"LOCALAPPDATA",base.data(),length);
        if(copied>0 && copied<length) {base.resize(copied);return std::filesystem::path(base)/L"SiegeRTS"/L"saves";}
    }
#else
    if(const auto* base=std::getenv("XDG_DATA_HOME")) return std::filesystem::path(base)/"siege-rts"/"saves";
    if(const auto* home=std::getenv("HOME")) return std::filesystem::path(home)/".local/share/siege-rts/saves";
#endif
    throw std::runtime_error("无法定位用户存档目录");
}
BattleArchive capture_battle(const GameShell& shell,std::string map_json,std::string stats_json) {
    require(shell.battle()!=nullptr,"没有可保存的对局");
    const auto& battle=*shell.battle();
    require(battle.world().now()<=kMaxSaveTicks && battle.player_events().size()<=kMaxEvents,"对局超出当前存档容量");
    BattleArchive result;
    result.map_json=std::move(map_json);result.stats_json=std::move(stats_json);
    result.seed=battle.world().seed();result.hash=battle.world().state_hash();result.tick=battle.world().now();
    result.wave=battle.world().wave();result.attempt=shell.attempt();result.choice=shell.chronicle().choice();
    result.events=battle.player_events();return result;
}
std::unique_ptr<DemoBattle> restore_battle(const BattleArchive& a,const RestoreProgress& progress) {
    require(a.tick>=0 && a.tick<=kMaxSaveTicks && a.events.size()<=kMaxEvents,"存档超出恢复范围");
    const auto map=MapLoader::from_string(a.map_json);
    auto battle=std::make_unique<DemoBattle>(map,StatsLoader::from_string(a.stats_json),a.seed);
    rts::Tick previous=0;
    const auto advance_to=[&](rts::Tick target) {
        while(battle->world().now()<target) {
            require(!battle->defeated(),"存档在败局之后仍有操作");
            const auto remaining=target-battle->world().now();
            battle->update(static_cast<int>(std::min<rts::Tick>(remaining,200)));
            if(progress && !progress(battle->world().now(),a.tick)) throw std::runtime_error("已取消恢复");
        }
    };
    for(const auto& event:a.events) {
        require(event.tick>=previous && event.tick<=a.tick,"存档操作时间无效");
        advance_to(event.tick);previous=event.tick;
        if(event.kind==0) battle->submit_defender(&event.command,1);
        else if(event.kind==1) battle->issue_move_order(event.ids,event.target);
        else if(event.kind==2) battle->issue_garrison_order(event.ids,event.target);
        else throw std::runtime_error("存档操作类型无效");
    }
    advance_to(a.tick);
    require(battle->world().state_hash()==a.hash && battle->world().wave()==a.wave,"存档校验不一致，未载入此局");
    require(a.attempt>0 && a.attempt<1000000,"存档局次无效");
    require(a.choice==ChronicleChoice::None || (a.wave>=70 && (a.choice==ChronicleChoice::Guard || a.choice==ChronicleChoice::Release)),"存档结局无效");
    return battle;
}
void write_archive(const std::filesystem::path& file,const BattleArchive& a) {
    Json events=Json::array();
    for(const auto& e:a.events) {
        Json ids=Json::array();
        for(const auto id:e.ids) ids.push_back({id.index(),id.generation()});
        events.push_back({{"tick",e.tick},{"kind",e.kind},{"cmd",{e.command.slot,static_cast<int>(e.command.kind),static_cast<int>(e.command.side),e.command.what,e.command.level}},
                          {"ids",ids},{"target",{e.target.i,e.target.j}}});
    }
    atomic_json(file,{{"format","siege-save"},{"version",kSaveVersion},{"platform",rts::kPlatformFingerprint},
        {"world",rts::kWorldHashTag},{"map",a.map_json},{"stats",a.stats_json},{"seed",a.seed},{"hash",a.hash},
        {"tick",a.tick},{"wave",a.wave},{"attempt",a.attempt},{"choice",static_cast<int>(a.choice)},{"events",events}});
}
void preserve_incompatible_archive(const std::filesystem::path& file) {
    const auto j=load_json(file);
    require(j.at("format")=="siege-save","不是对局存档");
    const int version=j.at("version").get<int>();
    const auto hash=j.at("hash").get<std::uint64_t>();
    auto backup=file;
    backup+=".legacy-v"+std::to_string(version)+"-"+std::to_string(hash)+".json";
    // 永不覆盖：同一旧档重复启动只保留一份，新快照按状态哈希另存。
    std::filesystem::copy_file(file,backup,std::filesystem::copy_options::skip_existing);
}

BattleArchive read_archive(const std::filesystem::path& file) {
    const auto j=load_json(file);
    require(j.at("format")=="siege-save" && j.at("version")==kSaveVersion,"存档版本不兼容");
    require(j.at("platform").get<std::string>()==rts::kPlatformFingerprint && j.at("world").get<std::string>()==rts::kWorldHashTag,"存档平台或仿真版本不兼容");
    BattleArchive a;
    a.map_json=j.at("map").get<std::string>();a.stats_json=j.at("stats").get<std::string>();
    a.seed=j.at("seed").get<std::uint64_t>();a.hash=j.at("hash").get<std::uint64_t>();
    a.tick=j.at("tick").get<rts::Tick>();a.wave=j.at("wave").get<int>();a.attempt=j.at("attempt").get<int>();
    const int choice=j.at("choice").get<int>();require(choice>=0 && choice<=2,"存档结局类型无效");
    a.choice=static_cast<ChronicleChoice>(choice);
    const auto& events=j.at("events");require(events.is_array() && events.size()<=kMaxEvents,"存档操作过多");
    for(const auto& row:events) {
        DemoBattle::PlayerEvent e;e.tick=row.at("tick").get<rts::Tick>();e.kind=row.at("kind").get<int>();
        const auto& c=row.at("cmd");require(c.is_array() && c.size()==5,"存档命令格式无效");
        const int slot=c[0].get<int>(),kind=c[1].get<int>(),side=c[2].get<int>(),what=c[3].get<int>(),level=c[4].get<int>();
        require(slot>=0 && slot<=65535 && kind>=0 && kind<rts::kCommandKindCount && side==static_cast<int>(rts::Side::Defender) && what>=0 && what<=255 && level>=1 && level<=255,"存档命令取值无效");
        e.command.slot=static_cast<std::uint16_t>(slot);e.command.kind=static_cast<rts::CommandKind>(kind);
        e.command.what=static_cast<std::uint8_t>(what);e.command.level=static_cast<std::uint8_t>(level);
        const auto& target=row.at("target");require(target.is_array() && target.size()==2,"存档目标格式无效");
        const int x=target[0].get<int>(),y=target[1].get<int>();
        require(x>=0 && y>=0 && x<=32767 && y<=32767,"存档目标越界");
        e.target={static_cast<std::int16_t>(x),static_cast<std::int16_t>(y)};
        const auto& ids=row.at("ids");require(ids.is_array() && ids.size()<=65535,"存档单位列表无效");
        for(const auto& id:ids) {
            require(id.is_array() && id.size()==2,"存档单位格式无效");
            const int index=id[0].get<int>(),generation=id[1].get<int>();
            require(index>=0 && index<65535 && generation>=0 && generation<=65535,"存档单位句柄无效");
            e.ids.push_back(rts::UnitId::make(static_cast<std::uint16_t>(index),static_cast<std::uint16_t>(generation)));
        }
        a.events.push_back(std::move(e));
    }
    require(a.tick>=0 && a.tick<=kMaxSaveTicks,"存档时长越界");return a;
}
int read_journal_progress(const std::filesystem::path& file) {
    auto backup=file;backup+=".bak";
    if(!std::filesystem::exists(file) && !std::filesystem::exists(backup)) return 1;
    const auto read=[](const std::filesystem::path& path) {
        const auto j=load_json(path);
        require(j.at("format")=="siege-journal" && j.at("version")==1,"日记记录版本不兼容");
        const int wave=j.at("highest_wave").get<int>();require(wave>=1 && wave<=1000000,"日记记录波次无效");return wave;
    };
    try {return read(file);} catch(const std::exception&) {return read(backup);}
}
void write_journal_progress(const std::filesystem::path& file,int highest_wave) {
    require(highest_wave>=1 && highest_wave<=1000000,"日记波次越界");
    const int old=read_journal_progress(file);
    if(highest_wave<=old && std::filesystem::exists(file)) return;
    atomic_json(file,{{"format","siege-journal"},{"version",1},{"highest_wave",std::max(old,highest_wave)}});
}
}
