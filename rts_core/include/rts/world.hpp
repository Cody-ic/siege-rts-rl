// 世界状态：三组扁平数组 + 输入队列 + 迷雾 + 状态哈希 + **机制第一批**。
//
// 这是 `rts_core 接口契约.md` §2 表里的第一行，落地的是决定 ①（两段式
// `submit` / `advance`）、⑤（三组句柄）、⑥（每侧一份迷雾）。
//
// ## 机制的进度：第一批已在，剩下的照旧记账
//
// 1c（tracker 是 #57）分批落地。**第一批**（数值全部经 `WorldInit::stats` 进来，
// 见 `rts/stats.hpp`）：
//
//   * **承伤与死亡**：伤害走整数与千分比（决定 ⑫，`rts/combat_math.hpp`），
//     血量归零即销毁；障碍被守方打掉产出资源（数额查表，种类是 `harvest_of()`）
//   * **目标选择与攻击**：四个 `Atk*` 按 §1.1.1 的乙定案——打不到就空转，
//     掩码的攻击 4 位随之升级为**精确**（射程内有没有合法目标）。
//     出手是「承诺（前摇开始，**落点当场锁定**）→ 落地（前摇走完）」两拍，
//     `Ram` 的 AOE 砸的是锁定的坐标，散开真的能躲（CLAUDE.md「结构破坏规则」）
//   * **相邻格移动**：按 `move_delta` 连续推进，地形 + 穿角 + 占位建筑/障碍拦路；
//     **撞上可破坏的阻挡物自动开始破坏**（寻路把障碍当「高代价可通行」的
//     第一半——绕不绕远由策略选方向，撞上去就砸是机制）
//   * **建筑攻击**：`Tower` 对地、`Flak` 仅对空（结构性，不是数值）
//   * **视野**：逐 tick 重算两侧迷雾（半径查表 + `blocks_vision` 视线遮挡，
//     空中单位不受遮挡），可见格上的建筑写进记忆图
//
// **第二批**（经济闭环与命令解算，src/mechanics.cpp 的 tick_economy 一族）：
//
//   * **五种命令**：`Build`（验占位与点位、扣石+木、落工地）、`Repair`（扣木、
//     排工时）、`Cancel`（工地退款 / 放弃维修）、`Train`（扣金、耗时、兵营旁出兵）、
//     `Clear`（**记入世界状态的清野指令**——守方单兵由脚本驱动，
//     脚本读它翻译成逐单位动作，World 不代替单位砸树）
//   * **施工与维修由工匠推进**：`b_work_ > 0` 的建筑只在守方 `Mason` 在半径内时
//     推进工时（「点杀施工中的工匠」因此是真的能打断工期，CLAUDE.md）
//   * **经济收入**：每结算周期，完工的采集建筑在对应资源点上产出
//     （`resource_of()`），`Keep` 恒产极少量金币（兵力地板）
//
// **第三批**（驻守与高度优势，src/mechanics.cpp 的 tick_garrison 一族）：
//
//   * **驻守由逐单位的登墙意愿驱动**（2026-09 起，编队系统整体移除）：
//     每个守方单位带一份「想登哪段墙」（`u_garrison_target_`，经
//     `submit_garrison_wishes` 与动作同批提交）。目标格上有完工的
//     `Wall` / `Gate`、该格无人且单位相邻时**自动登墙**（延迟查表），
//     一格一人、登顶后钉住；意愿清空或改指别段墙即下墙。
//     **全部命令自此都有解算**，`deferred_command_count()` 按约定已删
//   * **高度优势**（Stronghold 三条，CLAUDE.md「防御建筑与城墙是消耗品」）：
//     低处打墙上单位有 miss 且伤害打折、远程驻守吃射程加成（幅度全查表）；
//     **墙血 < 一半三者一起消失**（「局部破损」的二值实现，见 `on_high_wall`）；
//     墙被拆 ⇒ 强制下墙站缺口
//
// **第四批**（冲锋与齐射）：
//
//   * **冲锋助跑**：`Charge` 兵种逐 tick 累计移动距离（站停 / 撞停归零、
//     前摇期间冻结），落地那一击按动量乘 `charge_permille`、随即耗尽——
//     「伤害 ∝ 助跑距离」（机制等级无关，参数查表）。撞上可破坏阻挡物的
//     自动破坏同样把动量带进去（骑士撞门是真撞）
//   * **反冲锋（克制倍率的三轴推导）**：`Melee × HeavySlow`（能架住冲锋的阵，
//     `counters_charge()`）打**有动量**的 `Charge` 单位吃克制倍率，幅度随
//     对方动量线性放大——同一份动量既给冲锋加成也给枪阵加成，
//     「开阔地克、巷战被反克」因此不需要读地形。二部图里这条只绑定
//     `Spear ──► Knight`。**没有 N×N 表**：关系由轴推导，幅度是一个全局数
//   * **`Tower` 齐射**：建筑 AOE（半径查表），落点在前摇开始那一刻锁定
//     （与 `Ram` 同一条承诺规则）；圈内不分敌我（溅射误伤是机制）。
//     **对空建筑（`Flak`）结构上忽略 AOE**——「AA 只做单体狙击型」
//
// **第五批**（在途弹丸，第四组实体）：
//
//   * **谁放弹丸是结构**：单位侧 `launches_projectile()`（Ranged × 非空中，
//     恰好 = Archer / Shade；Phoenix 俯冲直击）；建筑侧凡开火皆弹丸
//     （Tower / Flak）。`Ram` 照旧是前摇撞击——CLAUDE.md「结构破坏规则」原文
//   * **两类飞行**：单体弹丸**追踪目标句柄**（箭朝一个单位去，目标死了箭落空）；
//     齐射弹丸**飞向锁定落点**（散开的躲闪窗口从「前摇」延长为「前摇 + 飞行」）。
//     逐目标的倍率（高度 miss / 减伤）在**命中那一刻**结算，
//     发射方的状态（等级、居高与否）在**放箭那一刻**定格随弹携带
//   * **速度查表**（`proj_speed`，<= 0 = 瞬时命中）：§1.1.1 那条「落地帧瞬时
//     结算」的书面近似没有删，而是降级成 0 速度的退化形态
//   * 弹丸**无句柄**（没人引用它）：SoA + 稳定压实，全部字段进哈希
//
// **第六批**（flow field 与出城，1c 清单就此归零）：
//
//   * **flow field 寻路**不在本类里——它是 `WorldView` 之上的**纯函数**
//     （`rts/flow.hpp`）：不进世界状态、不进哈希、不进回放（回放存的是脚本
//     按 field 选出的动作，不是 field）。等级量化 3 档（组内定夺，#57），
//     代价 = 行军 + 破坏（「墙段按高代价可通行」的另一半），通行规则与
//     tick_movement 的 cell_open 逐条对应
//   * **城门对自己人是通的**：守方地面单位可穿行完工的 `Gate`（cell_open 的
//     唯一实体例外）。没有这条，守方被自己的墙圈死，「被迫出城」在结构上
//     不可能发生。攻方照旧要砸开它
//
// **1c 的机制清单已清空。** 后续批次（守方脚本执行层、波次循环）不再是
// 「机制」——它们在 `World` 之外消费上面这些。
//
// 每个 tick 内的阶段顺序固定，见 `advance()` 的注释——**改顺序等于让所有
// 已录回放失效**（要动就把 `kWorldHashTag` 一起进格）。
//
// ## 谁能写、谁只能读
//
// `World` 是唯一可写的入口；`render/`、脚本、观测打包一律经 `WorldView`
// （`rts/world_view.hpp`，决定 ④）。**不变量 2 因此是编译期的，不靠约定。**
//
// ## 数值一个都不在这里
//
// 血量、造价、速度、射程、破坏速率全部由**调用方**给：批量的经
// `WorldInit::stats`（数值表，`rts/stats.hpp`），逐实体的经 `spawn_unit` 一族的
// 参数。`World` 存表、算指纹、搬运与记账，但**本文件的字节布局不依赖表里的
// 任何一个值**——这是契约规则「接口与回放格式的承诺不得依赖任何待定数值」
// 在数据布局这一层的形式。

#ifndef RTS_WORLD_HPP
#define RTS_WORLD_HPP

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "rts/action.hpp"
#include "rts/combat_math.hpp"
#include "rts/command.hpp"
#include "rts/fog.hpp"
#include "rts/hash.hpp"
#include "rts/rng.hpp"
#include "rts/roster.hpp"
#include "rts/stats.hpp"
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

// 一次已承诺攻击的目标是三组实体之一（或没有）。**这不是 `Side::Neutral` 的
// 变体**——它不进观测通道，只是「前摇落地时去找谁」的判别标签。
enum class TgtKind : std::uint8_t {
    None = 0,
    Unit = 1,
    Bld = 2,
    Obstacle = 3,
};

inline constexpr int kTgtKindCount = 4;

// 「没在练兵」的哨兵（`b_train_type_` 用）。不用 0：0 是 `UnitType::Archer`，
// 拿它当哨兵会让「没在练」与「在练弓手」在字节上不可区分——
// 同 `kNoSlot` 不取 0 的理由（`rts/command.hpp`）。
inline constexpr std::uint8_t kNoTrain = 0xFFu;

// 「这枚弹丸是单位射的」的哨兵（`p_src_bld_` 用；否则该值是 `BldType`）。
// 不用 0 的理由同上（0 是 `BldType::Keep`）。渲染层用它挑精灵：
// `Flak` 的是弩矢（`Bolt`），其余都是箭（`Arrow`）——映射在 game/ 侧，
// 依据 `tools/sprite_gen/README.md` §8.2 的归属表。
inline constexpr std::uint8_t kProjFromUnit = 0xFFu;

constexpr std::string_view ident_of(TgtKind k) noexcept {
    switch (k) {
        case TgtKind::None:     return "None";
        case TgtKind::Unit:     return "Unit";
        case TgtKind::Bld:      return "Bld";
        case TgtKind::Obstacle: return "Obstacle";
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
// 不带任何性质标签：固定边缘候选点（2026-08-31 起「走廊」概念已取缔，
// 此前的走廊种类标签随 `spawns[].corridor` 字段一并删除），
// 仿真只消费它的位置。
struct SpawnSite {
    GridPos pos{};
};

// 资源点。采集建筑只能建在它上面（CLAUDE.md：三种资源全部来自地图资源点）。
struct ResourceSite {
    GridPos pos{};
    Resource kind = Resource::Stone;
    // **影响仿真**（CLAUDE.md「资源点随波数解禁」）：`World::wave()` 小于它时，
    // 就算已经盖好采集建筑也不入账，见 `tick_economy`。默认 1——`World::wave_`
    // 从 1 起，等于「从第一波就有」，城内保底资源点该给的正是这个默认值。
    // 地图侧对应字段是 `resources[].unlock_wave`（`地图与场景设计.md` 6.2），
    // **与 `tier` 是两个不相关的字段**：`tier` 是纯描述、不进这里（见
    // `game::make_world_init` 的注释），解禁波数则必须进——这正是两者的分界。
    int unlock_wave = 1;
};

// 一座初始建筑。
//
// **`hp` 与 `max_hp` 都由调用方给**，`World` 不查任何表。初始城圈是**残破**的
// （`地图与场景设计.md` 2.3），地图文件给的是 `hp_frac`，把它乘成绝对值需要
// `max_hp`——那一步在 `game::make_world_init`（查数值表做乘法），
// 这里保持对数值零依赖。
struct BldInit {
    BldType type = BldType::Wall;
    GridPos pos{};
    std::int64_t hp = 1;
    std::int64_t max_hp = 1;
};

// 一个初始的中立可破坏障碍。
//
// 形状与 `BldInit` 一样刻意，因为它们**确实是一类东西**：一段没有归属、
// 不参与战斗的「墙」（`无尽模式与地形分层.md` 6.1）。所以它不该长得不一样。
//
// **障碍只能从这里进世界，不能在跑动中生成。** 这不是懒省事，是 6.6 那条
// 「不可再生」的结构要求在契约层的形式：**放障碍的函数是私有的**，
// 只有构造函数调它，于是「可再生」这件事在 `World` 外面写不出来。
// 同理**建筑摧毁不产生 `Rubble` 实体**：那条封的是
// 「建墙 → 被拆 → 拆废墟得石材 → 打折重建」这条净赚回路。
//
// **这段话原先是错的，值得留个记号。** 它当初就这么写着，而 `place_obstacle()`
// 是公开的——于是「结构上写不出来」实际只是一条约定，**而注释读起来像是保证**。
// 这正是本仓库最防的那种形态（`创新点与答辩讲法.md` 附录记了这次），
// 所以修的是代码不是注释。
struct ObstacleInit {
    ObstacleType type = ObstacleType::Stump;
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
// `World` 建局时不查表（等级怎么缩放血量归调用方——`§1.4` 未定，
// 见 `rts/stats.hpp` 的 `GlobalStats`）。
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
    // 中立可破坏障碍。**顺序即槽位顺序**，同下面的 `units`，所以它进 `state_hash`。
    std::vector<ObstacleInit> obstacles;
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

    // 数值表（`rts/stats.hpp`）。**这是外生数值进入仿真的唯一路径**：
    // 射程、伤害、速度、视野、破坏倍率全从这里读，机制不得在别处藏一个数。
    // 指纹由 `World` 自己从它算（不由调用方给，理由见 stats.hpp 文件头），
    // 喂进 `state_hash` 并进回放头——于是「改了一个数字」报「数值表变了、重录」，
    // 而不是「第 N tick 状态不一致」。
    StatsTable stats{};
};

// ——`Command::slot` 对建造 / 维修这一族的编码（登墙意愿 `u_garrison_target_` 同码）：**格线性下标 `y*w + x`**——
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
// **它已经用过一次了，而且用法正是上面预演的那种。** `World/1` → `World/2`：
// §6 表决通过后新增 `CommandKind::Clear`，而 `deferred_` 是
// `std::array<..., kCommandKindCount>` 且整块进哈希，于是喂入长度从 11 变 12。
// 这不是「跑歪了」，是口径变了——不改这个串，`World/1` 时期的回放会报成
// 「第 0 tick 状态不一致」，而看到那句话的人会去查一个不存在的确定性缺陷。
//
// **教训值得留着：改哈希口径的不一定是「加了一组实体」这种显眼的改动。**
// 这次只是往一个枚举末尾加了一个成员，而那个枚举恰好是一个进哈希的数组的长度。
//
// `World/2` → `World/3`：数值表指纹进了喂入清单（`rts_core 接口契约.md` §1.1.2，
// 数值表的进入路径与指纹）。这次是**主动挑的时机**——当时已录的回放只有 `tests/`
// 里现生成的那几份，而往后每实现一条机制都会多一批夹具回放，越晚改越贵。
//
// `World/3` → `World/4`：机制第一批给单位加了四个字段（冷却 / 目标种类 /
// 目标句柄 / 锁定落点）、给建筑加了三个（冷却 / 前摇 / 目标），全部进哈希。
//
// `World/4` → `World/5`：机制第二批给建筑加了四个字段（完工位 / 在训兵种 /
// 在训剩余 / 在训编队）、给障碍加了清野指令位、另加守方的编队去处表，全部进哈希。
//
// `World/5` → `World/6`：机制第三批加了单位的上墙延迟字段与按格的驻守指令表
// （进哈希），并**删掉**了未解算命令计数器（12 种命令全部解算，它的喂入项
// 随之消失——删字段与加字段一样是改口径）。
//
// `World/6` → `World/7`：机制第四批给单位加了冲锋动量（`u_charge_`）、
// 给建筑加了锁定落点（`b_aim_`，齐射与 `Ram` 同一条承诺规则），都进哈希。
//
// `World/7` → `World/8`：机制第五批加了**第四组实体**（在途弹丸，全部字段
// 进哈希）——正是上面「这一条正是在途弹丸那个悬而未决的问题的答案」预演的
// 那次改动，按预演的方式兑现：不预留空位，改布局时进格。
//
// `World/8` → `World/9`：机制第六批把完工城门对守方地面单位改为可通行。
// **这是第一次为「行为变更」而非「布局变更」进格**：喂入清单一个字没动，
// 但相同输入的推演结果变了——不进格的话，旧录像会报 `Diverged`（读作
// 「确定性坏了」，一次注定失败的排查），进格则报 `HashTagMismatch`
// （读作「口径变了，重录」）。两种失效的诊断成本差一个下午。
// `World/9` → `World/10`：建筑等级上限（守方升级轴第一个输出）。新增
// `b_level_` / `b_upgrade_left_` 两组建筑状态，喂入清单跟着长——布局变更，
// 不是行为变更，与 `World/8→9` 那次不同类。
// `World/10` → `World/11`：兵种等级上限（守方升级轴第三个输出）。新增
// `u_upgrade_left_`（单位升级倒计时）与 `b_train_level_`（在训单位的目标
// 等级，`Train` 命令选级之后要记住选了哪个，出兵那一刻才用得上），都进哈希
// ——布局变更，不是行为变更。
// `World/11` → `World/12`：**编队系统整体移除**（2026-09，守方单兵改全自主）。
// 删掉 `u_force_` / `selected_force_` / `force_target_` / `garrison_order_` /
// `b_train_force_` 与 SelectForce / MoveForce / Garrison / UpgradeForce 四种
// 命令；驻守改为逐单位的登墙意愿 `u_garrison_target_`（进哈希）；兵种就地升级
// 随之移除（`u_upgrade_left_` 删，升级只经 `Train` 选级）。布局与行为双重变更。
// `World/12` → `World/13`：新增 `CommandKind::Demolish`，`deferred_` 的长度随
// `kCommandKindCount` 增加一格并进入哈希；与上面 `World/1 → World/2` 同类。
inline constexpr std::string_view kWorldHashTag = "World/13";

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

    // 数值表与它的指纹。表在建局时定死、之后不变——「波次进行中改数值」不是
    // 设计里的东西，而且它若可变，指纹就得变成逐 tick 状态而不是头部字段。
    const StatsTable& stats() const noexcept { return stats_; }
    std::uint64_t stats_fingerprint() const noexcept { return stats_fp_; }

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

    // 提交每个守方单位的「登墙意愿」：与 `submit_actions` 同序、同长度，
    // 值是墙段格线性下标，`kNoSlot` = 不想登。
    //
    // 守方单兵逻辑（DefenderScript）每决策拍决定谁该上哪段墙；登墙动作本身
    // 不在 `UnitAction` 里（避免给攻方动作空间开按侧分叉的口子），所以用这个
    // 并行通道表达。World 的 `tick_garrison` 读到非空目标且单位相邻墙段时
    // 自动开始登墙。
    void submit_garrison_wishes(Side side, const std::uint16_t* targets,
                                std::size_t count);

    // 推进 `ticks` 个 tick。`ticks == 0` 是合法的空操作。
    //
    // 每个 tick 内的阶段顺序**固定**（确定性的一部分，改它等于让所有已录回放
    // 失效——要动就把 `kWorldHashTag` 一起进格）：
    //
    //   1. 排空命令队列（守方先、攻方后，各按提交顺序）
    //   2. 单位战斗：冷却递减；已承诺的前摇递减、到 0 落地结算；
    //      空闲且动作为 `Atk*` 的按目标选择承诺出手（打不到就空转，乙；
    //      **在爬墙的不出手**——上墙延迟就是这段不设防）
    //   3. 单位移动：动作为 `Move*` 且不在前摇中的连续推进；
    //      撞上可破坏阻挡物自动承诺破坏；**驻守与在爬的被钉住**；
    //      冲锋动量在此累计（站停 / 撞停而未承诺出手 ⇒ 归零）；
    //      守方地面单位可穿行完工的 `Gate`（第六批，实体拦路的唯一例外）
    //   4. 驻守：在爬的推进上墙延迟、到 0 落位墙心；按槽位序受理每个单位的
    //      登墙意愿 `u_garrison_target_`——意愿改指别段墙或已清空就下墙；
    //      在地面、相邻目标墙格（完工且无人）就开始登墙
    //   5. 建筑战斗（`Tower` 对地 / `Flak` 对空；**未完工不开火**；
    //      齐射的落点在承诺那一刻锁定）
    //   6. 弹丸飞行与命中：追踪的刷新目的地（目标死了箭落空）、齐射的飞向
    //      锁定落点；到达即结算。**本 tick 刚放出的弹丸也走这一步**
    //      （阶段 2 / 5 在它之前），近距离的箭可以当 tick 命中
    //   7. 经济：施工 / 维修推进（守方 `Mason` 在半径内才走工时）、
    //      征兵倒计时与出兵、按结算周期入账（采集建筑 + `Keep` 的金币地板）
    //   8. 视野重算，两侧迷雾更新 + 记忆图写入（**未完工无视野**）
    //   9. tick + 1
    //
    // 各阶段内一律按**槽位下标升序**遍历——与 `enumerate_units` 同一个规范顺序。
    void advance(int ticks);

    // ——实体——
    //
    // `hp` / `max_hp` / `level` 全部由调用方给（见文件头「数值一个都不在这里」）。
    UnitId spawn_unit(UnitType type, Vec2 pos, std::int32_t level,
                      std::int64_t hp, std::int64_t max_hp);
    BldId place_bld(BldType type, GridPos pos, std::int64_t hp, std::int64_t max_hp,
                    std::int32_t work_left = 0);

    // **障碍没有对应的 `place_*`，这是刻意的**，见 `ObstacleInit` 那段：
    // 它们只能经 `WorldInit::obstacles` 在建局时进世界。放障碍的函数是**私有**的，
    // 于是「障碍可再生」与「建筑摧毁产生 `Rubble` 实体」这两条被禁的写法
    // 在这个类外面**没有语法形式**——而不是靠一条注释拦着。
    //
    // 反过来，**销毁是公开的**：1c 要用它（打穿一个树桩就是销毁一个障碍）。
    // 不对称是有意的，`spawn_unit` / `place_bld` 与这里的区别正是那条规则本身。

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
    // 在飞的弹丸数。弹丸数组**不含空槽**（每 tick 稳定压实），所以是 size。
    int live_proj_count() const noexcept { return static_cast<int>(p_pos_.size()); }

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
    std::int32_t unit_cooldown(UnitId id) const;
    TgtKind unit_target_kind(UnitId id) const;
    // 已承诺攻击的锁定落点。**在前摇开始那一刻定死**，此后目标走了它也不追
    // ——「散开克溅射」的全部实现就在这一条（CLAUDE.md「结构破坏规则」）。
    Vec2 unit_aim(UnitId id) const;
    // 驻守状态（第三批）。`garrison == kNoSlot` = 在地面；否则该值是墙格的
    // 线性下标，`mount > 0` = 还在爬（既不能打也不能走），`mount == 0` = 已登顶。
    std::uint16_t unit_garrison(UnitId id) const;
    std::int32_t unit_mount(UnitId id) const;
    // 冲锋动量（第四批）：已连续助跑的格数。只有 `Charge` 兵种会非零；
    // 站停 / 撞停归零、前摇冻结、落地耗尽（伤害倍率见 rts/combat_math.hpp）。
    float unit_charge(UnitId id) const;

    BldType bld_type(BldId id) const;
    GridPos bld_pos(BldId id) const;
    std::int64_t bld_hp(BldId id) const;
    // 是否**完过工**。第二批起它读 `b_built_` 而不是 `b_work_ == 0`：
    // 维修复用 `b_work_`，一座在修的塔若因此被判「未完工」，就会停火、失明
    // ——抢修残血结构本该是波次中的正当操作，不是自废武功。
    bool bld_complete(BldId id) const;
    std::int32_t bld_level(BldId id) const;
    std::int32_t bld_upgrade_left(BldId id) const;

    // ——资源——
    std::int64_t stock(Resource r) const noexcept {
        return stock_[static_cast<std::size_t>(r)];
    }
    // 直写存量。机制内的增减不走它（收入 / 扣款 / 清野产出都在解算里直接记）；
    // 留着它是给建局方设置初始资金（`game::make_world_init` 之后）与测试用。
    void set_stock(Resource r, std::int64_t v) noexcept;

    // ——攻方宏观状态——
    //
    // 两者都由命令写入，本版就能完整应用（都不需要任何数值）。
    std::uint16_t composition(UnitType t) const noexcept {
        return composition_[static_cast<std::size_t>(t)];
    }
    bool spawn_chosen(std::size_t spawn_index) const;

    // ——守方指令状态（第二批）——
    //
    // `Clear` 解算成**世界状态**而不是「World 代替单位砸树」：守方单兵由
    // 参数化脚本驱动（CLAUDE.md「守方 RL 只在决策层」），脚本每个决策步从这里
    // 读指令、翻译成逐单位的 `UnitAction`。存在 `World` 里而不是让脚本自己
    // 截听命令流，是因为命令可能由另一个模块提交（人类 UI、决策层 RL 经
    // bindings），脚本唯一稳定可读的汇合点就是世界状态。
    bool obstacle_clear_ordered(ObstacleId id) const;

    // `deferred_command_count()` 曾在这里：未解算的命令按种类记账，
    // 好让「本版没做这一类」是可断言的事实。第三批解算掉最后一种（驻守），
    // 计数器按当初的约定一并删除——全部命令自此都有解算。

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
    // 移动 8 位按地形与边界判（斜向额外吃 `kDiagonalNeedsBothOrthogonal`），
    // **攻击 4 位随目标选择落地已升级为精确**：射程内有没有一个合法目标
    // （§1.1.1 乙的另一半）。这不是推翻「掩码里未定的位默认允许」——
    // 那条管的是还判不出来的位，这四位现在判得出来了。`Stop` 恒为 1。
    std::uint16_t action_mask(UnitId id) const;

    // `kCommandKindCount` 位，第 k 位 = `command_kind_at(k)` 是否合法。
    // 按 `is_legal_for` 与波次阶段判。**刻意不判「买得起买不起」**：命令掩码是
    // 每侧一个、不带槽位维度，而「买不起」是逐建筑种类的事——那属决策层的
    // 因子化掩码（`守方AI与协同演化.md` 第 3 节），不是这 `kCommandKindCount`
    // 位能表达的。「掩码里未定的位默认允许」在这里继续成立：错误地允许只浪费样本。
    std::uint16_t command_mask(Side side) const noexcept;

    // 当前允许升到的建筑等级上限（不含 `Keep` 自己——它不受这条约束）。
    // `ceil(Keep 等级 / building_level_cap_divisor)`（`波次预算曲线与
    // 堡垒等级曲线.md` §2）。**唯一算这个公式的地方**——`apply_one` 校验
    // `Upgrade` 与 `WorldView` 给 UI 的提示都调用它，不各自重推一遍
    // （同 `repair_wood_cost` 那条「两处算法分叉是绿框骗人的来源」的纪律）。
    std::int32_t building_level_cap() const noexcept;

    // 当前允许的兵种等级上限——**直接等于堡垒等级，没有除数**（`波次预算曲线与
    // 堡垒等级曲线.md` §2：这条与 `building_level_cap()` 的公式来源本来就
    // 不同，不是漏抄）。`apply_one` 校验 `Train`、`WorldView` 给 UI 的提示都调它。
    std::int32_t unit_level_cap() const noexcept;

    // 造价/耗时曲线：`Train` 选级征兵的唯一计算公式（同 `building_level_cap()`
    // 那条「两处算法分叉是绿框骗人的来源」的纪律）。`cost_gold(L) = base × L`
    // 是纯线性；`train_ticks(L)` 走千分比系数，语义与实现见
    // `stats.hpp` 的 `train_ticks_permille_per_level`。
    std::int64_t train_cost_gold(UnitType ut, std::int32_t level) const noexcept;
    std::int32_t train_ticks_at(UnitType ut, std::int32_t level) const noexcept;

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

    // 只有构造函数调它（`WorldInit::obstacles` 那一遍）。**公开它就等于把
    // 「不可再生」从结构降级为约定**，理由见上面那段与 `ObstacleInit`。
    ObstacleId place_obstacle(ObstacleType type, GridPos pos, std::int64_t hp,
                              std::int64_t max_hp);

    void apply_one(const Command& c);
    void validate(Side side, const Command& c) const;

    // ——机制第一批的内部阶段（实现在 src/mechanics.cpp）——

    // 一次目标选择的结果。`found == false` 时其余字段无意义。
    struct TargetPick {
        bool found = false;
        TgtKind kind = TgtKind::None;
        std::uint32_t raw = 0;
        Vec2 pos{};        // 目标当前位置（承诺时会被锁定成 aim）
        float dist2 = 0;   // 与攻击者的平方距离
    };

    // 为槽位 `k` 上的单位按动作选目标。**只选，不检查射程**——
    // 射程判定分属两处：掩码问「射程内有没有」（加 dist2 条件），
    // 战斗阶段问「选中的在不在射程内」（不在就空转，乙）。
    TargetPick pick_target(std::size_t k, UnitAction a) const;

    void tick_unit_combat();
    void tick_movement();
    void tick_bld_combat();
    void tick_vision();
    // 移动被实体拦住时的自动破坏（「墙 = 高代价可通行」的机制那一半）。
    // 返回是否真的承诺了出手——冲锋动量只在**没承诺成**时归零
    // （骑士撞门是真撞，那一击把动量带进去）。
    bool try_bump_attack(std::size_t k, GridPos to);
    // 建筑出手落地：齐射（AOE 砸锁定落点，仅对地建筑）或单体。
    void land_bld_attack(std::size_t k);

    // ——机制第二批的内部阶段与助手（同在 src/mechanics.cpp）——

    // 施工 / 维修推进、征兵倒计时与出兵、按周期入账。顺序在 advance() 注释里。
    void tick_economy();
    // 守方 `Mason` 在 `pos` 施工半径内的人数（半径查表 `mason_work_radius`）。
    // 施工/维修/升级按人数线性加速（2026-09-01 起，此前是「在场与否」二值
    // 占位——试玩拍板：任务不够分时多人同任务必须真的更快）。
    std::int32_t mason_count(GridPos pos) const;
    // 守方 `Mason` 是否在 `pos` 的施工半径内。`mason_count(pos) > 0` 的
    // 便捷写法。
    bool mason_near(GridPos pos) const;
    // 给槽位 `k` 上的建筑找一个出兵格。找不到返回 false（下 tick 再试）。
    bool try_train_spawn(std::size_t k);
    // 八邻按行主序（dj 外层、di 内层）扫，取第一个地形可通行、无建筑无障碍、
    // 无地面单位站着的格。**顺序即规范**——征兵出兵与驻守下墙共用这一个，
    // 两处各扫一套就是两个会分叉的规范。
    bool find_free_ground_cell(GridPos at, GridPos& out) const;
    // 建筑升级到账：等级 +1、按新等级重算 `max_hp`、满血（完工即「ding」，
    // 不留半血尾巴）。`apply_one` 的 `Upgrade`（工期 <= 0，当场完工）与
    // `tick_economy`（倒计时归零）共用这一个实现，避免两处各推一遍公式。
    void finish_upgrade(std::size_t k);

    // ——机制第三批的内部阶段与助手（同在 src/mechanics.cpp）——

    // 驻守解算：在爬的推进延迟、到 0 落位墙心；按逐单位的登墙意愿
    // （`u_garrison_target_`）受理登墙与下墙。
    void tick_garrison();
    // 槽位 `k` 上的单位是否**站在有高度的墙上**：已登顶，且脚下的墙活着、
    // 血量 >= 一半。三条高度优势（miss / 减伤 / 远程射程加成）共用这一个判据
    // ——「局部破损：墙段掉血不影响墙上单位，直到高度降至原高度一半」的
    // 二值实现（CLAUDE.md 抄的 Stronghold 规则，不引入新数值）。
    bool on_high_wall(std::size_t k) const;
    // 槽位 `k` 上单位的有效射程：基础值 + 远程驻守的高度加成。
    // 掩码与战斗阶段都走它，两处不会各判一套（同 pick_target 那条纪律）。
    float effective_range(std::size_t k) const;
    // 通则仍由 UnitBehavior 承担；唯一需要世界上下文的例外是墙上 Archer 对空。
    bool unit_can_engage(std::size_t k, UnitType target) const;
    // 让槽位 `k` 上的单位离开墙（含还在爬的）。已登顶的落到第一个空邻格；
    // 邻格全被占则**保持驻守**（确定性无操作，登墙意愿再发一次可再试）——
    // 墙格几乎总有空邻格（校验器要求墙有内外两侧），这条边界不值得一个重试态。
    void dismount_unit(std::size_t k);

    // ——机制第五批的内部阶段与助手（同在 src/mechanics.cpp）——

    // 一枚待发 / 在飞的弹丸。发射方的状态在放箭那一刻定格进这里
    // （放箭之后发射方可以死、可以走，箭已与它无关）；逐目标的倍率
    // （高度 miss / 减伤）留到命中那一刻按目标**当时**的状态结算。
    struct ProjSpec {
        Vec2 pos{};                    // 当前位置（发射时 = 发射方位置）
        Vec2 aim{};                    // 目的地。追踪弹逐 tick 刷新成目标位置
        float speed = 0.0f;            // 格 / tick；<= 0 = 瞬时命中
        TgtKind kind = TgtKind::None;  // None = 齐射（AOE 砸 aim，只打地面单位）
        std::uint32_t raw = 0;         // 目标句柄的 raw()（kind != None 时）
        std::int64_t dmg = 0;          // Unit/None 目标 = 基础伤害（命中时再乘）；
                                       // Bld/Obstacle = 最终伤害（倍率与目标无关，
                                       // 放箭时一次乘完，只截断一次——决定 ⑫）
        std::int64_t lvl_pm = kPermilleOne;   // 发射方的等级倍率（放箭时定格）
        std::uint8_t from_high = 0;    // 发射方居高（空中 / 墙上 / 建筑）——
                                       // 命中时免掉目标的高度 miss 与减伤
        float aoe = 0.0f;              // 齐射半径（kind == None 时）
        Side side = Side::Defender;    // 伤害归属（障碍产出归最后一击方）
        std::uint8_t src_bld = kProjFromUnit;   // 渲染挑精灵用（Bolt vs Arrow）
    };

    // 登记一枚弹丸；`speed <= 0` 时当场命中（不进数组）——旧近似的退化形态。
    void launch_projectile(const ProjSpec& p);
    // 命中结算：掷高度 miss、乘逐目标倍率、deal_damage。齐射在这里展开圆圈。
    void impact_projectile(const ProjSpec& p);
    // 飞行阶段：刷新追踪目的地（目标死了箭落空）、推进、到达即结算，
    // 然后稳定压实（保序删除，顺序 = 放箭顺序 = 规范序）。
    void tick_projectiles();
    // 把第 i 枚弹丸的字段装回 ProjSpec（tick_projectiles 与哈希探针共用视角）。
    ProjSpec proj_at(std::size_t i) const;

    // 承诺一次攻击：锁定落点、起前摇、进冷却。`windup_ticks == 0` 时当场落地。
    void commit_attack(std::size_t k, const TargetPick& t);
    // 前摇走完，按锁定的落点 / 目标结算伤害（含 AOE 与死亡处理）。
    void land_attack(std::size_t k);
    // 对一个实体结算 `amount` 点伤害，归零即销毁；
    // 障碍被守方打掉时产出资源（`dealer_side` 用于产出归属）。
    void deal_damage(TgtKind kind, std::uint32_t raw, std::int64_t amount,
                     Side dealer_side);

    // ——静态（建局时定，之后不变）——
    TerrainMasks terrain_;
    GridPos keep_{};
    std::vector<SpawnSite> spawns_;
    std::vector<ResourceSite> resources_;
    std::string map_id_;
    std::array<unsigned char, 32> map_content_hash_{};
    std::uint64_t seed_ = 0;
    StatsTable stats_{};
    std::uint64_t stats_fp_ = 0;   // 建局时从 stats_ 算一次，之后只读

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
    // 登墙意愿：想登哪段墙（`submit_garrison_wishes` 逐拍提交），kNoSlot = 不想登。
    // 与 `u_garrison_`（已在哪段墙上）是两份状态：意愿是输入，驻守是结果——
    // 脚本每个决策拍重发一次，「补位」因此是脚本的事，世界不持常设指令。
    std::vector<std::uint16_t> u_garrison_target_;
    std::vector<std::int32_t> u_mount_;       // 上墙延迟剩余；>0 = 在爬（人还在地面）
    std::vector<float> u_charge_;             // 冲锋动量（已助跑格数）；非 Charge 恒 0
    std::vector<std::int32_t> u_cd_;          // 出手冷却剩余 tick，0 = 可承诺
    std::vector<TgtKind> u_tgt_kind_;         // 已承诺攻击的目标种类，None = 空闲
    std::vector<std::uint32_t> u_tgt_raw_;    // 目标句柄的 raw()，种类由上一条判别
    std::vector<Vec2> u_aim_;                 // 锁定落点（承诺那一刻定死）

    // ——建筑（全部属守方，见 `rts/roster.hpp`：不存 side）——
    SlotPool<BldTag> bld_pool_;
    std::vector<BldType> b_type_;
    std::vector<GridPos> b_pos_;
    std::vector<std::int64_t> b_hp_;
    std::vector<std::int64_t> b_max_hp_;
    std::vector<std::int32_t> b_work_;       // 施工 / 维修剩余工时，0 = 没有在干
    std::vector<std::int32_t> b_cd_;         // 出手冷却（只有 Tower / Flak 会非零）
    std::vector<std::int32_t> b_windup_;     // 已承诺攻击的前摇剩余
    std::vector<std::uint32_t> b_tgt_raw_;   // 目标单位句柄的 raw()（建筑只打单位）
    std::vector<Vec2> b_aim_;                // 锁定落点（齐射用，承诺那一刻定死）
    // 完过工没有。**不能用 `b_work_ == 0` 代替**：维修复用 `b_work_`，
    // 那个等式会把「在修的塔」判成「没盖完的塔」（停火 + 失明）。
    std::vector<std::uint8_t> b_built_;
    // 征兵状态（`Barrack` / `Keep`，一次一名）。kNoTrain = 没在练。
    std::vector<std::uint8_t> b_train_type_;
    std::vector<std::int32_t> b_train_left_;
    std::vector<std::int32_t> b_train_level_;   // 出兵等级（`Train` 命令的 c.level）
    // ——建筑等级上限（守方升级轴第一个输出）——
    // 1 起。`Keep` 不受 `building_level_cap()` 约束，其余建筑受它约束。
    std::vector<std::int32_t> b_level_;
    // 升级倒计时，0 = 没在升。与 `b_work_`（在建/在修）互斥——
    // 同一时刻只能有一件工程在推进，见 `apply_one` 的 `Upgrade`/`Repair` 分支。
    std::vector<std::int32_t> b_upgrade_left_;

    // ——中立可破坏障碍（第三组，决定 ⑤）——
    SlotPool<ObstacleTag> obstacle_pool_;
    std::vector<ObstacleType> o_type_;
    std::vector<GridPos> o_pos_;
    std::vector<std::int64_t> o_hp_;
    std::vector<std::int64_t> o_max_hp_;
    std::vector<std::uint8_t> o_clear_ordered_;   // 玩家下过 `Clear` 没有

    // ——在途弹丸（第四组，机制第五批）——
    //
    // **没有 SlotPool、没有句柄**，与前三组刻意不同：句柄与代数是给「别人会
    // 引用它、引用可能悬空」的实体准备的，而没有任何东西引用一枚弹丸——
    // 它自己引用别人。所以这里是纯 SoA + 每 tick 稳定压实（保序删除），
    // 顺序 = 放箭顺序，确定。**全部字段进哈希**（含数组长度——变长数组
    // 不喂长度会让不同的 (数量, 内容) 组合拼出相同的字节流）。
    // 字段含义见私有 `ProjSpec`（两边由 proj_at() / launch_projectile() 互换，
    // 不会各自漂移）。
    std::vector<Vec2> p_pos_;
    std::vector<Vec2> p_aim_;
    std::vector<float> p_speed_;
    std::vector<TgtKind> p_kind_;
    std::vector<std::uint32_t> p_raw_;
    std::vector<std::int64_t> p_dmg_;
    std::vector<std::int64_t> p_lvl_pm_;
    std::vector<std::uint8_t> p_from_high_;
    std::vector<float> p_aoe_;
    std::vector<Side> p_side_;
    std::vector<std::uint8_t> p_src_bld_;

    // ——资源与宏观——
    std::array<std::int64_t, kResourceCount> stock_{};
    std::array<std::uint16_t, kUnitTypeCount> composition_{};
    std::vector<std::uint8_t> spawn_chosen_;

    // ——输入队列——
    std::array<std::vector<Command>, kSideCount> cmd_queue_;

    // ——迷雾，每侧一份——
    std::array<FogLayer, kSideCount> fog_;

    // ——派生缓存：格 → 占位实体。**不进 `state_hash`**——
    //
    // 它们随 `place_* / destroy_*` 同步维护、可完全由实体数组重建，
    // 喂进哈希就是把同一份事实喂两遍（不一致时反而把分叉的报告点搞乱）。
    // 编码：0 = 空，否则 = 槽位 + 1。移动的拦路判定与记忆图写入都查它，
    // 免得每个移动单位对全部建筑做一次线性扫描。
    //
    // **往这两张表以外的地方给实体记「在哪一格」的第二份账之前，先读这段**：
    // 派生缓存的代价是每个写点都要记得维护，第三张表就是第三个写点清单。
    std::vector<std::uint16_t> bld_at_;
    std::vector<std::uint16_t> obstacle_at_;
};

}  // namespace rts

#endif  // RTS_WORLD_HPP
