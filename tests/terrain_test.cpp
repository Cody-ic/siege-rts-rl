// 地形枚举与三张位图。
//
// 这一批测的东西有一个共同点：**`地图与场景设计.md` 4.1 / 4.2 把它们写成了
// 断言句**（「五种恰好对应五个互不相同的标志位组合，没有冗余项」、
// 「格子可建造 ⇔ terrain.buildable AND NOT no_build」），
// 而写成断言句的东西就应该真的被断言。

#include <array>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/terrain.hpp"

TEST_CASE("五种地形的标志位组合两两不同", "[terrain]") {
    // 4.1 原文：「五种恰好对应五个互不相同的标志位组合，没有冗余项。」
    // 此前那一版只做前三种、把 Water 判为与 Rock 职能重复——正是这条测的东西。
    std::set<std::tuple<bool, bool, bool>> combos;
    for (int i = 0; i < rts::kTerrainCount; ++i) {
        const rts::Terrain t = rts::terrain_at(i);
        combos.insert({rts::is_passable(t), rts::is_buildable(t), rts::blocks_vision(t)});
    }
    REQUIRE(combos.size() == static_cast<std::size_t>(rts::kTerrainCount));
}

TEST_CASE("Water 是唯一「挡路但不挡视野」的地形", "[terrain]") {
    // 这一格是 Water 存在的全部理由：「你看得见他在对岸集结，但打不着，
    // 他也过不来，直到他上桥。」它同时是免费情报与需侦查情报的分界旋钮。
    int count = 0;
    for (int i = 0; i < rts::kTerrainCount; ++i) {
        const rts::Terrain t = rts::terrain_at(i);
        if (!rts::is_passable(t) && !rts::blocks_vision(t)) {
            ++count;
            REQUIRE(t == rts::Terrain::Water);
        }
    }
    REQUIRE(count == 1);
}

TEST_CASE("Bridge 可通行但不可建造", "[terrain]") {
    // 不可建造是结构性的：否则玩家能在桥上砌墙把唯一通路封死，
    // 而想要的形态是「要守只能在桥头设防」。
    REQUIRE(rts::is_passable(rts::Terrain::Bridge));
    REQUIRE_FALSE(rts::is_buildable(rts::Terrain::Bridge));
    // 只有 Plain 可建造——其余四种各有各的理由，但结论一样。
    for (int i = 0; i < rts::kTerrainCount; ++i) {
        const rts::Terrain t = rts::terrain_at(i);
        REQUIRE(rts::is_buildable(t) == (t == rts::Terrain::Plain));
    }
}

TEST_CASE("空中层对全部地形都通", "[terrain]") {
    // 4.1 末尾：「空中单位的位图里水是可通行的」。Phoenix 的唯一克制手段是
    // 位置性的防空，地形不参与——这条恒真是结构性事实，不是偷懒。
    for (int i = 0; i < rts::kTerrainCount; ++i) {
        const rts::Terrain t = rts::terrain_at(i);
        REQUIRE(rts::is_passable(t, rts::Mobility::Aerial));
        REQUIRE(rts::is_passable(t, rts::Mobility::Ground) == rts::is_passable(t));
    }
}

TEST_CASE("地形标识符唯一、非空、纯 ASCII", "[terrain]") {
    std::set<std::string> seen;
    for (int i = 0; i < rts::kTerrainCount; ++i) {
        const std::string s(rts::ident_of(rts::terrain_at(i)));
        REQUIRE_FALSE(s.empty());
        for (const char c : s) {
            REQUIRE(static_cast<unsigned char>(c) < 0x80);
        }
        REQUIRE(seen.insert(s).second);
    }
    REQUIRE(seen.size() == static_cast<std::size_t>(rts::kTerrainCount));
    // 它同时是精灵元数据的键前缀，所以名字与枚举必须逐字对应。
    REQUIRE(rts::ident_of(rts::Terrain::Bridge) == "Bridge");
}

TEST_CASE("buildable 是地形与 no_build 的合成", "[terrain]") {
    // 4.2 那条规则的四种组合。不明写规则的话
    // 「terrain=Plain + no_build=1」与「terrain=Bridge」谁管谁只能靠猜。
    const std::vector<rts::Terrain> terrain{
        rts::Terrain::Plain,   // no_build 0 → 可建
        rts::Terrain::Plain,   // no_build 1 → 不可建
        rts::Terrain::Bridge,  // no_build 0 → 不可建（地形否决）
        rts::Terrain::Bridge,  // no_build 1 → 不可建
    };
    const std::vector<std::uint8_t> no_build{0, 1, 0, 1};
    const rts::TerrainMasks m(4, 1, terrain.data(), no_build.data());

    REQUIRE(m.buildable(0, 0));
    REQUIRE_FALSE(m.buildable(1, 0));
    REQUIRE_FALSE(m.buildable(2, 0));
    REQUIRE_FALSE(m.buildable(3, 0));
}

TEST_CASE("no_build 为空等价于全 0", "[terrain]") {
    const std::vector<rts::Terrain> terrain{rts::Terrain::Plain, rts::Terrain::Plain};
    const rts::TerrainMasks m(2, 1, terrain.data(), nullptr);
    REQUIRE(m.buildable(0, 0));
    REQUIRE(m.buildable(1, 0));
}

TEST_CASE("非方形网格钉住行主序 y*w+x", "[terrain]") {
    // **必须用非方形网格。** 把 x 与 y 写反在方形网格上不报错、只是整张图转置
    // ——`tests/map_loader_test.cpp` 与 `mapfile.py` 用同一手法钉过同一件事。
    //
    // 3 宽 × 2 高，岩壁在逻辑格 (x=1, y=0)。**这个格子是刻意挑的**：
    // 行主序下标 = 0*3+1 = 1，而转置写法 x*h+y = 1*2+0 = 2 —— 两者不同。
    // 换成 (2, 1) 就不行：那里两个式子都算出 5，测试会对一个转置的实现放绿灯
    // （第一版正是这么写的，于是它测不出任何东西）。
    std::vector<rts::Terrain> terrain(6, rts::Terrain::Plain);
    terrain[0 * 3 + 1] = rts::Terrain::Rock;
    const rts::TerrainMasks m(3, 2, terrain.data(), nullptr);

    REQUIRE(m.width() == 3);
    REQUIRE(m.height() == 2);
    REQUIRE(m.at(1, 0) == rts::Terrain::Rock);
    REQUIRE_FALSE(m.passable(1, 0));
    // 转置的实现会在这里读到岩壁。
    REQUIRE(m.at(2, 0) == rts::Terrain::Plain);
    // 其余五格都是平地。
    int rock = 0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            if (m.at(x, y) == rts::Terrain::Rock) ++rock;
        }
    }
    REQUIRE(rock == 1);
    REQUIRE(m.cell_count() == 6);
}

TEST_CASE("layout_hash 对位置敏感", "[terrain]") {
    // 这个哈希的唯一职责就是让「地形被摆错位置」变成一个对不上的数。
    // 两块地形内容完全相同（各一格岩壁），只差那一格在哪 —— 下标 1 是逻辑格
    // (1,0) 的正确位置，下标 2 是同一个逻辑格按转置式子算出来的位置。
    std::vector<rts::Terrain> a(6, rts::Terrain::Plain);
    a[1] = rts::Terrain::Rock;
    std::vector<rts::Terrain> b(6, rts::Terrain::Plain);
    b[2] = rts::Terrain::Rock;

    const rts::TerrainMasks ma(3, 2, a.data(), nullptr);
    const rts::TerrainMasks mb(3, 2, b.data(), nullptr);
    REQUIRE(ma.layout_hash() != mb.layout_hash());

    // 同样的输入必须给同样的值（否则它没法用来比对）。
    const rts::TerrainMasks ma2(3, 2, a.data(), nullptr);
    REQUIRE(ma.layout_hash() == ma2.layout_hash());
}

TEST_CASE("layout_hash 对 no_build 敏感", "[terrain]") {
    // buildable 表进哈希，所以 no_build 改了它必须变——否则「地图的可建造区
    // 被改了但回放照旧」会静默通过。
    const std::vector<rts::Terrain> terrain(4, rts::Terrain::Plain);
    const std::vector<std::uint8_t> none{0, 0, 0, 0};
    const std::vector<std::uint8_t> one{0, 1, 0, 0};
    const rts::TerrainMasks a(4, 1, terrain.data(), none.data());
    const rts::TerrainMasks b(4, 1, terrain.data(), one.data());
    REQUIRE(a.layout_hash() != b.layout_hash());
}
