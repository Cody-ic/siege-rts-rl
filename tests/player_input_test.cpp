// 玩家输入的语义层（game/player_input.hpp）：右键按格上的东西分三类
// （驻墙 / 清野 / 开拔——编队移除后只有清野还落成 `rts::Command`，另两种
// 是脚本侧的临时单兵指令），建造虚影的合法性提示。这层不含像素，在默认
// 构建里测（GCC 侧也验）——「点墙是驻墙不是开拔」写错了，交互层就是一个
// 手感很怪的 bug 制造机。

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "game/iso_projection.hpp"
#include "game/player_input.hpp"
#include "game/selection_cycle.hpp"
#include "game/stats_loader.hpp"
#include "rts/roster.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

TEST_CASE("Overlapping buildings cycle and reset after movement or candidate changes", "[input]") {
    game::SelectionCycle cycle;
    const rts::GridPos barrack{3,4},keep{3,3};
    const std::vector<rts::GridPos> hits{barrack,keep};
    const rts::Vec2 mouse{100,100};
    CHECK(cycle.select(hits,mouse,mouse)==barrack);
    CHECK(cycle.select(hits,mouse,mouse)==keep);
    CHECK(cycle.select(hits,mouse,mouse)==barrack);
    cycle.observe({110,100},mouse); // Moving away and back resets, even without a click.
    CHECK(cycle.select(hits,mouse,mouse)==barrack);
    CHECK(cycle.select(hits,mouse,mouse)==keep);
    cycle.observe(mouse,{120,100}); // Camera moved under stationary pointer.
    CHECK(cycle.select(hits,mouse,mouse)==barrack);
    CHECK(cycle.select({keep},mouse,mouse)==keep);
    CHECK_FALSE(cycle.select({},mouse,mouse));
    CHECK(cycle.select(hits,mouse,mouse)==barrack);
}

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

TEST_CASE("Build drags choose one axis and preserve press-to-release order", "[input]") {
    const auto cells=game::build_line({4,2},{1,4},false);
    REQUIRE(cells == std::vector<rts::GridPos>{{4,2},{3,2},{2,2},{1,2}});
    REQUIRE(game::build_line({4,2},{1,4},true) == std::vector<rts::GridPos>{{4,2},{4,3},{4,4}});
    REQUIRE(game::build_line({1,1},{3,3},false) == std::vector<rts::GridPos>{{1,1},{2,1},{3,1}});
    REQUIRE(game::build_line({1,1},{1,1},false).size() == 1);
    for (const auto p : cells) {
        const auto command=game::build_command(rts::BldType::Wall,p,10);
        REQUIRE(command.slot == rts::slot_of(p,10));
        REQUIRE(command.kind == rts::CommandKind::Build);
        REQUIRE(command.what == static_cast<std::uint8_t>(rts::BldType::Wall));
    }
}

TEST_CASE("Build preview reserves resources only for legal affordable cells", "[input]") {
    auto init=iarena();
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Wall)].cost_stone=10;
    rts::World world(init);
    world.set_stock(rts::Resource::Stone,20);
    const auto cells=game::build_line({6,2},{9,2},false); // Rock at 7,2 must not consume money.
    const auto preview=game::preview_build(world.view(rts::Side::Defender),rts::BldType::Wall,cells);
    REQUIRE(preview.size()==4);
    REQUIRE(preview[0].legal); REQUIRE(preview[0].affordable);
    REQUIRE_FALSE(preview[1].legal);
    REQUIRE(preview[2].legal); REQUIRE(preview[2].affordable);
    REQUIRE(preview[3].legal); REQUIRE_FALSE(preview[3].affordable);
    const std::vector<rts::GridPos> repeated{{6,2},{6,2},{8,2}};
    const auto unique=game::preview_build(world.view(rts::Side::Defender),rts::BldType::Wall,repeated);
    REQUIRE_FALSE(unique[1].legal);
    REQUIRE(unique[2].legal); REQUIRE(unique[2].affordable);
}

TEST_CASE("右键的分类：完工墙/门=驻墙、障碍=清野、其余=开拔", "[input]") {
    rts::World w(iarena());
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);
    w.place_bld(rts::BldType::Gate, rts::GridPos{4, 3}, 30, 30);
    w.place_bld(rts::BldType::Tower, rts::GridPos{2, 2}, 50, 50);
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 4}, 40, 40, /*work_left=*/10);
    const rts::WorldView v = w.view(rts::Side::Defender);

    // 完工的墙与门：驻墙（「墙段」的判据是 Wall‖Gate，与机制第三批同一条）。
    REQUIRE(game::classify_click(v, rts::GridPos{4, 2}) == game::ClickTarget::Wall);
    REQUIRE(game::classify_click(v, rts::GridPos{4, 3}) == game::ClickTarget::Wall);
    // 工地状态的墙还没有可站的墙顶：开拔过去（等着也好、护着也好，归玩家）。
    REQUIRE(game::classify_click(v, rts::GridPos{4, 4}) == game::ClickTarget::Ground);
    // 塔不是墙段：开拔（走到它旁边），不是驻墙。
    REQUIRE(game::classify_click(v, rts::GridPos{2, 2}) == game::ClickTarget::Ground);
    // 活障碍：清野。
    REQUIRE(game::classify_click(v, rts::GridPos{5, 4}) == game::ClickTarget::Obstacle);
    // 空地：开拔。
    REQUIRE(game::classify_click(v, rts::GridPos{8, 1}) == game::ClickTarget::Ground);
}

TEST_CASE("clear_command 能被 World 原样受理：语义层给的形状与校验层对得上",
          "[input]") {
    // 右键语义里只有清野还走命令通道（驻墙与开拔都落成脚本的临时指令，
    // 不再是 `rts::Command`）。语义层若给出一条 submit 会拒的命令，
    // 交互层的症状是「一点就崩」，所以真提交一遍。
    rts::World w(iarena());
    const rts::Command c = game::clear_command(rts::GridPos{5, 4}, 10);
    REQUIRE(c.kind == rts::CommandKind::Clear);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &c, 1));
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
        game::train_command(rts::UnitType::Archer, /*level=*/1, rts::GridPos{2, 1}, 10);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &c, 1));
    w.advance(1);
    REQUIRE_FALSE(game::can_train_hint(w.view(rts::Side::Defender), rts::GridPos{2, 1}));
    REQUIRE(w.stock(rts::Resource::Gold) == 95);
}

TEST_CASE("征兵提示：人口满时 train_pop_full 报满，与逐格提示正交", "[input]") {
    rts::World w(iarena());
    // iarena 的 Keep 1 级 ⇒ cap = 8 + 2×1 = 10（`WorldInit` 默认值）。
    REQUIRE(w.view(rts::Side::Defender).defender_pop_cap() == 10);
    REQUIRE_FALSE(game::train_pop_full(w.view(rts::Side::Defender)));

    // 塞满 10 个守方单位（`spawn_unit` 是调试通道，不过人口检查——
    // 检查只在 `Train` 解算里）。
    for (int i = 0; i < 10; ++i) {
        w.spawn_unit(rts::UnitType::Archer,
                     rts::Vec2{2.5f + static_cast<float>(i) * 0.5f, 2.5f}, 1, 20, 20);
    }
    const rts::WorldView v = w.view(rts::Side::Defender);
    REQUIRE(v.defender_pop() == 10);
    REQUIRE(game::train_pop_full(v));
    // 「人口满」是全局状态，与「这格能不能弹菜单」正交：兵营照样点得开
    // （菜单里选项灰掉、标签写明「人口满」，见 render 的 Train 弹窗——同
    // 升级行印「先升堡垒」那条先例）。
    w.place_bld(rts::BldType::Barrack, rts::GridPos{2, 1}, 50, 50);
    REQUIRE(game::can_train_hint(w.view(rts::Side::Defender), rts::GridPos{2, 1}));
}

// ——框选（#57 重开）——

TEST_CASE("框选：矩形只框到落在其中的己方单位", "[input]") {
    rts::WorldInit init = iarena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 2.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Spear, rts::Vec2{2.5f, 2.6f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ranger, rts::Vec2{8.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    REQUIRE(ids.size() == 3);
    const rts::UnitId a = ids[0];
    const rts::UnitId b = ids[1];
    const rts::UnitId c = ids[2];
    const rts::WorldView v = w.view(rts::Side::Defender);
    const game::IsoProjection proj(256);

    // 矩形只套住 a、b 那一小片（两者格坐标相邻，屏幕像素上离得很近）。
    const rts::Vec2 pa = proj.world_to_screen(rts::Vec2{2.5f, 2.5f});
    const rts::Vec2 pb = proj.world_to_screen(rts::Vec2{2.5f, 2.6f});
    const float x0 = std::min(pa.x, pb.x) - 5.0f;
    const float y0 = std::min(pa.y, pb.y) - 5.0f;
    const float x1 = std::max(pa.x, pb.x) + 5.0f;
    const float y1 = std::max(pa.y, pb.y) + 5.0f;
    const std::vector<rts::UnitId> got =
        game::units_in_rect(v, ids, proj, game::Rect{x0, y0, x1 - x0, y1 - y0});

    REQUIRE(got.size() == 2);
    const bool has_a = std::find(got.begin(), got.end(), a) != got.end();
    const bool has_b = std::find(got.begin(), got.end(), b) != got.end();
    const bool has_c = std::find(got.begin(), got.end(), c) != got.end();
    REQUIRE(has_a);
    REQUIRE(has_b);
    REQUIRE_FALSE(has_c);
}

TEST_CASE("框选：矩形的两个角谁大谁小不影响结果（鼠标可能往任何方向拖）",
         "[input]") {
    rts::WorldInit init = iarena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 2.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    REQUIRE(ids.size() == 1);
    const rts::UnitId a = ids[0];
    const rts::WorldView v = w.view(rts::Side::Defender);
    const game::IsoProjection proj(256);
    const rts::Vec2 p = proj.world_to_screen(rts::Vec2{2.5f, 2.5f});

    // 反着给：x/width、y/height 都是负的（对角从右下往左上拖）。
    const game::Rect flipped{p.x + 10.0f, p.y + 10.0f, -20.0f, -20.0f};
    const std::vector<rts::UnitId> got = game::units_in_rect(v, ids, proj, flipped);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0] == a);
}

// 一次试玩报出来的 bug（与「造价提示」那条同批，但是另一层）：`render/`
// 里点一格该弹哪种菜单，曾按「维修 > 征兵」互斥地判——受损的兵营/堡垒
// 因此永远只弹维修菜单，练兵入口直接消失，严重影响正常游玩。
//
// 真正的根因在 `render/src/main.cpp` 的弹窗分支（这层没有默认构建测试，
// 需要目测/试玩验证），但它的前提条件在这一层：`can_train_hint` 与
// `can_repair_hint` 本就是两个**完全独立**的判断（前者只查兵种与是否在
// 练，后者只查血量与是否在修），一座掉血又没在练的兵营/堡垒会让两者
// **同时**为真。这条把这个前提钉死——`render/` 那边把它们当成互斥来判
// 才是 bug，不是这一层的两个函数有问题（它们本来就没查过对方）。
TEST_CASE("提示的独立性：掉血又没在练的兵营，征兵与维修提示同时为真（试玩报的 bug）",
         "[input]") {
    rts::WorldInit init = iarena();
    init.stats.global.repair_wood_per_1000hp = 100;
    rts::World w(init);
    w.place_bld(rts::BldType::Barrack, rts::GridPos{2, 1}, 30, 50);   // 残血、完工、没在练
    const rts::WorldView v = w.view(rts::Side::Defender);

    REQUIRE(game::can_train_hint(v, rts::GridPos{2, 1}));
    REQUIRE(game::can_repair_hint(v, rts::GridPos{2, 1}));
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

    // 没有缺口（满血）与「格子上根本没建筑」都该是 0，但两者的成因不同——
    // `repair_wood_cost` 对两者都返回 0，`can_afford_repair` 不能把后一种
    // 也顺着 0 判成「买得起」（那等于把一个无效目标判成合法）。
    REQUIRE(game::repair_wood_cost(v, rts::GridPos{4, 2}) == 0);
    REQUIRE(game::repair_wood_cost(v, rts::GridPos{8, 1}) == 0);
    REQUIRE_FALSE(game::can_afford_repair(v, rts::GridPos{8, 1}));

    // 下一条维修命令：木材扣掉、工时排上，于是这一格立刻变成不可再点
    // （已经在修了）——「点了两下扣两次木头」正是这条 hint 要防的。
    w.set_stock(rts::Resource::Wood, 1000);
    const rts::Command c = game::repair_command(rts::GridPos{4, 3}, 10);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &c, 1));
    w.advance(1);
    REQUIRE(w.stock(rts::Resource::Wood) < 1000);
    REQUIRE_FALSE(game::can_repair_hint(w.view(rts::Side::Defender), rts::GridPos{4, 3}));
}

TEST_CASE("拆除提示：工地只能全额取消，完工建筑只能八成拆除，堡垒不可拆", "[input]") {
    rts::World w(iarena());
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);
    w.place_bld(rts::BldType::Tower, rts::GridPos{4, 3}, 1, 40, /*work_left=*/10);
    const rts::WorldView v = w.view(rts::Side::Defender);

    REQUIRE(game::can_demolish_hint(v, rts::GridPos{4, 2}));
    REQUIRE_FALSE(game::can_cancel_build_hint(v, rts::GridPos{4, 2}));
    REQUIRE(game::can_cancel_build_hint(v, rts::GridPos{4, 3}));
    REQUIRE_FALSE(game::can_demolish_hint(v, rts::GridPos{4, 3}));
    REQUIRE_FALSE(game::can_demolish_hint(v, w.keep_pos()));
    REQUIRE_FALSE(game::can_cancel_build_hint(v, rts::GridPos{8, 1}));

    const rts::Command cancel = game::cancel_build_command(rts::GridPos{4, 3}, 10);
    REQUIRE(cancel.kind == rts::CommandKind::Cancel);
    REQUIRE(cancel.slot == rts::slot_of(rts::GridPos{4, 3}, 10));
    const rts::Command demolish = game::demolish_command(rts::GridPos{4, 2}, 10);
    REQUIRE(demolish.kind == rts::CommandKind::Demolish);
    REQUIRE(demolish.slot == rts::slot_of(rts::GridPos{4, 2}, 10));
}

// ——升级——
//
// `upgrade_block` 比一个 bool 多担一件事：弹窗要把「为什么灰着」印出来。
// 五档里 `LevelCap` 那一档**在默认表上就是开局的常态**——
// `building_level_cap_divisor` 默认 1、堡垒 1 级 ⇒ 上限 1 级，而新建筑就是
// 1 级。所以这条用例顺带钉住一件很容易被当成 bug 修掉的事：
// **开局一座建筑都升不动是设计，先升堡垒**（占位表里 divisor = 2，比这里
// 的默认值更紧）。
TEST_CASE("升级提示：四档理由各一例，Keep 不受上限约束", "[input]") {
    rts::World w(iarena());
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);   // 完工、满血
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 4}, 40, 40, /*work_left=*/10);
    const rts::WorldView v = w.view(rts::Side::Defender);

    REQUIRE(game::upgrade_block(v, rts::GridPos{8, 1}) ==
            game::UpgradeBlock::NoBuilding);   // 空地
    REQUIRE(game::upgrade_block(v, rts::GridPos{4, 4}) ==
            game::UpgradeBlock::Unbuilt);      // 工地往前盖，不谈升级
    REQUIRE(v.building_level_cap() == 1);
    REQUIRE(game::upgrade_block(v, rts::GridPos{4, 2}) ==
            game::UpgradeBlock::LevelCap);     // 1 级建筑顶着 1 级上限
    // 堡垒自己不受这条上限约束——「堡垒等级本身不设上限」（CLAUDE.md）。
    // 若这一条反了，玩家就再也抬不高上限，整条轴当场死锁。
    REQUIRE(game::upgrade_block(v, w.keep_pos()) == game::UpgradeBlock::None);

    // `can_upgrade_hint` 是 `== None` 的别名，不是第二份判定（两处分叉
    // 正是「绿框骗人」这类 bug 的成因）。
    REQUIRE_FALSE(game::can_upgrade_hint(v, rts::GridPos{4, 2}));
    REQUIRE(game::can_upgrade_hint(v, w.keep_pos()));

    // 弹窗要印 `Lv1 → 2`，所以等级也得能查；空地是 0 而不是 1。
    REQUIRE(game::bld_level_at(v, rts::GridPos{4, 2}) == 1);
    REQUIRE(game::bld_level_at(v, rts::GridPos{8, 1}) == 0);
}

TEST_CASE("升级提示：升堡垒抬高上限；在途升级与在途维修都归 Busy", "[input]") {
    rts::WorldInit init = iarena();
    // 给非零工期与维修单价：默认全 0 会让「在途」这个状态根本不存在
    // （提交即当场完工），下面两条 Busy 断言就会因为**错误的理由**通过。
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Wall)].upgrade_ticks = 20;
    init.stats.global.repair_wood_per_1000hp = 100;
    rts::World w(init);
    w.set_stock(rts::Resource::Wood, 1000);
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);   // 满血
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 3}, 10, 40);   // 残血

    const rts::GridPos wall{4, 2};
    REQUIRE(game::upgrade_block(w.view(rts::Side::Defender), wall) ==
            game::UpgradeBlock::LevelCap);
    // 升一级堡垒（默认表里造价与工期都是 0 ⇒ 当场完工），上限从 1 到 2。
    const rts::Command up_keep = game::upgrade_command(w.keep_pos(), 10);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &up_keep, 1));
    w.advance(1);
    REQUIRE(w.view(rts::Side::Defender).building_level_cap() == 2);
    REQUIRE(game::can_upgrade_hint(w.view(rts::Side::Defender), wall));

    // 在途升级 ⇒ Busy。工期 20 而 `iarena()` 里没有工匠，所以它不会自己
    // 推完（工匠在场才推进，同施工与维修）。
    const rts::Command up_wall = game::upgrade_command(wall, 10);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &up_wall, 1));
    w.advance(1);
    REQUIRE(game::upgrade_block(w.view(rts::Side::Defender), wall) ==
            game::UpgradeBlock::Busy);

    // 在途维修归同一档：同一时刻只能有一件工程在推进（机制侧的互斥）。
    const rts::Command rep = game::repair_command(rts::GridPos{4, 3}, 10);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &rep, 1));
    w.advance(1);
    REQUIRE(game::upgrade_block(w.view(rts::Side::Defender), rts::GridPos{4, 3}) ==
            game::UpgradeBlock::Busy);
}

TEST_CASE("升级造价：石与木都要够（同「绿框骗人」那一类）", "[input]") {
    rts::WorldInit init = iarena();
    rts::BldStats& ws = init.stats.bld[static_cast<std::size_t>(rts::BldType::Wall)];
    // Fortifications use their own materials curve: 40% of scale for 1 -> 2.
    // The UI quote and actual debit must still agree, regardless of unit growth.
    ws.upgrade_cost_stone = 30;
    ws.upgrade_cost_wood = 10;
    init.stats.global.fortification_hp_permille_per_level = 100;
    rts::World w(init);
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 2}, 40, 40);
    // 先把上限抬起来，否则下面测到的是 LevelCap 而不是造价。
    const rts::Command up_keep = game::upgrade_command(w.keep_pos(), 10);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &up_keep, 1));
    w.advance(1);

    const rts::GridPos wall{4, 2};
    const rts::WorldView v0 = w.view(rts::Side::Defender);
    REQUIRE(game::upgrade_cost_stone(v0, wall) == 12);
    REQUIRE(game::upgrade_cost_wood(v0, wall) == 4);
    // **UI 报价 = 世界扣款，同一个公式的同一次调用。** 这条不是重复：
    // `game::upgrade_cost_*` 曾经直接读表里那个常数，改成按等级缩放之后
    // 照旧读表就会让弹窗报一个与实际扣款不同的价钱——而那种分歧不会让
    // 任何别的测试变红。
    REQUIRE(game::upgrade_cost_stone(v0, wall) ==
            w.bld_upgrade_cost_stone(rts::BldType::Wall, 1));
    REQUIRE(game::upgrade_cost_wood(v0, wall) ==
            w.bld_upgrade_cost_wood(rts::BldType::Wall, 1));
    // 空地：两个造价都是 0，但那不等于「买得起」——同 `can_afford_repair`
    // 那条，不能顺着 0 把一个无效目标判成合法。
    REQUIRE(game::upgrade_cost_stone(v0, rts::GridPos{8, 1}) == 0);
    REQUIRE_FALSE(game::can_afford_upgrade(v0, rts::GridPos{8, 1}));

    SECTION("石够木不够：结构合法但买不起") {
        w.set_stock(rts::Resource::Stone, 100);
        w.set_stock(rts::Resource::Wood, 3);
        const rts::WorldView v = w.view(rts::Side::Defender);
        REQUIRE(game::can_upgrade_hint(v, wall));            // 结构那一半过
        REQUIRE_FALSE(game::can_afford_upgrade(v, wall));    // 造价那一半不过
    }
    SECTION("木够石不够：另一种资源同样要拦") {
        w.set_stock(rts::Resource::Stone, 10);
        w.set_stock(rts::Resource::Wood, 100);
        REQUIRE_FALSE(game::can_afford_upgrade(w.view(rts::Side::Defender), wall));
    }
    SECTION("两样都够：命令被受理，等级真的涨、两种资源都真的扣") {
        w.set_stock(rts::Resource::Stone, 100);
        w.set_stock(rts::Resource::Wood, 100);
        REQUIRE(game::can_afford_upgrade(w.view(rts::Side::Defender), wall));
        const rts::Command c = game::upgrade_command(wall, 10);
        REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &c, 1));
        w.advance(1);   // Wall 的 upgrade_ticks 是默认 0 ⇒ 当场完工
        REQUIRE(game::bld_level_at(w.view(rts::Side::Defender), wall) == 2);
        REQUIRE(w.stock(rts::Resource::Stone) == 88);
        REQUIRE(w.stock(rts::Resource::Wood) == 96);
    }
}

// 一次试玩报出来的 bug：招兵 / 建筑 / 维修的弹窗只查了位置合法性
// （`can_place_hint` / `can_train_hint` / `can_repair_hint`），没查资源够
// 不够——资源不足时弹窗仍然亮绿框，点了却被 `World` **静默**拒绝
// （买不起时解算是 `break`，什么都不说），玩家只看到「点了没反应」。
//
// 这条把新的 `can_afford_*` 与「真提交一条命令」对照检查：hint 说不能，
// 提交后资源纹丝不动（真被拒了，不是碰巧）；hint 说能，提交后资源确实
// 按公式扣掉（真的生效了，不是碰巧变绿）。三种资源各占一条决策轴
// （石/木 → 建造，金 → 征兵，木 → 维修），所以三个都要查，不能只查一个
// 就当全查过了。
TEST_CASE("造价提示：资源不够时红框，够了才绿（试玩报的 bug）", "[input]") {
    rts::WorldInit init = iarena();
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Wall)].cost_stone = 50;
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Wall)].cost_wood = 20;
    init.stats.unit[static_cast<std::size_t>(rts::UnitType::Archer)].cost_gold = 60;
    init.stats.global.repair_wood_per_1000hp = 100;
    rts::World w(init);
    w.place_bld(rts::BldType::Barrack, rts::GridPos{2, 1}, 50, 50);
    w.place_bld(rts::BldType::Wall, rts::GridPos{4, 3}, 10, 40);   // 残血，缺口 30

    // ——建造：位置合法，但石木都不够——
    {
        const rts::WorldView v = w.view(rts::Side::Defender);
        REQUIRE(game::can_place_hint(v, rts::BldType::Wall, rts::GridPos{8, 1}));
        REQUIRE_FALSE(game::can_afford_build(v, rts::BldType::Wall));   // 0 < 50
        const rts::Command c = game::build_command(rts::BldType::Wall, rts::GridPos{8, 1}, 10);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        REQUIRE(w.stock(rts::Resource::Stone) == 0);   // 真被拒了：一分没扣
    }
    w.set_stock(rts::Resource::Stone, 100);
    w.set_stock(rts::Resource::Wood, 100);
    {
        const rts::WorldView v = w.view(rts::Side::Defender);
        REQUIRE(game::can_afford_build(v, rts::BldType::Wall));
        const rts::Command c = game::build_command(rts::BldType::Wall, rts::GridPos{8, 1}, 10);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        REQUIRE(w.stock(rts::Resource::Stone) == 50);   // 真生效了：扣了造价
    }

    // ——征兵：兵营完工可点，但金不够——
    w.set_stock(rts::Resource::Gold, 10);
    {
        const rts::WorldView v = w.view(rts::Side::Defender);
        REQUIRE(game::can_train_hint(v, rts::GridPos{2, 1}));
        REQUIRE_FALSE(game::can_afford_train(v, rts::UnitType::Archer, 1));   // 10 < 60
        const rts::Command c =
            game::train_command(rts::UnitType::Archer, /*level=*/1,
                               rts::GridPos{2, 1}, 10);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        REQUIRE(w.stock(rts::Resource::Gold) == 10);   // 真被拒了
    }
    w.set_stock(rts::Resource::Gold, 100);
    {
        const rts::WorldView v = w.view(rts::Side::Defender);
        REQUIRE(game::can_afford_train(v, rts::UnitType::Archer, 1));
        const rts::Command c =
            game::train_command(rts::UnitType::Archer, /*level=*/1,
                               rts::GridPos{2, 1}, 10);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        REQUIRE(w.stock(rts::Resource::Gold) == 40);   // 真生效了
    }

    // ——维修：缺口 30，价 = ceil(30×100÷1000) = 3，先给不够的——
    w.set_stock(rts::Resource::Wood, 2);
    {
        const rts::WorldView v = w.view(rts::Side::Defender);
        REQUIRE(game::can_repair_hint(v, rts::GridPos{4, 3}));
        // 弹窗把这个数印在「维修」后面（试玩报出来的 bug：原来只有两个字，
        // 看不出要花多少木材）——钉住具体数值，不止钉「买不买得起」。
        REQUIRE(game::repair_wood_cost(v, rts::GridPos{4, 3}) == 3);
        REQUIRE_FALSE(game::can_afford_repair(v, rts::GridPos{4, 3}));   // 2 < 3
        const rts::Command c = game::repair_command(rts::GridPos{4, 3}, 10);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        REQUIRE(w.stock(rts::Resource::Wood) == 2);   // 真被拒了
    }
    w.set_stock(rts::Resource::Wood, 10);
    {
        const rts::WorldView v = w.view(rts::Side::Defender);
        REQUIRE(game::can_afford_repair(v, rts::GridPos{4, 3}));
        const rts::Command c = game::repair_command(rts::GridPos{4, 3}, 10);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        REQUIRE(w.stock(rts::Resource::Wood) == 7);   // 真生效了：扣了 3
    }
}

// ——侦查面板：编成读数与克制提示——

// 克制提示表必须**与机制一致**，而不只是「填了字」。
//
// 这张表是 `CLAUDE.md`「克制二部图」的玩家侧读数，用途是兑现「克制必须在 UI
// 中完全透明」。它**不是**伤害倍率表（倍率仍只从三条正交轴的机制来），但其中
// 几条是**机制决定的、不是选择**——那几条必须能与数值表/花名册交叉核对，
// 否则面板会理直气壮地教玩家一件错事，而画面上完全看不出来。
TEST_CASE("克制提示：与机制交叉核对，且攻方无孤立节点", "[input]") {
    const rts::StatsTable stats = game::StatsLoader::from_file(
        std::string(GAME_DATA_DIR) + "/stats_placeholder.json");

    SECTION("攻方每一种都至少有一个克制者（CLAUDE.md：无孤立节点）") {
        for (int i = 0; i < rts::kUnitTypeCount; ++i) {
            const rts::UnitType t = rts::unit_at(i);
            if (rts::side_of(t) != rts::Side::Attacker) continue;
            const game::CounterHint c = game::counters_of(t);
            CAPTURE(rts::ident_of(t));
            REQUIRE((!c.units.empty() || !c.blds.empty()));
        }
    }

    SECTION("守方五种不出现在表里（它们不是「来袭」）") {
        for (int i = 0; i < rts::kUnitTypeCount; ++i) {
            const rts::UnitType t = rts::unit_at(i);
            if (rts::side_of(t) != rts::Side::Defender) continue;
            const game::CounterHint c = game::counters_of(t);
            CAPTURE(rts::ident_of(t));
            REQUIRE(c.units.empty());
            REQUIRE(c.blds.empty());
        }
    }

    SECTION("列出的克制者必须全是守方的东西") {
        for (int i = 0; i < rts::kUnitTypeCount; ++i) {
            const game::CounterHint c = game::counters_of(rts::unit_at(i));
            for (const rts::UnitType u : c.units) {
                CAPTURE(rts::ident_of(u));
                REQUIRE(rts::side_of(u) == rts::Side::Defender);
            }
        }
    }

    SECTION("「Spear 克 Knight」是机制决定的：反冲锋倍率必须真的 > 1") {
        const game::CounterHint c = game::counters_of(rts::UnitType::Knight);
        REQUIRE(c.units.size() == 1);
        REQUIRE(c.units[0] == rts::UnitType::Spear);
        // 倍率 ≤ 1000‰ 时枪阵根本不克骑兵，那时这条提示就是在骗玩家。
        // （`数值设计与成本产出矩阵.md` §3 记过：占位 1800 曾经不够，现为 3200。）
        REQUIRE(stats.global.anti_charge_permille > 1000);
    }

    SECTION("「只有 Flak 与墙上的 Archer 能对付 Phoenix」要求它真是空中单位") {
        REQUIRE(rts::is_aerial(rts::UnitType::Phoenix));
        const game::CounterHint c = game::counters_of(rts::UnitType::Phoenix);
        // 建筑那一侧只能是 Flak：AA 只做单体狙击型、且只能对空（结构）。
        REQUIRE(c.blds.size() == 1);
        REQUIRE(c.blds[0] == rts::BldType::Flak);
        // 单位那一侧是弓手（要登墙才够得着），且它必须是远程——近战打不到空军。
        REQUIRE(c.units.size() == 1);
        REQUIRE(c.units[0] == rts::UnitType::Archer);
        REQUIRE(stats.of(rts::UnitType::Archer).range > 1.5f);
    }

    SECTION("「只有 Flak 与墙上的 Archer 能对付 Wraith」要求它真是空中单位") {
        // 2026-09-03 起 `Wraith` 会飞：地面机动单位再也够不着它，
        // 克制提示与 `Phoenix` 同一组（位置性防空）。
        REQUIRE(rts::is_aerial(rts::UnitType::Wraith));
        const game::CounterHint c = game::counters_of(rts::UnitType::Wraith);
        // 建筑那一侧只能是 Flak：AA 只做单体狙击型、且只能对空（结构）。
        REQUIRE(c.blds.size() == 1);
        REQUIRE(c.blds[0] == rts::BldType::Flak);
        // 单位那一侧是弓手（要登墙才够得着），且它必须是远程——近战打不到空军。
        REQUIRE(c.units.size() == 1);
        REQUIRE(c.units[0] == rts::UnitType::Archer);
        REQUIRE(stats.of(rts::UnitType::Archer).range > 1.5f);
    }
}

TEST_CASE("窥使警报的判定：只报看得见的窥使，与侦查面板同一判据", "[input]") {
    rts::WorldInit init = iarena();
    // 一只窥使 + 一只步兵，分两格放，好让「看见步兵」与「看见窥使」可分别构造。
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Wraith, rts::Vec2{3.5f, 2.5f}, 1, 10, 10});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 10, 10});
    rts::World w(std::move(init));

    // 全图未探索：没有警报——「敌人还没来」与「什么都没侦查到」在玩家侧是
    // 同一个观感（同 `draw_intel_panel` 那条「看不见就整块不画」）。
    REQUIRE_FALSE(game::enemy_wraith_sighted(w.view(rts::Side::Defender)));

    // 看见的是步兵那格：有情报、但没有窥使警报。警报只在窥使**本人**入镜时
    // 响，否则它与侦查面板就没有分工了（面板管「看见了什么」，警报管「现在
    // 是能反制的窗口」）。
    w.fog_mut(rts::Side::Defender).mark_visible(8, 4, 0);
    REQUIRE_FALSE(game::enemy_wraith_sighted(w.view(rts::Side::Defender)));

    // 窥使入镜：响；且它必须同时出现在侦查面板里——两处一个判据，报出画面
    // 上看不见的单位就等于把「需侦查」偷偷挪回「免费」那一列。
    w.fog_mut(rts::Side::Defender).mark_visible(3, 2, 0);
    const rts::WorldView v = w.view(rts::Side::Defender);
    REQUIRE(game::enemy_wraith_sighted(v));
    const std::vector<game::SightedType> seen = game::sighted_composition(v);
    REQUIRE(seen.size() == 2);
    REQUIRE(seen[0].type == rts::UnitType::Ghoul);   // 面板按花名册顺序输出
    REQUIRE(seen[1].type == rts::UnitType::Wraith);
}

TEST_CASE("交战判定：攻击四位任一亮起即算接战，够不着时不算", "[input]") {
    // 放在第 3 行：第 2 行有块 Rock（iarena），别让地形走进射程判定里。
    const auto arena_with = [](float ghoul_x) {
        rts::WorldInit init = iarena();
        init.stats.unit[static_cast<std::size_t>(rts::UnitType::Archer)].range = 4.0f;
        init.stats.unit[static_cast<std::size_t>(rts::UnitType::Archer)].damage = 5;
        init.stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)].range = 1.5f;
        init.stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)].damage = 5;
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 3.5f}, 1, 20, 20});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{ghoul_x, 3.5f}, 1, 20, 20});
        return init;
    };

    // 相距 6 格：谁都够不着谁（弓手射程 4）——这是行军，不是交战。
    rts::World far(arena_with(8.5f));
    far.advance(1);
    REQUIRE_FALSE(game::combat_engaged(far));

    // 步兵压到 2 格：弓手的 AtkNear/AtkWeak 亮（`Ghoul` 射程 1.5 够不着弓手
    // ——单侧亮也算，「接战」不需要双方互殴）。
    rts::World near(arena_with(4.5f));
    near.advance(1);
    REQUIRE(game::combat_engaged(near));
}

TEST_CASE("Wall runs cross gates but never turn around corners or along branches", "[input][wallrun]") {
    rts::World w(iarena());
    for (int x = 1; x <= 6; ++x)
        w.place_bld(x == 3 ? rts::BldType::Gate : rts::BldType::Wall,
                    {static_cast<std::int16_t>(x),1},100,100);
    for (int y = 2; y <= 4; ++y) {
        w.place_bld(rts::BldType::Wall,{1,static_cast<std::int16_t>(y)},100,100);
        w.place_bld(rts::BldType::Wall,{4,static_cast<std::int16_t>(y)},100,100);
    }
    const auto view = w.view(rts::Side::Defender);
    const auto before = w.state_hash();
    const std::vector<rts::GridPos> horizontal{{1,1},{2,1},{3,1},{4,1},{5,1},{6,1}};
    CHECK(game::wall_run(view,{3,1},game::WallAxis::I) == horizontal); // Gate can select a face.
    CHECK(game::wall_run(view,{1,1},game::WallAxis::I) == horizontal);
    CHECK(game::wall_run(view,{1,1},game::WallAxis::J) ==
          std::vector<rts::GridPos>{{1,1},{1,2},{1,3},{1,4}});
    CHECK(game::wall_run(view,{4,1},game::WallAxis::I) == horizontal); // T junction.
    CHECK(game::wall_run(view,{4,1},game::WallAxis::J) ==
          std::vector<rts::GridPos>{{4,1},{4,2},{4,3},{4,4}});
    CHECK(w.state_hash() == before);
    w.destroy_bld(view.bld_at({3,1}));
    CHECK(game::wall_run(view,{1,1},game::WallAxis::I) ==
          std::vector<rts::GridPos>{{1,1},{2,1}});
    CHECK(game::wall_run(view,{4,1},game::WallAxis::I) ==
          std::vector<rts::GridPos>{{4,1},{5,1},{6,1}});
    CHECK(game::wall_run(view,{3,1},game::WallAxis::I).empty());
}

TEST_CASE("Wall runs use live sites, stop at map edges and keep fence upgrades separate", "[input][wallrun]") {
    rts::World w(iarena());
    w.place_bld(rts::BldType::Wall,{0,1},100,100);
    w.place_bld(rts::BldType::Wall,{1,1},10,100,20);
    w.place_bld(rts::BldType::Wall,{2,1},100,100);
    w.place_bld(rts::BldType::Fence,{3,1},100,100);
    w.place_bld(rts::BldType::Fence,{4,1},100,100);
    w.place_bld(rts::BldType::Tower,{5,1},100,100);
    w.place_bld(rts::BldType::Wall,{6,1},100,100);
    w.place_bld(rts::BldType::Wall,{9,4},100,100);
    w.place_bld(rts::BldType::Wall,{9,5},100,100);
    const auto view = w.view(rts::Side::Defender);
    CHECK(game::wall_run(view,{0,1},game::WallAxis::I) ==
          std::vector<rts::GridPos>{{0,1},{1,1},{2,1}});
    CHECK(game::wall_run(view,{4,1},game::WallAxis::I) ==
          std::vector<rts::GridPos>{{3,1},{4,1}});
    CHECK(game::wall_run(view,{6,1},game::WallAxis::I) == std::vector<rts::GridPos>{{6,1}});
    CHECK(game::wall_run(view,{9,5},game::WallAxis::J) == std::vector<rts::GridPos>{{9,4},{9,5}});
    for (const auto cell : {rts::GridPos{-1,1},{10,1},{0,6},{0,0},{5,1},{7,1}})
        CHECK(game::wall_run(view,cell,game::WallAxis::I).empty());
}

TEST_CASE("Building box modes separate walls and budgeted upgrades retain construction", "[input]") {
    auto init=iarena();
    auto& ts=init.stats.bld[static_cast<std::size_t>(rts::BldType::Tower)];
    ts.upgrade_cost_stone=30;ts.upgrade_cost_wood=10;ts.upgrade_ticks=20;
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Barrack)]=ts;
    rts::World w(init);
    const auto keep_up=game::upgrade_command(w.keep_pos(),w.width());
    w.submit(rts::Side::Defender,&keep_up,1);w.advance(1);
    const auto tower=w.place_bld(rts::BldType::Tower,{2,1},1000,1000);
    const auto wall=w.place_bld(rts::BldType::Wall,{3,1},1000,1000);
    const auto barrack=w.place_bld(rts::BldType::Barrack,{4,1},1000,1000);
    REQUIRE(tower.valid());REQUIRE(wall.valid());REQUIRE(barrack.valid());
    const auto view=w.view(rts::Side::Defender);game::IsoProjection proj(64);
    const auto walls=game::buildings_in_rect(view,proj,{-1000,-1000,2000,2000},true);
    const auto buildings=game::buildings_in_rect(view,proj,{-1000,-1000,2000,2000},false);
    REQUIRE(walls.size()==1);REQUIRE(walls[0]==rts::GridPos{3,1});REQUIRE(buildings.size()==3);
    const auto stone=game::upgrade_cost_stone(view,{2,1}),wood=game::upgrade_cost_wood(view,{2,1});
    w.set_stock(rts::Resource::Stone,stone);w.set_stock(rts::Resource::Wood,wood);
    const std::vector<rts::GridPos> targets{{4,1},{2,1},{2,1}};
    const auto batch=game::plan_upgrades(view,targets);
    REQUIRE(batch.commands.size()==1);REQUIRE(batch.commands[0].slot==rts::slot_of({2,1},w.width()));
    REQUIRE(batch.stone==stone);REQUIRE(batch.wood==wood);
    w.submit(rts::Side::Defender,batch.commands.data(),batch.commands.size());w.advance(1);
    REQUIRE(view.bld_level()[tower.index()]==1); // Scheduling never grants an instant level.
    REQUIRE(view.bld_upgrade_left()[tower.index()]>0);
    REQUIRE(view.bld_upgrade_left()[barrack.index()]==0);
    REQUIRE(game::plan_upgrades(view,targets).commands.empty());
}
