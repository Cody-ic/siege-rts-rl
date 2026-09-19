// 机制第四批：冲锋助跑、反冲锋（克制倍率的三轴推导）、Tower 齐射。
//
// 与前几批同一条纪律：锁的是机制正确性，每条对着设计原文写——
// 「骑兵冲锋伤害取决于助跑距离（开阔地怕枪阵，巷战反克）」、
// 「克制关系由三条正交属性轴推导，不要退化成平铺的 N×N 伤害倍率表」、
// 「镇野箭楼……齐射覆盖（克制步兵一拥而上啃墙）」、「AA 只做单体狙击型」。
//
// 方向名注意：格坐标的正西是 `MoveNW`、正东是 `MoveSE`（`rts/action.hpp`——
// 枚举名是屏幕方位，按名字猜格增量会恰好猜反）。
//
// 数值全部是测试自带的占位表，只求出手节奏与动量可手算，无任何平衡含义。

#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/action.hpp"
#include "rts/combat_math.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace {

rts::UnitStats& us(rts::StatsTable& t, rts::UnitType u) {
    return t.unit[static_cast<std::size_t>(u)];
}
rts::BldStats& bs(rts::StatsTable& t, rts::BldType b) {
    return t.bld[static_cast<std::size_t>(b)];
}

rts::StatsTable charge_stats() {
    rts::StatsTable t;
    for (rts::UnitStats& u : t.unit) {
        u.max_hp = 30;
        u.vision = 4.0f;
        u.cooldown_ticks = 3;
        u.windup_ticks = 1;
    }
    // Knight 速度取 0.25（二进制精确），8 tick = 恰好 2.0 格动量，预期可手算。
    us(t, rts::UnitType::Knight) = {40, 8, 1.2f, 0.25f, 6.0f, 1, 4, 1000, 0.0f};
    us(t, rts::UnitType::Spear) = {24, 6, 1.2f, 0.08f, 4.0f, 1, 3, 500, 0.0f};
    us(t, rts::UnitType::Ranger) = {18, 6, 1.2f, 0.30f, 5.0f, 1, 2, 1000, 0.0f};
    us(t, rts::UnitType::Archer) = {20, 5, 4.0f, 0.10f, 5.0f, 1, 4, 500, 0.0f};
    us(t, rts::UnitType::Ghoul) = {30, 4, 1.2f, 0.25f, 4.0f, 1, 3, 2000, 0.0f};
    us(t, rts::UnitType::Phoenix) = {24, 8, 3.0f, 0.30f, 6.0f, 1, 4, 800, 0.0f};
    bs(t, rts::BldType::Keep).max_hp = 200;
    bs(t, rts::BldType::Wall).max_hp = 40;
    bs(t, rts::BldType::Gate).max_hp = 24;
    t.global.hp_permille_per_level = 100;
    t.global.dmg_permille_per_level = 100;
    // 冲锋：每格 +250‰、封顶 4 格、枪阵对满动量 +800‰。全是驱动测试的占位。
    t.global.charge_bonus_permille_per_cell = 250;
    t.global.charge_max_cells = 4.0f;
    t.global.anti_charge_permille = 1800;
    return t;
}

rts::WorldInit arena() {
    rts::WorldInit init;
    init.width = 14;
    init.height = 8;
    init.terrain.assign(112, rts::Terrain::Plain);
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.seed = 77;
    init.map_id = "charge-arena";
    init.nominal_level = 1;
    init.stats = charge_stats();
    return init;
}

void act(rts::World& w, rts::Side side, std::initializer_list<rts::UnitAction> a) {
    std::vector<rts::UnitAction> v(a);
    w.submit_actions(side, v.data(), v.size());
}

rts::UnitId one(const rts::World& w, rts::Side side, std::size_t k = 0) {
    std::vector<rts::UnitId> ids;
    w.enumerate_units(side, ids);
    return ids[k];
}

}  // namespace

// ——冲锋助跑——

TEST_CASE("冲锋伤害 ∝ 助跑距离，落地耗尽，之后回到基础值", "[charge]") {
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 30, 30});
    rts::World w(std::move(init));
    const rts::UnitId a = one(w, rts::Side::Defender);
    const rts::UnitId kn =
        w.spawn_unit(rts::UnitType::Knight, rts::Vec2{8.5f, 4.5f}, 1, 40, 40);

    // 助跑：向格西直线 8 tick × 0.25 = 恰好 2.0 格动量。
    act(w, rts::Side::Defender, {rts::UnitAction::Stop});
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveNW});
    w.advance(8);
    REQUIRE(w.unit_charge(kn) == 2.0f);
    REQUIRE(w.unit_pos(kn).x == 6.5f);   // 到弓手贴脸（距离 1.0 <= 射程 1.2）

    // 冲锋那一击：基础 8 × 等级 1000‰ × 冲锋 (1000 + 250×2)‰。
    act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
    w.advance(2);   // 承诺 + 落地
    const std::int64_t charged = rts::apply_permille(
        8, {1000, rts::charge_permille(2.0f, 4.0f, 250)});
    REQUIRE(charged == 12);   // 预期用与机制同一对函数现算，数字只是核对
    REQUIRE(w.unit_hp(a) == 30 - charged);
    REQUIRE(w.unit_charge(kn) == 0.0f);   // 冲完就是冲完了

    // 下一击（原地站着等冷却）：没有助跑，回到基础值。
    w.advance(4);
    REQUIRE(w.unit_hp(a) == 30 - charged - 8);
}

TEST_CASE("站停归零：停一拍再打，动量没了", "[charge]") {
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 30, 30});
    rts::World w(std::move(init));
    const rts::UnitId a = one(w, rts::Side::Defender);
    const rts::UnitId kn =
        w.spawn_unit(rts::UnitType::Knight, rts::Vec2{8.5f, 4.5f}, 1, 40, 40);

    act(w, rts::Side::Defender, {rts::UnitAction::Stop});
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveNW});
    w.advance(8);
    REQUIRE(w.unit_charge(kn) == 2.0f);

    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(1);
    REQUIRE(w.unit_charge(kn) == 0.0f);

    act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
    w.advance(2);
    REQUIRE(w.unit_hp(a) == 30 - 8);   // 基础值：站住就没有助跑可言
}

TEST_CASE("骑士撞门是真撞：自动破坏把动量带进那一击", "[charge]") {
    rts::WorldInit init = arena();
    rts::World w(std::move(init));
    const rts::BldId gate = w.place_bld(rts::BldType::Gate, rts::GridPos{6, 4}, 24, 24);
    const rts::UnitId kn =
        w.spawn_unit(rts::UnitType::Knight, rts::Vec2{8.5f, 4.5f}, 1, 40, 40);

    // 直线冲门：第 7 tick 撞上（此前累了 6 步 = 1.5 格），撞击承诺自动破坏，
    // 第 8 tick 落地。动量若在承诺前被清掉，这一击就退化成基础拆墙值。
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveNW});
    w.advance(8);
    const std::int64_t hit = rts::apply_permille(
        8, {1000, 1000, rts::charge_permille(1.5f, 4.0f, 250)});
    REQUIRE(w.bld_hp(gate) == 24 - hit);
    REQUIRE(w.unit_charge(kn) == 0.0f);
}

// ——反冲锋：克制关系由轴推导，幅度随动量放大——

TEST_CASE("枪阵克骑由轴推导：只对有动量的冲锋成立，且随动量放大", "[charge]") {
    // 布局共用：骑士助跑 2.0 格后对枪卫承诺出手（前摇拉长到 10，动量因此
    // **冻结**在 2.0——反冲锋的判定窗口是确定的，不用赌 tick）。
    const auto build = [](rts::UnitType defender) {
        rts::WorldInit init = arena();
        us(init.stats, rts::UnitType::Knight).windup_ticks = 10;
        init.units.push_back(
            rts::UnitInit{defender, rts::Vec2{5.5f, 4.5f}, 1, 24, 24});
        return init;
    };

    SECTION("Spear（Melee × HeavySlow）打冻结着 2.0 格动量的骑士：吃克制") {
        rts::World w(build(rts::UnitType::Spear));
        const rts::UnitId kn =
            w.spawn_unit(rts::UnitType::Knight, rts::Vec2{8.5f, 4.5f}, 1, 40, 40);
        act(w, rts::Side::Defender, {rts::UnitAction::Stop});
        act(w, rts::Side::Attacker, {rts::UnitAction::MoveNW});
        w.advance(8);   // 动量 2.0，贴脸
        act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(2);   // 骑士承诺（前摇 10，动量冻结）；枪卫承诺 + 落地
        REQUIRE(w.unit_charge(kn) == 2.0f);   // 冻结中，还没打出去
        // 半动量（2/4）⇒ 克制幅度取满额 1800‰ 的一半：1400‰。
        const std::int64_t hit = rts::apply_permille(
            6, {1000, rts::anti_charge_permille(2.0f, 4.0f, 1800)});
        REQUIRE(hit == 8);
        REQUIRE(w.unit_hp(kn) == 40 - hit);
    }
    SECTION("Ranger（LightFast）不是枪阵：同样的局面只打基础值") {
        rts::World w(build(rts::UnitType::Ranger));
        const rts::UnitId kn =
            w.spawn_unit(rts::UnitType::Knight, rts::Vec2{8.5f, 4.5f}, 1, 40, 40);
        act(w, rts::Side::Defender, {rts::UnitAction::Stop});
        act(w, rts::Side::Attacker, {rts::UnitAction::MoveNW});
        w.advance(8);
        act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(2);
        REQUIRE(w.unit_hp(kn) == 40 - 6);
    }
    SECTION("巷战被反克的机制形式：没有动量的骑士，枪阵克制自动消失") {
        rts::World w(build(rts::UnitType::Spear));
        const rts::UnitId kn =
            w.spawn_unit(rts::UnitType::Knight, rts::Vec2{6.5f, 4.5f}, 1, 40, 40);
        act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(2);   // 骑士原地承诺（动量 0）；枪卫落地
        REQUIRE(w.unit_hp(kn) == 40 - 6);   // 基础值：克的是冲锋，不是兵种名
    }
}

// ——Tower 齐射——

TEST_CASE("Tower 齐射：AOE 砸锁定落点，友军与空中不挨砸", "[charge]") {
    rts::WorldInit init = arena();
    bs(init.stats, rts::BldType::Tower) = {50, 7, 5.0f, 5.0f, 1, 5};
    bs(init.stats, rts::BldType::Tower).aoe_radius = 1.5f;
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Spear, rts::Vec2{5.5f, 4.5f}, 1, 24, 24});
    rts::World w(std::move(init));
    w.place_bld(rts::BldType::Tower, rts::GridPos{4, 4}, 50, 50);
    const rts::UnitId ga =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{6.5f, 4.5f}, 1, 30, 30);
    const rts::UnitId gb =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{7.5f, 4.5f}, 1, 30, 30);
    const rts::UnitId ph =
        w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{6.5f, 4.5f}, 1, 24, 24);
    const rts::UnitId sp = one(w, rts::Side::Defender);

    act(w, rts::Side::Defender, {rts::UnitAction::Stop});
    act(w, rts::Side::Attacker,
        {rts::UnitAction::Stop, rts::UnitAction::Stop, rts::UnitAction::Stop});
    w.advance(2);   // 承诺（锁定最近者 ga 的位置）+ 落地
    REQUIRE(w.unit_hp(ga) == 30 - 7);   // 目标本人
    REQUIRE(w.unit_hp(gb) == 30 - 7);   // 挤在圈里的同伙——齐射克密集
    REQUIRE(w.unit_hp(sp) == 24);       // 己方枪卫可在箭塔掩护下作战
    REQUIRE(w.unit_hp(ph) == 24);       // 空中不挨砸（箭雨对地）
}

TEST_CASE("齐射落点在承诺那一刻锁定：跑掉的躲开，站着的挨", "[charge]") {
    rts::WorldInit init = arena();
    // 前摇拉长到 20：给目标 5 格的逃逸窗口（0.25 × 20）。冷却更长，免得第二轮
    // 齐射把「躲开了」的断言打脏。
    bs(init.stats, rts::BldType::Tower) = {50, 7, 5.0f, 5.0f, 20, 60};
    bs(init.stats, rts::BldType::Tower).aoe_radius = 1.5f;
    rts::World w(std::move(init));
    w.place_bld(rts::BldType::Tower, rts::GridPos{4, 4}, 50, 50);
    const rts::UnitId ga =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{6.5f, 4.5f}, 1, 30, 30);
    const rts::UnitId gb =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{7.5f, 4.5f}, 1, 30, 30);

    // 目标（最近者 ga）在前摇里向格东跑；gb 站着不动。
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE, rts::UnitAction::Stop});
    w.advance(21);   // 承诺（t1，锁定 (6.5,4.5)）→ 前摇 20 → 落地（t21）
    REQUIRE(w.unit_hp(ga) == 30);       // 跑出 1.5 格圈：散开真的能躲齐射
    REQUIRE(w.unit_hp(gb) == 30 - 7);   // 离锁定点 1.0 格：站着就挨
}

TEST_CASE("AA 只做单体狙击（结构）：表里给 Flak 配了 AOE 也不齐射", "[charge]") {
    rts::WorldInit init = arena();
    bs(init.stats, rts::BldType::Flak) = {40, 9, 5.0f, 6.0f, 0, 30};
    // 故意配一个非零半径：机制侧必须忽略它——「AA 只做单体狙击型」是结构，
    // 不靠表里的 0 承载（同「表不能把瞭望塔配成印钞机」）。
    bs(init.stats, rts::BldType::Flak).aoe_radius = 2.0f;
    rts::World w(std::move(init));
    w.place_bld(rts::BldType::Flak, rts::GridPos{4, 4}, 40, 40);
    const rts::UnitId pa =
        w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{6.5f, 4.5f}, 1, 24, 24);
    const rts::UnitId pb =
        w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{7.5f, 4.5f}, 1, 24, 24);

    act(w, rts::Side::Attacker, {rts::UnitAction::Stop, rts::UnitAction::Stop});
    w.advance(1);   // 前摇 0：当场落地
    REQUIRE(w.unit_hp(pa) == 24 - 9);   // 被狙的那一只
    REQUIRE(w.unit_hp(pb) == 24);       // 半径 2 内的第二只毫发无损
}
