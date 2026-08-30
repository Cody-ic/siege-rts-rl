// 守方单兵执行层脚本（README 第 6 项）。
//
// 锁两类东西：
//
//   * **克制二部图里写死的三条战术真的在脚本里**（`守方AI与协同演化.md` 2.5
//     的判据：图里写死的进脚本、没写的留给 RL）——拉扯、堵口不追、
//     避骑士摸攻城锤。每条都配一个「关掉参数就退化」的对照，
//     免得断言碰巧绿在别的机制上
//   * **玩家命令的执行**（World 记账、脚本走路）：编队开拔**穿城门出城**、
//     驻守走到墙边等 World 拉上墙、清野走过去撞上自动破坏、工匠自动找活。
//     出城那条是第六批城门修正的端到端验收
//
// 数值全部是测试自带的占位表，只求「几个决策拍能追上/拉开」可手算。

#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/defender_script.hpp"
#include "rts/action.hpp"
#include "rts/command.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

namespace {

rts::UnitStats& us(rts::StatsTable& t, rts::UnitType u) {
    return t.unit[static_cast<std::size_t>(u)];
}

rts::StatsTable script_stats() {
    rts::StatsTable t;
    for (rts::UnitStats& u : t.unit) {
        u.max_hp = 30;
        u.vision = 6.0f;
        u.windup_ticks = 1;
        u.cooldown_ticks = 3;
    }
    // 拉扯的可行性是速度差给的：弓手 0.30 > 追兵 0.15，弹速 0 = 瞬时（免算飞行）。
    us(t, rts::UnitType::Archer) = {20, 5, 5.0f, 0.30f, 6.0f, 2, 4, 500, 0.0f, 0.0f};
    us(t, rts::UnitType::Ghoul) = {30, 4, 1.2f, 0.15f, 4.0f, 1, 3, 2000, 0.0f, 0.0f};
    us(t, rts::UnitType::Spear) = {24, 6, 1.2f, 0.08f, 4.0f, 1, 3, 1000, 0.0f, 0.0f};
    us(t, rts::UnitType::Ranger) = {18, 6, 1.2f, 0.30f, 5.0f, 1, 2, 1000, 0.0f, 0.0f};
    us(t, rts::UnitType::Knight) = {26, 7, 1.2f, 0.40f, 5.0f, 1, 3, 500, 0.0f, 0.0f};
    us(t, rts::UnitType::Ram) = {60, 10, 1.5f, 0.10f, 3.0f, 3, 5, 4000, 1.5f, 0.0f};
    us(t, rts::UnitType::Mason) = {12, 0, 0.0f, 0.20f, 4.0f, 0, 1, 0, 0.0f, 0.0f};
    t.bld[static_cast<std::size_t>(rts::BldType::Keep)].max_hp = 200;
    t.bld[static_cast<std::size_t>(rts::BldType::Wall)].max_hp = 40;
    t.bld[static_cast<std::size_t>(rts::BldType::Gate)].max_hp = 30;
    t.obstacle[static_cast<std::size_t>(rts::ObstacleType::Stump)] = {12, 7};
    t.global.hp_permille_per_level = 100;
    t.global.dmg_permille_per_level = 100;
    t.global.mason_work_radius = 2.0f;
    return t;
}

rts::WorldInit sarena(int w = 24, int h = 5) {
    rts::WorldInit init;
    init.width = w;
    init.height = h;
    init.terrain.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                        rts::Terrain::Plain);
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.seed = 11;
    init.map_id = "script-arena";
    init.nominal_level = 1;
    init.stats = script_stats();
    return init;
}

bool has(std::uint16_t mask, rts::UnitAction a) {
    return (mask & static_cast<std::uint16_t>(1u << static_cast<unsigned>(a))) != 0;
}

// 决策拍循环：守方走脚本；攻方是最简追击者——打得着就打，否则按给定方向走。
// 每拍按当时的存活单位重发（数量会变，submit 的长度检查是硬的）。
void run(rts::World& w, game::DefenderScript& s, int ticks,
         rts::UnitAction atk_move = rts::UnitAction::Stop) {
    std::vector<rts::UnitId> ids;
    std::vector<rts::UnitAction> acts;
    for (int t = 0; t < ticks; t += rts::kDecisionPeriodMin) {
        w.enumerate_units(rts::Side::Defender, ids);
        s.decide(w.view(rts::Side::Defender), ids, acts);
        w.submit_actions(rts::Side::Defender, acts.data(), acts.size());

        w.enumerate_units(rts::Side::Attacker, ids);
        acts.clear();
        for (const rts::UnitId id : ids) {
            acts.push_back(has(w.action_mask(id), rts::UnitAction::AtkNear)
                               ? rts::UnitAction::AtkNear
                               : atk_move);
        }
        w.submit_actions(rts::Side::Attacker, acts.data(), acts.size());
        w.advance(rts::kDecisionPeriodMin);
    }
}

}  // namespace

// ——克制表三条：每条一手一个对照——

TEST_CASE("弓手拉扯：追兵永远够不着，反被一路放风筝打死", "[script]") {
    // 克制表原文「Ghoul ──► Archer 拉扯 + Tower 齐射」的前一半。
    // 威胁进 2.5 格就后撤（弓手 0.30 > 追兵 0.15），退出圈就回头放箭——
    // 「打一下退一步」由距离震荡自然给出。
    SECTION("拉扯开（占位默认参数）：弓手无伤，追兵被耗死") {
        rts::WorldInit init = sarena();
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{16.5f, 2.5f}, 1, 20, 20});
        rts::World w(std::move(init));
        const rts::UnitId a = [&] {
            std::vector<rts::UnitId> ids;
            w.enumerate_units(rts::Side::Defender, ids);
            return ids[0];
        }();
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{18.5f, 2.5f}, 1, 30, 30);

        game::DefenderScript s(game::ScriptParams{}, 3);
        run(w, s, 200, rts::UnitAction::MoveNW);   // 追兵向格西追

        REQUIRE(w.unit_hp(a) == 20);               // 一下都没挨
        REQUIRE(w.live_unit_count(rts::Side::Attacker) == 0);   // 追兵被耗死
    }
    SECTION("对照：拉扯关（kite_permille = 0）⇒ 弓手站桩挨打") {
        rts::WorldInit init = sarena();
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{16.5f, 2.5f}, 1, 20, 20});
        rts::World w(std::move(init));
        const rts::UnitId a = [&] {
            std::vector<rts::UnitId> ids;
            w.enumerate_units(rts::Side::Defender, ids);
            return ids[0];
        }();
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{18.5f, 2.5f}, 1, 30, 30);

        game::ScriptParams p;
        p.kite_permille = 0;
        game::DefenderScript s(p, 3);
        run(w, s, 200, rts::UnitAction::MoveNW);

        REQUIRE((!w.alive(a) || w.unit_hp(a) < 20));   // 真的挨了打
    }
}

TEST_CASE("枪卫堵缺口不追击：敌人在射程外就一步都不挪", "[script]") {
    // 被风筝出阵位正是 Shade 克枪卫的机制（克制表「Spear ──► Shade」），
    // 所以「不追」不是懒，是这条克制关系存在的前提。
    rts::WorldInit init = sarena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Spear, rts::Vec2{5.5f, 2.5f}, 1, 24, 24});
    rts::World w(std::move(init));
    const rts::UnitId sp = [&] {
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        return ids[0];
    }();
    w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{10.5f, 2.5f}, 1, 30, 30);

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 100, rts::UnitAction::Stop);   // 敌人站着不动，构成「诱饵」

    REQUIRE(w.unit_pos(sp).x == 5.5f);       // 一步都没挪
    REQUIRE(w.unit_pos(sp).y == 2.5f);
}

TEST_CASE("游骑：骑士靠近就脱离，没命令时主动摸攻城锤", "[script]") {
    SECTION("避骑士：距离只增不减（不与骑士对冲）") {
        rts::WorldInit init = sarena();
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ranger, rts::Vec2{8.5f, 2.5f}, 1, 18, 18});
        rts::World w(std::move(init));
        const rts::UnitId r = [&] {
            std::vector<rts::UnitId> ids;
            w.enumerate_units(rts::Side::Defender, ids);
            return ids[0];
        }();
        const rts::UnitId k =
            w.spawn_unit(rts::UnitType::Knight, rts::Vec2{10.5f, 2.5f}, 1, 26, 26);

        game::DefenderScript s(game::ScriptParams{}, 3);
        run(w, s, 40, rts::UnitAction::Stop);   // 骑士原地（贴近由初始距离 2.0 构成）

        const float dx = w.unit_pos(r).x - w.unit_pos(k).x;
        const float dy = w.unit_pos(r).y - w.unit_pos(k).y;
        REQUIRE(dx * dx + dy * dy > 3.0f * 3.0f);   // 已脱离保持距离圈
    }
    SECTION("摸攻城锤：没有别的命令就贴上去打（出城的执行手段）") {
        rts::WorldInit init = sarena();
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ranger, rts::Vec2{4.5f, 2.5f}, 1, 18, 18});
        rts::World w(std::move(init));
        const rts::UnitId ram =
            w.spawn_unit(rts::UnitType::Ram, rts::Vec2{12.5f, 2.5f}, 1, 60, 60);

        game::DefenderScript s(game::ScriptParams{}, 3);
        run(w, s, 200, rts::UnitAction::Stop);

        REQUIRE((!w.alive(ram) || w.unit_hp(ram) < 60));   // 真的打上了
    }
}

// ——命令执行：World 记账，脚本走路——

TEST_CASE("编队开拔穿城门出城：门血一点不掉（第六批的端到端验收）", "[script]") {
    // 门刻意**不在直线上**（墙线顶端）：直奔目标会怼在自家墙上（守方不打
    // 自家墙、贪心方向也全被挡），必须由 flow field 绕行穿门——这条测试
    // 锁的就是「脚本的开拔真的在用 field」。门在直线上时贪心碰巧也能过，
    // 那样的几何锁不住任何东西（破坏性验证时抓出来的）。
    rts::WorldInit init = sarena(14, 5);
    for (int j = 1; j < 5; ++j) {
        init.buildings.push_back(rts::BldInit{
            rts::BldType::Wall, rts::GridPos{6, static_cast<std::int16_t>(j)}, 40, 40});
    }
    init.buildings.push_back(
        rts::BldInit{rts::BldType::Gate, rts::GridPos{6, 0}, 30, 30});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ranger, rts::Vec2{3.5f, 2.5f}, 1, 18, 18, 1});
    rts::World w(std::move(init));
    const rts::UnitId r = [&] {
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        return ids[0];
    }();

    rts::Command c;
    c.kind = rts::CommandKind::MoveForce;
    c.side = rts::Side::Defender;
    c.force = 1;
    c.slot = rts::slot_of(rts::GridPos{10, 2}, w.width());
    w.submit(rts::Side::Defender, &c, 1);

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 160);

    // 到了墙外的集合点，而门是走过去的、不是砸开的。
    const rts::Vec2 p = w.unit_pos(r);
    const float dx = p.x - 10.5f;
    const float dy = p.y - 2.5f;
    REQUIRE(dx * dx + dy * dy <= 1.5f * 1.5f);
    std::int64_t gate_hp = -1;
    const auto view = w.view(rts::Side::Defender);
    for (std::size_t k = 0; k < view.bld_type().size(); ++k) {
        if (view.bld_alive()[k] && view.bld_type()[k] == rts::BldType::Gate) {
            gate_hp = view.bld_hp()[k];
        }
    }
    REQUIRE(gate_hp == 30);
}

TEST_CASE("驻守指令：各奔最近的指令格，两段墙都有人", "[script]") {
    // 场景刻意取**两格相隔的驻守指令**：`Garrison` 解算时 World 会把
    // `force_target_` 顺手指到（最后一条命令的）指令格，单格场景下光靠它
    // 也能走到——锁不住脚本的「走向最近指令格」。两格相隔时差异是实质的：
    // 没有这条逻辑，全编队挤向最后一格，另一格永远空着（一格一人）。
    rts::WorldInit init = sarena(14, 5);
    init.buildings.push_back(
        rts::BldInit{rts::BldType::Wall, rts::GridPos{6, 1}, 40, 40});
    init.buildings.push_back(
        rts::BldInit{rts::BldType::Wall, rts::GridPos{6, 3}, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 1.5f}, 1, 20, 20, 0});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 3.5f}, 1, 20, 20, 0});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);

    rts::Command cmds[2];
    for (int k = 0; k < 2; ++k) {
        cmds[k].kind = rts::CommandKind::Garrison;
        cmds[k].side = rts::Side::Defender;
        cmds[k].force = 0;
        cmds[k].slot = rts::slot_of(
            rts::GridPos{6, static_cast<std::int16_t>(1 + 2 * k)}, w.width());
    }
    w.submit(rts::Side::Defender, cmds, 2);

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 160);

    // 两人都在墙上，且占的是**不同**的两格——顺序无所谓，集合相等即可。
    const std::uint16_t g0 = w.unit_garrison(ids[0]);
    const std::uint16_t g1 = w.unit_garrison(ids[1]);
    const std::uint16_t s1 = rts::slot_of(rts::GridPos{6, 1}, w.width());
    const std::uint16_t s3 = rts::slot_of(rts::GridPos{6, 3}, w.width());
    REQUIRE(((g0 == s1 && g1 == s3) || (g0 == s3 && g1 == s1)));
}

TEST_CASE("清野指令：没别的事的战斗单位走过去砸，产出入账", "[script]") {
    rts::WorldInit init = sarena(14, 5);
    init.obstacles.push_back(
        rts::ObstacleInit{rts::ObstacleType::Stump, rts::GridPos{8, 2}, 12, 12});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ranger, rts::Vec2{3.5f, 2.5f}, 1, 18, 18});
    rts::World w(std::move(init));
    const std::int64_t wood0 = w.stock(rts::Resource::Wood);

    rts::Command c;
    c.kind = rts::CommandKind::Clear;
    c.side = rts::Side::Defender;
    c.slot = rts::slot_of(rts::GridPos{8, 2}, w.width());
    w.submit(rts::Side::Defender, &c, 1);

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 160);

    REQUIRE(w.live_obstacle_count() == 0);
    REQUIRE(w.stock(rts::Resource::Wood) == wood0 + 7);   // 表里 Stump 产 7 木
}

TEST_CASE("工匠自动找活：走进半径，工时才开始动", "[script]") {
    rts::WorldInit init = sarena(14, 5);
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Mason, rts::Vec2{2.5f, 2.5f}, 1, 12, 12});
    rts::World w(std::move(init));
    const rts::BldId site =
        w.place_bld(rts::BldType::Wall, rts::GridPos{8, 2}, 1, 40, /*work_left=*/60);
    const std::size_t slot = site.index();

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 120);

    REQUIRE(w.view(rts::Side::Defender).bld_work_left()[slot] < 60);   // 真开工了
}

// ——确定性——

// ——框选临时指令 + 无指令时的集结点默认值（#57 重开）——
//
// ## 破坏性验证（做过，改这份文件前请重做）
//
// | 把 defender_script.cpp 改成 | 应当红的用例 |
// |---|---|
// | `muster_fallback` 插到 `Ranger`/`Mason` 自己的兜底**之前** | 「摸攻城锤」「工匠自动找活」两条既有用例——它们的兜底会被截胡，永远走不到 |
// | `follow_orders` 的临时指令检查漏了世代比对 | 无法直接构造（依赖槎位复用的时序），改动前请至少手动推演一遍 |
// | 最后一行 `return rts::UnitAction::Stop;` 忘了改成 `return std::nullopt;` | 「已到达的编队仍应保持不动」——会被错误地送去集结点 |

TEST_CASE("没有任何指令：走向按编队错开的集结点待命，不再永远杵在原地", "[script]") {
    rts::WorldInit init = sarena(20, 20);
    init.keep = rts::GridPos{10, 10};
    init.buildings.clear();
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 2.5f}, 1, 20, 20, 0});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 3.5f}, 1, 20, 20, 1});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    const rts::UnitId a0 = ids[0];
    const rts::UnitId a1 = ids[1];

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 400);   // 无攻方、无命令：纯看它自己会不会挪窝

    const rts::Vec2 p0 = w.unit_pos(a0);
    const rts::Vec2 p1 = w.unit_pos(a1);
    // 两支编队的集结点在堡垒不同侧，所以两个终点必须不同——
    // 若两者挤到同一格，说明 muster_point_for 没有按编队区分。
    const float dx = p0.x - p1.x, dy = p0.y - p1.y;
    REQUIRE(dx * dx + dy * dy > 1.0f);
    // 都离开了出生点（原地不动是这条要防的旧行为）。
    REQUIRE((p0.x != 2.5f || p0.y != 2.5f));
    REQUIRE((p1.x != 2.5f || p1.y != 3.5f));
}

TEST_CASE("框选临时指令：只影响被点到的单位，同编队其余成员照旧听编队指令",
         "[script]") {
    rts::WorldInit init = sarena(20, 20);
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 2.5f}, 1, 20, 20, 0});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 3.5f}, 1, 20, 20, 0});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    const rts::UnitId picked = ids[0];    // 只框选这一个
    const rts::UnitId other = ids[1];

    // 编队 0 的持久指令：去 A。
    rts::Command c;
    c.kind = rts::CommandKind::MoveForce;
    c.side = rts::Side::Defender;
    c.force = 0;
    c.slot = rts::slot_of(rts::GridPos{16, 2}, w.width());
    w.submit(rts::Side::Defender, &c, 1);

    game::DefenderScript s(game::ScriptParams{}, 3);
    // 框选到的那一个改去 B（离出生点更近，好与 A 区分）。
    const rts::UnitId picked_ids[1] = {picked};
    s.issue_move_order(picked_ids, rts::GridPos{4, 8});

    // 只跑一小段，看**方向**而不是等「到没到」——到达时机对两段路径的
    // 长短很敏感，纯看进度不用猜时长：B 比 A 近，若跑到 picked 真到了 B，
    // 它会清空临时指令继续往 A 走，反而看不出「只影响 picked」这件事
    // （下一条用例才是测「到达后退回编队指令」）。
    run(w, s, 20);

    const rts::Vec2 pa = w.unit_pos(picked);
    const rts::Vec2 pb = w.unit_pos(other);
    // 被框选的那个朝 B（4, 8）走：B 的 y 比起点大得多，y 应显著增大。
    REQUIRE(pa.y > 4.0f);
    // 没被框选的那个没有跟着去 B（y 不该被拉向 8 那一侧），而是朝编队
    // 持久指令 A（16, 2）走：x 应显著增大。
    REQUIRE(pb.y < 4.0f);
    REQUIRE(pb.x > 4.0f);
}

TEST_CASE("框选临时指令到达后清空：退回编队的持久指令，不是空等一拍",
         "[script]") {
    rts::WorldInit init = sarena(20, 20);
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 2.5f}, 1, 20, 20, 0});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    const rts::UnitId a = ids[0];

    // 编队持久指令：去远处 A（18, 2）。
    rts::Command c;
    c.kind = rts::CommandKind::MoveForce;
    c.side = rts::Side::Defender;
    c.force = 0;
    c.slot = rts::slot_of(rts::GridPos{18, 2}, w.width());
    w.submit(rts::Side::Defender, &c, 1);

    game::DefenderScript s(game::ScriptParams{}, 3);
    // 临时指令：先去近处 B（4, 2），到了应当继续赶去 A，不会卡在 B。
    const rts::UnitId picked_ids[1] = {a};
    s.issue_move_order(picked_ids, rts::GridPos{4, 2});

    run(w, s, 400);   // 给足够 tick 走完 B 再走完 A

    const rts::Vec2 p = w.unit_pos(a);
    const float dx = p.x - 18.5f, dy = p.y - 2.5f;
    REQUIRE(dx * dx + dy * dy <= 1.5f * 1.5f);
}

TEST_CASE("同种子同输入 ⇒ 同一局：脚本不是不确定性的来源", "[script]") {
    const auto build = [] {
        rts::WorldInit init = sarena();
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{16.5f, 2.5f}, 1, 20, 20});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{18.5f, 2.5f}, 1, 30, 30});
        return rts::World(init);
    };
    rts::World w1 = build();
    rts::World w2 = build();
    game::DefenderScript s1(game::ScriptParams{}, 42);
    game::DefenderScript s2(game::ScriptParams{}, 42);
    run(w1, s1, 80, rts::UnitAction::MoveNW);
    run(w2, s2, 80, rts::UnitAction::MoveNW);
    REQUIRE(w1.state_hash() == w2.state_hash());
}
