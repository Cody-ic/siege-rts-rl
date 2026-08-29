// 11 个兵种的派生类，以及造它们的工厂。
//
// **派生类刻意全部藏在这个 .cpp 里**：头文件只暴露 `Unit` 与 `make_unit()`，
// 于是「加一个兵种」只改这一处，调用方永远只看见 `UnitType` 与 `Unit&`。
//
// **每个类的注释只写它与通则的差别**，共性在基类（`unit.hpp`）。
// 三轴取值一律引用 CLAUDE.md 花名册的「定位」列与克制二部图，不自己发明。

#include "rts/unit.hpp"

#include <utility>

namespace rts {

// ——基类的通则实现——

bool Unit::can_engage(UnitType target) const noexcept {
    // 见头文件：无战力打不了任何东西；且没有任何单位能对空。
    return combat() && !is_aerial(target);
}

bool Unit::can_break_structure() const noexcept {
    // 通则是「任何单位都可以破坏城墙」，所以只排除无战力的。
    // `Phoenix` 那条例外在它自己的类里 override。
    return combat();
}

namespace {

// 三轴取值相同的兵种很多，用一个中间基类把样板收掉。
//
// **这不是「为了少打字」**：11 个类各写四个 `override` 会让真正的差异
// （谁 override 了 `can_break_structure`）淹没在样板里，而那正是读这份文件的人
// 唯一要找的东西。模板参数是编译期常量，没有运行时代价。
template <UnitType kType, EngageRange kRange, StrikeForm kForm, MobilityKind kMove>
class TypedUnit : public Unit {
public:
    TypedUnit(std::int32_t level, std::int64_t max_hp, Vec2 pos) noexcept
        : Unit(level, max_hp, pos) {}

    UnitType type() const noexcept override { return kType; }
    EngageRange engage_range() const noexcept override { return kRange; }
    StrikeForm strike_form() const noexcept override { return kForm; }
    MobilityKind mobility() const noexcept override { return kMove; }
};

// ——守方（人类王国）——

// 戍卫弓手：中程单体，上墙吃高度加成，防线主力输出。通则全适用。
using ArcherUnit = TypedUnit<UnitType::Archer, EngageRange::Ranged,
                             StrikeForm::Single, MobilityKind::LightFast>;

// 铁壁枪卫：近战抗骑，堵缺口。
// `HeavySlow` 不是数值：克制表「`Spear` ──► `Shade`（慢速近战被风筝）」
// 那条反克制正是慢速的直接后果。
using SpearUnit = TypedUnit<UnitType::Spear, EngageRange::Melee,
                            StrikeForm::Single, MobilityKind::HeavySlow>;

// 逐风猎骑：快速近战，**出城的执行手段**。
//
// 机动性取 `LightFast` 而**不是** `Charge`：CLAUDE.md 把冲锋助跑写给攻方的
// `Knight`，这一侧的定位是「快速切入」。克制表里两者是不对称的一对——
// 「`Ram` ──► `Ranger` 快速切入」vs「`Ranger` ──► `Knight`（骑兵对冲）」，
// 前者靠速度、后者被冲锋克。若这里也给 `Charge`，那条对冲就没有强弱可分了。
using RangerUnit = TypedUnit<UnitType::Ranger, EngageRange::Melee,
                             StrikeForm::Single, MobilityKind::LightFast>;

// 游猎斥候：极快、大视野、**无战力**。
//
// 三轴仍要给值（它要移动、要被打），但 `combat()` 为假使通则自动让
// `can_engage` 与 `can_break_structure` 都返回 false——**这里一行 override 都没有**。
// 这正是「基类给通则」相对「每类各写一遍」的收益。
using ScoutUnit = TypedUnit<UnitType::Scout, EngageRange::Melee,
                            StrikeForm::Single, MobilityKind::LightFast>;

// 工匠：无战力，维修。同 `Scout`，通则已覆盖。
using MasonUnit = TypedUnit<UnitType::Mason, EngageRange::Melee,
                            StrikeForm::Single, MobilityKind::LightFast>;

// ——攻方（亡灵 / 魔物大军）——

// 亡灵步兵：近战单体、耐打，主力肉盾。
using GhoulUnit = TypedUnit<UnitType::Ghoul, EngageRange::Melee,
                            StrikeForm::Single, MobilityKind::HeavySlow>;

// 亡灵弓手：中程单体，持续压制墙头。
// 与 `Archer` 三轴完全相同——**这是对的**：它们是二部图里对称的一对，
// 差别在阵营与数值，不在结构。`side()` 已由 `roster.hpp` 区分。
using ShadeUnit = TypedUnit<UnitType::Shade, EngageRange::Ranged,
                            StrikeForm::Single, MobilityKind::LightFast>;

// 鬼域骑士团：**冲锋伤害 ∝ 助跑距离**。唯一取 `Charge` 的兵种。
using KnightUnit = TypedUnit<UnitType::Knight, EngageRange::Melee,
                             StrikeForm::Single, MobilityKind::Charge>;

// 幽影窥使：**地面**、极快、大视野、无战力。
//
// 「地面」是结构性决定（CLAUDE.md「空中单位」整节在论证）：若它会飞，
// 防空建筑就多了「否定侦查」这一每波稳定生效的用途，玩家无脑造 AA 即可，
// AA 的机会成本不再是两难。所以取 `LightFast` 而非 `Aerial`，
// 且 `roster.hpp` 的 `is_aerial()` 对它返回 false——**两处必须一致**，由测试钉住。
using WraithUnit = TypedUnit<UnitType::Wraith, EngageRange::Melee,
                             StrikeForm::Single, MobilityKind::LightFast>;

// 攻城锤：贴身、AOE、对建筑特攻。三轴在这里同时用满。
// CLAUDE.md：「对静止的建筑 AOE 全额命中……对**能散开的单位**效率很差」——
// 后半句正是 `Structural` 要表达的，它复用既有的「溅射克密集、散开克溅射」机制。
using RamUnit = TypedUnit<UnitType::Ram, EngageRange::Siege,
                          StrikeForm::Structural, MobilityKind::HeavySlow>;

// 不死鸟：**唯一的空中单位**，手术刀而非胜利条件。
//
// **全表唯一 override 结构性判定的类**：对城墙与城门破坏速率恒为 0。
// CLAUDE.md 把它列为「结构性约束（硬性，不是待平衡的数值）」，
// 理由是破墙只能靠地面部队，「压制墙头 → 破墙 → 缺口攻防」三阶段才完整保留。
//
// 写成 override 而不是在基类加 `if (type() == Phoenix)`：那个 if 会让「为什么」
// 离开它所属的兵种，而这条恰恰最容易被后人当成数值调掉——
// 「让不死鸟稍微能拆一点墙」听起来无害，实际废掉三阶段攻防的中段。
class PhoenixUnit final : public TypedUnit<UnitType::Phoenix, EngageRange::Ranged,
                                           StrikeForm::Single, MobilityKind::Aerial> {
public:
    using TypedUnit::TypedUnit;

    bool can_break_structure() const noexcept override { return false; }
};

}  // namespace

std::unique_ptr<Unit> make_unit(UnitType t, std::int32_t level, std::int64_t max_hp,
                                Vec2 pos) {
    // **本仓库唯一一处按兵种分支的地方。** 无 `default`：漏一个枚举值会被
    // `/w14062`（MSVC）与 `-Wswitch`（GCC）逮住，这正是「加了兵种忘了建类」
    // 唯一能在编译期被发现的位置。
    switch (t) {
        case UnitType::Archer:
            return std::make_unique<ArcherUnit>(level, max_hp, pos);
        case UnitType::Spear:
            return std::make_unique<SpearUnit>(level, max_hp, pos);
        case UnitType::Ranger:
            return std::make_unique<RangerUnit>(level, max_hp, pos);
        case UnitType::Scout:
            return std::make_unique<ScoutUnit>(level, max_hp, pos);
        case UnitType::Mason:
            return std::make_unique<MasonUnit>(level, max_hp, pos);
        case UnitType::Ghoul:
            return std::make_unique<GhoulUnit>(level, max_hp, pos);
        case UnitType::Shade:
            return std::make_unique<ShadeUnit>(level, max_hp, pos);
        case UnitType::Knight:
            return std::make_unique<KnightUnit>(level, max_hp, pos);
        case UnitType::Phoenix:
            return std::make_unique<PhoenixUnit>(level, max_hp, pos);
        case UnitType::Wraith:
            return std::make_unique<WraithUnit>(level, max_hp, pos);
        case UnitType::Ram:
            return std::make_unique<RamUnit>(level, max_hp, pos);
    }
    // 走到这里只可能是「把非枚举值强转成 UnitType」。不抛异常会让它静默变成
    // 一个空指针、在很远的地方解引用；这里直接失败，栈还在。
    throw ContractError("make_unit: UnitType 取值不在枚举内");
}

}  // namespace rts
