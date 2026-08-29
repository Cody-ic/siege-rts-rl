#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "game/map_data.hpp"
#include "game/map_loader.hpp"
#include "game/scene_model.hpp"

#ifndef GAME_TESTDATA_DIR
#error "GAME_TESTDATA_DIR 未定义，见 tests/CMakeLists.txt"
#endif

namespace {

game::MapData fixture() {
    return game::MapLoader::from_file(std::string(GAME_TESTDATA_DIR) +
                                      "/fixture_min.json");
}

rts::GridPos at(int x, int y) {
    return rts::GridPos{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
}

// 在绘制列表里找某一格上的某个精灵，返回它的下标；没有则返回 -1。
int index_of(const std::vector<game::DrawItem>& v, int x, int y,
             std::string_view sprite) {
    for (std::size_t k = 0; k < v.size(); ++k) {
        if (v[k].pos.i == x && v[k].pos.j == y && v[k].sprite == sprite) {
            return static_cast<int>(k);
        }
    }
    return -1;
}

}  // namespace

// 4.2.1：一格要画两张图，不是一张。
TEST_CASE("地形枚举展开成 (地砖, 叠加物)", "[scene]") {
    using game::SceneModel;
    using game::Terrain;

    SECTION("Plain 与 Water 自己就是地砖，没有叠加物") {
        REQUIRE(SceneModel::expand(Terrain::Plain).tile == "Plain");
        REQUIRE(SceneModel::expand(Terrain::Plain).overlay.empty());
        REQUIRE(SceneModel::expand(Terrain::Water).tile == "Water");
        REQUIRE(SceneModel::expand(Terrain::Water).overlay.empty());
    }

    SECTION("Rock 与 Forest 是立体物件，要 Plain 作底衬") {
        REQUIRE(SceneModel::expand(Terrain::Rock).tile == "Plain");
        REQUIRE(SceneModel::expand(Terrain::Rock).overlay == "Rock");
        REQUIRE(SceneModel::expand(Terrain::Forest).tile == "Plain");
        REQUIRE(SceneModel::expand(Terrain::Forest).overlay == "Forest");
    }

    // 这一条单独立一个 SECTION，因为它是 4.2.1 里唯一一条「反直觉」的：
    // 桥的底衬是 **Water** 而不是 Plain。铺平地会让河在桥这一格视觉上断掉，
    // 读作「水断了一格换成桥」而不是「桥架在水上」。
    SECTION("桥的底衬是 Water，不是 Plain") {
        REQUIRE(SceneModel::expand(Terrain::Bridge).tile == "Water");
        REQUIRE(SceneModel::expand(Terrain::Bridge).overlay == "Bridge");
    }
}

TEST_CASE("Plain 的贴图变体必须确定", "[scene]") {
    using game::SceneModel;

    // 同一格每次都得到同一个变体，否则画面每帧闪烁。
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            REQUIRE(SceneModel::plain_variant(at(x, y)) ==
                    SceneModel::plain_variant(at(x, y)));
        }
    }

    // 而且它得真的在挑，不能退化成永远同一张——那样这个函数就没有意义了。
    std::vector<std::string_view> seen;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const std::string_view v = SceneModel::plain_variant(at(x, y));
            if (std::find(seen.begin(), seen.end(), v) == seen.end()) seen.push_back(v);
        }
    }
    REQUIRE(seen.size() >= 2);

    // 只在两个中性草地变体之间挑。PlainDirt / PlainRoad / PlainAsh 暂不使用，
    // 因为地图格式里没有字段能表达「这格是路」——随机撒路面会得到一张断头路的图。
    for (std::string_view v : seen) {
        REQUIRE((v == "Plain" || v == "PlainB"));
    }
}

// 4.2.1.1：线性结构的 d 是**走向**，不是朝向。
TEST_CASE("线性结构的朝向由走向决定", "[scene]") {
    using game::SceneModel;
    const game::MapData m = fixture();
    using RunKind = game::SceneModel::RunKind;

    SECTION("沿 gi 铺的墙段用 SW") {
        // 夹具第 4 行有一段横墙 (0,4) (1,4) (2,4)。
        REQUIRE(SceneModel::run_direction(m, at(1, 4), RunKind::Wall) ==
                game::Facing::SW);
        REQUIRE(SceneModel::run_direction(m, at(0, 4), RunKind::Wall) ==
                game::Facing::SW);
    }

    SECTION("沿 gj 铺的墙段用 NW") {
        // 夹具右侧有一段竖墙 (6,0) (6,1) (6,2)：i 方向没有邻居。
        REQUIRE(SceneModel::run_direction(m, at(6, 1), RunKind::Wall) ==
                game::Facing::NW);
    }

    SECTION("桥看的是相邻的 Bridge 地形格，不是墙") {
        // 桥在 (3,2) (4,2)，沿 gi 铺。
        REQUIRE(SceneModel::run_direction(m, at(3, 2), RunKind::Bridge) ==
                game::Facing::SW);
        // 同一格若按「墙」去问，答案不同——这正是要分成两种 RunKind 的理由。
        REQUIRE(SceneModel::run_direction(m, at(3, 2), RunKind::Wall) ==
                game::Facing::NW);
    }
}

TEST_CASE("装配：地砖一遍，叠加物与实体混在同一个深度序列里", "[scene]") {
    const game::MapData m = fixture();
    const game::DrawLists d = game::SceneModel::build(m);

    SECTION("地砖每格一张") {
        REQUIRE(d.tiles.size() == static_cast<std::size_t>(m.width() * m.height()));
    }

    SECTION("桥那一格的地砖是 Water") {
        REQUIRE(index_of(d.tiles, 3, 2, "Water") >= 0);
        REQUIRE(index_of(d.tiles, 3, 2, "Plain") < 0);
        REQUIRE(index_of(d.tiles, 3, 2, "PlainB") < 0);
    }

    SECTION("深度非降") {
        for (std::size_t k = 1; k < d.sorted.size(); ++k) {
            REQUIRE(d.sorted[k - 1].depth() <= d.sorted[k].depth());
        }
    }

    // 这一条是本文件的重点：**叠加物与实体必须在同一个序列里排**。
    // 若实现成「先画完全部地形叠加物，再画全部实体」，站在岩壁前面的单位
    // 就不会遮住岩壁——而这两趟各自都是「按深度排好的」，看起来完全正常。
    SECTION("叠加物与墙段按深度交错，不是各走一趟") {
        const int rock = index_of(d.sorted, 1, 0, "Rock");        // 深度 1
        const int wall_left = index_of(d.sorted, 0, 4, "Wall");   // 深度 4
        const int bridge_b = index_of(d.sorted, 4, 2, "Bridge");  // 深度 6
        const int wall_right = index_of(d.sorted, 6, 2, "Wall");  // 深度 8

        REQUIRE(rock >= 0);
        REQUIRE(wall_left >= 0);
        REQUIRE(bridge_b >= 0);
        REQUIRE(wall_right >= 0);

        // 叠加物(1) < 实体(4) < 叠加物(6) < 实体(8)。
        // 两趟渲染排不出这个顺序：那样全部叠加物都会排在全部实体之前（或之后）。
        REQUIRE(rock < wall_left);
        REQUIRE(wall_left < bridge_b);
        REQUIRE(bridge_b < wall_right);
    }

    SECTION("同深度时叠加物先于实体") {
        // 深度 5 上正好有两个：桥 (3,2) 是叠加物，墙 (1,4) 是实体。
        const int bridge_a = index_of(d.sorted, 3, 2, "Bridge");
        const int wall_mid = index_of(d.sorted, 1, 4, "Wall");
        REQUIRE(bridge_a >= 0);
        REQUIRE(wall_mid >= 0);
        REQUIRE(d.sorted[static_cast<std::size_t>(bridge_a)].depth() ==
                d.sorted[static_cast<std::size_t>(wall_mid)].depth());
        REQUIRE(bridge_a < wall_mid);
    }

    SECTION("城门用自己的精灵") {
        REQUIRE(index_of(d.sorted, 2, 4, "Gate") >= 0);
        REQUIRE(index_of(d.sorted, 2, 4, "Wall") < 0);
    }

    SECTION("排序只由内容决定，与插入顺序无关") {
        // 同一张图装配两次，结果必须逐项相同。这条防的是「排序依赖了插入顺序」
        // ——那种实现今天恰好对，明天换个遍历方向就变。
        const game::DrawLists again = game::SceneModel::build(m);
        REQUIRE(again.sorted.size() == d.sorted.size());
        for (std::size_t k = 0; k < d.sorted.size(); ++k) {
            REQUIRE(again.sorted[k].pos == d.sorted[k].pos);
            REQUIRE(again.sorted[k].sprite == d.sorted[k].sprite);
            REQUIRE(again.sorted[k].facing == d.sorted[k].facing);
        }
    }
}
