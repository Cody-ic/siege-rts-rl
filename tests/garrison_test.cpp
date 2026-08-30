// 机制第三批：驻守与高度优势。
//
// 与 mechanics_test 同一条纪律：这批锁的是**机制正确性而不是确定性**，
// 每条都对着设计文档里的一句话写（CLAUDE.md「不做登墙阶梯」与
// 「防御建筑与城墙是消耗品」里抄的 Stronghold 三条），改断言前先读原文。
//
// miss 是概率，但**两端是确定的**：1000‰ 必落空、0‰ 必命中——
// 所以这里没有一条概率性断言，也就没有偶发红。中间值的分布归标定，不归测试。
//
// 数值全部是测试自带的占位表，只求出手节奏可手算，无任何平衡含义。

#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/action.hpp"
#include "rts/combat_math.hpp"
#include "rts/command.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace {

rts::UnitStats& us(rts::StatsTable& t, rts::UnitType u) {
    return t.unit[static_cast<std::size_t>(u)];
}
rts::BldStats& bs(rts::StatsTable& t, rts::BldType b) {
    return t.bld[static_cast<std::size_t>(b)];
}

rts::StatsTable garrison_stats() {
    rts::StatsTable t;
    for (rts::UnitStats& u : t.unit) {
        u.max_hp = 30;
        u.vision = 4.0f;
        u.cooldown_ticks = 3;
        u.windup_ticks = 1;
    }
    us(t, rts::UnitType::Archer) = {20, 5, 4.0f, 0.10f, 5.0f, 1, 4, 500, 0.0f};
    us(t, rts::UnitType::Spear) = {24, 6, 1.2f, 0.08f, 4.0f, 1, 3, 500, 0.0f};
    us(t, rts::UnitType::Ghoul) = {30, 4, 1.2f, 0.25f, 4.0f, 1, 3, 2000, 0.0f};
    us(t, rts::UnitType::Phoenix) = {24, 8, 3.0f, 0.30f, 6.0f, 1, 4, 800, 0.0f};
    bs(t, rts::BldType::Keep).max_hp = 200;
    bs(t, rts::BldType::Wall).max_hp = 40;
    bs(t, rts::BldType::Gate).max_hp = 24;
    // 等级系数不为零，好让「等级 1 时公式给基础值」也顺带被走到。
    t.global.hp_permille_per_level = 100;
    t.global.dmg_permille_per_level = 100;
    // 驻守四项按用例各自覆写；这里给「没有任何效果」的恒等默认，
    // 于是每条用例只把自己要测的那一项拧开。
    t.global.garrison_mount_ticks = 0;
    t.global.high_ground_miss_permille = 0;
    t.global.high_ground_dmg_permille = 1000;
    t.global.high_ground_range_bonus = 0.0f;
    return t;
}

// 空地图 + 远角落的 Keep（World 要求恰好一座）。墙与单位由各用例自摆。
rts::WorldInit arena() {
    rts::WorldInit init;
    init.width = 12;
    init.height = 8;
    init.terrain.assign(96, rts::Terrain::Plain);
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.seed = 99;
    init.map_id = "garrison-arena";
    init.nominal_level = 1;
    init.stats = garrison_stats();
    return init;
}

constexpr rts::GridPos kWall{6, 4};   // 各用例共用的墙位；中心 (6.5, 4.5)

std::uint16_t slot(rts::GridPos p) { return rts::slot_of(p, 12); }

rts::Command garr(std::uint8_t force, rts::GridPos p) {
    rts::Command c;
    c.kind = rts::CommandKind::Garrison;
    c.side = rts::Side::Defender;
    c.force = force;
    c.slot = slot(p);
    return c;
}

void act(rts::World& w, rts::Side side, std::initializer_list<rts::UnitAction> a) {
    std::vector<rts::UnitAction> v(a);
    w.submit_actions(side, v.data(), v.size());
}

rts::UnitId defender_at(const rts::World& w, std::size_t k) {
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    return ids[k];
}

bool has(std::uint16_t mask, rts::UnitAction a) {
    return (mask & (1u << static_cast<unsigned>(a))) != 0;
}

}  // namespace

// ——登墙、钉住、召回——

TEST_CASE("驻守：相邻单位自动登墙，延迟走完落位墙心，此后被钉住", "[garrison]") {
    rts::WorldInit init = arena();
    init.stats.global.garrison_mount_ticks = 4;
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);

    const rts::Command c = garr(0, kWall);
    w.submit(rts::Side::Defender, &c, 1);
    w.advance(1);
    // 指令已解算成世界状态；单位开始爬，人还在地面。
    REQUIRE(w.garrison_order(slot(kWall)) == 0);
    REQUIRE(w.unit_garrison(a) == slot(kWall));
    REQUIRE(w.unit_mount(a) == 4);
    REQUIRE(w.unit_pos(a).x == 5.5f);
    // 驻守含着「先走到那儿」：编队去处一并指过去（脚本执行层读它行军）。
    REQUIRE(w.force_target(0) == slot(kWall));
    // 在爬：掩码只剩 Stop——上墙延迟的代价是这段既不能打也不能走。
    REQUIRE(w.action_mask(a) ==
            (1u << static_cast<unsigned>(rts::UnitAction::Stop)));

    w.advance(4);
    REQUIRE(w.unit_mount(a) == 0);
    REQUIRE(w.unit_pos(a).x == 6.5f);   // 落位墙心：位置就是墙格中心
    REQUIRE(w.unit_pos(a).y == 4.5f);

    // 钉住：Move 动作空转，掩码的移动 8 位全灭（「上墙的代价是机动性」）。
    act(w, rts::Side::Defender, {rts::UnitAction::MoveE});
    w.advance(5);
    REQUIRE(w.unit_pos(a).x == 6.5f);
    const std::uint16_t m = w.action_mask(a);
    for (int d = 0; d < rts::kMoveDirCount; ++d) {
        REQUIRE_FALSE(has(m, rts::move_of(d)));
    }
    REQUIRE(has(m, rts::UnitAction::Stop));
}

TEST_CASE("上墙延迟期间不出手：那段不设防就是延迟的代价", "[garrison]") {
    rts::WorldInit init = arena();
    init.stats.global.garrison_mount_ticks = 6;
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{4.5f, 4.5f}, 1, 30, 30});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> foes;
    w.enumerate_units(rts::Side::Attacker, foes);
    const rts::UnitId g = foes[0];

    const rts::Command c = garr(0, kWall);
    w.submit(rts::Side::Defender, &c, 1);
    w.advance(1);   // 开始爬（mount = 6）
    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(6);   // 整段攀爬期：目标近在咫尺（距离 2 < 射程 4）也一箭不发
    REQUIRE(w.unit_hp(g) == 30);
    REQUIRE(w.unit_mount(defender_at(w, 0)) == 0);
    w.advance(2);   // 登顶后照常：承诺 + 落地
    REQUIRE(w.unit_hp(g) == 25);
}

TEST_CASE("MoveForce 即召回：下墙到第一个空邻格，常设指令清除", "[garrison]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);

    const rts::Command c = garr(0, kWall);
    w.submit(rts::Side::Defender, &c, 1);
    w.advance(1);   // 零延迟：当场登顶
    REQUIRE(w.unit_garrison(a) == slot(kWall));

    rts::Command mv;
    mv.kind = rts::CommandKind::MoveForce;
    mv.side = rts::Side::Defender;
    mv.force = 0;
    mv.slot = slot(rts::GridPos{1, 1});
    w.submit(rts::Side::Defender, &mv, 1);
    w.advance(1);
    REQUIRE(w.unit_garrison(a) == rts::kNoSlot);
    REQUIRE(w.garrison_order(slot(kWall)) == rts::kNoForce);
    // 第一个空邻格按规范扫描序（dj 外层、di 内层）是 (5,3)。
    REQUIRE(w.unit_pos(a).x == 5.5f);
    REQUIRE(w.unit_pos(a).y == 3.5f);
    REQUIRE(w.force_target(0) == slot(rts::GridPos{1, 1}));
}

TEST_CASE("墙塌了：人落在缺口里，指令随墙作废，行动恢复", "[garrison]") {
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);
    const rts::BldId wall = w.place_bld(rts::BldType::Wall, kWall, 40, 40);

    const rts::Command c = garr(0, kWall);
    w.submit(rts::Side::Defender, &c, 1);
    w.advance(1);
    REQUIRE(w.unit_garrison(a) == slot(kWall));

    w.destroy_bld(wall);
    // 不是「下墙」，是脚下的东西没了：不给延迟，人当场站在缺口那一格。
    REQUIRE(w.unit_garrison(a) == rts::kNoSlot);
    REQUIRE(w.unit_pos(a).x == 6.5f);
    REQUIRE(w.garrison_order(slot(kWall)) == rts::kNoForce);
    // 缺口可通行（CLAUDE.md），人也解了钉：能走了。
    act(w, rts::Side::Defender, {rts::UnitAction::MoveE});
    w.advance(4);
    REQUIRE(w.unit_pos(a).x > 6.5f);
}

TEST_CASE("一格一人；驻守者阵亡后同编队相邻单位自动补位（常设指令）", "[garrison]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{7.5f, 4.5f}, 1, 20, 20, 0});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);
    const rts::UnitId b = defender_at(w, 1);

    const rts::Command c = garr(0, kWall);
    w.submit(rts::Side::Defender, &c, 1);
    w.advance(1);
    // 槽位序小的上墙，另一个留在地面——一格一人。
    REQUIRE(w.unit_garrison(a) == slot(kWall));
    REQUIRE(w.unit_garrison(b) == rts::kNoSlot);

    w.kill_unit(a);
    w.advance(1);
    // 指令是常设的：同编队的相邻单位自动补位。
    REQUIRE(w.unit_garrison(b) == slot(kWall));
    REQUIRE(w.unit_pos(b).x == 6.5f);
}

TEST_CASE("驻守指令只落在完工的墙段上：Gate 算墙；工地、空格、其他建筑不算",
          "[garrison]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Gate, kWall, 24, 24});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);
    // 一座还在施工的墙：完工前不是墙，站上去没有「上」可言。
    w.place_bld(rts::BldType::Wall, rts::GridPos{6, 2}, 1, 40, 10);

    const rts::Command cmds[] = {
        garr(0, rts::GridPos{3, 3}),   // 空格
        garr(0, rts::GridPos{6, 2}),   // 工地
        garr(0, rts::GridPos{0, 0}),   // Keep：不是墙
        garr(0, kWall),                // Gate：「墙段」的既有判据是 Wall‖Gate
    };
    w.submit(rts::Side::Defender, cmds, 4);
    w.advance(1);
    REQUIRE(w.garrison_order(slot(rts::GridPos{3, 3})) == rts::kNoForce);
    REQUIRE(w.garrison_order(slot(rts::GridPos{6, 2})) == rts::kNoForce);
    REQUIRE(w.garrison_order(slot(rts::GridPos{0, 0})) == rts::kNoForce);
    REQUIRE(w.garrison_order(slot(kWall)) == 0);
    REQUIRE(w.unit_garrison(a) == slot(kWall));
}

// ——高度优势——

TEST_CASE("高度优势：远程驻守吃射程加成，近战不吃，破损墙不给", "[garrison]") {
    SECTION("远程：墙上打得到地面打不到的目标") {
        rts::WorldInit init = arena();
        init.stats.global.high_ground_range_bonus = 2.0f;
        init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{11.5f, 4.5f}, 1, 30, 30});
        rts::World w(std::move(init));
        const rts::UnitId a = defender_at(w, 0);
        std::vector<rts::UnitId> foes;
        w.enumerate_units(rts::Side::Attacker, foes);
        const rts::UnitId g = foes[0];

        act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
        act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
        w.advance(4);
        REQUIRE(w.unit_hp(g) == 30);   // 地面：距离 6 > 射程 4，空转（乙）
        REQUIRE_FALSE(has(w.action_mask(a), rts::UnitAction::AtkNear));

        const rts::Command c = garr(0, kWall);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);   // 登顶：墙心到目标距离 5 <= 4 + 2
        REQUIRE(has(w.action_mask(a), rts::UnitAction::AtkNear));
        w.advance(2);
        REQUIRE(w.unit_hp(g) == 25);
    }
    SECTION("近战不吃加成：给枪卫加射程不是高度优势，是改机制") {
        rts::WorldInit init = arena();
        init.stats.global.high_ground_range_bonus = 2.0f;
        init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Spear, rts::Vec2{5.5f, 4.5f}, 1, 24, 24, 0});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 30, 30});
        rts::World w(std::move(init));
        std::vector<rts::UnitId> foes;
        w.enumerate_units(rts::Side::Attacker, foes);
        const rts::UnitId g = foes[0];

        const rts::Command c = garr(0, kWall);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
        act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
        w.advance(6);
        // 距离 2 > 近战射程 1.2；若加成错误地作用于近战，1.2 + 2 = 3.2 就够着了。
        REQUIRE(w.unit_hp(g) == 30);
    }
    SECTION("局部破损：墙血 < 一半，加成消失") {
        rts::WorldInit init = arena();
        init.stats.global.high_ground_range_bonus = 2.0f;
        init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 19, 40});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{11.5f, 4.5f}, 1, 30, 30});
        rts::World w(std::move(init));
        std::vector<rts::UnitId> foes;
        w.enumerate_units(rts::Side::Attacker, foes);
        const rts::UnitId g = foes[0];

        const rts::Command c = garr(0, kWall);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
        act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
        w.advance(6);
        REQUIRE(w.unit_hp(g) == 30);   // 残墙给不了高度：19 × 2 < 40
    }
}

TEST_CASE("低处打墙上单位：miss 与伤害打折（两端确定，可无偶发地验证）",
          "[garrison]") {
    // 共用布局：墙上一名弓手（Stop 不还手），墙脚一只亡灵步兵贴脸砍
    // （距离 1.0 <= 近战射程 1.2）。四段只拧数值表，机制路径完全相同。
    const auto build = [](std::int32_t miss, std::int32_t dmg,
                          std::int64_t wall_hp) {
        rts::WorldInit init = arena();
        init.stats.global.high_ground_miss_permille = miss;
        init.stats.global.high_ground_dmg_permille = dmg;
        init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, wall_hp, 40});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{6.5f, 3.5f}, 1, 20, 20, 0});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{5.5f, 4.5f}, 1, 30, 30});
        return init;
    };
    const auto run = [](rts::WorldInit init, int ticks) {
        rts::World w(std::move(init));
        const rts::Command c = garr(0, kWall);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);   // 弓手登顶（零延迟）
        act(w, rts::Side::Defender, {rts::UnitAction::Stop});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(ticks);
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        return w.unit_hp(ids[0]);
    };

    SECTION("miss = 1000‰：从低处来的每一发都落空") {
        REQUIRE(run(build(1000, 1000, 40), 12) == 20);
    }
    SECTION("miss = 0：必中，但伤害按表打折") {
        // 亡灵步兵基础 4，高度减伤 500‰ ⇒ 每发 apply_permille(4, {1000, 500}) = 2。
        REQUIRE(run(build(0, 500, 40), 2) ==
                20 - rts::apply_permille(4, {1000, 500}));
    }
    SECTION("局部破损：墙血 < 一半，miss 与减伤一起消失") {
        REQUIRE(run(build(1000, 500, 19), 2) == 20 - 4);   // 全额挨打
    }
    SECTION("空中来的不算低处：Phoenix 无视 miss 与减伤") {
        rts::WorldInit init = arena();
        init.stats.global.high_ground_miss_permille = 1000;
        init.stats.global.high_ground_dmg_permille = 500;
        init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{6.5f, 3.5f}, 1, 20, 20, 0});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Phoenix, rts::Vec2{6.5f, 2.5f}, 1, 24, 24});
        rts::World w(std::move(init));
        const rts::UnitId a = defender_at(w, 0);

        const rts::Command c = garr(0, kWall);
        w.submit(rts::Side::Defender, &c, 1);
        w.advance(1);
        act(w, rts::Side::Defender, {rts::UnitAction::Stop});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(2);
        REQUIRE(w.unit_hp(a) == 20 - 8);   // 全额：高度优势挡不住从上面来的
    }
}

// ——契约与确定性——

TEST_CASE("编队是守方概念：攻方初始单位带编队在建局时被拒", "[garrison]") {
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{3.5f, 3.5f}, 1, 30, 30, 0});
    const auto make = [&] { rts::World w(std::move(init)); };
    REQUIRE_THROWS_AS(make(), rts::ContractError);
}

TEST_CASE("新驻守状态逐字段进 state_hash（各自隔离验证）", "[garrison]") {
    // 「两个世界不同 ⇒ 哈希不同」的笼统写法在这里会假绿：`u_garrison_` 与
    // `force_target_` 本来就在哈希里，漏喂两个**新**字段照样能红不了。
    // 所以每个新字段单独隔离——让两个世界的差异恰好只剩它。

    SECTION("garrison_order_") {
        // Garrison 与 MoveForce 都把 force_target_ 指到同一格，而编队里没有
        // 单位、不会有人登墙，于是全部差异恰好是那张按格的指令表。
        const auto fresh = [] {
            rts::WorldInit init = arena();
            init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
            return init;
        };
        rts::World wa(fresh());
        rts::World wb(fresh());
        const rts::Command g = garr(0, kWall);
        rts::Command mv;
        mv.kind = rts::CommandKind::MoveForce;
        mv.side = rts::Side::Defender;
        mv.force = 0;
        mv.slot = slot(kWall);
        wa.submit(rts::Side::Defender, &g, 1);
        wb.submit(rts::Side::Defender, &mv, 1);
        wa.advance(1);
        wb.advance(1);
        REQUIRE(wa.state_hash() != wb.state_hash());
    }

    SECTION("u_mount_") {
        // 同一份建局、同一条指令，只是下达时刻差一 tick。推进到同一 tick 时
        // 两边都在爬、人都在地面原位、驻守格与指令表全同——唯独倒计时差 1。
        const auto fresh = [] {
            rts::WorldInit init = arena();
            init.stats.global.garrison_mount_ticks = 4;
            init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
            init.units.push_back(rts::UnitInit{rts::UnitType::Archer,
                                               rts::Vec2{5.5f, 4.5f}, 1, 20, 20, 0});
            return init;
        };
        rts::World wa(fresh());
        rts::World wb(fresh());
        const rts::Command g = garr(0, kWall);
        wb.submit(rts::Side::Defender, &g, 1);
        wb.advance(1);   // B 第 1 tick 起爬
        wa.advance(1);
        wa.submit(rts::Side::Defender, &g, 1);
        wa.advance(2);   // A 第 2 tick 起爬
        wb.advance(2);
        REQUIRE(wa.now() == wb.now());
        const rts::UnitId ua = defender_at(wa, 0);
        const rts::UnitId ub = defender_at(wb, 0);
        REQUIRE(wa.unit_mount(ua) == 3);
        REQUIRE(wb.unit_mount(ub) == 2);
        REQUIRE(wa.unit_pos(ua).x == wb.unit_pos(ub).x);   // 都还没登顶
        REQUIRE(wa.state_hash() != wb.state_hash());
    }
}
