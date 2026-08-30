// 机制第二批：经济闭环与命令解算（Build / Repair / Cancel / Train / MoveForce /
// Clear）。`Garrison` 仍在记账，它的用例在 world_test.cpp 那条记账测试里。
//
// 与 mechanics_test.cpp 同一条纪律：**数值全是驱动测试的占位**，试图从这里
// 反推平衡性是误读。凡是「结构 vs 数值」的分界（Keep 不可建、采集建筑必须踩点、
// 退款上界 1000）都在断言旁边写明它是哪一种。

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "rts/combat_math.hpp"
#include "rts/command.hpp"
#include "rts/replay.hpp"
#include "rts/roster.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

namespace {

// 只填本文件用到的格子；其余保持「一眼看出没标定」的默认。
rts::StatsTable econ_stats() {
    rts::StatsTable t;
    {
        rts::UnitStats& m = t.unit[static_cast<std::size_t>(rts::UnitType::Mason)];
        m.max_hp = 10;
    }
    {
        rts::UnitStats& a = t.unit[static_cast<std::size_t>(rts::UnitType::Archer)];
        a.max_hp = 20;
        a.cost_gold = 30;
        a.train_ticks = 5;
    }
    // 指定初始化器而不是按位置列一排数：字段重排时这里要么编译错、要么照旧对，
    // 不会静默串位。
    auto bld = [&t](rts::BldType b) -> rts::BldStats& {
        return t.bld[static_cast<std::size_t>(b)];
    };
    bld(rts::BldType::Keep) = {.max_hp = 100, .income_amount = 2};
    bld(rts::BldType::Tower) = {
        .max_hp = 40, .cost_stone = 25, .cost_wood = 10, .build_ticks = 4};
    bld(rts::BldType::Wall) = {
        .max_hp = 30, .cost_stone = 10, .cost_wood = 5, .build_ticks = 3};
    bld(rts::BldType::Quarry) = {.max_hp = 20,
                                 .cost_stone = 5,
                                 .cost_wood = 5,
                                 .build_ticks = 2,
                                 .income_amount = 7};
    bld(rts::BldType::Barrack) = {
        .max_hp = 50, .cost_stone = 10, .cost_wood = 10, .build_ticks = 2};
    t.obstacle[static_cast<std::size_t>(rts::ObstacleType::Stump)] = {.max_hp = 12,
                                                                      .yield_amount = 6};
    t.global.income_period_ticks = 10;
    t.global.mason_work_radius = 1.6f;
    t.global.repair_hp_per_work_tick = 5;
    t.global.repair_wood_per_1000hp = 100;   // 缺 20 血 ⇒ ceil(20×100/1000) = 2 木
    t.global.cancel_refund_permille = 600;
    return t;
}

// 8×6 全平地：Keep 在 (1,1)，石料点在 (5,2)，一株树桩在 (6,4)。
rts::WorldInit arena() {
    rts::WorldInit init;
    init.width = 8;
    init.height = 6;
    init.terrain.assign(48, rts::Terrain::Plain);
    init.keep = rts::GridPos{1, 1};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 100, 100});
    init.resources.push_back(
        rts::ResourceSite{rts::GridPos{5, 2}, rts::Resource::Stone});
    init.obstacles.push_back(
        rts::ObstacleInit{rts::ObstacleType::Stump, rts::GridPos{6, 4}, 12, 12});
    init.spawns.push_back(rts::SpawnSite{rts::GridPos{0, 5}});
    init.seed = 42;
    init.map_id = "econ-arena";
    init.nominal_level = 1;
    init.stats = econ_stats();
    return init;
}

rts::Command build_cmd(rts::BldType what, rts::GridPos p, int width) {
    rts::Command c;
    c.kind = rts::CommandKind::Build;
    c.side = rts::Side::Defender;
    c.what = static_cast<std::uint8_t>(what);
    c.slot = rts::slot_of(p, width);
    return c;
}

rts::Command slot_cmd(rts::CommandKind k, rts::GridPos p, int width) {
    rts::Command c;
    c.kind = k;
    c.side = rts::Side::Defender;
    c.slot = rts::slot_of(p, width);
    return c;
}

std::int64_t stone(const rts::World& w) { return w.stock(rts::Resource::Stone); }
std::int64_t wood(const rts::World& w) { return w.stock(rts::Resource::Wood); }
std::int64_t gold(const rts::World& w) { return w.stock(rts::Resource::Gold); }

}  // namespace

TEST_CASE("Build：扣造价落工地，工匠在场才盖，完工那一刻血量到满", "[econ]") {
    rts::World w(arena());
    w.set_stock(rts::Resource::Stone, 100);
    w.set_stock(rts::Resource::Wood, 100);

    const rts::Command c = build_cmd(rts::BldType::Tower, rts::GridPos{4, 1}, w.width());
    w.submit(rts::Side::Defender, &c, 1);
    w.advance(1);

    REQUIRE(w.live_bld_count() == 2);   // Keep + 工地
    REQUIRE(stone(w) == 75);            // 100 - 25
    REQUIRE(wood(w) == 90);             // 100 - 10

    // 没有工匠：工时不走，怎么等都盖不起来。
    w.advance(10);
    const rts::WorldView v = w.view(rts::Side::Defender);
    std::size_t site = 0;
    for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
        if (v.bld_alive()[k] && v.bld_type()[k] == rts::BldType::Tower) site = k;
    }
    REQUIRE(v.bld_built()[site] == 0);
    REQUIRE(v.bld_hp()[site] == 1);

    // 工匠到场：4 工时盖完，血量恰好到满（挨打会压低过程血量、不延长工期，
    // 那条占位决定的注释在 tick_economy）。
    w.spawn_unit(rts::UnitType::Mason, rts::center_of(rts::GridPos{4, 2}), 1, 10, 10);
    w.advance(4);
    REQUIRE(v.bld_built()[site] == 1);
    REQUIRE(v.bld_hp()[site] == 40);
}

TEST_CASE("Build 的拒绝路径一律无操作：钱、占位、资源点规则、Keep", "[econ]") {
    rts::World w(arena());
    const int before = w.live_bld_count();

    auto reject = [&w, before](rts::Command c) {
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        REQUIRE(w.live_bld_count() == before);
    };

    // 买不起（存量为零）。
    reject(build_cmd(rts::BldType::Tower, rts::GridPos{4, 1}, w.width()));

    w.set_stock(rts::Resource::Stone, 1000);
    w.set_stock(rts::Resource::Wood, 1000);

    // 格被建筑占着（Keep 那一格）/ 被障碍占着（树桩那一格）。
    reject(build_cmd(rts::BldType::Tower, rts::GridPos{1, 1}, w.width()));
    reject(build_cmd(rts::BldType::Tower, rts::GridPos{6, 4}, w.width()));
    // 采集建筑不在对应资源点上（结构：收入只来自资源点）。
    reject(build_cmd(rts::BldType::Quarry, rts::GridPos{3, 3}, w.width()));
    // 非采集建筑压资源点（结构：把资源点糊死是不可逆浪费，结构封死）。
    reject(build_cmd(rts::BldType::Tower, rts::GridPos{5, 2}, w.width()));
    // `Keep` 不可再建（结构：唯一性是「丢失即败」与兵力地板的共同前提）。
    reject(build_cmd(rts::BldType::Keep, rts::GridPos{4, 4}, w.width()));
    // 地面单位站着的格不落地基。
    w.spawn_unit(rts::UnitType::Mason, rts::center_of(rts::GridPos{4, 1}), 1, 10, 10);
    reject(build_cmd(rts::BldType::Tower, rts::GridPos{4, 1}, w.width()));

    // 全程一分钱没花。
    REQUIRE(stone(w) == 1000);
    REQUIRE(wood(w) == 1000);
}

TEST_CASE("收入：采集建筑踩点按周期产出，Keep 恒产金币地板，拆了就断", "[econ]") {
    rts::World w(arena());
    // 采石场直接以完工态摆上石料点（初始建筑那条路，不经 Build）。
    const rts::BldId quarry =
        w.place_bld(rts::BldType::Quarry, rts::GridPos{5, 2}, 20, 20);

    w.advance(10);   // 周期 10：第一次结算落在第 10 个 tick 推完时
    REQUIRE(stone(w) == 7);
    REQUIRE(gold(w) == 2);    // Keep 的兵力地板
    REQUIRE(wood(w) == 0);

    w.advance(10);
    REQUIRE(stone(w) == 14);
    REQUIRE(gold(w) == 4);

    // 「攻其必救」的经济那一半：拆掉采石场，石头断流，金币地板还在——
    // Keep 不可摧毁（丢失即败），所以只要没输这条地板一定在。
    w.destroy_bld(quarry);
    w.advance(10);
    REQUIRE(stone(w) == 14);
    REQUIRE(gold(w) == 6);
}

TEST_CASE("收入：采集建筑不踩点就一分不产（付账条件是唯一真相）", "[econ]") {
    rts::World w(arena());
    // 完工的采石场，但摆在普通平地上（初始建筑不经 Build 校验，只有付账拦得住）。
    w.place_bld(rts::BldType::Quarry, rts::GridPos{3, 3}, 20, 20);
    w.advance(20);
    REQUIRE(stone(w) == 0);
}

TEST_CASE("Repair：扣木排工时，工匠在场修满；不够木或满血都无操作", "[econ]") {
    rts::World w(arena());
    // 一段残破的墙（初始城圈是残破的，hp < max 直接摆进来）。
    const rts::BldId wall = w.place_bld(rts::BldType::Wall, rts::GridPos{2, 4}, 10, 30);
    REQUIRE(w.bld_complete(wall));   // 残破 ≠ 没盖完

    const rts::Command repair =
        slot_cmd(rts::CommandKind::Repair, rts::GridPos{2, 4}, w.width());

    // 木材不够（缺 20 血要 2 木）：无操作。
    w.set_stock(rts::Resource::Wood, 1);
    w.submit(rts::Side::Defender, &repair, 1);
    w.advance(1);
    REQUIRE(wood(w) == 1);
    REQUIRE(w.bld_hp(wall) == 10);

    // 够了：预付 2 木，排 ceil(20/5) = 4 个工时。
    w.set_stock(rts::Resource::Wood, 5);
    w.submit(rts::Side::Defender, &repair, 1);
    w.advance(1);
    REQUIRE(wood(w) == 3);

    // 没工匠不动；工匠到场 4 tick 修满。
    w.advance(6);
    REQUIRE(w.bld_hp(wall) == 10);
    w.spawn_unit(rts::UnitType::Mason, rts::center_of(rts::GridPos{3, 4}), 1, 10, 10);
    w.advance(4);
    REQUIRE(w.bld_hp(wall) == 30);

    // 满血再下 Repair：无操作（一分木不扣）。
    w.submit(rts::Side::Defender, &repair, 1);
    w.advance(1);
    REQUIRE(wood(w) == 3);
}

TEST_CASE("Cancel：撤工地按表比例退款，放弃维修不退；对完好建筑无操作", "[econ]") {
    rts::World w(arena());
    w.set_stock(rts::Resource::Stone, 100);
    w.set_stock(rts::Resource::Wood, 100);

    // 撤工地：花 25/10，退 600‰ = 15/6。工时不折算——工地暴露是设计要的风险。
    const rts::Command build =
        build_cmd(rts::BldType::Tower, rts::GridPos{4, 1}, w.width());
    const rts::Command cancel =
        slot_cmd(rts::CommandKind::Cancel, rts::GridPos{4, 1}, w.width());
    w.submit(rts::Side::Defender, &build, 1);
    w.advance(1);
    REQUIRE(w.live_bld_count() == 2);
    w.submit(rts::Side::Defender, &cancel, 1);
    w.advance(1);
    REQUIRE(w.live_bld_count() == 1);
    REQUIRE(stone(w) == 90);   // 100 - 25 + 15
    REQUIRE(wood(w) == 96);    // 100 - 10 + 6

    // 放弃维修：预付的木不退（占位决定，注释在 apply_one）。
    const rts::BldId wall = w.place_bld(rts::BldType::Wall, rts::GridPos{2, 4}, 10, 30);
    const rts::Command repair =
        slot_cmd(rts::CommandKind::Repair, rts::GridPos{2, 4}, w.width());
    const rts::Command cancel_wall =
        slot_cmd(rts::CommandKind::Cancel, rts::GridPos{2, 4}, w.width());
    w.submit(rts::Side::Defender, &repair, 1);
    w.advance(1);
    REQUIRE(wood(w) == 94);
    w.submit(rts::Side::Defender, &cancel_wall, 1);
    w.advance(1);
    REQUIRE(wood(w) == 94);
    REQUIRE(w.alive(wall));   // 完工建筑不会被 Cancel 拆掉

    // 对完好且没在修的建筑再下 Cancel：无操作。
    w.submit(rts::Side::Defender, &cancel_wall, 1);
    w.advance(1);
    REQUIRE(w.alive(wall));
}

TEST_CASE("Train：扣金倒计时，在兵营邻格出兵并编入编队；忙时无操作", "[econ]") {
    rts::World w(arena());
    w.place_bld(rts::BldType::Barrack, rts::GridPos{4, 3}, 50, 50);
    w.set_stock(rts::Resource::Gold, 100);

    rts::Command train = slot_cmd(rts::CommandKind::Train, rts::GridPos{4, 3}, w.width());
    train.what = static_cast<std::uint8_t>(rts::UnitType::Archer);
    train.force = 2;

    // 同一批里下两单：第一单入训，第二单撞上「一次一名」，只扣一份钱。
    const std::array<rts::Command, 2> both{train, train};
    w.submit(rts::Side::Defender, both.data(), both.size());
    w.advance(1);
    REQUIRE(gold(w) == 70);
    REQUIRE(w.live_unit_count() == 0);

    // 5 个 tick 的倒计时走完后出兵（提交那 tick 就开始倒数）。
    w.advance(5);
    REQUIRE(w.live_unit_count() == 1);

    const rts::WorldView v = w.view(rts::Side::Defender);
    for (std::size_t k = 0; k < v.unit_alive().size(); ++k) {
        if (!v.unit_alive()[k]) continue;
        REQUIRE(v.unit_type()[k] == rts::UnitType::Archer);
        REQUIRE(v.unit_force()[k] == 2);
        REQUIRE(v.unit_hp()[k] == 20);   // 1 级 = 基础值（等级公式照走）
        // 出兵格与兵营相邻（规范扫描顺序给出的第一个空格）。
        const rts::GridPos g = rts::grid_of(v.unit_pos()[k]);
        REQUIRE(std::abs(g.i - 4) <= 1);
        REQUIRE(std::abs(g.j - 3) <= 1);
    }

    // 金不够：无操作——没扣 30，也没多出一名兵。这 6 个 tick（第 6..11）
    // 恰好跨过一次周期结算，Keep 的金币地板 +2；第一版预期漏了它，
    // 而「补兵的钱永远会慢慢回来」正是地板的本职，所以是测试的账错了。
    w.set_stock(rts::Resource::Gold, 10);
    w.submit(rts::Side::Defender, &train, 1);
    w.advance(6);
    REQUIRE(gold(w) == 12);   // 10 + 2（地板），而不是 10 - 30
    REQUIRE(w.live_unit_count() == 1);
}

TEST_CASE("MoveForce 与 Clear 解算成世界状态，脚本执行层读得到", "[econ]") {
    rts::World w(arena());

    rts::Command mv;
    mv.kind = rts::CommandKind::MoveForce;
    mv.side = rts::Side::Defender;
    mv.force = 3;
    mv.slot = rts::slot_of(rts::GridPos{6, 1}, w.width());
    w.submit(rts::Side::Defender, &mv, 1);

    const rts::Command clear =
        slot_cmd(rts::CommandKind::Clear, rts::GridPos{6, 4}, w.width());
    w.submit(rts::Side::Defender, &clear, 1);

    // 空格上的 Clear：无操作（不置任何位）。
    const rts::Command clear_empty =
        slot_cmd(rts::CommandKind::Clear, rts::GridPos{0, 0}, w.width());
    w.submit(rts::Side::Defender, &clear_empty, 1);

    w.advance(1);
    REQUIRE(w.force_target(3) == rts::slot_of(rts::GridPos{6, 1}, w.width()));
    REQUIRE(w.force_target(4) == rts::kNoSlot);   // 没下过的编队保持「无」

    const rts::WorldView v = w.view(rts::Side::Defender);
    REQUIRE(v.obstacle_clear_ordered()[0] == 1);
}

TEST_CASE("指令状态进了 state_hash：只差一条指令的两个世界哈希必须不同", "[econ]") {
    // 喂入清单漏一条的症状是「两个不同的世界哈希相同」，而**回放测不出来**：
    // 重演会得到同样的漏。只有这种成对构造抓得到。挑 `force_target_` 与
    // `o_clear_ordered_` 做探针，因为它们是纯指令状态——除了自己，
    // 不牵动任何别的会进哈希的东西（钱、实体都不变），探针是隔离的。
    {
        rts::World a(arena());
        rts::World b(arena());
        rts::Command mv;
        mv.kind = rts::CommandKind::MoveForce;
        mv.side = rts::Side::Defender;
        mv.force = 1;
        mv.slot = 3;
        b.submit(rts::Side::Defender, &mv, 1);
        a.advance(1);
        b.advance(1);
        REQUIRE(a.state_hash() != b.state_hash());
    }
    {
        rts::World a(arena());
        rts::World b(arena());
        const rts::Command clear =
            slot_cmd(rts::CommandKind::Clear, rts::GridPos{6, 4}, a.width());
        b.submit(rts::Side::Defender, &clear, 1);
        a.advance(1);
        b.advance(1);
        REQUIRE(a.state_hash() != b.state_hash());
    }
}

TEST_CASE("含第二批命令的回放照旧逐 tick 一致", "[econ]") {
    // 经济是纯机制产物：回放只存命令流，收入、施工、出兵全部由 advance 重演。
    // 这条锁的是「第二批没有引入任何非确定性」。
    rts::WorldInit init = arena();
    rts::World w(std::move(init));
    w.set_stock(rts::Resource::Stone, 100);
    w.set_stock(rts::Resource::Wood, 100);
    w.set_stock(rts::Resource::Gold, 100);

    rts::ReplayRecorder rec(w, 1);
    const rts::Command build =
        build_cmd(rts::BldType::Wall, rts::GridPos{3, 1}, w.width());
    rec.submit(rts::Side::Defender, &build, 1);
    rec.advance(5);
    const rts::Command clear =
        slot_cmd(rts::CommandKind::Clear, rts::GridPos{6, 4}, w.width());
    rec.submit(rts::Side::Defender, &clear, 1);
    rec.advance(20);
    const rts::Replay r = rec.finish();

    rts::WorldInit init2 = arena();
    rts::World fresh(std::move(init2));
    fresh.set_stock(rts::Resource::Stone, 100);
    fresh.set_stock(rts::Resource::Wood, 100);
    fresh.set_stock(rts::Resource::Gold, 100);
    const rts::ReplayResult res = rts::replay_verify(r, fresh);
    REQUIRE(res.verdict == rts::ReplayVerdict::Match);
}
