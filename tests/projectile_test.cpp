// 机制第五批：在途弹丸（第四组实体）。
//
// 与前几批同一条纪律：锁的是机制正确性，每条对着设计原文写——
// 「除 `Ram` 外，`Archer` / `Shade` / `Tower` / `Flak` 的攻击真有在途弹丸」、
// 「承诺攻击的那一帧」与「伤害落地」是两件事（`render/sprite_atlas.hpp` 那段）、
// 「溅射克密集、散开克溅射」（躲闪窗口如今 = 前摇 + 飞行）。
//
// miss 是概率，但两端确定（1000‰ 必落空、0‰ 必命中），没有偶发红。
//
// **弹丸字段的逐字段哈希隔离不可构造**（同第四批的 u_charge_ / b_aim_：
// 弹丸状态几乎处处是输入史的函数，造不出「只差一枚箭、其余全同」的两个世界），
// 覆盖由最后那条「回放跨越放箭 / 飞行 / 命中的窗口」承担——飞行数学或
// 结算顺序的任何不确定性都会让它 Diverged。
//
// 数值全部是测试自带的占位表，只求飞行时间可手算，无任何平衡含义。

#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/action.hpp"
#include "rts/combat_math.hpp"
#include "rts/command.hpp"
#include "rts/replay.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace {

rts::UnitStats& us(rts::StatsTable& t, rts::UnitType u) {
    return t.unit[static_cast<std::size_t>(u)];
}
rts::BldStats& bs(rts::StatsTable& t, rts::BldType b) {
    return t.bld[static_cast<std::size_t>(b)];
}

rts::StatsTable proj_stats() {
    rts::StatsTable t;
    for (rts::UnitStats& u : t.unit) {
        u.max_hp = 30;
        u.vision = 4.0f;
        u.cooldown_ticks = 60;   // 一条用例里只放一箭，别让第二箭把断言打脏
        u.windup_ticks = 2;
    }
    // 速度取二进制精确的 0.25 / 0.5，飞行 tick 数可手算。
    us(t, rts::UnitType::Archer) = {20, 8, 5.0f, 0.10f, 5.0f, 2, 60, 500, 0.0f, 0.5f};
    us(t, rts::UnitType::Shade) = {24, 8, 6.0f, 0.10f, 5.0f, 2, 60, 500, 0.0f, 0.25f};
    us(t, rts::UnitType::Ghoul) = {30, 4, 1.2f, 0.25f, 4.0f, 2, 60, 2000, 0.0f, 0.0f};
    us(t, rts::UnitType::Phoenix) = {24, 8, 3.0f, 0.20f, 6.0f, 2, 60, 800, 0.0f, 0.0f};
    us(t, rts::UnitType::Spear) = {24, 6, 1.2f, 0.08f, 4.0f, 2, 60, 500, 0.0f, 0.0f};
    bs(t, rts::BldType::Keep).max_hp = 200;
    bs(t, rts::BldType::Wall).max_hp = 40;
    t.global.hp_permille_per_level = 100;
    t.global.dmg_permille_per_level = 100;
    // 高度与驻守各用例自己拧；默认恒等（miss 0、减伤 1000、登墙零延迟）。
    t.global.high_ground_dmg_permille = 1000;
    return t;
}

rts::WorldInit arena() {
    rts::WorldInit init;
    init.width = 14;
    init.height = 8;
    init.terrain.assign(112, rts::Terrain::Plain);
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.seed = 55;
    init.map_id = "proj-arena";
    init.nominal_level = 1;
    init.stats = proj_stats();
    return init;
}

void act(rts::World& w, rts::Side side, std::initializer_list<rts::UnitAction> a) {
    std::vector<rts::UnitAction> v(a);
    w.submit_actions(side, v.data(), v.size());
}

}  // namespace

// ——箭真的在飞——

TEST_CASE("箭矢有飞行时间：放箭在落地帧，伤害在命中帧", "[proj]") {
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 30, 30);

    // 距离 3.0、弹速 0.5：前摇 2 之后放箭，飞 6 tick（第 6 步起点距 0.5 时命中）。
    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(2);   // t1 承诺、t2 前摇
    REQUIRE(w.live_proj_count() == 0);   // 箭还没离弦
    w.advance(1);   // t3 前摇走完 = 放箭（同 tick 飞出第一步）
    REQUIRE(w.live_proj_count() == 1);
    REQUIRE(w.unit_hp(g) == 30);
    w.advance(4);   // t4–t7 仍在飞
    REQUIRE(w.live_proj_count() == 1);
    REQUIRE(w.unit_hp(g) == 30);
    w.advance(1);   // t8 命中
    REQUIRE(w.unit_hp(g) == 30 - 8);
    REQUIRE(w.live_proj_count() == 0);
}

TEST_CASE("弹速 0 = 瞬时命中：旧近似成为退化形态", "[proj]") {
    rts::WorldInit init = arena();
    us(init.stats, rts::UnitType::Archer).proj_speed = 0.0f;
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 30, 30);

    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(3);   // 前摇走完那一帧伤害当场落地——第五批之前的全部行为
    REQUIRE(w.unit_hp(g) == 30 - 8);
    REQUIRE(w.live_proj_count() == 0);
}

// ——追踪与落空——

TEST_CASE("单体箭矢追踪目标：目标在飞行中移动，箭跟上去命中", "[proj]") {
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 30, 30);

    // 目标向格东跑（0.25/tick），箭 0.5/tick 从后追——净接近 0.25/tick。
    //
    // **命中时机是这条测试的主断言**：单看「最终掉血」区分不了追踪与锁点——
    // 锁点的箭飞到旧位置后按句柄结算照样扣血，只是时机不同。手算两个时刻：
    // 放箭在 t3（前摇 2），锁点版本 t9 就命中（放箭时差距 3.5 ÷ 0.5）；
    // 追踪版本净接近 0.25/tick，要到 t16。t10 处「还没打中」把两者分开。
    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE});
    w.advance(10);
    REQUIRE(w.live_proj_count() == 1);   // t10：锁点的箭此刻已经结算完了
    REQUIRE(w.unit_hp(g) == 30);
    w.advance(10);
    REQUIRE(w.unit_hp(g) == 30 - 8);     // t16 追上（多给 4 tick 余量）
    REQUIRE(w.live_proj_count() == 0);
}

TEST_CASE("箭还在飞，人已经没了：弹丸落空消失，不转火旁人", "[proj]") {
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 30, 30);
    const rts::UnitId bystander =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{9.5f, 4.5f}, 1, 30, 30);

    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop, rts::UnitAction::Stop});
    w.advance(5);   // t3 放箭（对更近的 g），t4–t5 在飞
    REQUIRE(w.live_proj_count() == 1);
    w.kill_unit(g);
    w.advance(1);   // 下一次飞行检查：目标没了，箭落空
    REQUIRE(w.live_proj_count() == 0);
    w.advance(10);
    REQUIRE(w.unit_hp(bystander) == 30);   // 箭不换目标——那是箭，不是导弹
}

// ——齐射弹雨：锁定落点，躲闪窗口 = 前摇 + 飞行——

TEST_CASE("齐射弹雨飞向锁定落点：飞行期间跑掉的躲开，站着的挨", "[proj]") {
    rts::WorldInit init = arena();
    // 前摇 0：躲闪窗口**全部**来自飞行时间——比第四批那条（窗口 = 前摇）更锐。
    bs(init.stats, rts::BldType::Tower) = {50, 14, 7.0f, 5.0f, 0, 90};
    bs(init.stats, rts::BldType::Tower).aoe_radius = 1.5f;
    bs(init.stats, rts::BldType::Tower).proj_speed = 0.25f;
    rts::World w(std::move(init));
    w.place_bld(rts::BldType::Tower, rts::GridPos{4, 4}, 50, 50);
    const rts::UnitId stander =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{7.5f, 4.5f}, 1, 30, 30);
    const rts::UnitId runner =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 30, 30);

    // t1 承诺（锁定最近者 stander 的位置 (7.5,4.5)）+ 前摇 0 当场放箭；
    // 弹雨飞 3.0 格 ÷ 0.25 = 12 tick。runner 起点离落点 1.0（圈内），
    // 命中前向格东跑出 3.0 格——散开真的能躲，且躲的是**飞行中的**弹雨。
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop, rts::UnitAction::MoveSE});
    w.advance(11);
    REQUIRE(w.live_proj_count() == 1);
    REQUIRE(w.unit_hp(stander) == 30);
    w.advance(1);   // t12 弹雨落地
    REQUIRE(w.live_proj_count() == 0);
    REQUIRE(w.unit_hp(stander) == 30 - 14);
    REQUIRE(w.unit_hp(runner) == 30);
}

TEST_CASE("Flak 弩矢追踪空中目标：建筑单体路径也真有弹丸", "[proj]") {
    rts::WorldInit init = arena();
    bs(init.stats, rts::BldType::Flak) = {40, 9, 6.0f, 6.0f, 0, 90};
    bs(init.stats, rts::BldType::Flak).proj_speed = 0.5f;
    rts::World w(std::move(init));
    w.place_bld(rts::BldType::Flak, rts::GridPos{4, 4}, 40, 40);
    const rts::UnitId ph =
        w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{8.5f, 4.5f}, 1, 24, 24);

    act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE});
    w.advance(1);   // t1 放箭（前摇 0），距离 4.0，目标在逃
    REQUIRE(w.live_proj_count() == 1);
    w.advance(39);  // 净接近 0.3/tick，追上为止
    REQUIRE(w.unit_hp(ph) == 24 - 9);
    REQUIRE(w.live_proj_count() == 0);
}

// ——高度优势在命中那一刻结算——

TEST_CASE("高度 miss / 减伤按命中时刻的目标状态判：飞行中登墙真的能挡箭", "[proj]") {
    // 布局共用：Shade（地面）在 (10.5,4.5) 射 (5.5,4.5) 的弓手，距离 5.0、
    // 弹速 0.25 ⇒ 放箭后要飞约 20 tick——给「箭在半路、人登上墙」留足窗口。
    // 登墙零延迟（garrison_mount_ticks 默认 0），墙满血（高度未失）。
    const auto build = [] {
        rts::WorldInit init = arena();
        init.buildings.push_back(
            rts::BldInit{rts::BldType::Wall, rts::GridPos{6, 4}, 40, 40});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
        return init;
    };
    const auto garrison_wish = [] {
        // 登墙意愿（编队移除后的新通道）：守方只有那名弓手，数组长度 1。
        return rts::slot_of(rts::GridPos{6, 4}, 14);
    };

    SECTION("miss 1000‰：放箭时人在地面，命中前登墙 ⇒ 箭整发落空") {
        rts::WorldInit init = build();
        init.stats.global.high_ground_miss_permille = 1000;
        rts::World w(std::move(init));
        const rts::UnitId a = w.spawn_unit(rts::UnitType::Shade,
                                           rts::Vec2{10.5f, 4.5f}, 1, 24, 24);
        (void)a;
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        const rts::UnitId archer = ids[0];

        act(w, rts::Side::Defender, {rts::UnitAction::Stop});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(4);   // t3 放箭（目标在地面——放箭那一刻还没有 miss 可言）
        REQUIRE(w.live_proj_count() == 1);
        const std::uint16_t wish = garrison_wish();
        w.submit_garrison_wishes(rts::Side::Defender, &wish, 1);
        w.advance(40);  // t5 登墙（零延迟），箭追到墙心，命中时刻人已在高处
        REQUIRE(w.live_proj_count() == 0);
        REQUIRE(w.unit_hp(archer) == 20);   // 1000‰ 必落空：登墙真的挡住了这箭
    }
    SECTION("对照：不登墙 ⇒ 全额命中（地面目标不掷 miss）") {
        rts::WorldInit init = build();
        init.stats.global.high_ground_miss_permille = 1000;
        rts::World w(std::move(init));
        w.spawn_unit(rts::UnitType::Shade, rts::Vec2{10.5f, 4.5f}, 1, 24, 24);
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        const rts::UnitId archer = ids[0];

        act(w, rts::Side::Defender, {rts::UnitAction::Stop});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(44);
        REQUIRE(w.unit_hp(archer) == 20 - 8);
    }
    SECTION("miss 0、减伤 700‰：命中墙上目标按命中时刻打折") {
        rts::WorldInit init = build();
        init.stats.global.high_ground_dmg_permille = 700;
        rts::World w(std::move(init));
        w.spawn_unit(rts::UnitType::Shade, rts::Vec2{10.5f, 4.5f}, 1, 24, 24);
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        const rts::UnitId archer = ids[0];

        const std::uint16_t wish = garrison_wish();
        w.submit_garrison_wishes(rts::Side::Defender, &wish, 1);   // 开打前就在墙上
        act(w, rts::Side::Defender, {rts::UnitAction::Stop});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(44);
        // 预期用与机制同一对函数现算（等级 1 恒等 × 高度 700‰）。
        const std::int64_t hit = rts::apply_permille(8, {1000, 700});
        REQUIRE(w.unit_hp(archer) == 20 - hit);
    }
}

// ——确定性覆盖——

TEST_CASE("回放跨越放箭 / 飞行 / 命中的窗口逐 tick 一致", "[proj]") {
    // 弹丸字段的逐字段哈希探针不可构造（见文件头）；这条是它们的覆盖：
    // 飞行数学、追踪目的地刷新、结算顺序、压实顺序里任何不确定性都会 Diverged。
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 30, 30});
    rts::WorldInit init2 = init;

    rts::World w(std::move(init));
    rts::ReplayRecorder rec(w, 1);   // 逐 tick 取哈希
    const std::vector<rts::UnitAction> d{rts::UnitAction::AtkNear};
    const std::vector<rts::UnitAction> a{rts::UnitAction::MoveNW};
    rec.submit_actions(rts::Side::Defender, d.data(), d.size());
    rec.submit_actions(rts::Side::Attacker, a.data(), a.size());
    rec.advance(30);   // 覆盖承诺、放箭、追一个移动目标、命中、压实
    const rts::Replay r = rec.finish();

    rts::World fresh(std::move(init2));
    const rts::ReplayResult res = rts::replay_verify(r, fresh);
    REQUIRE(res.verdict == rts::ReplayVerdict::Match);
}

// ——结构：谁没有弹丸——

TEST_CASE("Phoenix 俯冲直击（结构）：表里配了弹丸速度也不放箭", "[proj]") {
    rts::WorldInit init = arena();
    // 故意配一个非零弹速：机制侧必须忽略它——「除 Ram 外，Archer/Shade/
    // Tower/Flak 的攻击真有在途弹丸」是点名清单，Phoenix 不在内
    // （launches_projectile = Ranged × 非空中）。同「表不能把瞭望塔配成
    // 印钞机」「Flak 配了 AOE 也不齐射」的先例。
    us(init.stats, rts::UnitType::Phoenix).proj_speed = 0.5f;
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Spear, rts::Vec2{5.5f, 4.5f}, 1, 24, 24});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    const rts::UnitId sp = ids[0];
    w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{7.5f, 4.5f}, 1, 24, 24);

    act(w, rts::Side::Defender, {rts::UnitAction::Stop});
    act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
    w.advance(2);
    REQUIRE(w.live_proj_count() == 0);
    w.advance(1);   // 前摇走完：伤害当场落地，全程没有弹丸实体
    REQUIRE(w.unit_hp(sp) == 24 - 8);
    REQUIRE(w.live_proj_count() == 0);
}
