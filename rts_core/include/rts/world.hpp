// 世界状态：三组扁平数组 + 输入队列 + 迷雾 + 状态哈希。
//
// 这是 `rts_core 接口契约.md` §2 表里的第一行，落地的是决定 ①（两段式
// `submit` / `advance`）、⑤（三组句柄）、⑥（每侧一份迷雾）。
//
// ## 本版**没有任何机制**，这是刻意的分片，不是半成品
//
// 战斗解算、寻路、视野、经济归 1c（@ArLiangz，#28）。本文件给的是
// **它们要写进去的那个形状**：数据在哪、句柄怎么失效、输入怎么进来、
// 状态怎么压成一个可比对的标量。
//
// 「没有机制」的边界画在一条清晰的线上：**凡是需要一个待标定数值才能推进的，
// 本版不做；凡是不需要的，本版做完**。于是 `advance()` 不是空操作——
// 它推进 tick、排空命令队列、把能在无数值前提下完全应用的命令当场应用、
// 递减两个纯计数器（攻击前摇、施工进度）。这一点很重要：
// **一个什么都不做的 `advance()` 会让回放测试变成永远绿的摆设。**
//
// 剩下六种命令（`Build` / `Repair` / `Cancel` / `Train` / `MoveForce` /
// `Garrison`）需要造价与耗时，所以本版不解算它们——但**不静默丢弃**，
// 而是累加到 `deferred_command_count()`。于是「本版没做这一类」是一个
// 可以被读出、可以被断言的事实。1c 实现它们时把计数器一并删掉。
//
// ## 谁能写、谁只能读
//
// `World` 是唯一可写的入口；`render/`、脚本、观测打包一律经 `WorldView`
// （`rts/world_view.hpp`，决定 ④）。**不变量 2 因此是编译期的，不靠约定。**
//
// ## 数值一个都不在这里
//
// 血量、造价、速度、射程、破坏速率全部由**调用方**给（`WorldInit` 与
// `spawn_unit` 的参数）。`World` 只搬运与记账。这是契约规则
// 「接口与回放格式的承诺不得依赖任何待定数值」在数据布局这一层的形式：
// 那份 JSON 数值表还不存在，而本文件的字节布局已经可以定死。

#ifndef RTS_WORLD_HPP
#define RTS_WORLD_HPP

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "rts/action.hpp"
#include "rts/command.hpp"
#include "rts/fog.hpp"
#include "rts/hash.hpp"
#include "rts/rng.hpp"
#include "rts/roster.hpp"
#include "rts/terrain.hpp"
#include "rts/types.hpp"

namespace rts {

// 调用方违反了接口契约：动作数组长度不对、命令给错了侧、句柄已失效、槽位越界。
//
// **为什么是异常而不是断言。** `assert` 只在 Debug 生效，而这些错误的唯一
// 现实来源是 Python 侧（`bindings/`）——训练一律跑 Release。断言在那条路径上
// 等于没写，而症状会是「训练不收敛」：动作数组少一格，此后每个单位都拿到
// 邻居的动作，一切照常运行。
//
// **它不在 tick 热路径上。** 抛出点全在 `submit*` 与按句柄的访问器里，
// 每个决策步各一次；`advance()` 内部不抛。热路径读的是 `WorldView` 的连续数组，
// 那条路上一次检查都没有。
class ContractError : public std::logic_error {
public:
    explicit ContractError(const std::string& what) : std::logic_error(what) {}
};

// 波次阶段。一波 = 一个 RL episode，波与波之间是建造 / 准备阶段（CLAUDE.md）。
enum class WavePhase : std::uint8_t {
    Build = 0,     // 建造 / 准备
    Assault = 1,   // 攻方已发起进攻
};

inline constexpr int kWavePhaseCount = 2;

constexpr std::string_view ident_of(WavePhase p) noexcept {
    switch (p) {
        case WavePhase::Build:   return "Build";
        case WavePhase::Assault: return "Assault";
    }
    return {};
}

// ——槽位池：代数 + 空闲表——
//
// 三组实体共用这一份簿记。模板而非虚函数——CLAUDE.md 从结构上禁止逐实体多态
// 分派，而这里本来也不需要运行期多态：三组的**字段**不同，**簿记**完全相同。
//
// `Tag` 直接沿用 `rts/types.hpp` 里那三个空 tag，于是 `SlotPool<UnitTag>` 的
// `Id` 就是 `UnitId`，传错一组在编译期就挡住了。
template <class Tag>
class SlotPool {
public:
    using Id = Handle<Tag>;

    // 取一个槽位。**优先复用空闲槽，且是 LIFO。**
    //
    // LIFO 不是随口定的：它必须是确定的，否则同一串输入会在两次运行里落到
    // 不同的下标上，而下标顺序决定了 `state_hash` 的喂入顺序与观测的打包顺序。
    // 「复用哪个槽」于是是仿真状态的一部分，见下面 `feed_hash` 里为什么连
    // 空闲表一起喂。
    std::uint16_t acquire() {
        if (!free_.empty()) {
            const std::uint16_t k = free_.back();
            free_.pop_back();
            alive_[k] = 1;
            return k;
        }
        if (slot_count() >= static_cast<std::size_t>(Id::kMaxIndex)) {
            throw ContractError("实体槽位耗尽：索引上限是 Handle::kMaxIndex");
        }
        generation_.push_back(0);
        alive_.push_back(1);
        return static_cast<std::uint16_t>(slot_count() - 1);
    }

    // 释放槽位并**推进代数**。旧句柄随即失配，于是悬空引用变成一次可检出的
    // 失败，而不是静默读到后来装进这个槽的另一个实体。
    void release(std::uint16_t k) {
        assert(k < slot_count());
        assert(alive_[k] != 0);
        alive_[k] = 0;
        // 代数溢出会让一个很老的句柄重新变得「有效」。65536 次复用同一个槽在
        // 一局里够不着（单波单位数硬上限约 40），但这是「该红却绿」的形态，
        // 所以让它环绕到 1 而不是 0——0 只属于从未被复用过的新槽。
        generation_[k] = generation_[k] == 0xFFFFu
                             ? std::uint16_t{1}
                             : static_cast<std::uint16_t>(generation_[k] + 1);
        free_.push_back(k);
    }

    bool alive(Id id) const noexcept {
        if (!id.valid()) return false;
        const std::size_t k = id.index();
        return k < slot_count() && alive_[k] != 0 && generation_[k] == id.generation();
    }

    Id id_at(std::uint16_t k) const noexcept {
        assert(k < slot_count());
        return Id::make(k, generation_[k]);
    }

    bool alive_at(std::uint16_t k) const noexcept {
        assert(k < slot_count());
        return alive_[k] != 0;
    }

    std::size_t slot_count() const noexcept { return generation_.size(); }

    int live_count() const noexcept {
        int n = 0;
        for (const std::uint8_t a : alive_) n += (a != 0) ? 1 : 0;
        return n;
    }

    const std::uint8_t* alive_bytes() const noexcept { return alive_.data(); }

    // **空闲表与代数都进哈希。**
    //
    // 只喂「哪些槽活着」是不够的：两个世界可以实体完全一致，而空闲表顺序不同，
    // 于是**下一次 spawn 落在不同的槽**、此后一路分叉。回放的价值在于尽早发现
    // 分叉，把这两张表漏掉就等于把发现时机推迟到它影响到某个动作之后。
    void feed_hash(StateHash& h) const noexcept {
        h.feed(alive_.data(), alive_.size());
        h.feed(generation_.data(), generation_.size() * sizeof(std::uint16_t));
        h.feed(free_.data(), free_.size() * sizeof(std::uint16_t));
    }

private:
    std::vector<std::uint16_t> generation_;
    std::vector<std::uint8_t> alive_;
    std::vector<std::uint16_t> free_;   // LIFO
};

// ——建局参数——
//
// 全部**持有**（`std::vector` 而不是 `std::span`）。建一局是每 episode 一次，
// 不在热路径上，而 span 会带来一整类生命周期问题：`WorldInit` 天然是个临时量，
// 它指向的 `MapData` 什么时候析构不由这里决定。

// 集结点。**下标即 `CommandKind::PickSpawn` 的 `slot`。**
//
// 刻意**不带走廊种类**（`game::CorridorKind`）：那是地图设计与校验器的概念
// （「每个集结点对应一条性质不同的走廊」是对**地图**的结构约束），
// 仿真不消费它。带进来会让人以为可以按走廊种类写分支，而那正是把
// 一条设计约束偷偷变成机制。
struct SpawnSite {
    GridPos pos{};
};

// 资源点。采集建筑只能建在它上面（CLAUDE.md：三种资源全部来自地图资源点）。
struct ResourceSite {
    GridPos pos{};
    Resource kind = Resource::Stone;
};

// 一座初始建筑。
//
// **`hp` 与 `max_hp` 都由调用方给**，`World` 不查任何表。初始城圈是**残破**的
// （`地图与场景设计.md` 2.3），地图文件给的是 `hp_frac`，把它乘成绝对值需要
// `max_hp`——而 `max_hp` 在那份还不存在的数值表里。于是这一步留在调用方，
// 契约本身对数值零依赖。
struct BldInit {
    BldType type = BldType::Wall;
    GridPos pos{};
    std::int64_t hp = 1;
    std::int64_t max_hp = 1;
};

// 一个初始单位。
//
// **这个结构是写回放格式时才发现需要的**，理由值得记下来，因为它是「先定契约、
// 再定回放」这个顺序的第一个产出：
//
// 回放 = 建局参数 + 外生输入流（`rts/replay.hpp`）。于是**初始状态只能由
// `WorldInit` 表达**——回放文件里没有别的地方能放它。而在加这个字段之前，
// `WorldInit` 只能描述「一座堡垒 + 若干墙段，零个单位」的世界，
// 那意味着 `submit_actions`（两条输入通道之一，且是字节量占九成的那一条）
// **在任何回放里都不可能被走到**：它要求动作数组长度恰好等于活着的单位数，
// 而那个数恒为 0。
//
// 换句话说，缺了它，回放测试会是一个「跑得通、但测不到主要通道」的摆设——
// 与 `advance()` 若是空操作的那个问题同一族。
//
// 与 `BldInit` 同样的纪律：`hp` / `max_hp` / `level` 全部由调用方给，
// `World` 不查任何表（数值表还不存在）。
struct UnitInit {
    UnitType type = UnitType::Ghoul;
    Vec2 pos{};
    std::int32_t level = kMinUnitLevel;
    std::int64_t hp = 1;
    std::int64_t max_hp = 1;
};

struct WorldInit {
    int width = 0;
    int height = 0;
    std::vector<Terrain> terrain;          // 行主序 y*w+x，长度 w*h
    std::vector<std::uint8_t> no_build;    // 同上；留空 = 全 0
    GridPos keep{};                        // 领主堡垒所在格
    std::vector<SpawnSite> spawns;
    std::vector<ResourceSite> resources;
    std::vector<BldInit> buildings;        // 必须含一座位于 `keep` 的 `Keep`
    // 初始单位。**顺序即槽位顺序**，因此它进 `state_hash`，也决定
    // `enumerate_units()` 的枚举次序——录回放时提交的动作数组按那个次序排，
    // 所以这里换个顺序等于换一份回放。
    std::vector<UnitInit> units;
    std::uint64_t seed = 0;

    // 回放要绑定到一张具体的地图（`地图与场景设计.md` 6.3）。**在建局时就记下**，
    // 而不是等录回放时再去问地图：录制发生在世界已经跑起来之后，那时地图对象
    // 可能已经不在手上了，而「回放头里的 map_id 是从哪儿来的」不该有第二个答案。
    std::string map_id;
    std::array<unsigned char, 32> map_content_hash{};   // SHA-256

    // 本波名义等级 `L(w)` = 兵力预算 ÷ 编成位（决定 ⑨），观测里一切等级通道的分母。
    // 两条曲线都待标定，所以**由外部给**，`rts_core` 不知道任何曲线。
    std::int32_t nominal_level = kMinUnitLevel;
};

// ——`Command::slot` 对建造 / 维修 / 驻守这一族的编码：**格线性下标 `y*w + x`**——
//
// 这一条需要解释，因为它与 `守方AI与协同演化.md` 第 3 节说的「槽位 index，约 60 个」
// 看起来不一样。两者的关系与「建筑四组粗化」完全同构：
//
//   * **仿真接受的是完备信息**——玩家能点任意一格，所以 `slot` 必须能表达任意一格
//   * 决策层那个约 60 项的槽位列表是**地图给出的一个子集**，而地图格式里
//     还没有这个字段（`地图与场景设计.md` 未定）。从 60 项的动作头映射到具体格，
//     属策略侧，归 `train/`
//
// 于是编码只有一份，且是最粗的那一层。**上界必须被检查**：`slot` 是 uint16，
// 而 `kNoSlot = 0xFFFF` 被占，所以格数必须 < 65535。地图边长待标定
// （量级 64–128 ⇒ 最多 16384 格，余量很大），但 256×256 就会正好越界——
// 所以这是 `World` 构造时的一条运行期检查，不是一条注释。
inline std::uint16_t slot_of(GridPos p, int width) noexcept {
    return static_cast<std::uint16_t>(p.j * width + p.i);
}

inline GridPos pos_of_slot(std::uint16_t slot, int width) noexcept {
    return GridPos{static_cast<std::int16_t>(slot % width),
                   static_cast<std::int16_t>(slot / width)};
}

// ——状态哈希的口径标签——
//
// `state_hash()` 喂入的第一样东西。**改了状态布局就改这个串**，于是旧回放
// 当场对不上，而不是悄悄给出一个不同的数。
//
// 它之所以要**公开**（而不是留在 `src/world.cpp` 里当一个字面量），
// 是因为回放文件头要存一份（`rts/replay.hpp`）。有了它，
// 「哈希口径变了」与「仿真真的跑歪了」在诊断上是两件事：
// 前者报「这份回放是用 World/1 录的，本二进制是 World/2，重录」，
// 后者报「第 N tick 状态不一致」。少了它，两者都表现为后者，
// 而后者会让人去找一个不存在的确定性缺陷。
//
// **这一条正是在途弹丸那个悬而未决的问题的答案。** 若 1c 把弹丸做成第四组实体，
// `state_hash` 的喂入清单就变了，所有已录回放随之失效——但只要这个标签一起改成
// `"World/2"`，失效就会以「口径变了、重录」的形式报出来。所以回放格式
// **不需要预留一组空的实体位**（那是原先打算的兜底），需要的只是改布局时
// 顺手改这个串。
inline constexpr std::string_view kWorldHashTag = "World/1";

class WorldView;

class World {
public:
    // 构造即校验：尺寸、数组长度、坐标在界内、`keep` 处确有一座 `Keep`。
    // 不合法抛 `ContractError`——与 `game::MapData`「存在即合法」同一条纪律。
    explicit World(WorldInit init);

    // ——时间与波次——
    Tick now() const noexcept { return tick_; }
    int wave() const noexcept { return wave_; }
    WavePhase phase() const noexcept { return phase_; }
    std::int32_t nominal_level() const noexcept { return nominal_level_; }

    int width() const noexcept { return terrain_.width(); }
    int height() const noexcept { return terrain_.height(); }
    const TerrainMasks& terrain() const noexcept { return terrain_; }
    GridPos keep_pos() const noexcept { return keep_; }
    const std::vector<SpawnSite>& spawns() const noexcept { return spawns_; }
    const std::vector<ResourceSite>& resources() const noexcept { return resources_; }
    const std::string& map_id() const noexcept { return map_id_; }
    const std::array<unsigned char, 32>& map_content_hash() const noexcept {
        return map_content_hash_;
    }

    // 建局种子。回放文件头存一份（`rts/replay.hpp`）：验证时世界由调用方自己
    // 建好，所以这一份是**交叉核对**用的——「这份回放当初用的是哪个种子」
    // 在回放对不上时是第一个要看的东西。
    //
    // **刻意不进 `state_hash`**：播种之后的 RNG 状态已经在哈希里了，
    // 而它是种子的单射函数。存两份等于给同一件事两个真相来源。
    std::uint64_t seed() const noexcept { return seed_; }

    // 波次推进。**两者都是公开的，因为触发条件在 1c**：
    // 「波长到了」与「本波打完了」都要读机制。本版的触发者只有
    // `CommandKind::Summon`（提前召唤，CLAUDE.md 明写「必须提供」）与测试。
    void begin_assault() noexcept;
    // 进入下一波。`nominal_level` 必须一并给——它是本波兵力预算的函数，
    // 而那条曲线在 `rts_core` 之外。
    void begin_next_wave(std::int32_t next_nominal_level);

    // ——输入（决定 ①：两段式，对「哪一侧由谁控制」保持中立）——
    //
    // `submit*` 只入队并校验，**不改世界**；`advance()` 才排空队列。
    // 于是人类的异步输入与 RL 的一次一批走同一条路。

    // 玩家级 / 宏观命令。逐条校验：`is_legal_for(kind, side)`、`c.side == side`、
    // 以及用到 `slot` 的那几种的槽位范围。任一条不过就抛，**一条都不入队**
    // （全有或全无：半批入队会让「这次提交失败了」与「部分生效了」不可区分）。
    void submit(Side side, const Command* cmds, std::size_t count);

    // 战术动作，**一个活着的单位一个**，按下面 `enumerate_units` 的顺序。
    //
    // 长度必须**恰好**等于 `live_unit_count(side)`，多一格少一格都抛。
    // 这条检查是这个函数存在的主要理由：观测按同一顺序打包，
    // 长度错位的症状是「每个单位都拿到邻居的动作」——不报错、不崩、只是学不动。
    void submit_actions(Side side, const UnitAction* actions, std::size_t count);

    // 推进 `ticks` 个 tick。`ticks == 0` 是合法的空操作。
    //
    // 每个 tick 内的顺序固定：**排空命令队列（守方先、攻方后，各按提交顺序）
    // → 递减前摇与施工进度 → tick + 1**。顺序固定是确定性的一部分，
    // 改它等于让所有已录回放失效。
    void advance(int ticks);

    // ——实体——
    //
    // `hp` / `max_hp` / `level` 全部由调用方给（见文件头「数值一个都不在这里」）。
    UnitId spawn_unit(UnitType type, Vec2 pos, std::int32_t level,
                      std::int64_t hp, std::int64_t max_hp);
    BldId place_bld(BldType type, GridPos pos, std::int64_t hp, std::int64_t max_hp,
                    std::int32_t work_left = 0);
    ObstacleId place_obstacle(ObstacleType type, GridPos pos, std::int64_t hp,
                              std::int64_t max_hp);

    void kill_unit(UnitId id);
    void destroy_bld(BldId id);
    void destroy_obstacle(ObstacleId id);

    bool alive(UnitId id) const noexcept { return unit_pool_.alive(id); }
    bool alive(BldId id) const noexcept { return bld_pool_.alive(id); }
    bool alive(ObstacleId id) const noexcept { return obstacle_pool_.alive(id); }

    int live_unit_count() const noexcept { return unit_pool_.live_count(); }
    int live_unit_count(Side side) const noexcept;
    int live_bld_count() const noexcept { return bld_pool_.live_count(); }
    int live_obstacle_count() const noexcept { return obstacle_pool_.live_count(); }

    // **一侧活着的单位，按槽位下标升序。这是唯一的规范顺序。**
    //
    // `submit_actions` 按它取动作，观测打包必须按它写行。两处各自遍历一遍
    // 是可以的（顺序由本函数定义），但两处都不许自己另定一个顺序——
    // 那种错位不报错。
    //
    // 填进调用方给的 vector（而不是返回一个新的），因为它每个决策步都要用一次，
    // 复用同一块内存就没有分配。
    void enumerate_units(Side side, std::vector<UnitId>& out) const;

    // 按句柄读。句柄失效即抛（理由见 `ContractError`）。
    // **热路径不要走这里**，走 `WorldView` 的连续数组。
    UnitType unit_type(UnitId id) const;
    Side unit_side(UnitId id) const;
    std::int32_t unit_level(UnitId id) const;
    std::int64_t unit_hp(UnitId id) const;
    Vec2 unit_pos(UnitId id) const;
    UnitAction unit_action(UnitId id) const;
    std::int32_t unit_windup(UnitId id) const;

    BldType bld_type(BldId id) const;
    GridPos bld_pos(BldId id) const;
    std::int64_t bld_hp(BldId id) const;
    bool bld_complete(BldId id) const;   // 施工 / 维修进度是否已走完

    // ——资源——
    std::int64_t stock(Resource r) const noexcept {
        return stock_[static_cast<std::size_t>(r)];
    }
    // 存量的增减在 1c（产出速率与造价都是数值）。本版只提供存取，
    // 让「资源是仿真状态、要进哈希」这一条先定死。
    void set_stock(Resource r, std::int64_t v) noexcept;

    // ——攻方宏观状态——
    //
    // 两者都由命令写入，本版就能完整应用（都不需要任何数值）。
    std::uint16_t composition(UnitType t) const noexcept {
        return composition_[static_cast<std::size_t>(t)];
    }
    bool spawn_chosen(std::size_t spawn_index) const;
    std::uint8_t selected_force(Side side) const noexcept {
        return selected_force_[static_cast<std::size_t>(index_of(side))];
    }

    // 本版没解算的命令，按种类计数。**1c 实现那六种时把这个函数一并删掉。**
    std::int64_t deferred_command_count(CommandKind k) const noexcept {
        return deferred_[static_cast<std::size_t>(k)];
    }

    // ——迷雾（决定 ⑥）——
    //
    // 取任一侧的迷雾。**这是 god 视角**，双视图演示要的正是它
    // （真实战场 vs AI 的记忆图）。策略永远拿不到它——策略只见打包好的张量，
    // 而硬要求 1 落在打包那一层，见 `rts/world_view.hpp` 里那段说明。
    const FogLayer& fog(Side side) const noexcept {
        return fog_[static_cast<std::size_t>(index_of(side))];
    }
    FogLayer& fog_mut(Side side) noexcept {
        return fog_[static_cast<std::size_t>(index_of(side))];
    }

    // ——动作掩码——
    //
    // 13 位，第 k 位 = `action_at(k)` 是否合法。掩码的作用是别让 agent 把样本
    // 浪费在学习规则本身（CLAUDE.md）。
    //
    // **本版只算「不需要数值就能判」的那部分**：8 个移动方向按地形与边界判，
    // 斜向额外吃 `kDiagonalNeedsBothOrthogonal`。攻击类四位与 `Stop` 一律置 1。
    //
    // 攻击位默认**允许**而不是默认禁止，方向是刻意选的：错误地允许只是浪费样本，
    // 错误地禁止会让 agent **永远学不到**那个动作，而后者不会有任何东西提示。
    std::uint16_t action_mask(UnitId id) const;

    // 11 位，第 k 位 = `command_kind_at(k)` 是否合法。
    // 本版按 `is_legal_for` 与波次阶段判；买得起买不起要造价，属 1c。
    std::uint16_t command_mask(Side side) const noexcept;

    // ——状态哈希——
    //
    // 回放比对的基元。**喂入顺序固定**，见 `src/world.cpp` 里那段清单。
    //
    // 代价值得知道：它要走一遍全部实体数组**与两侧的迷雾**，
    // 128×128 的地图上迷雾是主项（约 2 × 1.4 MB）。所以回放**周期性**取哈希，
    // 不是每 tick 取（决定 ⑦）。测试在小地图上逐 tick 取，那没问题。
    std::uint64_t state_hash() const noexcept;

    WorldView view(Side side) const noexcept;

private:
    friend class WorldView;

    std::size_t require(UnitId id) const;
    std::size_t require(BldId id) const;
    std::size_t require(ObstacleId id) const;

    void apply_one(Side side, const Command& c);
    void validate(Side side, const Command& c) const;

    // ——静态（建局时定，之后不变）——
    TerrainMasks terrain_;
    GridPos keep_{};
    std::vector<SpawnSite> spawns_;
    std::vector<ResourceSite> resources_;
    std::string map_id_;
    std::array<unsigned char, 32> map_content_hash_{};
    std::uint64_t seed_ = 0;

    // ——时间与波次——
    Tick tick_ = 0;
    int wave_ = 1;
    WavePhase phase_ = WavePhase::Build;
    std::int32_t nominal_level_ = kMinUnitLevel;
    Rng rng_;

    // ——单位（SoA）——
    //
    // SoA 而不是 AoS：`WorldView` 要给出连续数组，观测打包才能退化成
    // 「一次遍历 + 写通道」而不是逐单位 gather（CLAUDE.md「AoS 还是 SoA」）。
    //
    // **`side` 不存**：它由 `side_of(type)` 决定（`rts/roster.hpp`），
    // 存一份就是第二个真相来源，而两个真相来源迟早只更新一个。
    SlotPool<UnitTag> unit_pool_;
    std::vector<UnitType> u_type_;
    std::vector<std::int32_t> u_level_;
    std::vector<std::int64_t> u_hp_;
    std::vector<std::int64_t> u_max_hp_;
    std::vector<Vec2> u_pos_;
    std::vector<std::int32_t> u_windup_;      // 攻击前摇剩余 tick，0 = 可出手
    std::vector<UnitAction> u_action_;        // 上个决策边界选的动作，保持 4–8 tick
    std::vector<std::uint16_t> u_garrison_;   // 驻守的墙段槽位，kNoSlot = 没上墙
    std::vector<std::uint8_t> u_force_;       // 编队，kNoForce = 未编队

    // ——建筑（全部属守方，见 `rts/roster.hpp`：不存 side）——
    SlotPool<BldTag> bld_pool_;
    std::vector<BldType> b_type_;
    std::vector<GridPos> b_pos_;
    std::vector<std::int64_t> b_hp_;
    std::vector<std::int64_t> b_max_hp_;
    std::vector<std::int32_t> b_work_;       // 施工 / 维修剩余 tick，0 = 完工

    // ——中立可破坏障碍（第三组，决定 ⑤）——
    SlotPool<ObstacleTag> obstacle_pool_;
    std::vector<ObstacleType> o_type_;
    std::vector<GridPos> o_pos_;
    std::vector<std::int64_t> o_hp_;
    std::vector<std::int64_t> o_max_hp_;

    // ——资源与宏观——
    std::array<std::int64_t, kResourceCount> stock_{};
    std::array<std::uint16_t, kUnitTypeCount> composition_{};
    std::vector<std::uint8_t> spawn_chosen_;
    std::array<std::uint8_t, kSideCount> selected_force_{kNoForce, kNoForce};

    // ——输入队列——
    std::array<std::vector<Command>, kSideCount> cmd_queue_;
    std::array<std::int64_t, kCommandKindCount> deferred_{};

    // ——迷雾，每侧一份——
    std::array<FogLayer, kSideCount> fog_;
};

}  // namespace rts

#endif  // RTS_WORLD_HPP
