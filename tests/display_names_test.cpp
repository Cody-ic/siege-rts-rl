#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "game/display_names.hpp"
#include "game/map_data.hpp"
#include "game/map_loader.hpp"
#include "rts/roster.hpp"

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

// 把一串 UTF-8 拆成**一个个字符**。
//
// 只需要一条规则：续字节的高两位是 `10`。对合法 UTF-8 足够，而这里的输入
// 全部来自源码里的字面量与 `snprintf` 的数字，不会有非法序列。
//
// 手写而不用 raylib 的 `LoadCodepoints`：这个测试跑在**默认构建**里，
// 那里根本没有 raylib——而它能跑在默认构建里正是把这些逻辑放进 `game/` 的理由。
std::vector<std::string> chars_of(std::string_view s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t n = 1;
        while (i + n < s.size() &&
               (static_cast<unsigned char>(s[i + n]) & 0xC0) == 0x80) {
            ++n;
        }
        out.emplace_back(s.substr(i, n));
        i += n;
    }
    return out;
}

bool contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("每个枚举值都有非空且互异的中文名", "[names]") {
    // 完备性主要**靠编译器**（display_names.cpp 里的 switch 都没有 default，
    // 漏一个值就是 C4062 / -Wswitch，而两套工具链都开着警告即错误）。
    // 这条测试兜的是另一半：有人加了 `default:` 之后，漏掉的值会静默返回空串。
    const auto check = [](const std::vector<std::string_view>& names) {
        std::set<std::string_view> seen;
        for (std::string_view n : names) {
            REQUIRE_FALSE(n.empty());
            // 互异：两个地形叫同一个名字，玩家就没法区分它们，
            // 而这是纯粹的展示层 bug——代码里两者一直是不同的枚举值，全绿。
            REQUIRE(seen.insert(n).second);
        }
    };

    SECTION("Terrain") {
        std::vector<std::string_view> v;
        for (int i = 0; i < game::kTerrainCount; ++i) {
            v.push_back(game::display_name(static_cast<game::Terrain>(i)));
        }
        check(v);
        REQUIRE(game::display_name(game::Terrain::Bridge) == "桥");
    }
    SECTION("ResourceType") {
        std::vector<std::string_view> v;
        for (int i = 0; i < game::kResourceTypeCount; ++i) {
            v.push_back(game::display_name(static_cast<game::ResourceType>(i)));
        }
        check(v);
    }
    SECTION("ResourceTier") {
        std::vector<std::string_view> v;
        for (int i = 0; i < game::kResourceTierCount; ++i) {
            v.push_back(game::display_name(static_cast<game::ResourceTier>(i)));
        }
        check(v);
    }
    // 2026-08-31：CorridorKind 已随「走廊」概念一起删除，SECTION 同删。
    SECTION("WallKind") {
        std::vector<std::string_view> v;
        for (int i = 0; i < game::kWallKindCount; ++i) {
            v.push_back(game::display_name(static_cast<game::WallKind>(i)));
        }
        check(v);
    }
    SECTION("UnitType") {
        std::vector<std::string_view> v;
        for (int i = 0; i < rts::kUnitTypeCount; ++i) {
            v.push_back(game::display_name(rts::unit_at(i)));
        }
        check(v);
        // 抽查两个：一个守方风味名、一个攻方器械描述名。
        // 「器物用描述名、阵营单位用风味名」是命名纪律里的一条，
        // 而它只在这一层看得见——枚举标识符两类都是英文单词。
        REQUIRE(game::display_name(rts::UnitType::Archer) == "戍卫弓手");
        REQUIRE(game::display_name(rts::UnitType::Ram) == "攻城锤");
    }
    SECTION("BldType") {
        std::vector<std::string_view> v;
        for (int i = 0; i < rts::kBldTypeCount; ++i) {
            v.push_back(game::display_name(rts::bld_at(i)));
        }
        check(v);
    }
    SECTION("ObstacleType") {
        std::vector<std::string_view> v;
        for (int i = 0; i < rts::kObstacleTypeCount; ++i) {
            v.push_back(game::display_name(rts::obstacle_at(i)));
        }
        check(v);
    }
}

TEST_CASE("城墙与城门在两个枚举里叫同一个名字", "[names]") {
    // `game::WallKind` 是 `rts::BldType` 在「地图文件里会出现的那两种」上的
    // 一个刻意的限制（于是构造不出 `WallSegment{Tower}`），代价是同一个东西
    // 有两个枚举、各带一份中文。
    //
    // 两份不一致的话，同一段墙在光标信息条与建造菜单里叫两个名字——
    // 而两边的代码都没错，编译器与上面那条互异性检查都不会说什么
    // （它们各自在自己的枚举内互异）。这是**跨枚举**的一致性，只能单独查。
    REQUIRE(game::display_name(game::WallKind::Wall) ==
            game::display_name(rts::BldType::Wall));
    REQUIRE(game::display_name(game::WallKind::Gate) ==
            game::display_name(rts::BldType::Gate));
}

TEST_CASE("花名册的展示名全部进了字体码点清单", "[names]") {
    // `all_display_strings()` 是前端推导字体码点集合的**唯一**输入。
    // 加了 display_name 重载却忘了在那个函数里遍历它，后果是那批汉字没进码点集合
    // ——而字体里明明有，于是渲成图集里的第一个字形（「镇野箭楼」→「平平平平」）。
    //
    // 这条查的正是那一步：25 个实体名，一个都不许漏。
    std::set<std::string_view> registered(game::all_display_strings().begin(),
                                          game::all_display_strings().end());
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        const std::string_view n = game::display_name(rts::unit_at(i));
        INFO("单位 " << rts::ident_of(rts::unit_at(i)));
        REQUIRE(registered.count(n) == 1);
    }
    for (int i = 0; i < rts::kBldTypeCount; ++i) {
        const std::string_view n = game::display_name(rts::bld_at(i));
        INFO("建筑 " << rts::ident_of(rts::bld_at(i)));
        REQUIRE(registered.count(n) == 1);
    }
    for (int i = 0; i < rts::kObstacleTypeCount; ++i) {
        const std::string_view n = game::display_name(rts::obstacle_at(i));
        INFO("障碍 " << rts::ident_of(rts::obstacle_at(i)));
        REQUIRE(registered.count(n) == 1);
    }
}

TEST_CASE("describe_cell 说出这一格上真正有的东西", "[names]") {
    const game::MapData map = fixture();

    SECTION("界外给一句明确的话，不是空串") {
        // 空串在画面上与「这一格什么都没有」无法区分，而两者的处置完全不同。
        REQUIRE(game::describe_cell(map, at(7, 0)) == "光标不在地图内");
        REQUIRE(game::describe_cell(map, at(0, 5)) == "光标不在地图内");
        REQUIRE(game::describe_cell(map, at(-1, 0)) == "光标不在地图内");
    }

    SECTION("坐标按 [x, y] 印出来") {
        // 夹具是 7×5 非方形，所以 (6, 0) 合法而 (0, 6) 不合法——
        // 把 x/y 印反的话这两条会一起变。
        REQUIRE(contains(game::describe_cell(map, at(6, 0)), "(6, 0)"));
        REQUIRE(game::describe_cell(map, at(0, 6)) == "光标不在地图内");
    }

    SECTION("地形") {
        REQUIRE(contains(game::describe_cell(map, at(1, 0)), "岩壁"));
        REQUIRE(contains(game::describe_cell(map, at(2, 0)), "密林"));
        REQUIRE(contains(game::describe_cell(map, at(3, 0)), "水域"));
        REQUIRE(contains(game::describe_cell(map, at(3, 2)), "桥"));
    }

    SECTION("残血墙段带百分比，满血的不带") {
        const std::string full = game::describe_cell(map, at(0, 4));    // hp_frac 1.0
        REQUIRE(contains(full, "城墙"));
        REQUIRE_FALSE(contains(full, "残血"));

        const std::string hurt = game::describe_cell(map, at(1, 4));    // hp_frac 0.45
        REQUIRE(contains(hurt, "城墙"));
        REQUIRE(contains(hurt, "残血 45%"));

        REQUIRE(contains(game::describe_cell(map, at(2, 4)), "城门"));
    }

    SECTION("堡垒、禁建、资源点、集结点") {
        REQUIRE(contains(game::describe_cell(map, at(1, 1)), "领主堡垒"));
        REQUIRE(contains(game::describe_cell(map, at(5, 3)), "禁建"));

        REQUIRE(contains(game::describe_cell(map, at(1, 3)), "石材(城内)"));
        // 城外那个木材点在 (5,2)：它的位置受校验器第 7 条约束（森林带要 4 连通
        // 通到地图边界），不能随手挪，见夹具的 _note。
        REQUIRE(contains(game::describe_cell(map, at(5, 2)), "木材(城外)"));

        const std::string s0 = game::describe_cell(map, at(0, 0));
        REQUIRE(contains(s0, "集结点 0"));
        // 2026-08-31：`corridor` 字段已废除，集结点不再带走廊性质标签，
        // 原来的「开阔平原走廊」「隘口走廊」两条断言随之删除。
        REQUIRE_FALSE(contains(s0, "走廊"));
        const std::string s1 = game::describe_cell(map, at(6, 4));
        REQUIRE(contains(s1, "集结点 1"));
        REQUIRE_FALSE(contains(s1, "走廊"));
    }

    SECTION("普通空地只说地形，不堆废话") {
        // 这一条防的是「所有项都恒显示」——那样信息条会长到刷满屏幕，
        // 而真正有东西的那一格反而看不出特别。
        //
        // (5, 4) 是夹具里干净的一格：`rows[4] = "0003300"` 故 x=5 是平地，
        // 且它不在墙段、资源点、集结点、禁建的任何一张表上。
        // （原先写的 (4, 4) 其实是水域——这条断言当场就红了，正是它该干的事。）
        const std::string plain = game::describe_cell(map, at(5, 4));
        REQUIRE(contains(plain, "平地"));
        REQUIRE_FALSE(contains(plain, "城墙"));
        REQUIRE_FALSE(contains(plain, "集结点"));
        REQUIRE_FALSE(contains(plain, "禁建"));
    }
}

// **本文件最要紧的一条。** 它把「字体码点集合必须机械推导」变成会红的测试。
//
// 背景（实测，见 render/text.hpp）：字体里**有**、但没进码点集合的字符，
// 会被渲成 `?`，或（集合不含 ASCII 时）渲成图集里的第一个字形——
// 实测「镇野箭楼」渲成「平平平平」，四个看着完全合理的汉字。
// 所以「加了句文案忘了登记」是一个肉眼都不一定看得出来的错。
//
// 这条测试跑在默认构建里、不需要 raylib、不需要字体文件，
// 因为它查的是纯粹的字符集合关系。
TEST_CASE("describe_cell 用到的每个字符都已登记", "[names]") {
    std::set<std::string> allowed;

    // 前端无条件载入全部可打印 ASCII（数字、括号、逗号、百分号都来自
    // 运行时拼出来的串，没法从任何清单推导）。这里照同一条规则放行。
    for (int c = 0x20; c <= 0x7E; ++c) {
        allowed.insert(std::string(1, static_cast<char>(c)));
    }
    for (std::string_view s : game::all_display_strings()) {
        for (const std::string& ch : chars_of(s)) allowed.insert(ch);
    }

    const game::MapData map = fixture();

    // 扫**每一格**，外加一圈界外格（好把「光标不在地图内」也覆盖到）。
    // 逐格扫而不是挑几格：文案里的分支（残血、禁建、资源、集结点）分布在不同格上，
    // 挑样必然漏掉某一支，而漏掉的那一支正是新加的那一支。
    for (int y = -1; y <= map.height(); ++y) {
        for (int x = -1; x <= map.width(); ++x) {
            const std::string text = game::describe_cell(map, at(x, y));
            for (const std::string& ch : chars_of(text)) {
                // 失败时把整句一起打出来，否则只知道「有个字没登记」、
                // 不知道是哪句话里的。
                INFO("未登记的字符 [" << ch << "]，整句是：" << text);
                REQUIRE(allowed.count(ch) == 1);
            }
        }
    }
}
