#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>

#include "rts/action.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
// 「可破坏障碍与地形枚举不重名」那条要同时看两张枚举。刻意显式 include 而不是
// 靠 roster.hpp 顺带拉进来 —— 头文件自足性那条守卫防的正是这种隐式依赖。
#include "rts/terrain.hpp"

// 这份测试查的**不是「函数会不会算错」**——那些函数全是穷举 switch，写错一眼能看出来。
// 查的是三类会静默漂移的东西：
//
//   1. 计数常量落后于枚举（加了成员忘了 +1）
//   2. 结构性属性的**总数**变了（多了一个空中单位、少了一个无战力单位），
//      而那些是 CLAUDE.md 逐条写成「结构性决定，不要改」的
//   3. 标识符重复或与花名册不符

TEST_CASE("三个计数常量与枚举末尾一致", "[roster]") {
    // 编译期已经有 static_assert 钉住末尾成员，这里再从**另一个方向**查一次：
    // 遍历到 count 为止，每一个下标都能拿到一个有名字的成员。
    // 两条都对才能排除「末尾对了但中间空了一个洞」——枚举可以显式指定值，
    // 那时 static_assert 仍然过。
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        REQUIRE_FALSE(rts::ident_of(rts::unit_at(i)).empty());
    }
    for (int i = 0; i < rts::kBldTypeCount; ++i) {
        REQUIRE_FALSE(rts::ident_of(rts::bld_at(i)).empty());
    }
    for (int i = 0; i < rts::kObstacleTypeCount; ++i) {
        REQUIRE_FALSE(rts::ident_of(rts::obstacle_at(i)).empty());
    }
}

TEST_CASE("标识符不重复，且一律不超过 7 字符", "[roster]") {
    // ≤7 字符是 CLAUDE.md 命名纪律的硬要求（纯渲染层的素材键才有豁免，
    // 而花名册标识符**被仿真引用**，不在豁免范围内）。
    //
    // 重复会更坏：素材按标识符查图，两个实体撞名会静默共用一张图。
    std::set<std::string_view> seen;
    const auto check = [&seen](std::string_view id) {
        INFO("标识符 " << id);
        REQUIRE(id.size() <= 7);
        REQUIRE(seen.insert(id).second);
    };
    for (int i = 0; i < rts::kUnitTypeCount; ++i) check(rts::ident_of(rts::unit_at(i)));
    for (int i = 0; i < rts::kBldTypeCount; ++i) check(rts::ident_of(rts::bld_at(i)));
    for (int i = 0; i < rts::kObstacleTypeCount; ++i) {
        check(rts::ident_of(rts::obstacle_at(i)));
    }
    REQUIRE(seen.size() == 25);   // 11 + 11 + 3
}

TEST_CASE("可破坏障碍与地形枚举不重名", "[roster]") {
    // 这是 `地图与场景设计.md` §8.1 **第 19 条**在今天能落地的那一半。
    //
    // 第 19 条整条是「`Forest` 与 `Rock` 不得出现在可破坏障碍列表里」，它保护的是
    // 2.1.5：森林带是 2.1 整节唯一的承载者，玩家若能砍掉一段森林就能围墙，
    // 于是那一节退化成纯数值劝退。**2.1.5 已定案**（随 #23）。
    //
    // **校验器那一侧现在也实现了**（§6 于 2026-08-29 通过、6.2 随之新增 `obstacles`
    // 字段，第 19 条的两个阻塞一起解开）。所以这条约束现在**两侧各查一半**，
    // 而两半查的不是同一件事，都要留着：
    //
    //   * 这里查**代码里的枚举**——`ObstacleType` 的成员不与地形枚举重名
    //   * `tools/map_gen/validate.py` 的第 19 条查**地图文件写了什么**
    //     ——`obstacles[].type` 只能取那三个白名单值
    //
    // 少了前者，有人往 `ObstacleType` 里加 `Forest` 不会有任何地方红（地图文件里
    // 没写它，校验器就查不到）；少了后者，一张把 `Forest` 写进 `obstacles` 的图
    // 会在 C++ 载入时才炸，而那时错误信息指向查表失败、不指向 2.1.5。
    //
    // 查「重名」而不是查两个具体名字，是因为前者不随地形枚举增长而过期：
    // 将来 4.1 若再添一种不可破坏地形，它自动被覆盖，不需要有人记得回来改这里。
    std::set<std::string_view> terrain;
    for (int i = 0; i < rts::kTerrainCount; ++i) {
        terrain.insert(rts::ident_of(rts::terrain_at(i)));
    }
    REQUIRE(terrain.size() == static_cast<std::size_t>(rts::kTerrainCount));

    for (int i = 0; i < rts::kObstacleTypeCount; ++i) {
        const std::string_view id = rts::ident_of(rts::obstacle_at(i));
        INFO("可破坏障碍 " << id);
        REQUIRE(terrain.count(id) == 0);
    }

    // 反过来也钉一下，否则上面那条在 kObstacleTypeCount 被改成 0 时会空过 ——
    // 「循环体一次都没执行」是这份测试开头列的第一类静默漂移。
    REQUIRE(rts::kObstacleTypeCount > 0);
}

TEST_CASE("守方与攻方各自的单位数与常量一致", "[roster]") {
    // `side_of` 是逐成员的 switch，而 kDefenderUnitCount 是一个独立写下的数字。
    // 让两者互相印证：有人在守方那一段末尾插一个攻方单位时，
    // static_assert（查分块边界）与这条（查总数）会一起红。
    int def = 0;
    int atk = 0;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        if (rts::side_of(rts::unit_at(i)) == rts::Side::Defender) {
            ++def;
        } else {
            ++atk;
        }
    }
    REQUIRE(def == rts::kDefenderUnitCount);
    REQUIRE(atk == rts::kAttackerUnitCount);
}

TEST_CASE("守方单位在前、攻方在后，中间没有插队", "[roster]") {
    // 分块这条性质本身有用：观测的 one-hot、以及将来按侧切分数组时都靠它。
    // static_assert 只查了边界那一个下标，这里查整段单调。
    bool seen_attacker = false;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const bool is_atk = rts::side_of(rts::unit_at(i)) == rts::Side::Attacker;
        if (is_atk) seen_attacker = true;
        INFO("下标 " << i << " = " << rts::ident_of(rts::unit_at(i)));
        REQUIRE(is_atk == seen_attacker);   // 一旦进入攻方段就不许回到守方
    }
}

TEST_CASE("空中单位恰好两个：Phoenix 与 Wraith", "[roster]") {
    // **2026-09-03 设计变更**：`Wraith` 由地面改为空中（移动机制同 `Phoenix`）。
    // 原论证（AA 会因此每波稳赚、机会成本不再是两难）作为已知代价被接受，
    // 推导与取舍见 CLAUDE.md「空中单位」。
    //
    // 这条测试钉的是**总数**：再多一个会飞的兵种，防空的价值结构就要重新论证。
    int aerial = 0;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        if (rts::is_aerial(rts::unit_at(i))) ++aerial;
    }
    REQUIRE(aerial == 2);
    REQUIRE(rts::is_aerial(rts::UnitType::Phoenix));
    REQUIRE(rts::is_aerial(rts::UnitType::Wraith));
}

TEST_CASE("守方没有空军", "[roster]") {
    // 同样是结构性的（凡人不会飞）：空中威胁只能被**位置性**否定，
    // 无法通过机动拦截。玩家的决策因此是「AA 摆在哪、覆盖谁」这一空间问题。
    for (int i = 0; i < rts::kDefenderUnitCount; ++i) {
        REQUIRE_FALSE(rts::is_aerial(rts::unit_at(i)));
    }
}

TEST_CASE("无战力单位恰好三个", "[roster]") {
    // Scout / Mason / Wraith。它们不进克制二部图，因此实际的平衡负担
    // 小于花名册的单位总数——成本产出矩阵的定义域就是剩下那 8 个。
    int noncombat = 0;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        if (!rts::is_combat(rts::unit_at(i))) ++noncombat;
    }
    REQUIRE(noncombat == 3);
    REQUIRE_FALSE(rts::is_combat(rts::UnitType::Scout));
    REQUIRE_FALSE(rts::is_combat(rts::UnitType::Mason));
    REQUIRE_FALSE(rts::is_combat(rts::UnitType::Wraith));
}

TEST_CASE("Ranger 与 Scout 是两个不同的单位", "[roster]") {
    // CLAUDE.md 花名册下方专门论证过这两条不可合并，而合并的**代码形态**
    // 就是有人删掉其中一个、把它的用途并给另一个。
    //
    //   * 斥候必须便宜到可以消耗，情报交易才诚实；若侦查要动用主力机动部队，
    //     代价是「缺口没人堵」，玩家将永不侦查
    //   * Spear 太慢够不到墙外矿场，没有 Ranger 则「被迫出城争夺外部资源」
    //     缺乏执行工具
    //
    // 一个是无战力的，一个是战斗单位——这条差别正好可机械检查。
    REQUIRE(rts::is_combat(rts::UnitType::Ranger));
    REQUIRE_FALSE(rts::is_combat(rts::UnitType::Scout));
}

TEST_CASE("等级从 1 起", "[roster]") {
    // 已有 static_assert，这里重复一次是为了让**测试报告**里也有它。
    // 它保护的是观测里「等级和为 0 ⟺ 该格无敌人」，见 rts/obs.hpp。
    REQUIRE(rts::kMinUnitLevel == 1);
}

TEST_CASE("没有任何可破坏障碍产金币", "[roster]") {
    // **这条是三条结构约束里最要紧的一条，所以它单占一个 TEST_CASE。**
    // 原先它和下面那三行具体映射写在同一个用例里，而 `REQUIRE` 一失败就中止整个
    // 用例——于是「把 Rubble 改成产金币」这个探针**先撞上具体映射那一行**，
    // 这条循环一次都没跑到。两条断言写在一个用例里，靠后的那条就是这样悄悄失效的。
    //
    // 它保护两条独立的设计（任一足够）：金币管人力而清野不产人力（三种资源
    // 各对应一条决策轴）；以及 `Keep` 的「兵力地板」是**唯一**的非资源点金币来源，
    // 而 CLAUDE.md 明写想删它得先另找一条护栏——再开一个口子等于悄悄削弱它。
    //
    // **机械地查而不是列举那三种**：加第四种障碍时，逐条列举的写法照旧全绿。
    for (int i = 0; i < rts::kObstacleTypeCount; ++i) {
        REQUIRE(rts::harvest_of(rts::obstacle_at(i)) != rts::Resource::Gold);
    }
}

TEST_CASE("可破坏障碍的产出跟着材质走", "[roster]") {
    // CLAUDE.md「命名对照表：可破坏障碍」那一列的机械检查。
    // 木质的产木材、石质的产石材，于是玩家不需要额外记一张表——
    // 视觉规则（「木质、矮 = 可破坏」）已经把材质写进设计了。
    // 完整论证见 `无尽模式与地形分层.md` 6.6.2。
    REQUIRE(rts::harvest_of(rts::ObstacleType::Stump) == rts::Resource::Wood);
    REQUIRE(rts::harvest_of(rts::ObstacleType::Sapling) == rts::Resource::Wood);
    REQUIRE(rts::harvest_of(rts::ObstacleType::Rubble) == rts::Resource::Stone);
}

TEST_CASE("清野是玩家级命令，不是战术动作", "[roster]") {
    // §6 通过后曾考虑给战术枚举加一个 `AtkObst`，否掉了（理由见
    // CLAUDE.md「RL 设计决策」）。**这条用例锁的是那个否决**：
    // 战术动作仍是 13 个，而清野走 `CommandKind::Clear`。
    //
    // 放在 [roster] 而不是 [command]，是因为它要同时摸 `UnitAction` 与
    // `CommandKind` 两个枚举——查的是「这件事落在哪一侧」，不是命令自身的形状。
    REQUIRE(rts::kUnitActionCount == 13);
    REQUIRE(rts::owner_of(rts::CommandKind::Clear) == rts::Side::Defender);

    // 攻方拆障碍不需要动作：寻路已把障碍当「高代价可通行」，撞上去自动破坏。
    // 所以攻方的动作空间里不该出现任何清野/拆障碍的入口。
    REQUIRE_FALSE(rts::is_legal_for(rts::CommandKind::Clear, rts::Side::Attacker));
}
