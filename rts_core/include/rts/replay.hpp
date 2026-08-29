// 确定性回放：格式、录制器、比对骨架。
//
// `CLAUDE.md` 把这件事列为**主要的防 bug 手段**，原文是：
//
//   > **确定性回放测试是核心测试形态**：存一串随机种子 + 指令流，
//   > 重放必须逐 tick 状态一致。新增任何影响仿真的改动都要能通过回放测试。
//
// 本文件是那句话的字节层落地，也是 `rts_core 接口契约.md` §2 的最后一项。
//
// ## 回放里存什么、不存什么——这是本文件唯一真正需要论证的判决
//
// 回放 = **建局参数** + **外生输入流** + **周期性状态哈希**。就这三样。
//
// 「外生」是关键限定词，它把 `World` 的公开方法切成两类：
//
// | 类 | 例子 | 进回放吗 |
// |---|---|---|
// | **外生输入**：只能来自人或策略，仿真自己推不出来 | `submit`、`submit_actions` | **存** |
// | **机制产物**：在完整的仿真里由规则算出来 | `spawn_unit`、`kill_unit`、`place_bld`、`begin_next_wave` | **不存** |
//
// 第二类不存的理由不是省字节，而是**存了会让回放变成假绿灯**：
// 若把 `kill_unit` 也记进流里，重放时就会照着流把那个单位杀掉——
// 于是即便战斗解算算出了完全不同的结果，回放照样对得上。
// 那正好把回放存在的意义抵消掉：它要查的就是「解算是不是每次都一样」。
// **一份记录了机制产物的回放，测的是它自己的日志，不是仿真。**
//
// 代价是诚实的：本版（1b-2）机制尚未实现，所以**初始状态必须由
// `WorldInit` 完整表达**，包括初始单位——这正是 `WorldInit::units`
// 存在的原因（见 `rts/world.hpp` 那段）。缺了它，两条输入通道里
// 字节量占九成的 `submit_actions` 在任何回放里都走不到。
//
// ## 地图按**引用**存，不嵌入
//
// 头里只有 `map_id` 与 32 字节的 `map_content_hash`（`地图与场景设计.md` 6.3）。
// 于是回放文件是几 KB 而不是几百 KB，而「地图在回放录完之后被改过」
// 会以哈希不符的形式当场报出来，不会静默地跑出一局不同的仗。
//
// 代价：重放需要那张地图在手。这是可接受的——演示地图入库，训练地图由
// `tools/map_gen/` 按种子重生成。
//
// ## 头里那四个「让诊断不再是猜」的字段
//
// 回放对不上时，**最贵的不是修 bug，是分辨这到底是不是 bug**。
// 四个字段各消掉一种误判：
//
// 1. **`platform_fp`** —— 编译器 + 操作系统 + 架构。`CLAUDE.md` 明写
//    「回放文件在 Windows 与 Linux 之间不可复现」，而本项目**天生跨两套工具链**
//    （游戏在 MSVC、训练在 GCC）。没有这个字段，跨平台比对会表现为
//    「第 N tick 状态不一致」，然后有人花一天去找一个不存在的缺陷。
//
//    注意它**不导致拒绝加载**：本版仿真里一次浮点运算都没有，所以跨平台
//    很可能照样对得上。硬拒绝会挡掉有用的比对，而把差异如实报出来
//    并附一句「别据此建立跨平台工作流」才是对的。
//
// 2. **`hash_tag`** —— `kWorldHashTag`，即 `state_hash()` 喂入的第一个串。
//    状态布局一改它就改，于是「哈希口径变了、旧回放该重录」与
//    「仿真真的跑歪了」是两条不同的诊断。
//
//    **这一条同时是在途弹丸那个悬而未决的问题的答案。** 原先打算的兜底是
//    「在回放里给弹丸留一组空的实体位」；有了这个字段就不需要了——
//    1c 若加第四组实体，改 `kWorldHashTag` 即可，失效会以「重录」的形式报出来。
//    留空位反而更糟：它是一个必须被后人维护、且永远不知道能不能删的坑。
//
// 3. **`file_hash`** —— 全文 FNV-1a。截断或损坏的文件报「文件坏了」，
//    不报「第 N tick 状态不一致」。
//
// 4. **`stats_fp`** —— 数值表指纹（`rts/stats.hpp`）。数值表是外生输入，
//    改一个数字就该重录；没有这个字段，症状是「第 0 tick 状态不一致」，
//    与「地图变了」「初始单位不同」并回同一句话（三者是 tick 0 分歧的
//    全部成因，前两个已各有字段分开）。见 `rts_core 接口契约.md` §1.1.2。
//
// **刻意不存 `kObsVersion` 与观测指纹。** 回放里没有观测——观测是从状态派生的。
// 存进去会暗示「改了观测布局则旧回放失效」，而那不成立，
// 于是每次动通道都要无意义地重录一批回放。
//
// ## 为什么记录器**包住** `World` 而不是「记得顺手记一笔」
//
// `ReplayRecorder` 转发 `submit` / `submit_actions` / `advance`。
// 若改成「调用 `World` 之后再调一次 `replay.push_*`」，那么漏记一次的症状是
// **回放对不上**——一个假阳性，而假阳性会让人开始不相信这套测试。
// 结构上封死比写在注释里可靠（`CLAUDE.md`「结构封死，而非数值劝退」的同一条准则）。
//
// ## 字节布局（全部小端）
//
// ```
// 偏移  长度  字段
//    0     8  magic = "RTSRPLY\x1A"
//    8     2  format_version        u16
//   10     4  header_bytes          u32   —— 头部主体的字节数
//   14    ..  头部主体：
//              2+n  platform_fp     u16 长度 + UTF-8
//              2+n  hash_tag        同上
//              2+n  map_id          同上
//               32  map_content_hash
//                8  stats_fp        u64   —— 数值表指纹
//                8  seed            u64
//                4  nominal_level   i32
//                4  hash_period     u32
//                4  total_ticks     i32
//                4  record_count    u32
//                4  cmd_count       u32   —— 载荷区命令总数
//                4  act_count       u32   —— 载荷区动作总数
//      16×N  记录区（定长 16 字节 × record_count，**按录制顺序**）：
//                4  tick   i32
//                1  kind   u8   0=Commands 1=Actions 2=Hash
//                1  side   u8   Hash 时无意义
//                2  count  u16  Hash 时为 0
//                8  hash   u64  仅 Hash 有意义
//       6×M  命令载荷（按记录出现顺序拼接）
//       1×K  动作载荷（同上）
//        8    file_hash    u64  —— 前面全部字节的 FNV-1a
// ```
//
// **记录是定长的**，因为那让「文件被截断」在读到第一条记录之前就被发现
// （`record_count × 16` 对不上剩余字节数），而不是解析到一半才崩。
// 为此 `Commands` / `Actions` 记录各浪费 8 字节，值。
//
// **哈希点在同一条流里，不单列一区。** 单列的话就要额外规定
// 「同一个 tick 上，哈希是在该 tick 的输入之前还是之后取的」——
// 而那条规定一旦与录制器的实际顺序不符，比对就会在一个完全正确的仿真上报错。
// 放进同一条流，顺序**就是**录制时的顺序，那条规定不必存在。
//
// ## 载荷里的枚举在**读入时**就校验
//
// 文件可以来自任何地方（手写、截断、将来某版写坏了）。
// `static_cast<CommandKind>(0xFF)` 之后进 `switch` 是未定义行为，
// 所以边界上就把范围查掉，而不是指望 `World::submit` 兜住。

#ifndef RTS_REPLAY_HPP
#define RTS_REPLAY_HPP

// ——一条铁律，在这里变成编译错误——
//
// `CLAUDE.md`：「服务器侧编译**不要开 `-ffast-math`**，MSVC 侧保持默认
// `/fp:precise`，不要用 `/fp:fast`」。开了它，浮点求值顺序与结合律都可能被重排，
// 于是同一份代码在同一台机器上都不保证给同一个结果——回放测试随之失去意义，
// 而症状是**偶发**的哈希不符，最难定位的一种。
//
// 放在本文件是因为它在这里**每次构建都会被检查**：`rts_core/CMakeLists.txt`
// 的头文件自足性守卫给 `include/rts/` 下每个头各生成一个只包含它自己的 TU。
// 更理想的位置是构建配置，但那要改 `cmake/` 里两条工具链各自的编译选项，
// 与队友的 PR 会撞；效果相同，先落在这里。
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#  error "本项目禁止 -ffast-math / /fp:fast：它会重排浮点求值顺序，" \
         "使确定性回放（CLAUDE.md 的主要防 bug 手段）失效。见 CLAUDE.md「确定性要求」。"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rts/action.hpp"
#include "rts/command.hpp"
#include "rts/types.hpp"
#include "rts/world.hpp"

namespace rts {

// 8 字节。末尾那个 `\x1A` 是 DOS 的文件结束符，作用是 `type`（Windows）
// 或 `cat` 撞上它就停下，不会把一屏二进制喷进终端。
inline constexpr std::string_view kReplayMagic = "RTSRPLY\x1A";
static_assert(kReplayMagic.size() == 8);

// **改字节布局就 +1。** 与 `kWorldHashTag` 分工不同：
// 这个管「文件怎么排」，那个管「状态怎么折成一个数」。
// 两者会各自独立地变，所以是两个字段。
//
// 1 → 2：头部加了 `stats_fp`（数值表指纹）。这次两个都动了（布局变了、
// 喂入清单也变了），但那是巧合不是规律——上一次 `World/1 → World/2` 就只动了一边。
inline constexpr std::uint16_t kReplayFormatVersion = 2;

// ——平台指纹——
//
// 拼成 `编译器-版本/系统/架构`，例如 `msvc-1943/win/x64`、`gcc-13.2/linux/x64`。
//
// 三项都会改变浮点结果：编译器（求值顺序、是否收缩成 FMA）、
// 系统（标准库实现）、架构（x87 vs SSE、有无 FMA 指令）。
//
// **不含浮点模型一项**：`-ffast-math` / `/fp:fast` 已经在上面变成编译错误了，
// 所以那一栏只可能有一个取值。指纹里放一个恒定的字段是误导——
// 它会让人以为那一栏在变。
#define RTS_REPLAY_STR2(x) #x
#define RTS_REPLAY_STR(x) RTS_REPLAY_STR2(x)

// clang 必须排在最前：它同时定义 `__GNUC__`，而 clang-cl 还定义 `_MSC_VER`。
#if defined(__clang__)
#  define RTS_REPLAY_CC "clang-" RTS_REPLAY_STR(__clang_major__) "." \
                                 RTS_REPLAY_STR(__clang_minor__)
#elif defined(_MSC_VER)
#  define RTS_REPLAY_CC "msvc-" RTS_REPLAY_STR(_MSC_VER)
#elif defined(__GNUC__)
#  define RTS_REPLAY_CC "gcc-" RTS_REPLAY_STR(__GNUC__) "." RTS_REPLAY_STR(__GNUC_MINOR__)
#else
#  define RTS_REPLAY_CC "cc-unknown"
#endif

#if defined(_WIN32)
#  define RTS_REPLAY_OS "win"
#elif defined(__linux__)
#  define RTS_REPLAY_OS "linux"
#elif defined(__APPLE__)
#  define RTS_REPLAY_OS "mac"
#else
#  define RTS_REPLAY_OS "os-unknown"
#endif

#if defined(_M_X64) || defined(__x86_64__)
#  define RTS_REPLAY_ARCH "x64"
#elif defined(_M_ARM64) || defined(__aarch64__)
#  define RTS_REPLAY_ARCH "arm64"
#elif defined(_M_IX86) || defined(__i386__)
#  define RTS_REPLAY_ARCH "x86"
#else
#  define RTS_REPLAY_ARCH "arch-unknown"
#endif

inline constexpr std::string_view kPlatformFingerprint =
    RTS_REPLAY_CC "/" RTS_REPLAY_OS "/" RTS_REPLAY_ARCH;

// 宏用完就撤，不往包含者身上留东西。
#undef RTS_REPLAY_STR2
#undef RTS_REPLAY_STR
#undef RTS_REPLAY_CC
#undef RTS_REPLAY_OS
#undef RTS_REPLAY_ARCH

// ——记录——

enum class ReplayRecordKind : std::uint8_t {
    Commands = 0,
    Actions = 1,
    Hash = 2,
};

inline constexpr int kReplayRecordKindCount = 3;

constexpr std::string_view ident_of(ReplayRecordKind k) noexcept {
    switch (k) {
        case ReplayRecordKind::Commands: return "Commands";
        case ReplayRecordKind::Actions:  return "Actions";
        case ReplayRecordKind::Hash:     return "Hash";
    }
    return {};
}

struct ReplayRecord {
    Tick tick = 0;
    ReplayRecordKind kind = ReplayRecordKind::Commands;
    Side side = Side::Defender;      // `Hash` 时无意义
    std::uint16_t count = 0;         // `Hash` 时为 0
    std::uint32_t offset = 0;        // 进 `commands()` / `actions()` 的起点，**不入文件**
    std::uint64_t hash = 0;          // 仅 `Hash`
};

// 一份回放。既是读出来的，也是录出来的——只有一种格式，所以只需要一个类型。
class Replay {
public:
    Replay() = default;

    // 从一个**刚建好**的世界开始录：头部字段全部从它取。
    // `hash_period` 必须 >= 1（每多少 tick 记一个哈希点）。
    //
    // 要求 `w.now() == 0`：从中途开始录的回放无法只靠 `WorldInit` 验证——
    // 那需要状态快照，而快照是另一件事（`CLAUDE.md` 把回放定义为种子 + 指令流）。
    // 「从一个有单位的局面开始」由 `WorldInit::units` 表达，不需要中途起录。
    static Replay begin(const World& w, std::uint32_t hash_period);

    // ——头部——
    std::uint16_t format_version() const noexcept { return format_version_; }
    const std::string& platform_fingerprint() const noexcept { return platform_fp_; }
    const std::string& hash_tag() const noexcept { return hash_tag_; }
    const std::string& map_id() const noexcept { return map_id_; }
    const std::array<unsigned char, 32>& map_content_hash() const noexcept {
        return map_content_hash_;
    }
    std::uint64_t stats_fp() const noexcept { return stats_fp_; }
    std::uint64_t seed() const noexcept { return seed_; }
    std::int32_t nominal_level() const noexcept { return nominal_level_; }
    std::uint32_t hash_period() const noexcept { return hash_period_; }
    Tick total_ticks() const noexcept { return total_ticks_; }

    // ——内容——
    const std::vector<ReplayRecord>& records() const noexcept { return records_; }
    const std::vector<Command>& commands() const noexcept { return commands_; }
    const std::vector<UnitAction>& actions() const noexcept { return actions_; }

    // 哈希点的条数。**零条的回放验证不了任何东西**，`replay_verify` 会判它失败。
    int hash_count() const noexcept;

    // ——追加（`ReplayRecorder` 用；手写用例也用它造反例）——
    void push_commands(Tick t, Side s, const Command* p, std::size_t n);
    void push_actions(Tick t, Side s, const UnitAction* p, std::size_t n);
    void push_hash(Tick t, std::uint64_t h);
    void set_total_ticks(Tick t) noexcept { total_ticks_ = t; }

    // ——字节——
    std::vector<unsigned char> to_bytes() const;

    // `[[nodiscard]]`：忽略返回值会拿到一份空回放，而空回放**看起来像**一份
    // 合法回放。空的那份不会验证通过（零哈希点即失败），但把错误从
    // 「加载失败」推迟到「验证失败」会让报错指错地方。
    [[nodiscard]] static bool from_bytes(const unsigned char* data, std::size_t size,
                                         Replay* out, std::string* err);

    // 文件 I/O 一律经 `rts/utf8_path.hpp`（中文路径，见那份头）。
    [[nodiscard]] bool save(std::string_view utf8_path) const;
    [[nodiscard]] static bool load(std::string_view utf8_path, Replay* out,
                                   std::string* err);

private:
    std::uint16_t format_version_ = kReplayFormatVersion;
    std::string platform_fp_{kPlatformFingerprint};
    std::string hash_tag_{kWorldHashTag};
    std::string map_id_;
    std::array<unsigned char, 32> map_content_hash_{};
    std::uint64_t stats_fp_ = 0;
    std::uint64_t seed_ = 0;
    std::int32_t nominal_level_ = kMinUnitLevel;
    std::uint32_t hash_period_ = 1;
    Tick total_ticks_ = 0;

    std::vector<ReplayRecord> records_;
    std::vector<Command> commands_;
    std::vector<UnitAction> actions_;
};

// 录制器：把 `World` 的三个输入方法包起来，顺手记流。
//
// **不要绕过它直接调 `World`**——那样录出来的回放会缺一条输入，
// 重放时对不上，而那是一个假阳性。
class ReplayRecorder {
public:
    ReplayRecorder(World& w, std::uint32_t hash_period);

    void submit(Side side, const Command* cmds, std::size_t count);
    void submit_actions(Side side, const UnitAction* actions, std::size_t count);
    void advance(int ticks);

    const World& world() const noexcept { return *w_; }
    const Replay& replay() const noexcept { return r_; }

    // 记下末尾哈希点、写入 `total_ticks`，交出回放。调用之后本对象不再可用。
    Replay finish();

private:
    World* w_;
    Replay r_;
    bool finished_ = false;
};

// ——比对——

// **四项头部核对里有三项当场拦、一项留到分歧之后**，因为契约不同：
//
//   * `hash_tag` 不同 ⇒ 两个二进制对「状态是什么」的定义都不一样，
//     比对本身没有意义，哈希碰巧相等也不构成证据。**当场拦。**
//   * 地图身份不同 ⇒ 调用方拿错了地图，不是仿真的发现。**当场拦。**
//   * 数值表指纹不同 ⇒ 两局跑在不同的规则下，同上没有比对的意义。**当场拦。**
//   * `platform_fp` 只管**浮点**可复现性。纯整数的仿真跨平台确实给出同一个结果，
//     所以那里的 `Match` 是真话，硬拦会挡掉有用的比对。
//     它因此只在真的对不上时才升格成成因。
enum class ReplayVerdict : std::uint8_t {
    Match = 0,          // 每个哈希点都对上了
    Diverged,           // 对不上，且平台指纹也一致 ⇒ **这是一个真的确定性缺陷**
    PlatformMismatch,   // 对不上，且平台指纹不同 ⇒ 预期，换回录制平台再比
    HashTagMismatch,    // 状态哈希口径不同 ⇒ 比对没有意义，重录
    MapMismatch,        // 地图身份不同 ⇒ 换对的地图
    StatsMismatch,      // 数值表不同 ⇒ 表改过了，重录；不是确定性缺陷
    Vacuous,            // 一个哈希点都没有 ⇒ 验证不了任何东西，判失败
    BadInput,           // 给的世界不是新建的，或流里的输入应用不上
};

constexpr std::string_view ident_of(ReplayVerdict v) noexcept {
    switch (v) {
        case ReplayVerdict::Match:            return "Match";
        case ReplayVerdict::Diverged:         return "Diverged";
        case ReplayVerdict::PlatformMismatch: return "PlatformMismatch";
        case ReplayVerdict::HashTagMismatch:  return "HashTagMismatch";
        case ReplayVerdict::MapMismatch:      return "MapMismatch";
        case ReplayVerdict::StatsMismatch:    return "StatsMismatch";
        case ReplayVerdict::Vacuous:          return "Vacuous";
        case ReplayVerdict::BadInput:         return "BadInput";
    }
    return {};
}

struct ReplayResult {
    ReplayVerdict verdict = ReplayVerdict::BadInput;

    // 三项头部核对。**`platform_matches` 即使在 verdict == Match 时也要看**：
    // 本版仿真没有浮点运算，跨平台很可能照样对上，
    // 但那不构成「跨平台回放可复现」——不要据此建工作流（CLAUDE.md）。
    // 另两项不同时 verdict 一定不是 Match（见上）。
    bool platform_matches = false;
    bool hash_tag_matches = false;
    bool map_matches = false;
    // 数值表指纹。契约同 `map_matches`：不同 ⇒ 两局跑在不同的规则下，
    // 比对没有意义，**当场拦**（`StatsMismatch`）。
    bool stats_matches = false;

    int checked_hashes = 0;
    Tick first_divergent_tick = -1;      // -1 = 无分歧
    std::uint64_t expected = 0;
    std::uint64_t actual = 0;
    std::string message;                 // 人读的诊断，区分上面那几类

    bool ok() const noexcept { return verdict == ReplayVerdict::Match; }
};

// 拿一个**刚建好的**世界重放 `r`，逐哈希点比对。
//
// 取 `World&` 而不是工厂或 `WorldInit`，理由有两条：
//   * `rts_core` 不读地图文件（那是 `game/` 的事），所以世界只能由调用方建
//   * 「刚建好」是可检查的（`now() == 0`），而工厂是否每次给同一个世界不可检查
//
// `CLAUDE.md` 禁止仿真里出现虚函数分派，所以这里也没有回调接口——
// 它是一个普通函数，不需要模板也不需要虚基类。
ReplayResult replay_verify(const Replay& r, World& fresh);

}  // namespace rts

#endif  // RTS_REPLAY_HPP
