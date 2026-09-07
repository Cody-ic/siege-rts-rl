#ifndef GAME_SAVE_GAME_HPP
#define GAME_SAVE_GAME_HPP
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "game/game_shell.hpp"
namespace game {
// 优先恢复完整快照（含脚本/RNG/波次机）；损坏时回退操作日志，末态哈希必须一致。
// 修改 DemoBattle 的规则/默认参数时须提升兼容版本；旧档保留并明确报错。
inline constexpr int kSaveVersion=3;
struct BattleArchive {
    std::string map_json,stats_json,snapshot;
    std::uint64_t seed=0,hash=0,snapshot_hash=0;
    rts::Tick tick=0;
    int wave=1,attempt=1;
    ChronicleChoice choice=ChronicleChoice::None;
    std::vector<DemoBattle::PlayerEvent> events;
};
using RestoreProgress=std::function<bool(rts::Tick,rts::Tick)>;
BattleArchive capture_battle(const GameShell& shell,std::string map_json,std::string stats_json);
std::unique_ptr<DemoBattle> restore_battle(const BattleArchive& archive,const RestoreProgress& progress={});
void write_archive(const std::filesystem::path& file,const BattleArchive& archive);
BattleArchive read_archive(const std::filesystem::path& file);
void preserve_incompatible_archive(const std::filesystem::path& file);
int read_journal_progress(const std::filesystem::path& file);
void write_journal_progress(const std::filesystem::path& file,int highest_wave);
std::filesystem::path default_save_directory();
std::string read_save_text(const std::filesystem::path& file);
}
#endif
