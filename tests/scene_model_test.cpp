#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "game/map_data.hpp"
#include "game/map_loader.hpp"
#include "game/scene_model.hpp"
#include "game/battle_scene.hpp"
#include "rts/world.hpp"
#include "rts/roster.hpp"
#include "rts/unit_behavior.hpp"

#ifndef GAME_TESTDATA_DIR
#error "GAME_TESTDATA_DIR 未定义，见 tests/CMakeLists.txt"
#endif
// 演示地图（交付物那一份）另有一条用例要读它——桥的那个形状只在**那张图**上，
// 抄进夹具就等于把「玩家看到的图修好了没有」换成「我抄对了没有」。
#ifndef GAME_DATA_DIR
#error "GAME_DATA_DIR 未定义，见 tests/CMakeLists.txt"
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

TEST_CASE("Player walls use live neighbors including construction sites", "[scene]") {
    const auto map = fixture();
    rts::WorldInit init;
    init.width = map.width();
    init.height = map.height();
    init.terrain.assign(static_cast<std::size_t>(init.width * init.height), rts::Terrain::Plain);
    init.keep = at(0, 0);
    init.buildings.push_back({rts::BldType::Keep, init.keep, 100, 100});
    rts::World world(init);
    const auto facing = [&](int x, int y) {
        const auto items = game::BattleScene::sorted(map, world.view(rts::Side::Defender), world.now());
        const int index = index_of(items, x, y, "Wall");
        REQUIRE(index >= 0);
        return items[static_cast<std::size_t>(index)].facing;
    };

    SECTION("Horizontal walls on previously empty cells include unfinished gates") {
        for (int x = 1; x <= 5; ++x) {
            REQUIRE(map.wall_at(x, 1) == nullptr);
            world.place_bld(x == 3 ? rts::BldType::Gate : rts::BldType::Wall,
                            at(x, 1), 10, 100, 20);
        }
        REQUIRE(facing(1, 1) == game::Facing::NE);
        REQUIRE(facing(2, 1) == game::Facing::NE);
        REQUIRE(facing(4, 1) == game::Facing::NE);
        REQUIRE(facing(5, 1) == game::Facing::NE);
    }
    SECTION("Vertical walls ignore nearby non-wall buildings") {
        for (int y = 1; y <= 3; ++y) world.place_bld(rts::BldType::Wall, at(3, y), 100, 100);
        world.place_bld(rts::BldType::Tower, at(2, 2), 100, 100);
        REQUIRE(facing(3, 2) == game::Facing::SE);
    }
    SECTION("Placement preview shares live neighbor and corner rules without mutating the world") {
        world.place_bld(rts::BldType::Wall, at(3, 3), 100, 100);
        const auto before=world.state_hash();
        const std::vector<rts::GridPos> planned{at(2,2),at(3,2),at(4,2)};
        const auto view=world.view(rts::Side::Defender);
        REQUIRE(game::SceneModel::run_direction(view,at(3,2),planned)==game::Facing::NE);
        REQUIRE(game::SceneModel::is_wall_corner(view,at(3,2),planned));
        REQUIRE(game::SceneModel::run_direction(view,at(3,2))==game::Facing::SE);
        REQUIRE_FALSE(view.bld_at(at(3,2)).valid());
        REQUIRE(world.state_hash()==before);
    }
    SECTION("Demolition updates corners and does not revive initial map walls") {
        world.place_bld(rts::BldType::Wall, at(2, 2), 100, 100);
        const auto east = world.place_bld(rts::BldType::Gate, at(3, 2), 100, 100);
        world.place_bld(rts::BldType::Wall, at(2, 3), 10, 100, 20);
        const auto count = [&] {
            const auto items = game::BattleScene::sorted(map, world.view(rts::Side::Defender), world.now());
            return std::count_if(items.begin(), items.end(), [](const auto& item) {
                return item.sprite == "Wall" && item.pos == at(2, 2);
            });
        };
        REQUIRE(count() == 2);
        world.destroy_bld(east);
        REQUIRE(count() == 1);
        REQUIRE(facing(2, 2) == game::Facing::SE);
        world.place_bld(rts::BldType::Wall, at(1, 4), 100, 100);
        REQUIRE(facing(1, 4) == game::Facing::SE);
    }
    SECTION("A gap keeps horizontal orientation while outer neighbors survive") {
        rts::BldId middle;
        for (int x = 1; x <= 5; ++x) {
            const auto id = world.place_bld(rts::BldType::Wall, at(x, 1), 100, 100);
            if (x == 3) middle = id;
        }
        world.destroy_bld(middle);
        REQUIRE(facing(2, 1) == game::Facing::NE);
        REQUIRE(facing(4, 1) == game::Facing::NE);
    }
}

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

    SECTION("沿 gi 铺的墙段用 NE") {
        // 夹具第 4 行有一段横墙 (0,4) (1,4) (2,4)。
        //
        // **这一条上一版写的是 SW**，改成 NE 与下面那条竖墙从 NW 改成 SE 是
        // **同一个原因**（scene_model.hpp 那张逐朝向实测表）：四个朝向共用一个
        // 锚点，而 `SW`/`NW` 两张的内容偏离它、`Wall` 还被画布切掉 228 px。
        // 上一轮只改了竖直那一档，于是南北门一直是错开的。
        REQUIRE(SceneModel::run_direction(m, at(1, 4), RunKind::Wall) ==
                game::Facing::NE);
        REQUIRE(SceneModel::run_direction(m, at(0, 4), RunKind::Wall) ==
                game::Facing::NE);
    }

    SECTION("沿 gj 铺的墙段用 SE") {
        // 夹具右侧有一段竖墙 (6,0) (6,1) (6,2)：i 方向没有邻居。
        //
        // **这一条上一版写的是 NW**，改成 SE 是重测的结果（判据多了「贴得正
        // 不正」，见 scene_model.hpp 那张表）：墙板 180° 对称，两个朝向拼起来
        // 都无缝，但这套素材的 NE / NW 内容偏离锚点，于是墙浮在格子外、
        // 门与墙错开约 0.9 格。
        REQUIRE(SceneModel::run_direction(m, at(6, 1), RunKind::Wall) ==
                game::Facing::SE);
    }

    SECTION("同一条墙线上，门与墙取到的朝向必须相同") {
        // 这条钉的是玩家看到的那个症状本身（「城门缩在城内、没和城墙连上」）。
        // 门与墙是两个 ident、画布与锚点都不同，一旦朝向不同就必然错开；
        // 而它们的朝向来自同一个函数，所以这条断言等价于「同一条墙线上
        // 每一段问出来的走向一致」。
        //
        // 夹具第 4 行：(0,4) 墙、(1,4) 墙、(2,4) 门，沿 gi。
        const game::Facing f = SceneModel::run_direction(m, at(0, 4), RunKind::Wall);
        REQUIRE(SceneModel::run_direction(m, at(1, 4), RunKind::Wall) == f);
        REQUIRE(SceneModel::run_direction(m, at(2, 4), RunKind::Wall) == f);
    }

    SECTION("桥的走向是过河的方向，看的是相邻的水格") {
        // 桥在 (3,2) (4,2)，河沿 gj（x=3、x=4 两列都是水），所以桥沿 gi。
        // **桥沿 gi 用 SE，与墙的表相反**（两个模型的长轴不同轴，见头文件那张表）。
        REQUIRE(SceneModel::run_direction(m, at(3, 2), RunKind::Bridge) ==
                game::Facing::SE);
    }

    SECTION("同样是沿 gi，墙与桥取到的朝向不同") {
        // 两张表是**反的**（两个模型的长轴不同轴）。这条把它钉住，免得哪天
        // 有人「顺手统一一下」——统一之后桥会横过来躺在河里，而墙会浮到格外。
        //
        // (1,4) 是沿 gi 的墙、(3,2) 是沿 gi 的桥（河沿 gj）。
        REQUIRE(SceneModel::run_direction(m, at(1, 4), RunKind::Wall) ==
                game::Facing::NE);
        REQUIRE(SceneModel::run_direction(m, at(3, 2), RunKind::Bridge) ==
                game::Facing::SE);
    }
}

// 墙与门的朝向**只能取「干净」那两个**。
//
// 四个朝向共用一个 `ground_anchor`，而 `SW`/`NW` 两张图的内容偏离它
// （`Wall` +34.5 px 且右侧被画布切掉 228 px、`Gate` −32.5 px，逐张实测见
// `scene_model.hpp` 那张表）。用到它们的后果是**画面照样出、只是错位**：
// 门与墙朝相反方向各偏一段、墙被切掉一截，没有任何别的测试会红。
//
// 这条断言只钉枚举值——像素那一侧（「这两个朝向确实是正的」）归 `render/` 的
// `SpriteAtlas::verify_linear_anchor()`，两边合起来才是完整的判据（§7 把像素
// 几何的唯一来源定在 `_sprite_meta.json`，`game/` 不许读它）。
TEST_CASE("墙与门的走向只取内容落在锚点上的那两个朝向", "[scene]") {
    using game::SceneModel;
    const game::MapData m = fixture();
    const game::DrawLists d = SceneModel::build(m);

    int walls = 0;
    for (const game::DrawItem& it : d.sorted) {
        if (it.sprite != "Wall" && it.sprite != "Gate") continue;
        ++walls;
        CAPTURE(it.pos.i, it.pos.j, it.sprite, game::to_string(it.facing));
        REQUIRE((it.facing == game::Facing::SE || it.facing == game::Facing::NE));
    }
    REQUIRE(walls > 0);   // 夹具真的有墙，否则这条恒真
}

// 演示地图上的桥：**两格桥并排在同一条一格宽的河上**，而这正是「看相邻桥格」
// 那条旧规则会答错的形状——它会把走向答成「沿着河」，于是桥板顺着水流铺，
// 画面上读作「河里漂着两块板」，玩家的原话是「bridge 没在水上」。
//
// 用真的演示地图而不是另造夹具：这个形状是**那张图**的形状，抄一份到夹具里
// 就等于把「玩家实际看到的图有没有被修好」换成「我抄对了没有」。
TEST_CASE("桥的走向：一格宽的河上并排两格桥，走向仍是过河方向", "[scene]") {
    using game::SceneModel;
    using RunKind = game::SceneModel::RunKind;
    const game::MapData demo =
        game::MapLoader::from_file(std::string(GAME_DATA_DIR) + "/demo_skirmish.json");

    // 河是 x=13 那一列，桥在 (13,5) 与 (13,6)——两格沿 gj 相邻。
    REQUIRE(demo.terrain_at(13, 5) == game::Terrain::Bridge);
    REQUIRE(demo.terrain_at(13, 6) == game::Terrain::Bridge);
    REQUIRE(demo.terrain_at(13, 4) == game::Terrain::Water);
    REQUIRE(demo.terrain_at(12, 5) == game::Terrain::Plain);

    // 过河的方向是 gi ⇒ 桥用 SE。（旧规则看相邻桥格，会答成沿 gj。）
    REQUIRE(SceneModel::run_direction(demo, at(13, 5), RunKind::Bridge) ==
            game::Facing::SE);
    REQUIRE(SceneModel::run_direction(demo, at(13, 6), RunKind::Bridge) ==
            game::Facing::SE);
    // 演示地图的墙线沿 gj（x=7，j=3..9），门在 (7,6)：整条线朝向一致，
    // 这就是「门缩在城内」那条修好之后该有的样子。
    const game::Facing wf = SceneModel::run_direction(demo, at(7, 3), RunKind::Wall);
    REQUIRE(wf == game::Facing::SE);
    for (int j = 4; j <= 9; ++j) {
        REQUIRE(SceneModel::run_direction(demo, at(7, j), RunKind::Wall) == wf);
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

// 资源点必须在画面上**可见**——2026-09-01 试玩反馈「城内外完全不会刷新金矿」，
// 而数据里金矿一直在：渲染层从来不画资源点。标记不是实体（不进花名册、不进
// 回放），贴图是一份单独存放的真实素材（`SpriteAtlas::register_decal_image`），
// 本用例钉的是**装配层把标记摆上了地图**这一半。
TEST_CASE("装配：每个资源点都有一枚地表标记，与叠加物同层", "[scene]") {
    const game::MapData m = fixture();
    const game::DrawLists d = game::SceneModel::build(m);

    SECTION("标识符与所属枚举成员同前缀") {
        REQUIRE(game::SceneModel::resource_marker(rts::Resource::Stone) == "StonePt");
        REQUIRE(game::SceneModel::resource_marker(rts::Resource::Wood) == "WoodPt");
        REQUIRE(game::SceneModel::resource_marker(rts::Resource::Gold) == "GoldPt");
    }

    SECTION("四个资源点（夹具图）各有对应标记，石/木/金各自的名字") {
        // fixture_min.json：stone [1,3]、wood [2,1]、gold [0,2]、wood [5,2]。
        REQUIRE(index_of(d.sorted, 1, 3, "StonePt") >= 0);
        REQUIRE(index_of(d.sorted, 2, 1, "WoodPt") >= 0);
        REQUIRE(index_of(d.sorted, 0, 2, "GoldPt") >= 0);
        REQUIRE(index_of(d.sorted, 5, 2, "WoodPt") >= 0);
    }

    SECTION("标记与叠加物同层：同深度时先于实体（采集建筑落成后盖住它）") {
        // 夹具图 (2,1) 上的标记深度 3；找一个同深度的实体项比不出来，
        // 就换层语义最直接的断言：标记格上的 tile 仍是地砖（标记不进 tiles）。
        REQUIRE(index_of(d.tiles, 1, 3, "StonePt") < 0);
        for (const game::DrawItem& it : d.sorted) {
            if (it.sprite == "StonePt" || it.sprite == "WoodPt" ||
                it.sprite == "GoldPt") {
                REQUIRE(it.hp_frac < 0.0f);   // 标记不带血条
            }
        }
    }
}

// 城圈四角的 L 形拐角：`run_direction` 对「左右有墙」的格恒判横板（SW），
// 于是拐角格只画了横的那一边、竖边缺一格——四个角在画面上是开的。
// 机制上走不进来（rts_core 的穿角禁令），是纯视觉缺陷。修复是给拐角格
// **补画一块竖板（SE）**，本用例钉住「拐角判定」与「拐角格出两块板」两件事。
//
// 用 from_string 另造一张带 L 形拐角的夹具，而不是改 fixture_min.json——
// 那张是全仓唯一真地图，位置是精挑细选过的（见其 _note），且没有拐角。
TEST_CASE("城圈四角：拐角格补一块竖板，非拐角不补", "[scene]") {
    using game::SceneModel;
    const char* corner_json = R"({
      "format": 1,
      "size": [5, 5],
      "map_id": "corner_fixture",
      "name": "拐角夹具",
      "keep": [2, 2],
      "layers": {
        "terrain": {"palette": ["Plain", "Rock", "Forest", "Water", "Bridge"],
                    "rows": ["00000","00000","00000","00000","00000"]},
        "no_build": {"rows": ["00000","00000","00000","00000","00000"]}
      },
      "spawns": [{"id": 0, "pos": [0, 0]}],
      "resources": [],
      "initial_walls": [
        {"kind": "Wall", "pos": [0, 3], "hp_frac": 1.0},
        {"kind": "Wall", "pos": [1, 3], "hp_frac": 1.0},
        {"kind": "Wall", "pos": [2, 3], "hp_frac": 1.0},
        {"kind": "Wall", "pos": [3, 3], "hp_frac": 1.0},
        {"kind": "Wall", "pos": [3, 4], "hp_frac": 1.0}
      ],
      "obstacles": [],
      "buildings": []
    })";
    const game::MapData m = game::MapLoader::from_string(corner_json, "<拐角夹具>");

    // 拐角 (3,3)：左 (2,3) 是墙、下 (3,4) 是墙——横竖两条边在此相交。
    REQUIRE(SceneModel::is_wall_corner(m, at(3, 3)));
    // 非拐角：横墙中段 (1,3)（上下无墙）、竖墙下端 (3,4)（左右无墙）。
    REQUIRE_FALSE(SceneModel::is_wall_corner(m, at(1, 3)));
    REQUIRE_FALSE(SceneModel::is_wall_corner(m, at(3, 4)));
    // 横墙最左端 (0,3)：左出界、右是墙，上下无——也不是拐角。
    REQUIRE_FALSE(SceneModel::is_wall_corner(m, at(0, 3)));

    // 拐角格出两块 Wall 板：run_direction 给的主板（横边 NE）+ 补的竖板（SE）。
    // 非拐角格只出一块。
    //
    // **两块板必须都落在「干净」那组 {SE, NE}**（scene_model.hpp 的逐朝向实测表）。
    // 这不是顺带要求：拐角处横板被裁掉的那一端，正是它该与竖板接上的那一端——
    // 所以「四个角没包起来」有两个独立成因，补板只解掉其中一个，横板走脏朝向
    // （`SW`，`Wall` 被切 228 px）时角照样是开的。
    const game::DrawLists d = SceneModel::build(m);
    int corner_boards = 0;
    bool saw_ne = false, saw_se = false;
    int mid_boards = 0;
    for (const game::DrawItem& it : d.sorted) {
        if (it.sprite != "Wall") continue;
        if (it.pos.i == 3 && it.pos.j == 3) {
            ++corner_boards;
            if (it.facing == game::Facing::NE) saw_ne = true;
            if (it.facing == game::Facing::SE) saw_se = true;
        }
        if (it.pos.i == 1 && it.pos.j == 3) ++mid_boards;
    }
    REQUIRE(corner_boards == 2);
    REQUIRE(saw_ne);
    REQUIRE(saw_se);
    REQUIRE(mid_boards == 1);
}

// `battle_scene.cpp` 挑弹丸精灵时，「单位射的」那一档**只按阵营分**：
// 守方 → 箭（`Archer`）、攻方 → 魔法弹（`Shade`）。
//
// 这么做是因为 `p_side_` 本来就在 `World` 里（放箭那刻定格、随压实搬移、进哈希），
// 而知道**具体兵种**要新增 `p_src_unit_`，那会动 `World` 布局、`state_hash` 的
// 喂入清单与 `kWorldHashTag`（旧回放重录）。用已有字段是零代价的那条路。
//
// **但它靠的是花名册的一条性质，不是结构不变量**：`launches_projectile()`
// （Ranged × 非空中）在每一侧恰好命中一个兵种。某一侧多出第二个远程地面兵种时，
// 按侧挑就会让两者共用同一张图——而那种错**画面照样出、只是画错了**，
// 没有任何别的测试会红。所以在这里钉住。
//
// **为什么不是 `static_assert`**：`launches_projectile()` 是虚函数、
// `behavior_of()` 返回静态实例的引用且不是 constexpr，编译期到不了。
// 这正是 `CLAUDE.md`「多态按兵种」那节说的——虚函数丢掉的完备性保证，
// 由测试把两边互相印证补回来。
//
// 触发之后**不要把数字从 1 改成 2 了事**：那说明「按侧挑」这条前提没了，
// 该去补 `p_src_unit_`（并把 `kWorldHashTag` 进格），
// 或者给新兵种复用同一张图并在 `battle_scene.cpp` 写明为什么可以共用。
TEST_CASE("弹丸精灵按阵营分档的前提：每侧恰好一个兵种会放弹丸", "[scene]") {
    int per_side[rts::kSideCount] = {};
    std::vector<rts::UnitType> launchers;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const auto t = static_cast<rts::UnitType>(i);
        if (!rts::behavior_of(t).launches_projectile()) continue;
        launchers.push_back(t);
        ++per_side[static_cast<int>(rts::side_of(t))];
    }
    CAPTURE(launchers.size());
    REQUIRE(per_side[static_cast<int>(rts::Side::Defender)] == 1);
    REQUIRE(per_side[static_cast<int>(rts::Side::Attacker)] == 1);

    // 顺带钉住是**哪两个**：数字对而兵种换了人，精灵映射同样会错
    // （比如某天 `Ranger` 改成远程、`Archer` 改成近战，计数仍是 1:1）。
    REQUIRE(launchers.size() == 2);
    REQUIRE(std::find(launchers.begin(), launchers.end(), rts::UnitType::Archer)
            != launchers.end());
    REQUIRE(std::find(launchers.begin(), launchers.end(), rts::UnitType::Shade)
            != launchers.end());
}
