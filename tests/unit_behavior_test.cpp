#include <catch2/catch_test_macros.hpp>

#include <set>

#include "rts/roster.hpp"
#include "rts/unit_behavior.hpp"

// 这份测试的首要职责是**补回一道编译器不再提供的保证**。
//
// `roster.hpp` 的属性函数是无 `default` 的穷举 switch，漏一个枚举值会被
// `/w14062`（MSVC）与 `-Wswitch`（GCC，含在 `-Wall` 里）逮住。
// 换成虚函数之后这道保证消失了：**少写一个派生类、或注册表里排错一个下标，
// 编译器一句话都不会说**，而症状是某个兵种静默拿到隔壁兵种的属性。
//
// 所以下面第一组用例做的事是：拿 11 个派生类与 `roster.hpp` 的 constexpr 函数
// **互相印证**，两者都对才叫对。这是 `roster.hpp` 自己对 `side_of()` 用过的手法
// （分块边界 + 逐成员 switch 互证）。
//
// 第二组查的是结构约束的**总数**——那些 CLAUDE.md 逐条写成
// 「结构性决定，不要改」的东西，最容易被后人当成数值调掉。

// ——第一组：注册表与花名册必须逐项对上——

TEST_CASE("behavior_of 取到的就是那个兵种，没有串位", "[behavior]") {
    // 注册表是一个手写下标的数组（`unit_behavior.cpp` 的 `Registry::by_type`）。
    // 排错一个下标不会有任何编译期反应，只会让属性静默串到隔壁兵种上。
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const rts::UnitType t = rts::unit_at(i);
        REQUIRE(rts::behavior_of(t).type() == t);
    }
}

TEST_CASE("每个兵种都有自己的行为实例，没有共用", "[behavior]") {
    // 若某个派生类被漏掉、而注册表里用同一个指针填了两格，上一条用例仍会过
    // （两格的 `type()` 只有一个对得上……不，两格返回同一个 type，
    // 于是另一格必然对不上）——但如果漏的是**两个相邻兵种同时填成同一个**，
    // 就要靠这条。地址在这里只用于「互不相同」，不作 key、不排序，
    // 因此不违反确定性约束。
    std::set<const rts::UnitBehavior*> seen;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        seen.insert(&rts::behavior_of(rts::unit_at(i)));
    }
    REQUIRE(seen.size() == static_cast<std::size_t>(rts::kUnitTypeCount));
}

TEST_CASE("委托方法与 roster.hpp 的 constexpr 函数一致", "[behavior]") {
    // 三个委托方法（`side` / `aerial` / `combat`）刻意不做成虚函数，
    // 就是为了不产生第二份会漂移的真相。这条用例把「不漂移」变成会红的检查。
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const rts::UnitType t = rts::unit_at(i);
        const rts::UnitBehavior& b = rts::behavior_of(t);
        REQUIRE(b.side() == rts::side_of(t));
        REQUIRE(b.aerial() == rts::is_aerial(t));
        REQUIRE(b.combat() == rts::is_combat(t));
    }
}

TEST_CASE("Aerial 机动性与 is_aerial 必须互相蕴含", "[behavior]") {
    // 两处各自表达「会不会飞」，写不一致的后果是分裂的：寻路按 `mobility()` 走地面、
    // 防空按 `is_aerial()` 判目标，于是出现一个「走地面但只能被防空打」的单位。
    //
    // 这条尤其针对 `Wraith`：CLAUDE.md 用一整节论证它**必须是地面单位**
    // （若会飞，防空就多了「否定侦查」这一每波稳定生效的用途，AA 的机会成本
    // 不再是两难）。而「幽影窥使」这个名字和蝙蝠造型都在往「会飞」上暗示。
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const rts::UnitType t = rts::unit_at(i);
        const bool by_mobility =
            rts::behavior_of(t).mobility() == rts::MobilityKind::Aerial;
        REQUIRE(by_mobility == rts::is_aerial(t));
    }
}

// ——第二组：结构约束的总数——

TEST_CASE("只有一个空中单位，且是 Phoenix", "[behavior]") {
    // CLAUDE.md：「`Phoenix` 是唯一的空中单位。`Wraith` 是地面单位——
    // 这是结构性决定，不要改。」
    int n = 0;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        if (rts::behavior_of(rts::unit_at(i)).mobility() == rts::MobilityKind::Aerial) {
            ++n;
        }
    }
    REQUIRE(n == 1);
    REQUIRE(rts::behavior_of(rts::UnitType::Phoenix).mobility() ==
            rts::MobilityKind::Aerial);
}

TEST_CASE("只有一个兵种吃冲锋助跑，且是 Knight", "[behavior]") {
    // 冲锋伤害 ∝ 助跑距离是 CLAUDE.md 列举的「机制性克制」之一
    // （开阔地怕枪阵、巷战反克）。给第二个兵种也加上冲锋，
    // 「开阔地 vs 巷战」这条地形相关的取舍就被摊薄了。
    int n = 0;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        if (rts::behavior_of(rts::unit_at(i)).charges()) ++n;
    }
    REQUIRE(n == 1);
    REQUIRE(rts::behavior_of(rts::UnitType::Knight).charges());
    // 守方骑兵**不**吃冲锋——克制表里「`Ranger` ──► `Knight`（骑兵对冲，遏制出城）」
    // 那条不对称正依赖于此。
    REQUIRE_FALSE(rts::behavior_of(rts::UnitType::Ranger).charges());
}

TEST_CASE("没有任何单位能攻击空中目标", "[behavior]") {
    // behavior 是无世界状态的地面通则；墙上 Archer 的例外由 garrison_test
    // 用真实驻守状态覆盖，这里仍要求所有兵种模板默认不能对空。
    // 这条连着「守方没有空军，空中威胁只能被位置性否定」整段论证——
    // 任何一个能对空的单位都会让 AA 的机会成本失去两难性质。
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const rts::UnitBehavior& b = rts::behavior_of(rts::unit_at(i));
        REQUIRE_FALSE(b.can_engage(rts::UnitType::Phoenix));
    }
}

TEST_CASE("无战力单位既打不了单位也拆不了结构", "[behavior]") {
    // `Scout` / `Mason` / `Wraith`。它们靠基类通则自动满足，
    // 三个派生类里一行 override 都没有——这条用例保证那份「靠通则」是真的成立，
    // 而不是碰巧没人测。
    for (const rts::UnitType t :
         {rts::UnitType::Scout, rts::UnitType::Mason, rts::UnitType::Wraith}) {
        const rts::UnitBehavior& b = rts::behavior_of(t);
        REQUIRE_FALSE(b.combat());
        REQUIRE_FALSE(b.can_break_structure());
        REQUIRE_FALSE(b.can_engage(rts::UnitType::Ghoul));
        REQUIRE_FALSE(b.can_engage(rts::UnitType::Archer));
    }
}

TEST_CASE("Phoenix 拆不了结构，而其余战斗单位都能", "[behavior]") {
    // CLAUDE.md 两条合起来：
    //   「**任何单位都可以破坏城墙**」（通则，刻意不学 Stronghold 2 禁止步兵破墙）
    //   「`Phoenix` 对**城墙与城门破坏速率为 0**」（硬性结构约束，非待平衡数值）
    //
    // 后者是全表唯一的 override。它最容易被当成数值调掉——
    // 「让不死鸟稍微能拆一点墙」听起来无害，实际废掉三阶段攻防的中段。
    REQUIRE_FALSE(rts::behavior_of(rts::UnitType::Phoenix).can_break_structure());

    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const rts::UnitType t = rts::unit_at(i);
        if (t == rts::UnitType::Phoenix) continue;
        const rts::UnitBehavior& b = rts::behavior_of(t);
        // 战斗单位能拆，无战力的不能——两者合起来正好覆盖全表。
        REQUIRE(b.can_break_structure() == rts::is_combat(t));
    }
}

TEST_CASE("攻城形态只有 Ram，且它同时是 Structural", "[behavior]") {
    // 三轴在 `Ram` 上同时用满，而「对建筑特攻 + 对散开单位效率差」这条
    // 复用既有的「溅射克密集、散开克溅射」机制，不需要特例
    // （CLAUDE.md「结构破坏规则」）。
    int siege = 0;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        if (rts::behavior_of(rts::unit_at(i)).engage_range() ==
            rts::EngageRange::Siege) {
            ++siege;
        }
    }
    REQUIRE(siege == 1);

    const rts::UnitBehavior& ram = rts::behavior_of(rts::UnitType::Ram);
    REQUIRE(ram.engage_range() == rts::EngageRange::Siege);
    REQUIRE(ram.strike_form() == rts::StrikeForm::Structural);
    REQUIRE(ram.can_break_structure());
}

TEST_CASE("三轴取值都落在合法范围内", "[behavior]") {
    // 防的是「加了新枚举成员但忘了更新计数常量」，与 roster_test 里
    // 「三个计数常量与枚举末尾一致」同源。
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const rts::UnitBehavior& b = rts::behavior_of(rts::unit_at(i));
        REQUIRE(static_cast<int>(b.engage_range()) < rts::kEngageRangeCount);
        REQUIRE(static_cast<int>(b.strike_form()) < rts::kStrikeFormCount);
        REQUIRE(static_cast<int>(b.mobility()) < rts::kMobilityKindCount);
    }
}

TEST_CASE("二部图对称的一对，结构相同而阵营不同", "[behavior]") {
    // `Archer` 与 `Shade` 都是中程单体轻甲——**这是对的**，
    // 它们是克制二部图里对称的一对，差别在阵营与数值，不在结构。
    // 写成用例是为了让「三轴相同」成为一个有意的结论，而不是看起来像复制粘贴的疏漏。
    const rts::UnitBehavior& archer = rts::behavior_of(rts::UnitType::Archer);
    const rts::UnitBehavior& shade = rts::behavior_of(rts::UnitType::Shade);

    REQUIRE(archer.engage_range() == shade.engage_range());
    REQUIRE(archer.strike_form() == shade.strike_form());
    REQUIRE(archer.mobility() == shade.mobility());
    REQUIRE(archer.side() != shade.side());
}

TEST_CASE("放弹丸的恰好是 Archer 与 Shade，且与冲锋 / 枪阵不共存", "[behavior]") {
    // 第一半锁谓词本身：launches_projectile = Ranged × 非空中，按 CLAUDE.md
    // 「除 Ram 外，Archer / Shade / Tower / Flak 的攻击真有在途弹丸」的单位那半。
    // Phoenix 是 Ranged × Aerial——俯冲直击，不放箭。
    int n = 0;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        if (rts::behavior_of(rts::unit_at(i)).launches_projectile()) ++n;
    }
    REQUIRE(n == 2);
    REQUIRE(rts::behavior_of(rts::UnitType::Archer).launches_projectile());
    REQUIRE(rts::behavior_of(rts::UnitType::Shade).launches_projectile());
    REQUIRE_FALSE(rts::behavior_of(rts::UnitType::Phoenix).launches_projectile());

    // 第二半锁两条组合约束——弹丸的命中路径**没有**冲锋与反冲锋倍率，
    // 其前提是「没有兵种既放箭又冲锋 / 又架枪阵」。新兵种破了任一条，
    // 这里先红：去 mechanics.cpp 的 impact_projectile 补对应倍率，再改这条。
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const rts::UnitBehavior& b = rts::behavior_of(rts::unit_at(i));
        REQUIRE_FALSE((b.launches_projectile() && b.charges()));
        REQUIRE_FALSE((b.launches_projectile() && b.counters_charge()));
    }
}
