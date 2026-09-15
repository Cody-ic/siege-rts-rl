// 守方单兵执行层脚本（README 第 6 项）。
//
// 锁两类东西：
//
//   * **克制二部图里写死的三条战术真的在脚本里**（`守方AI与协同演化.md` 2.5
//     的判据：图里写死的进脚本、没写的留给 RL）——拉扯、堵口不追、
//     避骑士摸攻城锤。每条都配一个「关掉参数就退化」的对照，
//     免得断言碰巧绿在别的机制上
//   * **自主默认与手动临时指令**（2026-09 编队移除后）：弓手没仗打自动找
//     最近的空墙段登墙（意愿经 `submit_garrison_wishes` 进 World）、框选
//     开拔**穿城门出城**（到达即失效、回归自主）、清野走过去撞上自动破坏、
//     工匠自动找活、闲人回堡垒周围的驻防环。出城那条是第六批城门修正的
//     端到端验收
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
// 登墙意愿与动作同拍提交（脚本的第二份输出，v4 起是独立的输入通道）。
void run(rts::World& w, game::DefenderScript& s, int ticks,
         rts::UnitAction atk_move = rts::UnitAction::Stop) {
    std::vector<rts::UnitId> ids;
    std::vector<rts::UnitAction> acts;
    std::vector<std::uint16_t> wishes;
    for (int t = 0; t < ticks; t += rts::kDecisionPeriodMin) {
        w.enumerate_units(rts::Side::Defender, ids);
        s.decide(w.view(rts::Side::Defender), ids, acts, wishes);
        w.submit_actions(rts::Side::Defender, acts.data(), acts.size());
        w.submit_garrison_wishes(rts::Side::Defender, wishes.data(), wishes.size());

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

TEST_CASE("Forced work reaches danger and expires with its job or identity", "[script]") {
    auto init = sarena(24, 9);
    init.stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)].damage = 0;
    rts::World w(init);
    const auto worker = w.spawn_unit(rts::UnitType::Mason, {2.5f, 4.5f}, 1, 10000, 10000);
    const auto job = w.place_bld(rts::BldType::Tower, {12, 4}, 100, 100, 300);
    w.spawn_unit(rts::UnitType::Ghoul, {14.5f, 4.5f}, 1, 10000, 10000);
    game::DefenderScript script({}, 7);
    const rts::UnitId ids[] = {worker};
    SECTION("Ordinary move remains safe; forced work actually completes the job") {
        script.issue_move_order(ids, {12, 4});
        run(w, script, 100);
        REQUIRE(w.unit_pos(worker).x < 10.5f);
        REQUIRE(script.issue_forced_work(w.view(rts::Side::Defender), ids, {12, 4}));
        run(w, script, 100);
        REQUIRE(w.unit_pos(worker).x >= 10.5f);
        REQUIRE(script.forced_work_active(w.view(rts::Side::Defender), worker));
        run(w, script, 400);
        REQUIRE_FALSE(script.forced_work_active(w.view(rts::Side::Defender), worker));
        REQUIRE(w.view(rts::Side::Defender).bld_work_left()[job.index()] == 0);
        REQUIRE(w.unit_pos(worker).x < 10.5f);
    }
    SECTION("Reused worker slot does not inherit forced work") {
        REQUIRE(script.issue_forced_work(w.view(rts::Side::Defender), ids, {12, 4}));
        w.kill_unit(worker);
        const auto replacement = w.spawn_unit(rts::UnitType::Mason, {2.5f, 4.5f}, 1, 10000, 10000);
        REQUIRE(replacement.index() == worker.index());
        REQUIRE_FALSE(script.forced_work_active(w.view(rts::Side::Defender), replacement));
        run(w, script, 100);
        REQUIRE(w.unit_pos(replacement).x < 10.5f);
    }
    SECTION("Destroyed building is not replaced by a new job in the same slot") {
        REQUIRE(script.issue_forced_work(w.view(rts::Side::Defender), ids, {12, 4}));
        w.destroy_bld(job);
        const auto replacement = w.place_bld(rts::BldType::Tower, {12, 4}, 100, 100, 300);
        REQUIRE(replacement.index() == job.index());
        REQUIRE_FALSE(script.forced_work_active(w.view(rts::Side::Defender), worker));
        run(w, script, 100);
        REQUIRE(w.unit_pos(worker).x < 10.5f);
    }
}

TEST_CASE("Worker builds outside during assault when route and site are safe", "[script]") {
    auto init=sarena(24,13);
    rts::World w(init);
    // A city ring with an east gate, and a job well outside the ring.
    for(int x=2;x<=8;++x) for(int y=2;y<=10;++y) {
        if(x!=2 && x!=8 && y!=2 && y!=10) continue;
        w.place_bld(x==8 && y==6?rts::BldType::Gate:rts::BldType::Wall,
                    {static_cast<std::int16_t>(x),static_cast<std::int16_t>(y)},40,40);
    }
    const auto worker=w.spawn_unit(rts::UnitType::Mason,{5.5f,6.5f},1,12,12);
    const auto job=w.place_bld(rts::BldType::Tower,{14,6},100,100,300);
    w.begin_assault();
    game::DefenderScript script({},7);
    run(w,script,160);
    CHECK(w.unit_pos(worker).x>8.5f);
    CHECK(w.view(rts::Side::Defender).bld_work_left()[job.index()]<300);
}

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

TEST_CASE("枪卫堵缺口不追击：圈外敌人在射程外就绝不朝它挪一步", "[script]") {
    // 被风筝出阵位正是 Shade 克枪卫的机制（克制表「Spear ──► Shade」），
    // 所以「不追圈外敌人」不是懒，是这条克制关系存在的前提。
    // （2026-09-01 起枪卫对**进堡垒出击圈**的敌人会主动迎击——那是另一条
    // 用例的事；本条把敌人刻意摆在出击圈外：距堡垒 8.25 > 占位 8.0，且
    // 看得见（视野 4）但打不着（射程 1.2）的距离 3 上。）
    //
    // 编队移除后「原地杵着」不再是可断言行：没仗打时枪卫会回堡垒周围的
    // 驻防环（自主默认）。所以这条锁的是**距离只增不减**——它往哪儿走都行，
    // 就是不许朝敌人靠近。
    rts::WorldInit init = sarena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Spear, rts::Vec2{5.5f, 2.5f}, 1, 24, 24});
    rts::World w(std::move(init));
    const rts::UnitId sp = [&] {
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        return ids[0];
    }();
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{8.5f, 2.5f}, 1, 30, 30);

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 100, rts::UnitAction::Stop);   // 敌人站着不动，构成「诱饵」

    const float dx = w.unit_pos(sp).x - w.unit_pos(g).x;
    const float dy = w.unit_pos(sp).y - w.unit_pos(g).y;
    REQUIRE(dx * dx + dy * dy >= 3.0f * 3.0f);   // 一步都没有靠近
}

TEST_CASE("枪卫守家迎击：敌人进堡垒圈就主动出击，出圈不追", "[script]") {
    // 试玩反馈「敌人打进来了枪卫也不出击」。出击拴在堡垒半径上：进
    // `spear_engage_cells` 圈 ⇒ 迎击离堡垒最近的来敌；圈外 ⇒ 驻防环
    // 站桩（上一条用例守的那条风筝防线不变）。
    auto arena_20 = [] {
        rts::WorldInit init = sarena(20, 20);
        init.keep = rts::GridPos{10, 10};
        init.buildings.clear();
        init.buildings.push_back(
            rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Spear, rts::Vec2{12.5f, 12.5f}, 1, 24, 24});
        return init;
    };
    SECTION("进圈（距堡垒 3 < 8）：迎击并击杀来敌") {
        rts::World w(arena_20());
        const rts::UnitId g =
            w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{13.5f, 10.5f}, 1, 30, 30);

        game::DefenderScript s(game::ScriptParams{}, 3);
        run(w, s, 200, rts::UnitAction::Stop);

        REQUIRE(!w.alive(g));   // 被迎击的枪卫打死
    }
    SECTION("对照：迎击关（spear_engage_cells = 0）⇒ 敌人毫发无伤") {
        rts::World w(arena_20());
        const rts::UnitId g =
            w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{13.5f, 10.5f}, 1, 30, 30);

        game::ScriptParams p;
        p.spear_engage_cells = 0.0f;
        game::DefenderScript s(p, 3);
        run(w, s, 200, rts::UnitAction::Stop);

        REQUIRE(w.alive(g));
        REQUIRE(w.unit_hp(g) == 30);   // 枪卫驻防环站桩，没上来打
    }
}

TEST_CASE("游骑：骑士靠近就脱离，没命令时拦截城内攻城锤", "[script]") {
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
    SECTION("拦截攻城锤：城内目标主动迎击") {
        rts::WorldInit init = sarena();
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ranger, rts::Vec2{4.5f, 2.5f}, 1, 18, 18});
        rts::World w(std::move(init));
        const rts::UnitId ram =
            w.spawn_unit(rts::UnitType::Ram, rts::Vec2{7.5f, 2.5f}, 1, 60, 60);

        game::DefenderScript s(game::ScriptParams{}, 3);
        run(w, s, 200, rts::UnitAction::Stop);

        REQUIRE((!w.alive(ram) || w.unit_hp(ram) < 60));   // 真的打上了
    }
}

// ——自主默认与手动临时指令——

TEST_CASE("框选开拔穿城门出城：门血一点不掉（第六批的端到端验收）", "[script]") {
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
        rts::UnitInit{rts::UnitType::Ranger, rts::Vec2{3.5f, 2.5f}, 1, 18, 18});
    rts::World w(std::move(init));
    const rts::UnitId r = [&] {
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        return ids[0];
    }();

    game::DefenderScript s(game::ScriptParams{}, 3);
    // 编队移除后没有「调兵」命令，框选 + 右键落成脚本的临时开拔指令。
    const rts::UnitId picked[1] = {r};
    s.issue_move_order(picked, rts::GridPos{10, 2});

    // 指令「到达即失效」：到了之后它会回归自主（回驻防环），所以不能像旧
    // 编队时代那样跑满固定 tick 再验收终点——逐拍跑、到了就停。
    bool arrived = false;
    for (int t = 0; t < 300 && !arrived; t += rts::kDecisionPeriodMin) {
        std::vector<rts::UnitId> ids;
        std::vector<rts::UnitAction> acts;
        std::vector<std::uint16_t> wishes;
        w.enumerate_units(rts::Side::Defender, ids);
        s.decide(w.view(rts::Side::Defender), ids, acts, wishes);
        w.submit_actions(rts::Side::Defender, acts.data(), acts.size());
        w.submit_garrison_wishes(rts::Side::Defender, wishes.data(), wishes.size());
        w.advance(rts::kDecisionPeriodMin);
        const rts::Vec2 p = w.unit_pos(r);
        const float dx = p.x - 10.5f;
        const float dy = p.y - 2.5f;
        arrived = dx * dx + dy * dy <= 1.5f * 1.5f;
    }
    REQUIRE(arrived);   // 到了墙外的集合点
    // 而门是走过去的、不是砸开的。
    std::int64_t gate_hp = -1;
    const auto view = w.view(rts::Side::Defender);
    for (std::size_t k = 0; k < view.bld_type().size(); ++k) {
        if (view.bld_alive()[k] && view.bld_type()[k] == rts::BldType::Gate) {
            gate_hp = view.bld_hp()[k];
        }
    }
    REQUIRE(gate_hp == 30);
}

TEST_CASE("弓手自主驻墙：各奔最近的空墙段，两段墙都有人（不挤同一段）", "[script]") {
    // 编队移除后没有驻守指令：弓手「手上没仗打就找最近的空墙段登墙」是
    // 自主默认（`find_wall_post` + 登墙意愿）。场景刻意取**两格相隔的墙**：
    // 脚本那张拍级认领表就是为了防「两个弓手同时看上同一段墙」——没有它，
    // 两人挤同一格（一格一人，后到者永远轮不上），另一格永远空着。
    rts::WorldInit init = sarena(14, 5);
    init.buildings.push_back(
        rts::BldInit{rts::BldType::Wall, rts::GridPos{6, 1}, 40, 40});
    init.buildings.push_back(
        rts::BldInit{rts::BldType::Wall, rts::GridPos{6, 3}, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 1.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 3.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);

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

TEST_CASE("工匠自动认领堡垒升级并走到现场完工", "[script]") {
    rts::WorldInit init = sarena(18, 9);
    init.keep = rts::GridPos{9, 4};
    init.buildings[0].pos = init.keep;
    init.stats.bld[static_cast<std::size_t>(rts::BldType::Keep)].upgrade_ticks = 60;
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Mason, rts::Vec2{9.5f, 1.5f}, 1, 12, 12});
    rts::World w(std::move(init));
    w.set_stock(rts::Resource::Stone, 10000);
    w.set_stock(rts::Resource::Wood, 10000);
    rts::Command upgrade{};
    upgrade.kind = rts::CommandKind::Upgrade;
    upgrade.slot = rts::slot_of(w.keep_pos(), w.width());
    w.submit(rts::Side::Defender, &upgrade, 1);
    w.advance(1);
    const auto before = w.view(rts::Side::Defender);
    REQUIRE(before.bld_work_left()[0] == 0); // 升级使用独立工时，不能被漏掉。
    REQUIRE(before.bld_upgrade_left()[0] == 60);
    REQUIRE(before.bld_level()[0] == 1);

    game::DefenderScript script(game::ScriptParams{}, 3);
    run(w, script, 240);
    const auto after = w.view(rts::Side::Defender);
    REQUIRE(after.bld_upgrade_left()[0] == 0);
    REQUIRE(after.bld_level()[0] == 2);
}

TEST_CASE("工匠任务认领：两处工地各去一人，不挤同一处", "[script]") {
    // 试玩反馈「多个工匠常一起执行同一个任务」。两名工匠**同位**出发、两处
    // 工地等距：各算各的最近则两人挤同一处；认领表（`bld_claimed_`）强制
    // 后决的工匠去另一处。多人同任务的加速归机制层（economy_test 那条）。
    rts::WorldInit init = sarena(14, 5);
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Mason, rts::Vec2{2.5f, 2.5f}, 1, 12, 12});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Mason, rts::Vec2{2.5f, 2.5f}, 1, 12, 12});
    rts::World w(std::move(init));
    const rts::BldId s1 =
        w.place_bld(rts::BldType::Wall, rts::GridPos{8, 1}, 1, 40, /*work_left=*/60);
    const rts::BldId s3 =
        w.place_bld(rts::BldType::Wall, rts::GridPos{8, 3}, 1, 40, /*work_left=*/60);

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 120);

    const rts::WorldView v = w.view(rts::Side::Defender);
    REQUIRE(v.bld_work_left()[s1.index()] < 60);   // 两处都真开工了——
    REQUIRE(v.bld_work_left()[s3.index()] < 60);   // 挤同一处的话这行必红
}

// ——框选临时指令 + 无指令时的驻防环默认（编队移除后的新形态）——
//
// 旧编队时代的破坏性验证表（`muster_fallback` / `follow_orders` 那几行）
// 随编队系统一起作废。新形态下的等价破坏**值得重做一轮**再动这份文件：
//
// | 把 defender_script.cpp 改成 | 应当红的用例 |
// |---|---|
// | `hold_position` 兜底插到 `Ranger`/`Mason` 自己的兜底**之前** | 「摸攻城锤」「工匠自动找活」——兜底被截胡，永远走不到 |
// | 临时指令的世代比对删掉 | 无法直接构造（依赖槽位复用的时序），改动前请至少手动推演一遍 |
// | 到达后忘了清 `manual_order_` | 「框选临时指令到达即失效」——单位卡在终点 |
// | `find_wall_post` 不查本拍已指派的认领表 | 「弓手自主驻墙」——两人挤同一段墙 |
// | Mason 分支不查 `bld_claimed_`（回到各算各的最近） | 「工匠任务认领」——两人挤同一处工地 |
// | Spear 分支删掉迎击段（回到纯驻防环兜底） | 「枪卫守家迎击」的进圈段——敌人无伤 |
// | `tick_economy` 退回 `mason_near` 二值（不按 crew 加速） | economy_test「多名工匠同任务线性加速」 |

TEST_CASE("没有任何指令：走向按槽位错开的堡垒驻防环，不再永远杵在原地", "[script]") {
    rts::WorldInit init = sarena(20, 20);
    init.keep = rts::GridPos{10, 10};
    init.buildings.clear();
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 2.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 3.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    const rts::UnitId a0 = ids[0];
    const rts::UnitId a1 = ids[1];

    game::DefenderScript s(game::ScriptParams{}, 3);
    run(w, s, 400);   // 无攻方、无指令、没有墙可登：纯看驻防环默认

    const rts::Vec2 p0 = w.unit_pos(a0);
    const rts::Vec2 p1 = w.unit_pos(a1);
    // 两个槽位的驻防点在堡垒不同侧（半径 3 的环按槽位轮转），所以两个终点
    // 必须不同——若两者挤到同一格，说明 hold_point_for 没有按槽位区分。
    const float dx = p0.x - p1.x, dy = p0.y - p1.y;
    REQUIRE(dx * dx + dy * dy > 1.0f);
    // 都离开了出生点（原地不动是这条要防的旧行为）。
    REQUIRE((p0.x != 2.5f || p0.y != 2.5f));
    REQUIRE((p1.x != 2.5f || p1.y != 3.5f));
}

TEST_CASE("框选临时指令：只影响被点到的单位，其余的照走自主默认", "[script]") {
    rts::WorldInit init = sarena(20, 20);
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 2.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 3.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    const rts::UnitId picked = ids[0];    // 只框选这一个
    const rts::UnitId other = ids[1];

    game::DefenderScript s(game::ScriptParams{}, 3);
    // 框选到的那一个改去 B（4, 8）——离出生点够远，方向上与自主默认
    // （回堡垒 (0,0) 周围的驻防环）相反，好与它区分。
    const rts::UnitId picked_ids[1] = {picked};
    s.issue_move_order(picked_ids, rts::GridPos{4, 8});

    // 只跑一小段，看**方向**而不是等「到没到」——到达时机对路径长短很敏感：
    // 临时指令到达即失效，真等它到了 B，它已回归自主、往回走了，反而看不出
    // 「只影响 picked」这件事（下一条用例才是测「到达后回归自主」）。
    run(w, s, 20);

    const rts::Vec2 pa = w.unit_pos(picked);
    const rts::Vec2 pb = w.unit_pos(other);
    // 被框选的那个朝 B（4, 8）走：B 的 y 比起点大得多，y 应显著增大。
    REQUIRE(pa.y > 4.0f);
    // 没被框选的那个没有跟着去 B，而是朝堡垒方向的驻防点走：y 应明显减小。
    REQUIRE(pb.y < 3.0f);
}

TEST_CASE("框选临时指令到达即失效：回归自主驻防，不是卡在终点", "[script]") {
    rts::WorldInit init = sarena(20, 20);
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 2.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    const rts::UnitId a = ids[0];

    game::DefenderScript s(game::ScriptParams{}, 3);
    // 临时指令：去 B（4, 2）。到了应当清掉指令、回归自主——本图无墙可登，
    // 自主默认是回堡垒 (0,0) 周围的驻防环，所以它不该停在 B。
    const rts::UnitId picked_ids[1] = {a};
    s.issue_move_order(picked_ids, rts::GridPos{4, 2});

    run(w, s, 400);   // 给足够 tick 走完 B 再走完回程

    const rts::Vec2 p = w.unit_pos(a);
    // 已离开 B（指令失效了），且朝堡垒一侧回走——终点在出生点的西北方向。
    const float dx = p.x - 4.5f, dy = p.y - 2.5f;
    REQUIRE(dx * dx + dy * dy > 1.5f * 1.5f);
    REQUIRE(p.x < 2.5f);
    REQUIRE(p.y < 2.5f);
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
TEST_CASE("攻击目标进入射程后迎击指令会开火，不再只顾冲到敌人格子", "[script]") {
    auto init=sarena();init.units.push_back({rts::UnitType::Archer,{4.5f,2.5f},1,20,20});rts::World w(init);
    const auto target=w.spawn_unit(rts::UnitType::Ghoul,{8.5f,2.5f},1,100,100);
    std::vector<rts::UnitId> ids;w.enumerate_units(rts::Side::Defender,ids);
    game::DefenderScript script({},1);script.issue_move_order(ids,{8,2});run(w,script,20);
    REQUIRE(w.unit_hp(target)<100);
    REQUIRE(w.unit_pos(ids[0]).x<6.0f);
}
TEST_CASE("猎骑不会自动追出封闭城圈", "[script]") {
    auto init=sarena(20,20);init.keep={10,10};init.buildings[0].pos=init.keep;
    for(int x=5;x<=15;++x) for(int y=5;y<=15;++y) if(x==5||x==15||y==5||y==15)
        init.buildings.push_back({x==15&&y==10?rts::BldType::Gate:rts::BldType::Wall,{static_cast<std::int16_t>(x),static_cast<std::int16_t>(y)},40,40});
    init.units.push_back({rts::UnitType::Ranger,{12.5f,10.5f},1,18,18});rts::World w(init);
    w.spawn_unit(rts::UnitType::Ram,{17.5f,10.5f},1,60,60);
    std::vector<rts::UnitId> ids;w.enumerate_units(rts::Side::Defender,ids);game::DefenderScript script({},1);
    for(int t=0;t<200;t+=4) {run(w,script,4);REQUIRE(w.unit_pos(ids[0]).x<15.0f);}
}
