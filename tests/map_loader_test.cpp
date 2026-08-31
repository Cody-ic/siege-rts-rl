#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "game/map_data.hpp"
#include "game/map_loader.hpp"

// 夹具路径由 CMake 以编译期常量给出，不依赖 ctest 的工作目录。
#ifndef GAME_TESTDATA_DIR
#error "GAME_TESTDATA_DIR 未定义，见 tests/CMakeLists.txt"
#endif

namespace {

std::string fixture_path() {
    return std::string(GAME_TESTDATA_DIR) + "/fixture_min.json";
}

// 一份最小的合法地图，供「改一处使它变坏」的用例复用。
// 3x2，也是非方形——理由同夹具。
const char* const kMinimal = R"({
  "format": 1,
  "map_id": "t",
  "name": "t",
  "size": [3, 2],
  "layers": {
    "terrain":  { "palette": ["Plain","Rock","Forest","Water","Bridge"],
                  "rows": ["012", "340"] },
    "no_build": { "rows": ["000", "010"] }
  },
  "keep": [0, 0],
  "spawns": [{ "id": 0, "pos": [2, 1], "corridor": "open" }],
  "resources": [{ "type": "stone", "pos": [1, 0], "tier": "inner", "unlock_wave": 1 }],
  "initial_walls": [{ "kind": "Wall", "pos": [0, 1], "hp_frac": 1.0 }],
  "obstacles": [{ "type": "Stump", "pos": [2, 0] }],
  "buildings": []
})";

}  // namespace

TEST_CASE("夹具地图能从文件读出来", "[map]") {
    const game::MapData m = game::MapLoader::from_file(fixture_path());

    REQUIRE(m.width() == 7);
    REQUIRE(m.height() == 5);
    REQUIRE(m.map_id() == "fixture_min");
    REQUIRE(m.spawns().size() == 2);
    REQUIRE(m.resources().size() == 4);
    REQUIRE(m.walls().size() == 6);
}

// 这一条是本文件里最要紧的。
//
// `pos` 是 [x, y] 而 `rows[y][x]`。写反了**在方形地图上不报错**——整张图只是转置了，
// 长度、字符集、坐标范围全部照旧通过，而地形从此静静地错着。所以夹具刻意做成 7x5，
// 并在这里断言两个**不对称**的位置。
//
// 同样的手法钉住了 Python 侧（tools/map_gen/mapfile.py 的坐标约定一节）。
TEST_CASE("坐标约定：pos 是 [x, y]，而 rows[y][x]", "[map]") {
    const game::MapData m = game::MapLoader::from_file(fixture_path());

    // 第 0 行是 "0123320"：x=1 是 Rock，x=2 是 Forest。
    // （x=5 那格 Forest 是为了满足校验器第 7 条，见夹具的 _note，与本条无关。）
    REQUIRE(m.terrain_at(1, 0) == game::Terrain::Rock);
    REQUIRE(m.terrain_at(2, 0) == game::Terrain::Forest);

    // 转置之后这两格会变成 (0,1) 与 (0,2) 上的东西，而那两格都是 Plain。
    REQUIRE(m.terrain_at(0, 1) == game::Terrain::Plain);
    REQUIRE(m.terrain_at(0, 2) == game::Terrain::Plain);

    // 第 2 行是 "0004400"：桥在 x=3、4。
    REQUIRE(m.terrain_at(3, 2) == game::Terrain::Bridge);
    REQUIRE(m.terrain_at(4, 2) == game::Terrain::Bridge);
    REQUIRE(m.terrain_at(3, 1) == game::Terrain::Water);

    // no_build 与 terrain 用同一套索引，所以它也要钉一下。
    // no_build 与 terrain 用同一套索引，所以它也要钉一下。
    // no_build 的第 3 行是 "0000010"，全图唯一被盖住的格子是 (5, 3)。
    REQUIRE(m.no_build_at(5, 3));
    REQUIRE_FALSE(m.no_build_at(3, 3));  // 同一行的邻格
    REQUIRE_FALSE(m.no_build_at(0, 0));
}

TEST_CASE("点位实体解析", "[map]") {
    const game::MapData m = game::MapLoader::from_file(fixture_path());

    REQUIRE(m.keep().i == 1);
    REQUIRE(m.keep().j == 1);

    REQUIRE(m.spawns()[0].corridor == game::CorridorKind::Open);
    REQUIRE(m.spawns()[1].corridor == game::CorridorKind::Defile);

    REQUIRE(m.resources()[0].type == game::ResourceType::Stone);
    REQUIRE(m.resources()[0].tier == game::ResourceTier::Inner);
    REQUIRE(m.resources()[3].tier == game::ResourceTier::Outer);

    // 城门是结构上的既定薄弱点，渲染要用不同的精灵，所以类别不能丢。
    REQUIRE(m.walls()[2].kind == game::WallKind::Gate);
    // 2.3 要求初始城圈是**残破**的，hp_frac 因此不恒为 1。
    REQUIRE(m.walls()[1].hp_frac < 1.0f);

    REQUIRE(m.wall_at(0, 4) != nullptr);
    REQUIRE(m.wall_at(3, 3) == nullptr);
}

TEST_CASE("from_string 与 from_file 走同一条路径", "[map]") {
    // from_string 存在的理由：让测试不必为每个用例造一个夹具文件，
    // 于是也就不会有人为了省事绕开 MapLoader 直接手搭 MapData。
    const game::MapData m = game::MapLoader::from_string(kMinimal);
    REQUIRE(m.width() == 3);
    REQUIRE(m.height() == 2);
    REQUIRE(m.terrain_at(2, 0) == game::Terrain::Forest);
    REQUIRE(m.terrain_at(0, 1) == game::Terrain::Water);
}

// 每条格式规则都要有「应报」那一侧的用例。
// 只测「合法地图读得进来」的话，校验分支写错了不会有任何反应。
TEST_CASE("格式不合法要抛，而不是读出一张看起来正常的图", "[map]") {
    using game::MapFormatError;
    using game::MapLoader;

    SECTION("不是 JSON") {
        REQUIRE_THROWS_AS(MapLoader::from_string("{ 这不是 json"), MapFormatError);
    }

    SECTION("format 不认识") {
        std::string s = kMinimal;
        s.replace(s.find("\"format\": 1"), 11, "\"format\": 2");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("palette 顺序不对") {
        // 顺序一变，同一份 rows 就解出另一张图，而文件看起来完全正常。
        std::string s = kMinimal;
        s.replace(s.find("\"Plain\",\"Rock\""), 14, "\"Rock\",\"Plain\"");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("行数与 size 对不上") {
        std::string s = kMinimal;
        s.replace(s.find("\"012\", \"340\""), 12, "\"012\"");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("某一行长度不对") {
        std::string s = kMinimal;
        s.replace(s.find("\"340\""), 5, "\"3400\"");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("rows 里出现调色板之外的字符") {
        std::string s = kMinimal;
        s.replace(s.find("\"012\""), 5, "\"019\"");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("坐标越界") {
        std::string s = kMinimal;
        s.replace(s.find("\"keep\": [0, 0]"), 14, "\"keep\": [9, 0]");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("layers 里有未登记的层") {
        // 拼错的层若被静默忽略，那一层就相当于全 0，而地图看起来完全正常。
        std::string s = kMinimal;
        s.replace(s.find("\"no_build\""), 10, "\"no_bulid\"");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("corridor 不是枚举里的值") {
        std::string s = kMinimal;
        s.replace(s.find("\"open\""), 6, "\"opne\"");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("hp_frac 为 0") {
        // 0 表示「这段墙已经没了」，那应当表现为这一格根本没有墙段，
        // 而不是一段血量为 0 的墙——后者会让渲染层画出一段看不见摸不着的墙。
        std::string s = kMinimal;
        s.replace(s.find("\"hp_frac\": 1.0"), 14, "\"hp_frac\": 0.0");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("resources 缺 unlock_wave") {
        // 必填——缺失与「刻意写 1」不可区分会让第 18 条校验（解禁波数序列按
        // 距离单调）在少数图上悄悄少查一条，同 obstacles 必填的理由。
        std::string s = kMinimal;
        const std::string frag = ", \"unlock_wave\": 1";
        s.replace(s.find(frag), frag.size(), "");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("resources 的 unlock_wave 小于 1") {
        // World::wave() 从 1 起，小于 1 没有意义（不存在「第 0 波之前」）。
        std::string s = kMinimal;
        s.replace(s.find("\"unlock_wave\": 1"), 16, "\"unlock_wave\": 0");
        REQUIRE_THROWS_AS(MapLoader::from_string(s), MapFormatError);
    }

    SECTION("spawns 为空") {
        // 这一条**不用字符串手术**：`"spawns": [` 之后的第一个 `],` 会落在
        // `"pos": [2, 1],` 上，改出来的是一份坏 JSON——那样它照样抛异常，
        // 但抛的原因是「JSON 坏了」而不是「spawns 为空」，等于这条校验没被测到。
        // 整份写出来，贵几行，但测的是它该测的东西。
        const char* const kNoSpawns = R"({
          "format": 1, "map_id": "t", "name": "t", "size": [3, 2],
          "layers": {
            "terrain":  { "palette": ["Plain","Rock","Forest","Water","Bridge"],
                          "rows": ["012", "340"] },
            "no_build": { "rows": ["000", "010"] }
          },
          "keep": [0, 0],
          "spawns": [],
          "resources": [],
          "initial_walls": [],
          "obstacles": []
        })";
        REQUIRE_THROWS_AS(MapLoader::from_string(kNoSpawns), MapFormatError);
    }
}

// 上面那组「应报」的用例有一个前提：kMinimal 本身是合法的。
// 若它其实不合法，那十条会因为**别的原因**通过，而校验分支一条都没被走到。
TEST_CASE("上面用来做坏的那份最小地图本身是合法的", "[map]") {
    REQUIRE_NOTHROW(game::MapLoader::from_string(kMinimal));
}

TEST_CASE("可破坏障碍从地图读进来，三种都认得", "[map]") {
    // §6 的机制那一半于 2026-08-29 通过，6.2 随之新增 `obstacles` 字段。
    // 夹具里刻意一种放一个，好让 `kObstacleTypes` 那张查表的三行都被走到——
    // 只放一种的话，另外两行拼错了也不会有人知道。
    const game::MapData m = game::MapLoader::from_file(fixture_path());
    REQUIRE(m.obstacles().size() == 3);

    std::set<rts::ObstacleType> kinds;
    for (const game::ObstacleNode& o : m.obstacles()) {
        REQUIRE(m.in_bounds(o.pos.i, o.pos.j));
        kinds.insert(o.type);
    }
    REQUIRE(kinds.size() == static_cast<std::size_t>(rts::kObstacleTypeCount));
}

TEST_CASE("obstacles 是必填的，缺了要抛", "[map]") {
    // **必填而不是「缺了当空」**，理由是那条反复出现的原则：缺失与刻意为空
    // 不可区分是个洞。一张没有这个键的图，读者分不清「这张图没有障碍」
    // 与「写图的人不知道有这个字段」，而后者会让第 19 / 22 条那类校验空过。
    //
    // 这条用例与上面那组「改一处使它变坏」同族，但它验的是**删掉一处**。
    std::string doc = kMinimal;
    const std::size_t at = doc.find("\"obstacles\"");
    REQUIRE(at != std::string::npos);
    doc.erase(at - 3);              // 连前面那个逗号一起去掉
    doc += "\n})";
    REQUIRE_THROWS_AS(game::MapLoader::from_string(doc), game::MapFormatError);
}
