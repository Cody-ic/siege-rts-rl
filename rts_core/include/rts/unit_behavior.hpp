// 兵种行为：每个兵种一个类，用虚函数表达它们的结构性差异。
//
// ## 这一层为什么存在
//
// 两个理由，缺一个都不足以支撑它：
//
// **一、CLAUDE.md 要求克制关系由三条正交轴「推导」，而不是平铺成表。** 原文：
//
// > 克制关系由三条正交属性轴推导，**不要退化成平铺的 N×N 伤害倍率表**：
// > 交战距离（近战/中程/攻城）、打击形态（单体/溅射/破坏结构）、
// > 机动性（重甲慢速/轻甲快速/特殊位移）。
//
// 「推导」是**行为**，不是属性。而「通则 + 例外」正是虚函数最合适的用法：
// 基类按三轴给出默认判定，派生类只 override 自己那条例外，
// 并在 override 处写明是 CLAUDE.md 哪一条要求的。
// 写成一张平铺的表，则每一格的**理由**都无处安放——那正是原文要防的。
//
// **二、交付要求「描述软件的类对象设计」需要一张真实的类图。**
// `交付要求的工程影响.md` 曾把「系统层为类、实体为扁平数组是否满足要求」
// 列为唯一需要问老师的事项。本层提供 `UnitBehavior` + 11 个派生类的继承体系，
// 虚函数、override、多态分派都是真的在核心逻辑里跑，
// 而不是为报告造的壳子（见 issue #64 的权衡）。
//
// ## 它**不**替换 `roster.hpp` 的 constexpr 函数
//
// `side_of` / `is_aerial` / `is_combat` 保持原样，本层的同名方法**委托**给它们。
// 这是刻意的，替换掉会丢三样东西：
//
// - **编译期可求值**：`roster.hpp` 里有 8 条 `static_assert` 用着它们
// - **穷举完备性**：那些 switch 没有 `default`，漏一个枚举值会被 `/w14062`
//   与 GCC 的 `-Wswitch` 逮住；而「虚函数漏了一个派生类」编译器不管
// - 热路径的零开销
//
// 第二条的缺口由 `tests/unit_behavior_test.cpp` 补上：它拿 11 个派生类与
// `roster.hpp` 的 constexpr 函数**互相印证**，两者都对才叫对。
// 这是 `roster.hpp` 自己对 `side_of()` 用过的同一手法（分块边界 + 逐成员 switch）。
//
// ## 这里同样**没有数值**
//
// 与 `roster.hpp` 同一条边界（见该文件头注释）：血量、伤害、射程、速度、造价、
// 破坏速率一个都不在这里。CLAUDE.md「关于数值」要求它们以占位常量放在 JSON 里。
//
// 本层只放**结构约束**——那些 CLAUDE.md 逐条写成「结构性决定，不要改」的东西。
// 判据：若某项会随平衡标定而变，它就不属于这里。

#ifndef RTS_UNIT_BEHAVIOR_HPP
#define RTS_UNIT_BEHAVIOR_HPP

#include <cstdint>

#include "rts/roster.hpp"
#include "rts/types.hpp"

namespace rts {

// ——三条正交轴——
//
// 直接来自 CLAUDE.md「游戏设计要点」那一段的括号内分类，不是新发明的。
// 之所以做成三个独立枚举而不是一个「兵种档位」枚举：**正交**是原文的要求，
// 合成一个枚举就等于预先把组合枚举完，克制关系又退化成平铺表。

// 交战距离。决定「谁能贴上谁」，是风筝与冲脸这类机制的前提。
enum class EngageRange : std::uint8_t {
    Melee = 0,    // 近战：必须贴身
    Ranged,       // 中程：有射程，被贴脸即废（CLAUDE.md：弓手有攻击前摇与转身速度）
    Siege,        // 攻城：贴身但打的是建筑，前摇长、一击 AOE
};

// **`Siege` 是贴身撞锤，不是抛射器械**（2026-08-29 定）。这一行原写「弹道有
// 飞行时间」，那是误导性措辞——它与花名册那一列写的「贴身」直接矛盾，
// 而精灵流水线其实一直按撞锤做（撞木滑动，`tools/sprite_gen/README.md` §2）。
//
// **删掉「飞行时间」之后，「对能散开的单位效率很差」需要另一条实现要求撑着**，
// 而它必须写下来：**AOE 落点在前摇开始那一刻锁定成一个坐标**，伤害在前摇
// 结束时按那个坐标结算。若落点跟着目标句柄走（每 tick 重算），散开完全无效，
// 而**这条失效不会让任何测试变红**——它是机制正确性，不是确定性，
// 回放照样逐 tick 一致。归 1c（#28）。

// 打击形态。CLAUDE.md：「溅射克密集、散开克溅射」，且该机制被 `Ram` 复用。
enum class StrikeForm : std::uint8_t {
    Single = 0,   // 单体
    Splash,       // 溅射：对密集目标高效，对散开目标大幅 miss
    Structural,   // 破坏结构：对建筑特攻，对能散开的单位效率很差
};

// 机动性。CLAUDE.md 的第三条轴，「特殊位移」在本作里就是冲锋与飞行。
enum class MobilityKind : std::uint8_t {
    HeavySlow = 0,  // 重甲慢速
    LightFast,      // 轻甲快速
    Charge,         // 冲锋：伤害 ∝ 助跑距离（开阔地强、巷战被反克）
    Aerial,         // 飞行：**只有 Phoenix**，见 CLAUDE.md「空中单位」
};

inline constexpr int kEngageRangeCount = 3;
inline constexpr int kStrikeFormCount = 3;
inline constexpr int kMobilityKindCount = 4;

// ——基类——
//
// 纯虚的只有「三轴 + 自报兵种」四个。其余方法都有基类实现，
// 派生类**只 override 例外**——这样每个 override 都对应 CLAUDE.md 的一条具体要求，
// 而不是把 11 个兵种的所有属性抄 11 遍。
class UnitBehavior {
public:
    virtual ~UnitBehavior() = default;

    UnitBehavior(const UnitBehavior&) = delete;
    UnitBehavior& operator=(const UnitBehavior&) = delete;

    // 自报身份。用它把本层与 `roster.hpp` 接起来，
    // 也让下面几个委托方法不必在每个派生类里重复写一遍。
    virtual UnitType type() const noexcept = 0;

    virtual EngageRange engage_range() const noexcept = 0;
    virtual StrikeForm strike_form() const noexcept = 0;
    virtual MobilityKind mobility() const noexcept = 0;

    // ——委托给 roster.hpp，保持单一来源——
    //
    // 不做成虚函数：它们已经有 constexpr 的权威实现，再写一遍就是两份会漂移的真相。
    Side side() const noexcept { return side_of(type()); }
    bool aerial() const noexcept { return is_aerial(type()); }
    bool combat() const noexcept { return is_combat(type()); }

    // ——结构性判定，基类给通则——

    // 能不能攻击 `target` 这个兵种。
    //
    // 通则有两条，都是 CLAUDE.md 的硬性结构约束：
    //
    // 1. **无战力单位打不了任何东西。** `Scout` / `Mason` / `Wraith` 不进克制二部图
    // 2. **没有任何单位能攻击空中目标。** 克制表写着
    //    「`Phoenix` ──► `Flak`（**且仅此**，纯位置性）」，而 `Flak` 是建筑不是单位。
    //    这条连着「守方没有空军，空中威胁只能被位置性否定」——
    //    若哪个单位能对空，那一整节的机会成本论证就塌了
    //
    // 所以基类实现是 `combat() && !is_aerial(target)`，**11 个派生类没有一个 override 它**。
    // 留成虚函数是因为它是「行为」而非「属性」：将来若有兵种要破这条例外，
    // 该在它自己的类里写明理由，而不是在这里加一个 `if (type == ...)`。
    virtual bool can_engage(UnitType target) const noexcept;

    // 能不能破坏城墙 / 城门这类结构。
    //
    // 通则是**能**——CLAUDE.md：「**任何单位都可以破坏城墙**」，
    // 这条是刻意的（Stronghold 2 禁止步兵破墙被批评过，本作规避的方式是让
    // `Ram` 保有压倒性效率，而不是禁止）。
    //
    // 两处例外：无战力三个，以及 `Phoenix`（对城墙与城门破坏速率**恒为 0**，
    // 属硬性结构约束而非待平衡数值，见 `PhoenixBehavior`）。
    virtual bool can_break_structure() const noexcept;

    // 冲锋是否吃助跑距离。只有 `Charge` 机动性的兵种为真。
    // 做成方法而不是让调用方自己比 `mobility() == Charge`：
    // 那样这条判断会散落到寻路、战斗、观测三处。
    bool charges() const noexcept { return mobility() == MobilityKind::Charge; }

protected:
    UnitBehavior() = default;
};

// ——取某个兵种的行为——
//
// 返回的是**静态存储期的单例引用**，每个兵种一个，共 11 个。
//
// 这一点对确定性至关重要，也是本层与「逐实体多态继承」的根本区别
// （CLAUDE.md「硬性约束」那一节禁的是后者）：
//
// - **没有逐单位堆分配**：11 个对象在程序启动时就位，`spawn()` 不分配任何东西
// - **不参与任何遍历或排序**：仿真遍历的仍是 `World` 里那些扁平数组，
//   下标是 `UnitId`。behavior 只被「按兵种查一次」，
//   所以「拿地址当 key 或排序依据」这条破坏确定性的写法在这里无从发生
//
// 实现用静态数组按枚举值索引，**不用 map**——`unordered_map` 会被确定性守卫拦下，
// 而有序 map 在这里也只是把一次数组索引换成一次树查找。
const UnitBehavior& behavior_of(UnitType t) noexcept;

}  // namespace rts

#endif  // RTS_UNIT_BEHAVIOR_HPP
