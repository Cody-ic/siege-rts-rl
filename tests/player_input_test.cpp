// 玩家输入的语义层（game/player_input.hpp）：右键按格上的东西分三种命令，
// 建造虚影的合法性提示。这层不含像素，在默认构建里测（GCC 侧也验）——
// 「点墙下的是驻守不是开拔」写错了，交互层就是一个手感很怪的 bug 制造机。

#include <cstddef>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "game/player_input.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

namespace {

rts::WorldInit iarena() {
    rts::WorldInit init;
    init.width = 10;
    init.height = 6;
    init.terrain.assign(60, rts::Terrain::Plain);
    init.terrain[static_cast<std::size_t>(2) * 10 + 7] = rts::Terrain::Rock;  // (7,2)
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.obstacles.push_back(
        rts::ObstacleInit{rts::ObstacleType::Stump, rts::GridPos{5, 4}, 12, 12});
    init.seed = 3;
    init.map_id = "input-arena";
    init.nominal_level = 1;
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Keep)].max_hp = 200;
    return init;
}

}  // namespace

TEST_CASE("右键的语义：墙=驻守、障碍=清野、其余=开拔", "[input]") {
    rts::World w(iarena());
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);
    w.place_bld(rts::BldType::Gate, rts::GridPos{4, 3}, 30, 30);
    w.place_bld(rts::BldType::Tower, rts::GridPos{2, 2}, 50, 50);
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 4}, 40, 40, /*work_left=*/10);
    const rts::WorldView v = w.view(rts::Side::Defender);

    // 完工的墙与门：驻守（「墙段」的判据是 Wall‖Gate，与机制第三批同一条）。
    rts::Command c = game::command_for_click(v, 1, rts::GridPos{4, 2});
    REQUIRE(c.kind == rts::CommandKind::Garrison);
    REQUIRE(c.force == 1);
    REQUIRE(c.slot == rts::slot_of(rts::GridPos{4, 2}, 10));
    REQUIRE(game::command_for_click(v, 0, rts::GridPos{4, 3}).kind ==
            rts::CommandKind::Garrison);
    // 工地状态的墙还没有可站的墙顶：开拔过去（等着也好、护着也好，归玩家）。
    REQUIRE(game::command_for_click(v, 0, rts::GridPos{4, 4}).kind ==
            rts::CommandKind::MoveForce);
    // 塔不是墙段：开拔（走到它旁边），不是驻守。
    REQUIRE(game::command_for_click(v, 0, rts::GridPos{2, 2}).kind ==
            rts::CommandKind::MoveForce);
    // 活障碍：清野。
    REQUIRE(game::command_for_click(v, 0, rts::GridPos{5, 4}).kind ==
            rts::CommandKind::Clear);
    // 空地：开拔。
    REQUIRE(game::command_for_click(v, 0, rts::GridPos{8, 1}).kind ==
            rts::CommandKind::MoveForce);
}

TEST_CASE("命令能被 World 原样受理：语义层给的形状与校验层对得上", "[input]") {
    // 语义层若给出一条 submit 会拒的命令（越界槽位、错侧），交互层的症状是
    // 「一点就崩」。这条把三种命令各真提交一遍。
    rts::World w(iarena());
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);
    const rts::WorldView v = w.view(rts::Side::Defender);
    for (const rts::GridPos p :
         {rts::GridPos{4, 2}, rts::GridPos{5, 4}, rts::GridPos{8, 1}}) {
        const rts::Command c = game::command_for_click(v, 0, p);
        REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &c, 1));
    }
}

TEST_CASE("建造提示：地形、建筑、障碍三种占法都报不可放", "[input]") {
    rts::World w(iarena());
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);
    const rts::WorldView v = w.view(rts::Side::Defender);

    REQUIRE(game::can_place_hint(v, rts::GridPos{8, 1}));          // 空平地
    REQUIRE_FALSE(game::can_place_hint(v, rts::GridPos{7, 2}));    // 岩壁
    REQUIRE_FALSE(game::can_place_hint(v, rts::GridPos{4, 2}));    // 有墙
    REQUIRE_FALSE(game::can_place_hint(v, rts::GridPos{5, 4}));    // 有障碍
    REQUIRE_FALSE(game::can_place_hint(v, rts::GridPos{0, 0}));    // Keep 本体
    REQUIRE_FALSE(game::can_place_hint(v, rts::GridPos{-1, 0}));   // 越界给否
}
