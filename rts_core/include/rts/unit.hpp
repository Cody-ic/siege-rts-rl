// 单位的继承体系：`Unit` 基类 + 11 个兵种派生类。
//
// ## 为什么是继承体系，而不是扁平数组
//
// 这是一次**有意的方向变更**，推翻了本仓库早先的「扁平数组 + 整型 ID」写法
// （讨论见 issue #64）。驱动它的是交付要求而非工程判断：
//
// > `课程设计最终交付项.md`：「系统设计报告：描述软件的**类对象设计**和界面设计」
//
// 一张 `Unit → Archer / Spear / …` 的继承图是这条要求最直接的答案。
// 而机制性克制本来就是**行为差异**，写成派生类各自 `override` 比写成一个
// 按兵种分支的大 switch 更贴合 CLAUDE.md 那条要求：
//
// > 克制关系由三条正交属性轴推导，**不要退化成平铺的 N×N 伤害倍率表**。
//
// ## 代价，写在明处
//
// 早先那套扁平数组保护的是**确定性回放**，它最常见的破坏源是「拿地址当 key
// 或排序依据」——`unordered_map<Unit*, X>` 的迭代顺序随地址变化。
// 扁平数组从结构上让这类 bug **写不出来**；改用对象之后，这道护栏降级为
// 「写了会红」：`tools/check_determinism_bans.py` 新增了一条禁令扫它。
//
// **这是真实的降级，不是等价替换。** 新增涉及单位集合的代码时请留意。
//
// 另一处代价是观测打包从「按连续数组一次遍历」退化为逐单位 gather，
// 属训练吞吐问题，需在 Release 下实测（见 #64）。
//
// ## 但下面三条把确定性守住了
//
// 1. **`World` 仍按索引遍历**（`std::vector<std::unique_ptr<Unit>>` 的下标 =
//    `SlotPool` 的槽位），遍历顺序等于槽位顺序，**与对象地址无关**
// 2. **`UnitId` 句柄仍是「索引 + 代数」**，不是指针。跨 tick 引用一律用它
// 3. **不提供任何按 `Unit*` 建立的关联容器或排序**，见上面那条守卫
//
// 换言之：**堆分配本身不破坏确定性**（CLAUDE.md 早就澄清过这一条），
// 破坏它的是拿地址做决策。对象化之后前者出现了，后者仍被挡着。

#ifndef RTS_UNIT_HPP
#define RTS_UNIT_HPP

#include <cstdint>
#include <memory>

#include "rts/action.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
#include "rts/types.hpp"

namespace rts {

// ——三条正交轴——
//
// 直接来自 CLAUDE.md「游戏设计要点」括号内的分类，不是新发明的。
// 做成三个独立枚举而非一个「兵种档位」枚举：**正交**是原文的要求，
// 合成一个就等于预先把组合枚举完，克制关系又退化成平铺表。

enum class EngageRange : std::uint8_t {
    Melee = 0,    // 近战：必须贴身
    Ranged,       // 中程：有射程，被贴脸即废（CLAUDE.md：弓手有前摇与转身速度）
    Siege,        // 攻城：贴身但打的是建筑，装填慢、弹道有飞行时间
};

enum class StrikeForm : std::uint8_t {
    Single = 0,   // 单体
    Splash,       // 溅射：克密集，被散开克
    Structural,   // 破坏结构：对建筑特攻，对能散开的单位效率很差
};

enum class MobilityKind : std::uint8_t {
    HeavySlow = 0,  // 重甲慢速
    LightFast,      // 轻甲快速
    Charge,         // 冲锋：伤害 ∝ 助跑距离（开阔地强、巷战被反克）
    Aerial,         // 飞行：**只有 Phoenix**
};

inline constexpr int kEngageRangeCount = 3;
inline constexpr int kStrikeFormCount = 3;
inline constexpr int kMobilityKindCount = 4;

// ——基类——
//
// 状态是**共有的**（每个单位都有血量、位置、等级），所以放在基类的 protected 段；
// 差异是**行为的**（三轴、能不能打某个目标、能不能拆墙），所以是虚函数。
//
// 这条分工是刻意的：把状态也做成虚接口（`virtual int hp() const = 0`）
// 会让 11 个派生类各存一份同样的字段，那是继承被误用的经典形态。
class Unit {
public:
    virtual ~Unit() = default;

    Unit(const Unit&) = delete;
    Unit& operator=(const Unit&) = delete;

    // ——虚接口：兵种之间真正不同的部分——

    virtual UnitType type() const noexcept = 0;
    virtual EngageRange engage_range() const noexcept = 0;
    virtual StrikeForm strike_form() const noexcept = 0;
    virtual MobilityKind mobility() const noexcept = 0;

    // 能不能攻击 `target` 这个兵种。
    //
    // 基类给通则，两条都是 CLAUDE.md 的硬性结构约束：
    //
    // 1. **无战力单位打不了任何东西**（`Scout` / `Mason` / `Wraith` 不进克制二部图）
    // 2. **没有任何单位能攻击空中目标**——克制表写着
    //    「`Phoenix` ──► `Flak`（**且仅此**，纯位置性）」，而 `Flak` 是建筑不是单位。
    //    这条连着「守方没有空军，空中威胁只能被位置性否定」整段论证
    //
    // 11 个派生类目前无一 override 它。留成虚函数是因为它是**行为**：
    // 将来若有兵种要破这条例外，该在它自己的类里写明理由，
    // 而不是在这里加一个 `if (type() == ...)`。
    virtual bool can_engage(UnitType target) const noexcept;

    // 能不能破坏城墙 / 城门这类结构。
    //
    // 通则是**能**——CLAUDE.md：「任何单位都可以破坏城墙」。这条是刻意的：
    // Stronghold 2 禁止步兵破墙被长期批评，本作规避的方式是让 `Ram` 保有
    // 压倒性效率，而不是禁止。
    //
    // 全表唯一的 override 在 `Phoenix`（对城墙与城门破坏速率恒为 0）。
    virtual bool can_break_structure() const noexcept;

    // ——委托给 roster.hpp，保持单一来源——
    //
    // 不做成虚函数：那边已有 constexpr 的权威实现（还带无 `default` 的穷举 switch，
    // 由 `/w14062` 与 `-Wswitch` 保证完备），再写一遍就是两份会漂移的真相。
    Side side() const noexcept { return side_of(type()); }
    bool aerial() const noexcept { return is_aerial(type()); }
    bool combat() const noexcept { return is_combat(type()); }

    bool charges() const noexcept { return mobility() == MobilityKind::Charge; }

    // ——共有状态——
    //
    // 全部非虚。它们每个单位都有，且含义与兵种无关。

    std::int32_t level() const noexcept { return level_; }
    std::int64_t hp() const noexcept { return hp_; }
    std::int64_t max_hp() const noexcept { return max_hp_; }
    Vec2 pos() const noexcept { return pos_; }
    std::int32_t windup() const noexcept { return windup_; }
    UnitAction action() const noexcept { return action_; }
    std::uint16_t garrison() const noexcept { return garrison_; }
    std::uint8_t force() const noexcept { return force_; }

    bool dead() const noexcept { return hp_ <= 0; }

    void set_pos(Vec2 p) noexcept { pos_ = p; }
    void set_hp(std::int64_t v) noexcept { hp_ = v; }
    void set_windup(std::int32_t t) noexcept { windup_ = t; }
    void set_action(UnitAction a) noexcept { action_ = a; }
    void set_garrison(std::uint16_t s) noexcept { garrison_ = s; }
    void set_force(std::uint8_t f) noexcept { force_ = f; }

    // 受伤。夹到 0 而不允许为负：负血量会让「伤害溢出」这类 bug 在状态哈希里
    // 表现为一个看似正常的数，而 0 是可断言的。
    void damage(std::int64_t amount) noexcept {
        hp_ = amount >= hp_ ? 0 : hp_ - amount;
    }

protected:
    // 只有派生类的构造函数能调。`World` 经 `make_unit()` 创建，不直接 new。
    Unit(std::int32_t level, std::int64_t max_hp, Vec2 pos) noexcept
        : level_(level), hp_(max_hp), max_hp_(max_hp), pos_(pos) {}

    std::int32_t level_ = kMinUnitLevel;
    std::int64_t hp_ = 0;
    std::int64_t max_hp_ = 0;
    Vec2 pos_{};
    std::int32_t windup_ = 0;          // 攻击前摇剩余 tick，0 = 可出手
    UnitAction action_ = UnitAction::Stop;  // 上个决策边界选的动作，保持 4–8 tick
    std::uint16_t garrison_ = kNoSlot;      // 驻守的墙段槽位
    std::uint8_t force_ = kNoForce;         // 编队
};

// 按兵种造一个单位。
//
// 工厂而不是让调用方 `new ArcherUnit{...}`：派生类**全部藏在 .cpp 里**，
// 于是「加一个兵种」只改一处，而调用方永远只看见 `UnitType` 与 `Unit&`。
// 这也是把 `switch (type)` 收敛到唯一一处的办法——
// 除了这个工厂，仓库里不该再有第二处按兵种分支的地方。
std::unique_ptr<Unit> make_unit(UnitType t, std::int32_t level, std::int64_t max_hp,
                                Vec2 pos);

}  // namespace rts

#endif  // RTS_UNIT_HPP
