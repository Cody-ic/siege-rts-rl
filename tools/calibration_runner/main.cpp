// 《攻守实力模型与平衡分析.md》§7 校准 runner（2026-09-02）。
//
// ## 它是干什么的
//
// §7 说：`DemoBattle` + `DefenderScript` 能无渲染跑，写一个 runner，多种子跑
// 20 波，每波记「门/缺口首次被打开的时刻、`Ram` 到墙下时的血量与它挨了几座塔
// 的火、塔的每发 AOE 实际命中数、`Phoenix` 的实际目标序列、门边 13 格墙上有
// 几名弓手、每波守方损失（弓/塔/Flak）、堡垒血量、波长」——用这些数拟合 §1
// 的 `aoe_mult`、`Phoenix` 行为、`arch_conc`，再重跑 §3/§4 的表。
// 本文件就是那个 runner：跑真实对局、逐波记原始数、输出机器可读的 JSON。
//
// ## 边界（哪些量怎么来的）
//
// 能直接从 `WorldView` 读的不加埋点，只推理：
//
//   * 门/缺口打开 = 本波第一段 `Wall`/`Gate` 被杀死的 tick（`bld_alive` 消失）。
//     地图自带的设计缺口（环上没写墙段的格）单列在 maps 信息里，不算「打开」。
//   * `Ram` 到墙下 = 它第一次**贴着墙段承诺出手**的时刻（前摇进行中且
//     所在格与某段墙/门相邻；脚本里它若够得着墙上弓手会先 `AtkNear` 打人、
//     够不着才 `AtkWall` 砸墙，只认 `AtkWall` 会把「到墙下打弓手」的锤漏记成
//     「死在半路」）——那一刻记血量；「挨了几座塔的火」= 此刻在途的 `Tower`
//     齐射弹丸中、落点离它 2.5 格以内的那几发，按弹丸**发射位置**去重数塔
//     （齐射落点锁定在承诺那一刻，而 `Ram` 到墙前后 1 秒内只挪 ~1 格，
//     所以「锁定落点离它近」就是「在打它」的一个可靠代理）。
//   * `Phoenix` 目标序列 = 它每次新承诺出手（`windup` 从 0 跳升）时的
//     目标种类 + 锁定落点反解出的目标类型——它只会 `AtkWeak`（单位）与
//     `AtkBld`（建筑），落点在前摇开始那一刻锁定，逐 tick 采样不丢事件。
//   * 门边弓手 = 每座门两侧沿墙环各 6 格（含门格，窗口共 13 格，同 §1.4 的
//     「门两侧约 13 格墙」）上、已驻守（`unit_garrison != kNoSlot`）的弓手数，
//     记三个时刻：开打时 / 破口时 / 波末。
//   * 守方损失、堡垒血量、波长 = 波首波末两次快照的差。
//
// 只有一项在 rts_core 加了埋点：**塔齐射的每发命中数**（`World::volley_hits()`，
// 齐射弹丸落地时记一行「圈内实际挨打的地面单位数」，miss 过滤之后）——齐射结算
// 在机制内部一次做完，外部逐 tick 采样凑不齐这个数。埋点只是旁观计数器，
// 不进 `state_hash`、不进回放（理由见 world.hpp 那段注释）。
//
// ## 输出
//
// stdout（或 `--out` 文件）是一份 JSON：`schema: "calibration_runner/1"`，
// 顶层 maps 信息（门、缺口、环半径）+ runs（每局逐波记录）。除 schema 头外
// 全部字段都是 ASCII 或 UTF-8 原样路径，输出只含仿真结果、不含挂钟时间，
// 所以同一份输入两次运行字节一致（确定性纪律）。
//
// ## 用法
//
//   calibration_runner [--maps <目录>] [--seeds 1,2,3] [--max-waves 20]
//                      [--max-ticks 0] [--out 文件.json] [--compact] [--help]
//
// 默认跑 `<data>/maps/pool` 全部 12 张池图 × 3 种子。耗时摘要打到 stderr。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/demo_driver.hpp"
#include "game/map_data.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/action.hpp"
#include "rts/cli_args.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
#include "rts/utf8_path.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

namespace calib {
namespace {

// ——命令行——
//
// 不用第三方 argparse 一类的库：本目录唯一的依赖是 game 静态库（+ rts_core），
// 手写一份 60 行的解析器换掉一个依赖是划算的。风格与 tools/map_gen 一致：
// 长选项、`--help` 完整说明、坏参数退出码 2。
struct Options {
    std::string maps_dir;
    std::vector<std::uint64_t> seeds{1, 2, 3};
    int max_waves = 20;
    int max_ticks = 0;   // 0 = 不限
    int limit = 0;       // 只跑字典序前 n 张图，0 = 全部（冒烟/快速本地跑用）
    std::string out_path;   // 空 = stdout
    bool compact = false;
    bool help = false;
};

void print_help() {
    std::cout <<
        "calibration_runner —— 攻守实力模型 §7 校准 runner（2026-09-02）\n"
        "\n"
        "用 DemoBattle + DefenderScript 无渲染跑多图多种子对局，逐波记录 §7\n"
        "点名的每一项（破口时刻、Ram 到墙血量与塔火、塔 AOE 逐发命中数、\n"
        "Phoenix 目标序列、门边 13 格墙弓手数、守方损失、堡垒血量、波长），\n"
        "输出机器可读 JSON（stdout 或 --out 文件）。\n"
        "\n"
        "用法: calibration_runner [选项]\n"
        "\n"
        "  --maps <目录>     地图目录，*.json 按字典序全部参与（默认 <data>/maps/pool）\n"
        "  --seeds <列表>    逗号分隔的种子（默认 1,2,3）\n"
        "  --max-waves <n>   每局最多波数，跑完或堡垒陷落即停（默认 20）\n"
        "  --max-ticks <n>   每局 tick 上限，0 = 不限（默认 0）\n"
        "  --limit <n>       只跑字典序前 n 张图，0 = 全部（默认 0）\n"
        "  --out <文件>      JSON 输出文件（默认 stdout）\n"
        "  --compact         紧凑 JSON（默认缩进）\n"
        "  --help            本帮助\n";
}

bool parse_args(const std::vector<std::string>& args, Options& out) {
    const int argc = static_cast<int>(args.size());
    const auto need = [&](int& i, const char* name) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << "选项 " << name << " 需要一个参数\n";
            return {};
        }
        return args[static_cast<std::size_t>(++i)];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = args[static_cast<std::size_t>(i)];
        if (a == "--help" || a == "-h") {
            out.help = true;
        } else if (a == "--maps") {
            const std::string v = need(i, "--maps");
            if (v.empty()) return false;
            out.maps_dir = v;
        } else if (a == "--seeds") {
            const std::string v = need(i, "--seeds");
            if (v.empty()) return false;
            out.seeds.clear();
            std::size_t begin = 0;
            while (begin <= v.size()) {
                const std::size_t end = v.find(',', begin);
                const std::string tok =
                    v.substr(begin, end == std::string::npos ? std::string::npos
                                                             : end - begin);
                try {
                    std::size_t used = 0;
                    const std::uint64_t s =
                        std::stoull(tok, &used, /*base=*/10);
                    if (used != tok.size()) throw std::invalid_argument(tok);
                    out.seeds.push_back(s);
                } catch (const std::exception&) {
                    std::cerr << "种子不是无符号整数: \"" << tok << "\"\n";
                    return false;
                }
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            if (out.seeds.empty()) {
                std::cerr << "--seeds 列表为空\n";
                return false;
            }
        } else if (a == "--max-waves") {
            const std::string v = need(i, "--max-waves");
            if (v.empty()) return false;
            try {
                out.max_waves = std::stoi(v);
            } catch (const std::exception&) {
                std::cerr << "--max-waves 不是整数: \"" << v << "\"\n";
                return false;
            }
            if (out.max_waves < 1) {
                std::cerr << "--max-waves 必须 >= 1\n";
                return false;
            }
        } else if (a == "--max-ticks") {
            const std::string v = need(i, "--max-ticks");
            if (v.empty()) return false;
            try {
                out.max_ticks = std::stoi(v);
            } catch (const std::exception&) {
                std::cerr << "--max-ticks 不是整数: \"" << v << "\"\n";
                return false;
            }
            if (out.max_ticks < 0) {
                std::cerr << "--max-ticks 必须 >= 0\n";
                return false;
            }
        } else if (a == "--limit") {
            const std::string v = need(i, "--limit");
            if (v.empty()) return false;
            try {
                out.limit = std::stoi(v);
            } catch (const std::exception&) {
                std::cerr << "--limit 不是整数: \"" << v << "\"\n";
                return false;
            }
            if (out.limit < 0) {
                std::cerr << "--limit 必须 >= 0\n";
                return false;
            }
        } else if (a == "--out") {
            const std::string v = need(i, "--out");
            if (v.empty()) return false;
            out.out_path = v;
        } else if (a == "--compact") {
            out.compact = true;
        } else {
            std::cerr << "未知选项: " << a << "（--help 看用法）\n";
            return false;
        }
    }
    return true;
}

// ——JSON 输出——
//
// 手写，不引 nlohmann：输出端只有一个文件、一次组装，30 行转义 + 50 行缩进
// 状态机就够。转义覆盖字符串里可能出现的全部特殊字节（路径含反斜杠与中文，
// 中文按 UTF-8 原样输出——JSON 规范允许）。数值只输出 int / double / bool。
std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

class Json {
public:
    explicit Json(bool compact) : compact_(compact) {}
    std::string take() { return out_; }

    void begin_obj() {
        sep_value();
        out_ += '{';
        ++indent_;
        last_ = Last::None;
    }
    void end_obj() { end('}'); }
    void begin_arr() {
        sep_value();
        out_ += '[';
        ++indent_;
        last_ = Last::None;
    }
    void end_arr() { end(']'); }
    void key(std::string_view k) {
        if (last_ == Last::Value) out_ += ',';
        if (!compact_) {
            out_ += '\n';
            out_.append(static_cast<std::size_t>(indent_) * 2, ' ');
        }
        out_ += '"';
        out_ += k;
        out_ += compact_ ? "\":" : "\": ";
        last_ = Last::Key;
    }
    void val(std::int64_t v) {
        sep_value();
        out_ += std::to_string(v);
    }
    void val(std::uint64_t v) {
        sep_value();
        out_ += std::to_string(v);
    }
    void val(int v) { val(static_cast<std::int64_t>(v)); }
    void val(bool v) {
        sep_value();
        out_ += v ? "true" : "false";
    }
    void val(double v) {
        sep_value();
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.4f", v);
        out_ += buf;
    }
    void null() {
        sep_value();
        out_ += "null";
    }
    void str(std::string_view s) {
        sep_value();
        out_ += '"';
        out_ += json_escape(s);
        out_ += '"';
    }

private:
    enum class Last { None, Key, Value };

    void sep_value() {
        // 键后紧跟的值不加逗号；数组元素与前一个值之间加逗号。
        if (last_ == Last::Key) {
            last_ = Last::Value;
            return;
        }
        if (last_ == Last::Value) out_ += ',';
        if (!compact_) {
            out_ += '\n';
            out_.append(static_cast<std::size_t>(indent_) * 2, ' ');
        }
        last_ = Last::Value;
    }
    void end(char c) {
        --indent_;
        if (!compact_ && last_ != Last::None) {
            out_ += '\n';
            out_.append(static_cast<std::size_t>(indent_) * 2, ' ');
        }
        out_ += c;
        last_ = Last::Value;
    }

    bool compact_ = false;
    int indent_ = 0;
    std::string out_;
    Last last_ = Last::None;
};

// ——地图静态信息——
//
// 门、设计缺口、环半径都从 `MapData` 直接读——它们开打之前就定了，
// 属于「这张图长什么样」，放在 maps 信息里而不是逐波重复。
struct MapInfo {
    std::string file;   // 相对 maps 目录的文件名
    std::string map_id;
    int width = 0;
    int height = 0;
    int keep_i = 0;
    int keep_j = 0;
    int ring_radius = 0;   // 城环切比雪夫半径 = max(|墙格 − keep|)
    std::vector<std::pair<int, int>> gates;
    std::vector<std::pair<int, int>> gaps;   // 环上没写墙段的格（设计缺口）
    int wall_cells = 0;
    int spawn_count = 0;
};

MapInfo collect_map_info(const std::string& file, const game::MapData& map) {
    MapInfo m;
    m.file = file;
    m.map_id = map.map_id();
    m.width = map.width();
    m.height = map.height();
    m.keep_i = map.keep().i;
    m.keep_j = map.keep().j;
    int r = 0;
    for (const game::WallSegment& w : map.walls()) {
        const int dx = static_cast<int>(w.pos.i) - m.keep_i;
        const int dy = static_cast<int>(w.pos.j) - m.keep_j;
        const int cheb = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
        if (cheb > r) r = cheb;
    }
    m.ring_radius = r;
    m.wall_cells = static_cast<int>(map.walls().size());
    for (const game::WallSegment& w : map.walls()) {
        if (w.kind == game::WallKind::Gate) {
            m.gates.emplace_back(static_cast<int>(w.pos.i),
                                 static_cast<int>(w.pos.j));
        }
    }
    for (int x = 0; x < m.width; ++x) {
        for (int y = 0; y < m.height; ++y) {
            const int dx = x - m.keep_i;
            const int dy = y - m.keep_j;
            if (std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) != r) continue;
            if (map.wall_at(x, y) == nullptr) m.gaps.emplace_back(x, y);
        }
    }
    m.spawn_count = static_cast<int>(map.spawns().size());
    return m;
}

// ——逐波记录——
//
// 字段对应 §7 清单，一项不多一项不少；额外带 attacker_start/attacker_end
// （本波编成）与 nominal_level（§3 表重跑的输入）。

struct RamEvent {
    int unit_index = 0;        // 槽位下标（波内唯一，用于死亡补记）
    int arrived_tick = 0;      // 第一次贴墙承诺出手的时刻（到墙下）
    int assault_tick = 0;      // 相对本波开打的 tick
    std::int64_t hp = 0;
    std::int64_t max_hp = 0;
    double hp_frac = 0.0;
    int pos_i = 0;
    int pos_j = 0;
    int towers_firing = 0;     // 此刻在途齐射落点近它的塔数（按发射位去重）
    int volleys_inbound = 0;   // 在途齐射发数（未去重）
    int died_tick = -1;        // -1 = 波末仍活着
};

struct PhoenixEvent {
    int tick = 0;              // 新承诺出手的时刻
    int assault_tick = 0;
    std::string kind;          // "Unit" = AtkWeak / "Bld" = AtkBld
    std::string target;        // 落点反解出的目标类型 ident
    int pos_i = 0;
    int pos_j = 0;
};

struct GateWindow {
    int gi = 0;   // 门格
    int gj = 0;
    int window_cells = 0;                 // 窗口内墙格数（含门格，约 13）
    std::vector<std::uint16_t> cells;     // 窗口内墙格的线性下标（garrison 槽）
    int at_assault_start = 0;             // 开打时窗口上的弓手数
    int at_breach = -1;                   // 破口时（本波没破口 = -1）
    int at_wave_end = 0;                  // 波末
};

struct WaveRecord {
    int wave = 0;
    int start_tick = 0;   // 首次看到本波（建造期开始，攻方已生成）
    int end_tick = 0;     // 最后看到本波的 tick（波末快照来自它）
    int build_ticks = 0;
    int assault_start_tick = -1;
    int nominal_level = 1;
    std::map<std::string, int> attacker_start;
    std::map<std::string, int> attacker_end;
    bool breached = false;
    int breach_tick = -1;         // 本波第一段墙/门被杀死的 tick
    std::string breach_kind;      // "Wall" / "Gate"
    int breach_i = 0;
    int breach_j = 0;
    int rams_spawned = 0;
    int rams_died_en_route = 0;   // 没到墙下就死了的 Ram 数
    std::vector<RamEvent> ram_events;
    std::vector<PhoenixEvent> phoenix_events;
    int phoenix_died_tick = -1;
    std::vector<int> volley_hits;          // 本波塔齐射逐发命中数（埋点切片）
    std::vector<GateWindow> gate_windows;  // 门边 13 格墙弓手
    std::map<std::string, int> defender_start;
    std::map<std::string, int> defender_end;
    std::map<std::string, int> bld_start;
    std::map<std::string, int> bld_end;
    std::int64_t keep_hp_start = 0;
    std::int64_t keep_hp_end = 0;
};

struct RunRecord {
    std::string map_file;
    std::uint64_t seed = 0;
    bool defeated = false;
    bool truncated = false;   // 被 --max-ticks 截断
    int final_wave = 0;
    int total_ticks = 0;
    std::vector<WaveRecord> waves;
};

// ——对局观察器：逐 tick 喂状态，产出逐波记录——
//
// 只读世界（`WorldView` + `World` 的只读访问器），不往对局里写任何东西——
// runner 是旁观者，不是策略。逐 tick 采样（update(1) 一步一喂）不丢事件：
// 承诺出手的窗口（前摇 > 0）至少持续数 tick，`windup` 跳升的那一拍必被看见。
class BattleRecorder {
public:
    BattleRecorder(const game::MapData& map, game::DemoBattle& b)
        : map_(map), b_(b) {}

    // 每次 battle.update(...) 之后调一次。
    void observe();

    // 对局结束（败局定格 / 跑满波数 / tick 截断）后调：收尾最后一波。
    void close();

    const std::vector<WaveRecord>& waves() const { return waves_; }

private:
    float dist2(rts::Vec2 a, rts::Vec2 b) const {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return dx * dx + dy * dy;
    }
    std::map<std::string, int> count_units(rts::Side side) const;
    std::map<std::string, int> count_blds() const;
    std::int64_t keep_hp() const;
    int count_archers_on(const GateWindow& gw) const;
    int towers_firing_at(rts::Vec2 pos, int& volleys) const;
    std::pair<std::string, rts::GridPos> resolve_target(rts::TgtKind kind,
                                                        rts::Vec2 aim) const;

    void begin_wave();
    void finalize_wave();
    void scan_breach();
    void scan_rams();
    void scan_phoenix();
    void sync_volleys();

    const game::MapData& map_;
    game::DemoBattle& b_;

    std::vector<WaveRecord> waves_;
    WaveRecord cur_;
    bool have_cur_ = false;
    rts::WavePhase prev_phase_ = rts::WavePhase::Build;

    // 本波初始的墙/门快照：位置 + 种类（种类用于报破的是门还是墙）。
    std::vector<rts::GridPos> wall_pos_;
    std::vector<rts::BldType> wall_kind_;
    // 初始墙格的静态位图（行主序，只查墙格）：`Ram` 到墙判据用，建局后不再动。
    std::vector<std::uint8_t> wall_grid0_;
    // 每 tick 重填的「哪格现在还有墙」位图：破口检测用（墙被拆后这里灭灯）。
    std::vector<std::uint8_t> wall_grid_;

    // 攻方重点单位跟踪：Ram 每台一条、Phoenix 至多一条。
    struct UnitTrack {
        rts::UnitId id{};
        int prev_windup = 0;
        bool arrived = false;
        bool dead_counted = false;
    };
    std::vector<UnitTrack> ram_track_;
    UnitTrack phoenix_track_;
    bool have_phoenix_ = false;
    bool phoenix_dead_ = false;

    std::size_t volley_offset_ = 0;   // 已切给本波的位置
};

std::map<std::string, int> BattleRecorder::count_units(rts::Side side) const {
    const rts::WorldView v = b_.world().view(side);
    std::map<std::string, int> out;
    for (std::size_t k = 0; k < v.unit_alive().size(); ++k) {
        if (v.unit_alive()[k] == 0) continue;
        if (rts::side_of(v.unit_type()[k]) != side) continue;
        ++out[std::string(rts::ident_of(v.unit_type()[k]))];
    }
    return out;
}

std::map<std::string, int> BattleRecorder::count_blds() const {
    const rts::WorldView v = b_.world().view(rts::Side::Defender);
    std::map<std::string, int> out;
    for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
        if (v.bld_alive()[k] == 0) continue;
        ++out[std::string(rts::ident_of(v.bld_type()[k]))];
    }
    return out;
}

std::int64_t BattleRecorder::keep_hp() const {
    const rts::WorldView v = b_.world().view(rts::Side::Defender);
    for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
        if (v.bld_alive()[k] == 0) continue;
        if (v.bld_type()[k] == rts::BldType::Keep) return v.bld_hp()[k];
    }
    return -1;
}

int BattleRecorder::count_archers_on(const GateWindow& gw) const {
    const rts::WorldView v = b_.world().view(rts::Side::Defender);
    const auto alive = v.unit_alive();
    const auto type = v.unit_type();
    const auto garrison = v.unit_garrison();
    int n = 0;
    for (std::size_t k = 0; k < alive.size(); ++k) {
        if (alive[k] == 0) continue;
        if (type[k] != rts::UnitType::Archer) continue;
        if (garrison[k] == rts::kNoSlot) continue;
        if (std::find(gw.cells.begin(), gw.cells.end(), garrison[k]) !=
            gw.cells.end()) {
            ++n;
        }
    }
    return n;
}

int BattleRecorder::towers_firing_at(rts::Vec2 pos, int& volleys) const {
    const rts::WorldView v = b_.world().view(rts::Side::Defender);
    const auto ppos = v.proj_pos();
    const auto paim = v.proj_aim();
    const auto src = v.proj_src_bld();
    volleys = 0;
    std::vector<rts::GridPos> distinct;
    constexpr float kNear2 = 2.5f * 2.5f;   // 见文件头「挨了几座塔的火」
    for (std::size_t i = 0; i < paim.size(); ++i) {
        if (src[i] != static_cast<std::uint8_t>(rts::BldType::Tower)) continue;
        if (dist2(paim[i], pos) > kNear2) continue;
        ++volleys;
        const rts::GridPos cell = rts::grid_of(ppos[i]);
        bool seen = false;
        for (const rts::GridPos& g : distinct) {
            if (g.i == cell.i && g.j == cell.j) {
                seen = true;
                break;
            }
        }
        if (!seen) distinct.push_back(cell);
    }
    return static_cast<int>(distinct.size());
}

std::pair<std::string, rts::GridPos> BattleRecorder::resolve_target(
    rts::TgtKind kind, rts::Vec2 aim) const {
    // 落点在承诺那一刻锁定成目标**当时**的位置；本函数在同一 tick 内调用，
    // 所以按位置精确匹配（浮点逐位相等）必然命中。匹配不到就诚实报 Unknown，
    // 不许静默丢掉这条事件。
    const rts::WorldView v = b_.world().view(rts::Side::Defender);
    if (kind == rts::TgtKind::Bld) {
        for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
            if (v.bld_alive()[k] == 0) continue;
            if (dist2(rts::center_of(v.bld_pos()[k]), aim) < 1e-6f) {
                return {std::string(rts::ident_of(v.bld_type()[k])),
                        v.bld_pos()[k]};
            }
        }
        return {"Unknown", rts::grid_of(aim)};
    }
    if (kind == rts::TgtKind::Unit) {
        for (std::size_t k = 0; k < v.unit_alive().size(); ++k) {
            if (v.unit_alive()[k] == 0) continue;
            if (dist2(v.unit_pos()[k], aim) < 1e-6f) {
                return {std::string(rts::ident_of(v.unit_type()[k])),
                        rts::grid_of(aim)};
            }
        }
        return {"Unknown", rts::grid_of(aim)};
    }
    return {"Unknown", rts::grid_of(aim)};
}

void BattleRecorder::begin_wave() {
    const rts::World& w = b_.world();
    cur_ = WaveRecord{};
    cur_.wave = w.wave();
    cur_.start_tick = w.now();
    cur_.end_tick = w.now();
    cur_.nominal_level = w.nominal_level();
    cur_.attacker_start = count_units(rts::Side::Attacker);
    cur_.defender_start = count_units(rts::Side::Defender);
    cur_.bld_start = count_blds();
    cur_.keep_hp_start = keep_hp();
    cur_.rams_spawned = cur_.attacker_start.count("Ram") != 0
                            ? cur_.attacker_start.at("Ram")
                            : 0;

    // 墙快照：本波开始时所有活着的 Wall/Gate（位置 + 种类）。
    wall_pos_.clear();
    wall_kind_.clear();
    {
        const rts::WorldView v = w.view(rts::Side::Defender);
        for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
            if (v.bld_alive()[k] == 0) continue;
            const rts::BldType t = v.bld_type()[k];
            if (t != rts::BldType::Wall && t != rts::BldType::Gate) continue;
            wall_pos_.push_back(v.bld_pos()[k]);
            wall_kind_.push_back(t);
        }
    }
    wall_grid_.assign(
        static_cast<std::size_t>(w.width()) * static_cast<std::size_t>(w.height()),
        std::uint8_t{0});
    wall_grid0_ = wall_grid_;
    for (const rts::GridPos& p : wall_pos_) {
        wall_grid0_[static_cast<std::size_t>(p.j) *
                         static_cast<std::size_t>(w.width()) +
                     static_cast<std::size_t>(p.i)] = 1;
    }

    // 门边 13 格墙窗口：从地图静态墙表建（门的位置不随对局变）。§1.4 的
    // 「门两侧约 13 格墙」= 沿环到门的切比雪夫距离 <= 6 的墙格（直线段上
    // 恰好 6 + 门 + 6 = 13 格；靠近环角会多包进邻边几格，如实记 window_cells）。
    for (const game::WallSegment& g : map_.walls()) {
        if (g.kind != game::WallKind::Gate) continue;
        GateWindow gw;
        gw.gi = static_cast<int>(g.pos.i);
        gw.gj = static_cast<int>(g.pos.j);
        for (const game::WallSegment& s : map_.walls()) {
            const int dx = static_cast<int>(s.pos.i) - gw.gi;
            const int dy = static_cast<int>(s.pos.j) - gw.gj;
            if (std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) > 6) continue;
            gw.cells.push_back(rts::slot_of(s.pos, w.width()));
        }
        gw.window_cells = static_cast<int>(gw.cells.size());
        cur_.gate_windows.push_back(std::move(gw));
    }

    // 攻方重点单位：本波 Ram 与 Phoenix 的句柄（波中不再新增）。
    // enumerate 按规范序（槽位升序）返回，id.index() 就是槽位下标。
    ram_track_.clear();
    have_phoenix_ = false;
    phoenix_dead_ = false;
    {
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Attacker, ids);
        for (const rts::UnitId id : ids) {
            const rts::UnitType t = w.unit_type(id);
            if (t == rts::UnitType::Ram) {
                ram_track_.push_back(UnitTrack{id, 0, false, false});
            } else if (t == rts::UnitType::Phoenix) {
                phoenix_track_ = UnitTrack{id, 0, false, false};
                have_phoenix_ = true;
            }
        }
    }

    volley_offset_ = w.view(rts::Side::Defender).volley_hits().size();
    have_cur_ = true;
}

void BattleRecorder::finalize_wave() {
    // 收尾时守方状态与上一 tick 相同（波间只生攻方），弓手计数此刻取即可。
    for (GateWindow& gw : cur_.gate_windows) gw.at_wave_end = count_archers_on(gw);
    waves_.push_back(std::move(cur_));
}

void BattleRecorder::scan_breach() {
    const rts::World& w = b_.world();
    const rts::WorldView v = w.view(rts::Side::Defender);
    // 重填位图：扫一遍建筑数组，把活着的墙/门格点亮。
    std::fill(wall_grid_.begin(), wall_grid_.end(), std::uint8_t{0});
    for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
        if (v.bld_alive()[k] == 0) continue;
        const rts::BldType t = v.bld_type()[k];
        if (t != rts::BldType::Wall && t != rts::BldType::Gate) continue;
        const rts::GridPos p = v.bld_pos()[k];
        wall_grid_[static_cast<std::size_t>(p.j) *
                        static_cast<std::size_t>(w.width()) +
                    static_cast<std::size_t>(p.i)] = 1;
    }
    for (std::size_t k = 0; k < wall_pos_.size(); ++k) {
        const rts::GridPos p = wall_pos_[k];
        const std::size_t cell = static_cast<std::size_t>(p.j) *
                                     static_cast<std::size_t>(w.width()) +
                                 static_cast<std::size_t>(p.i);
        if (wall_grid_[cell] != 0) continue;
        // 本波第一段被拆掉的墙/门。
        cur_.breached = true;
        cur_.breach_tick = w.now();
        cur_.breach_kind = std::string(rts::ident_of(wall_kind_[k]));
        cur_.breach_i = static_cast<int>(p.i);
        cur_.breach_j = static_cast<int>(p.j);
        for (GateWindow& gw : cur_.gate_windows) {
            gw.at_breach = count_archers_on(gw);
        }
        return;
    }
}

void BattleRecorder::scan_rams() {
    const rts::World& w = b_.world();
    const rts::WorldView av = w.view(rts::Side::Attacker);
    const std::size_t stride = static_cast<std::size_t>(w.width());
    for (UnitTrack& t : ram_track_) {
        if (!w.alive(t.id)) {
            if (!t.dead_counted) {
                t.dead_counted = true;
                if (!t.arrived) {
                    ++cur_.rams_died_en_route;
                } else {
                    // 按槽位下标补记死亡（波内槽位不复用，下标即身份）。
                    for (RamEvent& e : cur_.ram_events) {
                        if (e.unit_index != static_cast<int>(t.id.index())) continue;
                        if (e.died_tick >= 0) continue;
                        e.died_tick = w.now();
                        break;
                    }
                }
            }
            continue;
        }
        if (t.arrived) continue;
        const std::size_t k = t.id.index();
        if (k >= av.unit_alive().size()) continue;
        // 到墙下 = 前摇进行中（承诺出手）且所在格与某段墙/门相邻。脚本里
        // `AtkNear` 优先于 `AtkWall`（墙上弓手够得着就先打人），所以不认动作
        // 种类，认位置——贴着墙的第一击就是「到墙下」。
        if (av.unit_windup()[k] <= 0) continue;
        if (av.unit_target_kind()[k] == rts::TgtKind::None) continue;
        const rts::Vec2 pos = av.unit_pos()[k];
        const rts::GridPos cell = rts::grid_of(pos);
        bool at_wall = false;
        for (int dj = -1; dj <= 1 && !at_wall; ++dj) {
            for (int di = -1; di <= 1 && !at_wall; ++di) {
                const int x = static_cast<int>(cell.i) + di;
                const int y = static_cast<int>(cell.j) + dj;
                if (x < 0 || y < 0 || x >= w.width() || y >= w.height()) continue;
                const std::size_t c =
                    static_cast<std::size_t>(y) * stride +
                    static_cast<std::size_t>(x);
                if (wall_grid0_[c] != 0) at_wall = true;
            }
        }
        if (!at_wall) continue;
        t.arrived = true;
        RamEvent e;
        e.unit_index = static_cast<int>(k);
        e.arrived_tick = w.now();
        e.assault_tick = w.now() - cur_.assault_start_tick;
        e.hp = av.unit_hp()[k];
        e.max_hp = av.unit_max_hp()[k];
        e.hp_frac = e.max_hp > 0
                        ? static_cast<double>(e.hp) / static_cast<double>(e.max_hp)
                        : 0.0;
        e.pos_i = static_cast<int>(std::floor(pos.x));
        e.pos_j = static_cast<int>(std::floor(pos.y));
        e.towers_firing = towers_firing_at(pos, e.volleys_inbound);
        cur_.ram_events.push_back(e);
    }
}

void BattleRecorder::scan_phoenix() {
    if (!have_phoenix_) return;
    const rts::World& w = b_.world();
    if (!w.alive(phoenix_track_.id)) {
        if (!phoenix_dead_) {
            phoenix_dead_ = true;
            cur_.phoenix_died_tick = w.now();
        }
        return;
    }
    const std::int32_t windup = w.unit_windup(phoenix_track_.id);
    // 承诺出手的那一拍 windup 从 0 跳升到前摇满值，此后单调递减——
    // 「比上一拍大」就是一次新承诺，且恰好只看见一次。
    if (windup > phoenix_track_.prev_windup && windup > 0) {
        const rts::TgtKind kind = w.unit_target_kind(phoenix_track_.id);
        if (kind != rts::TgtKind::None) {
            PhoenixEvent e;
            e.tick = w.now();
            e.assault_tick = w.now() - cur_.assault_start_tick;
            e.kind = kind == rts::TgtKind::Unit ? "Unit" : "Bld";
            const auto [target, cell] = resolve_target(kind, w.unit_aim(phoenix_track_.id));
            e.target = target;
            e.pos_i = static_cast<int>(cell.i);
            e.pos_j = static_cast<int>(cell.j);
            cur_.phoenix_events.push_back(std::move(e));
        }
    }
    phoenix_track_.prev_windup = windup;
}

void BattleRecorder::sync_volleys() {
    const auto& log = b_.world().view(rts::Side::Defender).volley_hits();
    while (volley_offset_ < log.size()) {
        cur_.volley_hits.push_back(
            static_cast<int>(log[volley_offset_]));
        ++volley_offset_;
    }
}

void BattleRecorder::observe() {
    const rts::World& w = b_.world();
    if (!have_cur_) {
        begin_wave();
    } else if (w.wave() != cur_.wave) {
        // 波变了：上一 tick 的快照就是上一波的波末，先收尾再开新波。
        finalize_wave();
        begin_wave();
    }

    // ——本 tick 波内快照（波末快照，逐 tick 覆盖）——
    cur_.end_tick = w.now();
    cur_.attacker_end = count_units(rts::Side::Attacker);
    cur_.defender_end = count_units(rts::Side::Defender);
    cur_.bld_end = count_blds();
    cur_.keep_hp_end = keep_hp();

    // ——开打那一拍——
    if (prev_phase_ == rts::WavePhase::Build &&
        w.phase() == rts::WavePhase::Assault) {
        cur_.assault_start_tick = w.now();
        cur_.build_ticks = w.now() - cur_.start_tick;
        for (GateWindow& gw : cur_.gate_windows) {
            gw.at_assault_start = count_archers_on(gw);
        }
    }
    prev_phase_ = w.phase();

    sync_volleys();

    // 建造期无事发生：攻方在集结点待命（Wraith 的侦查不落在 §7 清单里）。
    if (w.phase() != rts::WavePhase::Assault) return;

    if (!cur_.breached) scan_breach();
    scan_rams();
    scan_phoenix();
}

void BattleRecorder::close() {
    // 跑满波数正常停住时，最后开的那波是「只生成、没开打」的空壳（wave() 已
    // 越过 max_waves），丢进输出只会污染读法；败局或截断时的最后一波是真波。
    if (have_cur_ && (cur_.assault_start_tick >= 0 || b_.defeated())) {
        finalize_wave();
    }
}

// ——JSON 组装——

void write_pair_list(Json& j, const std::vector<std::pair<int, int>>& v) {
    j.begin_arr();
    for (const auto& [x, y] : v) {
        j.begin_arr();
        j.val(x);
        j.val(y);
        j.end_arr();
    }
    j.end_arr();
}

void write_count_map(Json& j, const std::map<std::string, int>& m) {
    j.begin_obj();
    for (const auto& [k, v] : m) {
        j.key(k);
        j.val(v);
    }
    j.end_obj();
}

void write_map_info(Json& j, const MapInfo& m) {
    j.begin_obj();
    j.key("file");
    j.str(m.file);
    j.key("map_id");
    j.str(m.map_id);
    j.key("width");
    j.val(m.width);
    j.key("height");
    j.val(m.height);
    j.key("keep");
    j.begin_arr();
    j.val(m.keep_i);
    j.val(m.keep_j);
    j.end_arr();
    j.key("ring_radius");
    j.val(m.ring_radius);
    j.key("wall_cells");
    j.val(m.wall_cells);
    j.key("spawn_count");
    j.val(m.spawn_count);
    j.key("gates");
    write_pair_list(j, m.gates);
    j.key("gaps");
    write_pair_list(j, m.gaps);
    j.end_obj();
}

void write_wave(Json& j, const WaveRecord& w) {
    j.begin_obj();
    j.key("wave");
    j.val(w.wave);
    j.key("start_tick");
    j.val(w.start_tick);
    j.key("end_tick");
    j.val(w.end_tick);
    j.key("length_ticks");
    j.val(w.end_tick - w.start_tick + 1);
    j.key("build_ticks");
    j.val(w.build_ticks);
    j.key("assault_start_tick");
    j.val(w.assault_start_tick);
    j.key("nominal_level");
    j.val(w.nominal_level);

    j.key("attacker_start");
    write_count_map(j, w.attacker_start);
    j.key("attacker_end");
    write_count_map(j, w.attacker_end);

    j.key("breach");
    if (w.breached) {
        j.begin_obj();
        j.key("tick");
        j.val(w.breach_tick);
        j.key("assault_tick");
        j.val(w.breach_tick - w.assault_start_tick);
        j.key("kind");
        j.str(w.breach_kind);
        j.key("pos");
        j.begin_arr();
        j.val(w.breach_i);
        j.val(w.breach_j);
        j.end_arr();
        j.end_obj();
    } else {
        j.null();
    }

    j.key("rams_spawned");
    j.val(w.rams_spawned);
    j.key("rams_died_en_route");
    j.val(w.rams_died_en_route);
    j.key("ram_events");
    j.begin_arr();
    for (const RamEvent& e : w.ram_events) {
        j.begin_obj();
        j.key("arrived_tick");
        j.val(e.arrived_tick);
        j.key("assault_tick");
        j.val(e.assault_tick);
        j.key("hp");
        j.val(e.hp);
        j.key("max_hp");
        j.val(e.max_hp);
        j.key("hp_frac");
        j.val(e.hp_frac);
        j.key("pos");
        j.begin_arr();
        j.val(e.pos_i);
        j.val(e.pos_j);
        j.end_arr();
        j.key("towers_firing");
        j.val(e.towers_firing);
        j.key("volleys_inbound");
        j.val(e.volleys_inbound);
        j.key("died_tick");
        j.val(e.died_tick);
        j.end_obj();
    }
    j.end_arr();

    j.key("phoenix_events");
    j.begin_arr();
    for (const PhoenixEvent& e : w.phoenix_events) {
        j.begin_obj();
        j.key("tick");
        j.val(e.tick);
        j.key("assault_tick");
        j.val(e.assault_tick);
        j.key("kind");
        j.str(e.kind);
        j.key("target");
        j.str(e.target);
        j.key("pos");
        j.begin_arr();
        j.val(e.pos_i);
        j.val(e.pos_j);
        j.end_arr();
        j.end_obj();
    }
    j.end_arr();
    j.key("phoenix_died_tick");
    j.val(w.phoenix_died_tick);

    j.key("volley_hits");
    j.begin_arr();
    for (const int h : w.volley_hits) j.val(h);
    j.end_arr();

    j.key("gate_windows");
    j.begin_arr();
    for (const GateWindow& g : w.gate_windows) {
        j.begin_obj();
        j.key("gate");
        j.begin_arr();
        j.val(g.gi);
        j.val(g.gj);
        j.end_arr();
        j.key("window_cells");
        j.val(g.window_cells);
        j.key("at_assault_start");
        j.val(g.at_assault_start);
        j.key("at_breach");
        j.val(g.at_breach);
        j.key("at_wave_end");
        j.val(g.at_wave_end);
        j.end_obj();
    }
    j.end_arr();

    j.key("defender_start");
    write_count_map(j, w.defender_start);
    j.key("defender_end");
    write_count_map(j, w.defender_end);
    j.key("defender_loss");
    {
        std::map<std::string, int> loss;
        for (const auto& [k, v] : w.defender_start) {
            const auto it = w.defender_end.find(k);
            const int end = it == w.defender_end.end() ? 0 : it->second;
            loss[k] = std::max(0, v - end);
        }
        write_count_map(j, loss);
    }
    j.key("bld_start");
    write_count_map(j, w.bld_start);
    j.key("bld_end");
    write_count_map(j, w.bld_end);
    j.key("keep_hp_start");
    j.val(w.keep_hp_start);
    j.key("keep_hp_end");
    j.val(w.keep_hp_end);
    j.end_obj();
}

void write_run(Json& j, const RunRecord& r) {
    j.begin_obj();
    j.key("map_file");
    j.str(r.map_file);
    j.key("seed");
    j.val(r.seed);
    j.key("defeated");
    j.val(r.defeated);
    j.key("truncated");
    j.val(r.truncated);
    j.key("final_wave");
    j.val(r.final_wave);
    j.key("total_ticks");
    j.val(r.total_ticks);
    j.key("waves");
    j.begin_arr();
    for (const WaveRecord& w : r.waves) write_wave(j, w);
    j.end_arr();
    j.end_obj();
}

}  // namespace
}  // namespace calib

int main(int argc, char** argv) {
    using namespace calib;
    // **命令行必须先转 UTF-8**：Windows 的窄 argv 是 ANSI 代码页，而下面
    // 一路都按 UTF-8 处理（`rts::path_from_utf8`）。少了这一步，仓库路径
    // 含中文时进程直接 __fastfail（0xC0000409），理由见 `rts/cli_args.hpp`。
    const std::vector<std::string> args = rts::utf8_args(argc, argv);
    Options opt;
    opt.maps_dir = std::string(GAME_DATA_DIR) + "/maps/pool";
    if (!parse_args(args, opt)) {
        std::cerr << "（--help 看用法）\n";
        return 2;
    }
    if (opt.help) {
        print_help();
        return 0;
    }

    // ——地图发现——
    //
    // **必须走 `rts::path_from_utf8`**：仓库路径含中文时，MSVC 的窄
    // `filesystem::path` 按 ANSI 代码页解释 UTF-8 字节、`is_directory` 直接为假
    // ——症状是「目录不存在」，而不是它真的不存在（demo_test 踩过的同款坑）。
    const std::filesystem::path dir = rts::path_from_utf8(opt.maps_dir);
    if (!std::filesystem::is_directory(dir)) {
        std::cerr << "地图目录不存在: " << opt.maps_dir << "\n";
        return 1;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".json") files.push_back(e.path());
    }
    if (files.empty()) {
        std::cerr << "地图目录里一张 .json 都没有: " << opt.maps_dir << "\n";
        return 1;
    }
    std::sort(files.begin(), files.end());

    const rts::StatsTable stats = game::StatsLoader::from_file(
        std::string(GAME_DATA_DIR) + "/stats_placeholder.json");

    // ——跑——
    std::vector<MapInfo> maps;
    std::vector<RunRecord> runs;
    double total_seconds = 0.0;
    const std::size_t map_cap =
        opt.limit > 0 ? static_cast<std::size_t>(opt.limit) : files.size();
    std::size_t maps_done = 0;
    for (const std::filesystem::path& f : files) {
        if (maps_done >= map_cap) break;
        ++maps_done;
        const std::string file = rts::utf8_from_path(f.filename());
        const game::MapData map =
            game::MapLoader::from_file(rts::utf8_from_path(f));
        const MapInfo info = collect_map_info(file, map);
        maps.push_back(info);

        for (const std::uint64_t seed : opt.seeds) {
            const auto ts = std::chrono::steady_clock::now();
            game::DemoBattle battle(map, stats, seed);
            BattleRecorder rec(map, battle);
            rec.observe();   // 开第 1 波（t=0 已在集结）

            RunRecord run;
            run.map_file = file;
            run.seed = seed;
            run.truncated = false;
            int ticks = 0;
            while (!battle.defeated() &&
                   battle.world().wave() <= opt.max_waves) {
                if (opt.max_ticks > 0 && ticks >= opt.max_ticks) {
                    run.truncated = true;
                    break;
                }
                battle.update(1);
                rec.observe();
                ++ticks;
            }
            rec.close();
            run.defeated = battle.defeated();
            run.total_ticks = ticks;
            run.waves = rec.waves();
            // 最终波数取**真的跑了的**最后一波：跑满 max_waves 正常停住时，
            // world().wave() 已经是 max_waves + 1（空壳波被 close() 丢掉了）。
            run.final_wave = run.waves.empty()
                                 ? battle.world().wave()
                                 : run.waves.back().wave;
            runs.push_back(std::move(run));

            const double secs = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - ts)
                                    .count();
            total_seconds += secs;
            // **摘要保持纯 ASCII**：Windows 控制台默认 cp936，UTF-8 中文到这里
            // 会乱码甚至让下游（ctest 的 Python 检查器按 locale 解码子进程输出）
            // 直接抛 UnicodeDecodeError——tools/map_gen/validate.py 踩过的同款坑。
            std::cerr << file << " seed " << seed << ": "
                      << (run.defeated ? "DEFEATED" : "survived") << " @ wave "
                      << run.final_wave << ", " << run.total_ticks << " ticks, "
                      << secs << " s\n";
        }
    }
    std::cerr << "total: " << runs.size() << " runs, " << total_seconds
              << " s (avg " << (runs.empty() ? 0.0 : total_seconds /
                    static_cast<double>(runs.size())) << " s/run)\n";

    // ——组装 JSON——
    Json j(opt.compact);
    j.begin_obj();
    j.key("schema");
    j.str("calibration_runner/1");
    j.key("maps_dir");
    j.str(opt.maps_dir);
    j.key("stats_fingerprint");
    j.val(static_cast<std::int64_t>(stats.fingerprint()));
    j.key("max_waves");
    j.val(opt.max_waves);
    j.key("max_ticks");
    j.val(opt.max_ticks);
    j.key("maps");
    j.begin_arr();
    for (const MapInfo& m : maps) write_map_info(j, m);
    j.end_arr();
    j.key("runs");
    j.begin_arr();
    for (const RunRecord& r : runs) write_run(j, r);
    j.end_arr();
    j.end_obj();
    const std::string text = j.take();

    // ——输出——
    if (opt.out_path.empty()) {
        std::cout << text << "\n";
    } else {
        const std::string bytes = text + "\n";
        if (!rts::write_file_bytes(
                opt.out_path,
                reinterpret_cast<const unsigned char*>(bytes.data()),
                bytes.size())) {
            std::cerr << "写不出输出文件: " << opt.out_path << "\n";
            return 1;
        }
    }
    return 0;
}
