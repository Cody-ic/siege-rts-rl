// 玩家输入的语义层（game/player_input.hpp）：右键按格上的东西分三种命令，
// 建造虚影的合法性提示。这层不含像素，在默认构建里测（GCC 侧也验）——
// 「点墙下的是驻守不是开拔」写错了，交互层就是一个手感很怪的 bug 制造机。

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

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
    const rts::BldType wall = rts::BldType::Wall;

    REQUIRE(game::can_place_hint(v, wall, rts::GridPos{8, 1}));          // 空平地
    REQUIRE_FALSE(game::can_place_hint(v, wall, rts::GridPos{7, 2}));    // 岩壁
    REQUIRE_FALSE(game::can_place_hint(v, wall, rts::GridPos{4, 2}));    // 有墙
    REQUIRE_FALSE(game::can_place_hint(v, wall, rts::GridPos{5, 4}));    // 有障碍
    REQUIRE_FALSE(game::can_place_hint(v, wall, rts::GridPos{0, 0}));    // Keep 本体
    REQUIRE_FALSE(game::can_place_hint(v, wall, rts::GridPos{-1, 0}));   // 越界给否
}

TEST_CASE("建造提示：资源点归属两个方向都要拦", "[input]") {
    // 这条钉的是玩家会撞上的那个形状：石木两种资源**只能**靠采集建筑产出，
    // 而采集建筑只能盖在对应种类的资源点上。少了这条提示，玩家会在草地上
    // 看到绿框、点出一个采石场，然后命令被**静默**拒绝——资源点在画面上
    // 没有专门标记，绿框是唯一的线索。
    rts::WorldInit init = iarena();
    init.resources.push_back(rts::ResourceSite{rts::GridPos{3, 1}, rts::Resource::Stone});
    init.resources.push_back(rts::ResourceSite{rts::GridPos{3, 3}, rts::Resource::Gold});
    rts::World w(init);
    const rts::WorldView v = w.view(rts::Side::Defender);

    // 采集建筑：必须在**对应种类**的点上。
    REQUIRE(game::can_place_hint(v, rts::BldType::Quarry, rts::GridPos{3, 1}));
    REQUIRE_FALSE(game::can_place_hint(v, rts::BldType::Quarry, rts::GridPos{3, 3}));
    REQUIRE_FALSE(game::can_place_hint(v, rts::BldType::Quarry, rts::GridPos{8, 1}));
    REQUIRE(game::can_place_hint(v, rts::BldType::Mine, rts::GridPos{3, 3}));

    // 其余建筑：不得占资源点（把点糊死是不可逆的浪费，World 按结构封死）。
    REQUIRE_FALSE(game::can_place_hint(v, rts::BldType::Wall, rts::GridPos{3, 1}));
    REQUIRE(game::can_place_hint(v, rts::BldType::Wall, rts::GridPos{8, 1}));
}

TEST_CASE("建造清单：Keep 不在列，采集三座在列", "[input]") {
    const std::vector<rts::BldType>& list = game::buildable_types();
    REQUIRE(list.size() >= 5);
    bool has_keep = false;
    int gatherers = 0;
    for (const rts::BldType b : list) {
        if (b == rts::BldType::Keep) has_keep = true;
        if (rts::is_gatherer(b)) ++gatherers;
    }
    // `Keep` 不可再建是结构（World 的 Build 解算第一行就 break）——列进去
    // 等于给玩家一个永远点不出东西的选项。
    REQUIRE_FALSE(has_keep);
    // 采集三座必须齐：石 / 木 / 金三条收入轴各一座，缺一座那种资源就永远
    // 没有流量（`Keep` 那点金币是地板、不是收入）。
    REQUIRE(gatherers == 3);
    // 清单里不能有重名项（TAB 循环会卡在同一个类型上两拍）。
    std::vector<rts::BldType> sorted(list.begin(), list.end());
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}

TEST_CASE("征兵提示：只有完工且没在练的兵营或堡垒可以点", "[input]") {
    rts::WorldInit init = iarena();
    // 数值在**建局参数**里给，不在构造之后改：数值表的指纹进状态哈希，
    // 事后改它等于让同一局有两份表（`rts/stats.hpp` 文件头）。
    init.stats.unit[static_cast<std::size_t>(rts::UnitType::Archer)].cost_gold = 5;
    init.stats.unit[static_cast<std::size_t>(rts::UnitType::Archer)].train_ticks = 50;
    rts::World w(init);
    w.place_bld(rts::BldType::Barrack, rts::GridPos{2, 1}, 50, 50);
    w.place_bld(rts::BldType::Barrack, rts::GridPos{2, 3}, 50, 50, /*work_left=*/10);
    w.place_bld(rts::BldType::Tower, rts::GridPos{6, 1}, 50, 50);
    const rts::WorldView v = w.view(rts::Side::Defender);

    REQUIRE(game::can_train_hint(v, rts::GridPos{2, 1}));          // 完工的兵营
    REQUIRE(game::can_train_hint(v, rts::GridPos{0, 0}));          // 堡垒也行（兜底）
    REQUIRE_FALSE(game::can_train_hint(v, rts::GridPos{2, 3}));    // 工地
    REQUIRE_FALSE(game::can_train_hint(v, rts::GridPos{6, 1}));    // 塔不出兵
    REQUIRE_FALSE(game::can_train_hint(v, rts::GridPos{8, 1}));    // 空地

    // 真的下一条征兵命令，然后这一格就该变成不可点（一次一名）。
    w.set_stock(rts::Resource::Gold, 100);
    const rts::Command c =
        game::train_command(rts::UnitType::Archer, 2, rts::GridPos{2, 1}, 10);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &c, 1));
    w.advance(1);
    REQUIRE_FALSE(game::can_train_hint(w.view(rts::Side::Defender), rts::GridPos{2, 1}));
    REQUIRE(w.stock(rts::Resource::Gold) == 95);
}

TEST_CASE("维修提示：只有完工、掉了血、且没在修的建筑可以点", "[input]") {
    rts::WorldInit init = iarena();
    // 维修单价给一个非零的占位值：默认表全是 0，那样「花木材」这条不成立，
    // 下面那句断言就会因为**错误的理由**通过（花了 0 木也叫没花钱）。
    init.stats.global.repair_wood_per_1000hp = 100;
    rts::World w(init);
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);        // 满血
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 3}, 10, 40);        // 残血
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 4}, 10, 40, /*work_left=*/10);
    const rts::WorldView v = w.view(rts::Side::Defender);

    REQUIRE_FALSE(game::can_repair_hint(v, rts::GridPos{4, 2}));   // 满血不用修
    REQUIRE(game::can_repair_hint(v, rts::GridPos{4, 3}));
    REQUIRE_FALSE(game::can_repair_hint(v, rts::GridPos{4, 4}));   // 工地不「修」
    REQUIRE_FALSE(game::can_repair_hint(v, rts::GridPos{8, 1}));   // 空地

    // 下一条维修命令：木材扣掉、工时排上，于是这一格立刻变成不可再点
    // （已经在修了）——「点了两下扣两次木头」正是这条 hint 要防的。
    w.set_stock(rts::Resource::Wood, 1000);
    const rts::Command c = game::repair_command(rts::GridPos{4, 3}, 10);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &c, 1));
    w.advance(1);
    REQUIRE(w.stock(rts::Resource::Wood) < 1000);
    REQUIRE_FALSE(game::can_repair_hint(w.view(rts::Side::Defender), rts::GridPos{4, 3}));
}
