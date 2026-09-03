// 11 个兵种的行为类。
//
// **每个类的注释只写它与通则的差别**，共性在基类（`unit_behavior.hpp`）。
// 这样读一遍就知道「这个兵种特殊在哪」，而不是从 11 份雷同的属性表里找不同。
//
// 三轴取值一律引用 CLAUDE.md 花名册的「定位」列与克制二部图，不自己发明。

#include "rts/unit_behavior.hpp"

#include <array>
#include <cstddef>

namespace rts {

// ——基类的通则实现——

bool UnitBehavior::can_engage(UnitType target) const noexcept {
    // 见头文件：无战力打不了任何东西；地面状态的单位不能对空。
    // 墙上 Archer 的有状态例外由 World::unit_can_engage 叠加。
    return combat() && !is_aerial(target);
}

bool UnitBehavior::can_break_structure() const noexcept {
    // 通则是「任何单位都可以破坏城墙」，所以只排除无战力的。
    // `Phoenix` 那条例外在它自己的类里 override。
    return combat();
}

namespace {

// ——守方（人类王国）——

// 戍卫弓手：中程单体，上墙吃高度加成，是防线主力输出。
// 无例外，通则全都适用。
class ArcherBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Archer; }
    EngageRange engage_range() const noexcept override { return EngageRange::Ranged; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::LightFast; }
};

// 铁壁枪卫：近战抗骑，堵缺口。
// 机动性取 `HeavySlow`——克制表里「`Spear` ──► `Shade`（慢速近战被风筝）」
// 那条反克制正是慢速的直接后果，所以这不是数值而是结构。
class SpearBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Spear; }
    EngageRange engage_range() const noexcept override { return EngageRange::Melee; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::HeavySlow; }
};

// 逐风猎骑：快速近战，**出城的执行手段**（摸攻城锤、争夺墙外矿场）。
//
// 机动性取 `LightFast` 而**不是** `Charge`：CLAUDE.md 把冲锋助跑写给攻方的
// `Knight`，而这一侧的定位是「快速切入」。两者的差别在克制表里是对称的——
// 「`Ram` ──► `Ranger` 快速切入」vs「`Ranger` ──► `Knight`（骑兵对冲）」，
// 前者靠速度、后者被冲锋克。若这里也给 `Charge`，那条对冲就没有强弱可分了。
class RangerBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Ranger; }
    EngageRange engage_range() const noexcept override { return EngageRange::Melee; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::LightFast; }
};

// 游猎斥候：极快、大视野、**无战力**。
//
// 三轴仍要给值（它要移动、要被打），但 `combat()` 为假使通则自动让
// `can_engage` 与 `can_break_structure` 都返回 false——**不必在这里 override**。
// 这正是把它们做成「基类通则」而非「每类各写一遍」的收益。
class ScoutBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Scout; }
    EngageRange engage_range() const noexcept override { return EngageRange::Melee; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::LightFast; }
};

// 工匠：无战力，维修。同 `Scout`，通则已覆盖。
class MasonBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Mason; }
    EngageRange engage_range() const noexcept override { return EngageRange::Melee; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::LightFast; }
};

// ——攻方（亡灵 / 魔物大军）——

// 亡灵步兵：近战单体、耐打，主力肉盾。
class GhoulBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Ghoul; }
    EngageRange engage_range() const noexcept override { return EngageRange::Melee; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::HeavySlow; }
};

// 亡灵法师：中程单体，**持续压制墙头守军**。
// 与 `Archer` 三轴完全相同——这是对的：它们是二部图里对称的一对，
// 差别在阵营与数值，不在结构。`side()` 已经由 `roster.hpp` 区分。
class ShadeBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Shade; }
    EngageRange engage_range() const noexcept override { return EngageRange::Ranged; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::LightFast; }
};

// 鬼域骑士团：**冲锋伤害 ∝ 助跑距离**，克远程与攻城锤，遏制守方出城。
// 唯一取 `Charge` 的兵种，`charges()` 因此只对它为真。
class KnightBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Knight; }
    EngageRange engage_range() const noexcept override { return EngageRange::Melee; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::Charge; }
};

// 不死鸟：空中单位，手术刀而非胜利条件。
//
// 这是 11 个类里唯一 override 结构性判定的：
// **对城墙与城门破坏速率为 0**，且无法攻击核心建筑。CLAUDE.md 把它列为
// 「结构性约束（硬性，不是待平衡的数值）」，理由是破墙只能靠地面部队，
// 「压制墙头 → 破墙 → 缺口攻防」三阶段攻防才完整保留。
//
// 写成 override 而不是在基类加 `if (type() == Phoenix)`：
// 那个 if 会让「为什么」离开它所属的兵种，而这条恰恰是最容易被后人当成
// 数值调掉的一条（「让不死鸟稍微能拆一点墙」听起来无害，实际废掉三阶段攻防）。
class PhoenixBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Phoenix; }
    EngageRange engage_range() const noexcept override { return EngageRange::Ranged; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::Aerial; }

    bool can_break_structure() const noexcept override { return false; }
};

// 幽影窥使：**空中**（2026-09-03 起）、极快、大视野、无战力。
//
// 「会飞」曾是反面教材（原论证：防空会因此每波都稳赚，AA 的机会成本不再是
// 两难）；组内最终决定改为空中单位，移动机制与 `Phoenix` 同一套——`mobility()`
// 取 `Aerial`，且 `roster.hpp` 的 `is_aerial()` 对它返回 true——**两处必须一致**，
// 由测试钉住（`tests/unit_behavior_test.cpp`）。
class WraithBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Wraith; }
    EngageRange engage_range() const noexcept override { return EngageRange::Melee; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Single; }
    MobilityKind mobility() const noexcept override { return MobilityKind::Aerial; }
};

// 攻城锤：贴身、AOE、对建筑特攻，是玩家的头号集火目标。
//
// 三轴在这里同时用满：`Siege` + `Structural` + `HeavySlow`。
// CLAUDE.md：「对**静止的建筑** AOE 全额命中且可波及相邻墙段与贴近的塔；
// 对**能散开的单位**效率很差」——后半句正是 `Structural` 这个取值要表达的，
// 它复用既有的「溅射克密集、散开克溅射」机制，不需要特例。
class RamBehavior final : public UnitBehavior {
public:
    UnitType type() const noexcept override { return UnitType::Ram; }
    EngageRange engage_range() const noexcept override { return EngageRange::Siege; }
    StrikeForm strike_form() const noexcept override { return StrikeForm::Structural; }
    MobilityKind mobility() const noexcept override { return MobilityKind::HeavySlow; }
};

// ——注册表——
//
// 静态存储期的 11 个实例，按 `UnitType` 的枚举值索引。
//
// 用聚合的 `struct` 持有全部实例而不是 11 个独立的 static：
// 后者的初始化顺序在跨 TU 时无保证，而这里全在同一个 TU 内、且顺序确定。
// 表本身是 `constinit`-友好的（全部平凡构造、无堆分配），
// 所以没有「静态初始化顺序」问题。
struct Registry {
    ArcherBehavior archer;
    SpearBehavior spear;
    RangerBehavior ranger;
    ScoutBehavior scout;
    MasonBehavior mason;
    GhoulBehavior ghoul;
    ShadeBehavior shade;
    KnightBehavior knight;
    PhoenixBehavior phoenix;
    WraithBehavior wraith;
    RamBehavior ram;

    // 顺序必须与 `UnitType` 逐一对应。**由测试逐个印证**（`behavior_of(t).type() == t`），
    // 而不是靠这里排得对——写错一个下标，属性会静默串到隔壁兵种上。
    std::array<const UnitBehavior*, kUnitTypeCount> by_type{
        &archer, &spear, &ranger, &scout, &mason,
        &ghoul, &shade, &knight, &phoenix, &wraith, &ram,
    };
};

// ——这 11 个派生类必须**无状态**，这条是编译期的——
//
// 它们是被**所有单位共用**的单例（`behavior_of()` 返回引用）。谁往某个派生类里
// 加一个非静态数据成员——比如「这个兵种本波已出手几次」——立刻有两个后果，
// 而两个都不是「稍微不好」：
//
//   1. **跨单位的隐式耦合**：所有该兵种的单位共享那一个字段
//   2. **哈希两条路都错**：不进 `state_hash` 就是一份不被记录的仿真状态；
//      进了就得给一个「按兵种」的东西找一个槽位顺序，而它根本不在任何实体数组里
//
// 判据与 `unit_behavior.hpp` 头注释那条同源（「若某项会随平衡标定而变，
// 它就不属于这里」）——**无状态是它的必要条件**：一个字段一定是状态。
//
// **检查用 `sizeof` 相等而不是 `std::is_empty_v`。** 后者对**任何**多态类都返回
// false（虚函数带来 vptr），所以写成 `is_empty_v` 会得到一条永远失败的断言——
// 即「该红却绿」的镜像，一样没用。而基类正好只有一个 vptr、没有尾部填充，
// 于是「派生类加了字段」必然让 `sizeof` 变大，这条比对是紧的。
#define RTS_ASSERT_STATELESS(T)                                             \
    static_assert(sizeof(T) == sizeof(UnitBehavior),                        \
                  #T " 加了非静态数据成员。兵种 behavior 是被所有单位共用的" \
                     "单例，不得有状态——见此处上方的注释。")

RTS_ASSERT_STATELESS(ArcherBehavior);
RTS_ASSERT_STATELESS(SpearBehavior);
RTS_ASSERT_STATELESS(RangerBehavior);
RTS_ASSERT_STATELESS(ScoutBehavior);
RTS_ASSERT_STATELESS(MasonBehavior);
RTS_ASSERT_STATELESS(GhoulBehavior);
RTS_ASSERT_STATELESS(ShadeBehavior);
RTS_ASSERT_STATELESS(KnightBehavior);
RTS_ASSERT_STATELESS(PhoenixBehavior);
RTS_ASSERT_STATELESS(WraithBehavior);
RTS_ASSERT_STATELESS(RamBehavior);

#undef RTS_ASSERT_STATELESS

const Registry& registry() noexcept {
    static const Registry r;
    return r;
}

}  // namespace

const UnitBehavior& behavior_of(UnitType t) noexcept {
    const auto i = static_cast<std::size_t>(t);
    // 越界只可能来自「把非枚举值强转成 UnitType」，那是调用方的错。
    // 不抛异常：本函数标了 noexcept，且它会被战斗解算每 tick 调用多次。
    // 退化为 `Archer` 而不是未定义行为——但测试会先逮住越界（枚举穷举遍历）。
    if (i >= kUnitTypeCount) {
        return *registry().by_type[0];
    }
    return *registry().by_type[i];
}

}  // namespace rts
