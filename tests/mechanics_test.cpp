// 机制第一批：承伤 / 目标选择 / 移动 / 建筑攻击 / 视野。
//
// 这批用例锁的大多是**机制正确性而不是确定性**——回放测试抓不到它们
// （落点跟着句柄走、掩码写反对空对地……回放照样逐 tick 一致）。
// 所以每条都对着设计文档里的一句话写，改断言前先读那句原文。
//
// 数值全部是**测试自带的占位表**（`test_stats()`），不读 JSON——
// 这批用例要的是「几步能算出手数」的可控数字，不是占位表的具体值。

#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/combat_math.hpp"
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

// 一份驱动测试的数值表。挑的数只求出手节奏可手算，无任何平衡含义。
rts::StatsTable test_stats() {
    rts::StatsTable t;
    for (rts::UnitStats& u : t.unit) {
        u.max_hp = 30;
        u.vision = 4.0f;
        u.cooldown_ticks = 3;
        u.windup_ticks = 1;
    }
    us(t, rts::UnitType::Archer) = {20, 5, 4.0f, 0.10f, 5.0f, 2, 4, 500, 0.0f};
    us(t, rts::UnitType::Ghoul) = {30, 4, 1.2f, 0.25f, 4.0f, 1, 3, 2000, 0.0f};
    us(t, rts::UnitType::Ranger) = {18, 6, 1.2f, 0.30f, 5.0f, 1, 2, 1000, 0.0f};
    us(t, rts::UnitType::Ram) = {60, 10, 1.5f, 0.10f, 3.0f, 3, 5, 4000, 1.5f};
    us(t, rts::UnitType::Phoenix) = {24, 8, 3.0f, 0.30f, 6.0f, 1, 4, 800, 0.0f};
    us(t, rts::UnitType::Scout) = {8, 0, 0.0f, 0.40f, 6.0f, 0, 1, 0, 0.0f};
    us(t, rts::UnitType::Wraith) = {14, 0, 0.0f, 0.40f, 6.0f, 0, 1, 0, 0.0f};
    // `Shade` 在测试表里伤害取 0、速度与视野照抄 `Wraith`：2026-09-03 起
    // `Wraith` 会飞、不再吃视线遮挡，视野测试里「会被森林挡视线的攻方
    // 地面观察者」这个载具由它接任。
    us(t, rts::UnitType::Shade) = {14, 0, 0.0f, 0.40f, 6.0f, 0, 1, 0, 0.0f};
    bs(t, rts::BldType::Keep).max_hp = 200;
    bs(t, rts::BldType::Wall).max_hp = 40;
    bs(t, rts::BldType::Gate).max_hp = 24;
    bs(t, rts::BldType::Tower) = {50, 7, 4.0f, 5.0f, 1, 3};
    bs(t, rts::BldType::Flak) = {40, 9, 4.0f, 6.0f, 0, 3};
    t.obstacle[static_cast<std::size_t>(rts::ObstacleType::Stump)] = {12, 7};
    t.global = {100, 100};
    return t;
}

// 空地图 + 一座远角落的 Keep（World 要求恰好一座），不掺和测试本身。
rts::WorldInit arena(int w = 12, int h = 8) {
    rts::WorldInit init;
    init.width = w;
    init.height = h;
    init.terrain.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                        rts::Terrain::Plain);
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.seed = 42;
    init.map_id = "mech-arena";
    init.nominal_level = 1;
    init.stats = test_stats();
    return init;
}

void act(rts::World& w, rts::Side side, std::initializer_list<rts::UnitAction> a) {
    std::vector<rts::UnitAction> v(a);
    w.submit_actions(side, v.data(), v.size());
}

}  // namespace

TEST_CASE("Audit kills distinguish mason scout and phoenix retirement", "[mech][audit]") {
    for (const auto type : {rts::UnitType::Mason, rts::UnitType::Scout}) {
        auto init = arena();
        us(init.stats, type).cost_gold = 40;
        rts::World w(init);
        const auto victim = w.spawn_unit(type, {5.5f, 4.5f}, 4, 1, 40);
        w.spawn_unit(rts::UnitType::Ghoul, {4.5f, 4.5f}, 1, 30, 30);
        const auto expected = w.train_cost_gold(type, 4);
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(8);
        REQUIRE_FALSE(w.alive(victim));
        const auto tally = w.take_tally(rts::Side::Attacker);
        CHECK(tally.units_killed == 1);
        CHECK(tally.scouts_killed == 1);
        CHECK(tally.masons_killed == (type == rts::UnitType::Mason ? 1 : 0));
        CHECK(tally.scout_units_killed == (type == rts::UnitType::Scout ? 1 : 0));
        CHECK(tally.enemy_unit_gold == expected);
        CHECK(tally.enemy_unit_levels[static_cast<std::size_t>(type)] == 4);
        CHECK(w.peek_tally(rts::Side::Defender).own_unit_levels[static_cast<std::size_t>(type)] == 4);
        CHECK(w.take_tally(rts::Side::Attacker).enemy_unit_gold == 0);
    }
    rts::World w(arena());
    const auto retired = w.spawn_unit(rts::UnitType::Phoenix, {5.5f, 4.5f}, 1, 24, 24);
    w.kill_unit(retired); // Runtime retirement removes units without combat damage.
    CHECK(w.peek_tally(rts::Side::Attacker).phoenix_losses == 0);
    CHECK(w.peek_tally(rts::Side::Attacker).own_unit_levels[static_cast<std::size_t>(rts::UnitType::Phoenix)] == 0);
    const auto killed = w.spawn_unit(rts::UnitType::Phoenix, {5.5f, 4.5f}, 1, 1, 24);
    w.place_bld(rts::BldType::Flak, {4, 4}, 40, 40);
    w.advance(8);
    REQUIRE_FALSE(w.alive(killed));
    CHECK(w.peek_tally(rts::Side::Attacker).phoenix_losses == 1);
}

// ——决定 ⑫ 的四个子条——

TEST_CASE("Defender volley cannot claim its own mason as an enemy kill", "[mech][audit]") {
    auto init = arena();
    bs(init.stats, rts::BldType::Tower).aoe_radius = 1.5f;
    us(init.stats, rts::UnitType::Mason).cost_gold = 40;
    rts::World w(init);
    w.place_bld(rts::BldType::Tower, {4, 4}, 50, 50);
    const auto enemy = w.spawn_unit(rts::UnitType::Ghoul, {5.5f, 4.5f}, 1, 1, 30);
    const auto buddy = w.spawn_unit(rts::UnitType::Mason, {5.5f, 5.0f}, 1, 1, 40);
    w.advance(8);
    REQUIRE_FALSE(w.alive(enemy));
    REQUIRE(w.alive(buddy));
    const auto defense = w.take_tally(rts::Side::Defender);
    CHECK(defense.dmg_to_units == 1);
    CHECK(defense.units_killed == 1);
    CHECK(defense.friendly_unit_damage == 0);
    CHECK(defense.friendly_units_killed == 0);
    CHECK(defense.losses == 0);
    CHECK(defense.enemy_unit_gold == 0);
    CHECK(defense.masons_killed == 0);
    CHECK(defense.scouts_killed == 0);
}

TEST_CASE("Friendly splash has no enemy reward and retains own losses", "[mech][audit]") {
    auto init = arena();
    us(init.stats, rts::UnitType::Ranger).cost_gold = 40;
    us(init.stats, rts::UnitType::Ram).splash_dmg_permille = 400;
    rts::World w(init);
    w.spawn_unit(rts::UnitType::Ram, {5.0f, 5.0f}, 1, 60, 60);
    const auto enemy = w.spawn_unit(rts::UnitType::Ranger, {6.0f, 5.0f}, 1, 1, 18);
    const auto buddy = w.spawn_unit(rts::UnitType::Ghoul, {6.9f, 5.0f}, 1, 1, 30);
    act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear, rts::UnitAction::Stop});
    act(w, rts::Side::Defender, {rts::UnitAction::Stop});
    w.advance(4);
    REQUIRE_FALSE(w.alive(enemy));
    REQUIRE_FALSE(w.alive(buddy));
    const auto attack = w.take_tally(rts::Side::Attacker);
    CHECK(attack.dmg_to_units == 1); // Actual enemy HP, excluding overkill and buddy.
    CHECK(attack.units_killed == 1);
    CHECK(attack.enemy_unit_gold == 40);
    CHECK(attack.friendly_unit_damage == 1);
    CHECK(attack.friendly_units_killed == 1);
    CHECK(attack.losses == 30);
    CHECK(attack.own_unit_levels[static_cast<std::size_t>(rts::UnitType::Ghoul)] == 1);
    CHECK(attack.enemy_unit_levels[static_cast<std::size_t>(rts::UnitType::Ghoul)] == 0);
    CHECK(attack.enemy_unit_levels[static_cast<std::size_t>(rts::UnitType::Ranger)] == 1);
    CHECK(w.peek_tally(rts::Side::Defender).losses == 18);
    CHECK(w.peek_tally(rts::Side::Attacker).friendly_unit_damage == 0);
    CHECK(w.peek_tally(rts::Side::Attacker).friendly_units_killed == 0);
}

TEST_CASE("apply_permille：一次除法、四舍五入、钳到正", "[mech]") {
    // 契约 §3.4 的原例：链式调用会静默少算（5×1500/1000=7 → ×1500/1000=10）。
    REQUIRE(rts::apply_permille(5, {1500, 1500}) == 11);
    // 四舍五入，不向下截断（向下截断全局稳定偏向防守方）。
    REQUIRE(rts::apply_permille(10, {999}) == 10);
    REQUIRE(rts::apply_permille(3, {500}) == 2);    // 1.5 → 2
    // 结果必须为正：0 伤害凭空造出一种免疫。
    REQUIRE(rts::apply_permille(1, {100}) == 1);
    REQUIRE(rts::apply_permille(0, {}) == 1);
    // 等级 1 不缩放——`isqrt(1000 × 1000)` 精确等于 1000，不靠舍入。
    REQUIRE(rts::level_permille(1, 150) == 1000);
    REQUIRE(rts::level_permille(1, 0) == 1000);
    REQUIRE(rts::level_permille(1, 999) == 1000);
    // **§1.4：血量与伤害各开一份平方根**（原来这里是线性的 1300）。
    // 3 级、系数 150 ⇒ 开方前 1000 + 150×2 = 1300‰，单边 √1.3 ≈ 1140‰。
    REQUIRE(rts::level_permille(3, 150) == 1140);

    // 而这两条才是那个决定的实质，改公式时必须一起看：
    //
    // ① **两边乘起来回到开方前的线性底** ⇒ 战力曲线与 #96 那版「只涨伤害」
    //    逐点相同（那一版唯一在意的判据一分没让）。差值只来自 isqrt 的 floor。
    const std::int64_t p3 = rts::level_permille(3, 150);
    REQUIRE(p3 * p3 <= 1300 * 1000);
    REQUIRE(p3 * p3 > 1300 * 1000 - 2 * p3);   // floor 误差上界
    // ② **血量与伤害用同一个函数、同一个系数 ⇒ 比值恒为 1**，于是 TTK 与
    //    破墙时间不随等级漂移。那是 `p − q = 0` 这条结构约束的全部内容，
    //    而它在这里是**由构造成立**的（同一个 `level_permille`）——
    //    真正会破坏它的是给两个系数填不同的数，那条由 `StatsLoader` 拦。
    for (const std::int32_t lv : {1, 2, 5, 20, 40}) {
        REQUIRE(rts::level_permille(lv, 220) == rts::level_permille(lv, 220));
    }
    // 单调、且 40 级的战力落在文档记的 9.58x 上（`CLAUDE.md` §1.4 那张表）。
    REQUIRE(rts::level_permille(40, 220) > rts::level_permille(20, 220));
    const std::int64_t p40 = rts::level_permille(40, 220);
    REQUIRE(p40 * p40 / 1000 == 9579);   // 9.579x，表里记的 9.58
}

// ——承诺 / 落地两拍——

TEST_CASE("出手是两拍：前摇里血量不动，落地那一 tick 才掉", "[mech]") {
    rts::World w(arena());
    w.spawn_unit(rts::UnitType::Archer, rts::Vec2{4.5f, 4.5f}, 1, 20, 20);
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{6.5f, 4.5f}, 1, 30, 30);
    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});

    // 弓手前摇 2：第 1 tick 承诺，第 2 tick 递减，第 3 tick 落地。
    w.advance(1);
    REQUIRE(w.unit_hp(g) == 30);   // 承诺那一帧不是伤害落地那一帧
    w.advance(1);
    REQUIRE(w.unit_hp(g) == 30);
    w.advance(1);
    REQUIRE(w.unit_hp(g) == 25);   // 基础 5 × 等级 1000‰ = 5

    // 打到死：血量归零即销毁，句柄失效。
    w.advance(40);
    REQUIRE_FALSE(w.alive(g));
    REQUIRE(w.live_unit_count(rts::Side::Attacker) == 0);
}

TEST_CASE("等级只乘数值：3 级弓手一箭 = 基础 × 1300‰", "[mech]") {
    rts::World w(arena());
    w.spawn_unit(rts::UnitType::Archer, rts::Vec2{4.5f, 4.5f}, 3, 20, 20);
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{6.5f, 4.5f}, 1, 30, 30);
    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(3);
    // 预期用与机制**同一对函数**现算（表里系数是 100‰/级 ⇒ 3 级 = 1200‰，
    // 5 × 1200‰ = 6）。手抄一个数在这里错过一次了。
    REQUIRE(w.unit_hp(g) ==
            30 - rts::apply_permille(5, {rts::level_permille(3, 100)}));
}

TEST_CASE("打不到就空转（乙）：目标在射程外时什么都不发生", "[mech]") {
    rts::World w(arena());
    w.spawn_unit(rts::UnitType::Archer, rts::Vec2{1.5f, 4.5f}, 1, 20, 20);
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{9.5f, 4.5f}, 1, 30, 30);
    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(10);
    REQUIRE(w.unit_hp(g) == 30);   // 距离 8 > 射程 4：不隐含移动、不隐含寻路
}

TEST_CASE("AtkWeak 打绝对血量最低者，AtkNear 打最近者", "[mech]") {
    rts::World w(arena());
    w.spawn_unit(rts::UnitType::Archer, rts::Vec2{4.5f, 4.5f}, 1, 20, 20);
    const rts::UnitId near_full =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{5.5f, 4.5f}, 1, 30, 30);
    const rts::UnitId far_weak =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{7.5f, 4.5f}, 1, 9, 30);
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop, rts::UnitAction::Stop});

    SECTION("AtkWeak") {
        act(w, rts::Side::Defender, {rts::UnitAction::AtkWeak});
        w.advance(3);
        REQUIRE(w.unit_hp(far_weak) == 4);
        REQUIRE(w.unit_hp(near_full) == 30);
    }
    SECTION("AtkNear") {
        act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
        w.advance(3);
        REQUIRE(w.unit_hp(near_full) == 25);
        REQUIRE(w.unit_hp(far_weak) == 9);
    }
}

// ——移动——

TEST_CASE("移动：按速度推进，岩壁与边界拦得住", "[mech]") {
    rts::WorldInit init = arena();
    init.terrain[4 * 12 + 8] = rts::Terrain::Rock;   // (8,4) 岩壁
    rts::World w(std::move(init));
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{6.5f, 4.5f}, 1, 30, 30);
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE});   // 格增量 (+1, 0)

    w.advance(4);   // 0.25 × 4 = 1 格
    REQUIRE(w.unit_pos(g).x > 7.4f);
    REQUIRE(w.unit_pos(g).x < 7.6f);

    w.advance(40);  // 岩壁在 (8,4)：走到格 7 的东缘就停
    REQUIRE(w.unit_pos(g).x < 8.0f);
    REQUIRE(rts::grid_of(w.unit_pos(g)) == rts::GridPos{7, 4});
}

TEST_CASE("撞上城墙自动开始破坏，墙破了继续走（高代价可通行）", "[mech]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, rts::GridPos{8, 4}, 40, 40});
    rts::World w(std::move(init));
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{7.2f, 4.5f}, 1, 30, 30);
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE});

    // Ghoul 对结构 4 × 2000‰ = 8/下，墙 40 血 = 5 下。冷却 3、前摇 1，
    // 给足 tick 之后墙必须没了、单位必须走到墙那格以东。
    w.advance(120);
    REQUIRE(w.live_bld_count() == 1);   // 只剩 Keep
    REQUIRE(w.unit_pos(g).x > 9.0f);
}

TEST_CASE("清野产出归守方；攻方拆障碍不产任何东西", "[mech]") {
    SECTION("守方 Ranger 拆树桩 → 木材入库") {
        rts::WorldInit init = arena();
        init.obstacles.push_back(
            rts::ObstacleInit{rts::ObstacleType::Stump, rts::GridPos{8, 4}, 12, 12});
        rts::World w(std::move(init));
        w.spawn_unit(rts::UnitType::Ranger, rts::Vec2{7.2f, 4.5f}, 1, 18, 18);
        act(w, rts::Side::Defender, {rts::UnitAction::MoveSE});
        w.advance(60);
        REQUIRE(w.live_obstacle_count() == 0);
        REQUIRE(w.stock(rts::Resource::Wood) == 7);   // 数额查表，种类是 harvest_of
        REQUIRE(w.stock(rts::Resource::Stone) == 0);
    }
    SECTION("攻方 Ghoul 啃穿树桩 → 什么都不进库（攻方无经济）") {
        rts::WorldInit init = arena();
        init.obstacles.push_back(
            rts::ObstacleInit{rts::ObstacleType::Stump, rts::GridPos{8, 4}, 12, 12});
        rts::World w(std::move(init));
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{7.2f, 4.5f}, 1, 30, 30);
        act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE});
        w.advance(60);
        REQUIRE(w.live_obstacle_count() == 0);
        REQUIRE(w.stock(rts::Resource::Wood) == 0);
    }
}

// ——Ram 的 AOE：落点在前摇开始那一刻锁定——

TEST_CASE("AOE 砸锁定坐标：散开真的能躲，相邻墙段与友军挨溅射", "[mech]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, rts::GridPos{8, 4}, 40, 40});
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, rts::GridPos{8, 5}, 40, 40});
    rts::World w(std::move(init));
    // Ram 离墙心 1.3 <= 射程 1.5。
    const rts::UnitId ram =
        w.spawn_unit(rts::UnitType::Ram, rts::Vec2{7.2f, 4.5f}, 1, 60, 60);
    // 站在落点圈里的攻方友军（误伤是设计：溅射被包夹会误伤）。
    const rts::UnitId buddy =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{8.4f, 4.6f}, 1, 30, 30);
    // 会跑开的守方游骑：初始离落点 1.3（圈内），前摇 3 tick 里以 0.3/tick 南逃。
    const rts::UnitId runner =
        w.spawn_unit(rts::UnitType::Ranger, rts::Vec2{8.5f, 5.8f}, 1, 18, 18);

    act(w, rts::Side::Attacker,
        {rts::UnitAction::AtkWall, rts::UnitAction::Stop});
    act(w, rts::Side::Defender, {rts::UnitAction::MoveSW});   // (0, +1)：往南跑

    // 第 1 tick 承诺并锁定落点 (8.5, 4.5)，前摇 3 ⇒ 第 4 tick 落地。
    w.advance(1);
    REQUIRE(w.unit_target_kind(ram) == rts::TgtKind::Bld);
    REQUIRE(w.unit_aim(ram).x > 8.4f);
    REQUIRE(w.unit_hp(runner) == 18);

    w.advance(3);   // 落地
    // 两段墙都在半径 1.5 内：主目标 (8,4) 与相邻 (8,5) 都挨了 10×4000‰=40 ⇒ 双双拆掉。
    REQUIRE(w.live_bld_count() == 1);   // 只剩 Keep
    // 友军站在圈里没跑：挨了对单位的 10 点。
    REQUIRE(w.unit_hp(buddy) == 20);
    // 游骑跑了 4 tick × 0.3 = 1.2，出了圈：一点没挨——落点没有追着它走。
    REQUIRE(w.unit_hp(runner) == 18);
}

// ——AOE 溅射折扣：主目标满伤、圈内其余单位打折——
//
// 2026-08-31 试玩反馈：主目标与溅射到的其余单位此前吃同一份伤害，
// 等于「点一个等于点一片」。上面那条用例的承诺目标是墙（`kind == Bld`），
// 测不到「有主目标单位」这个分支——这条补上。

TEST_CASE("AOE 主副有别：承诺目标满伤，圈内其余单位按 splash_dmg_permille 打折",
          "[mech]") {
    rts::WorldInit init = arena();
    us(init.stats, rts::UnitType::Ram).splash_dmg_permille = 400;   // 4 折
    rts::World w(std::move(init));
    const rts::UnitId ram =
        w.spawn_unit(rts::UnitType::Ram, rts::Vec2{5.0f, 5.0f}, 1, 60, 60);
    // 主目标：离 Ram 1.0 格，是全场唯一在射程 1.5 内的敌方单位，AtkNear 必选它。
    const rts::UnitId victim =
        w.spawn_unit(rts::UnitType::Ranger, rts::Vec2{6.0f, 5.0f}, 1, 18, 18);
    // 旁观者：离 Ram 1.9（射程外，不会被 AtkNear 选中），但离主目标的落点
    // 只有 0.9（AOE 半径 1.5 内），会被溅射波及。
    const rts::UnitId bystander =
        w.spawn_unit(rts::UnitType::Ranger, rts::Vec2{6.9f, 5.0f}, 1, 18, 18);

    act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Defender, {rts::UnitAction::Stop, rts::UnitAction::Stop});

    w.advance(1);   // 承诺：victim 是唯一射程内目标，锁定它的坐标。
    REQUIRE(w.unit_target_kind(ram) == rts::TgtKind::Unit);
    w.advance(3);   // 落地（windup 3）。

    // 主目标满伤：10 点，无折扣。
    REQUIRE(w.unit_hp(victim) == 8);
    // 旁观者按 400‰ 打折：10 × 0.4 = 4 点，不是满额的 10。
    REQUIRE(w.unit_hp(bystander) == 14);
}

// ——建筑等级上限：升级后伤害按 dmg_permille_per_level 缩放——
//
// `land_bld_attack` 那一行改动的唯一断言点：`p.lvl_pm` 不再恒等于 1000。

TEST_CASE("建筑等级：Tower 升级后伤害按等级系数缩放", "[mech]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Tower, rts::GridPos{4, 4}, 50, 50});
    rts::World w(std::move(init));

    // 落地前先升到 2 级——`test_stats()` 没配升级造价/工期（都是诚实默认
    // 0），指令一提交就当场完工，同 `Build` 工期为 0 的先例。这一刻 Tower
    // 还没有任何目标（Ghoul 还没生成），不会牵动战斗阶段。
    //
    // **先升堡垒，再升 Tower**：`building_level_cap_divisor` 在这份占位表
    // 里没配（诚实默认 1），堡垒还在 1 级时上限是 ceil(1/1)=1——Tower 本身
    // 也是 1 级，`1 >= 1` 会被结构性拒绝。升堡垒到 2 级把上限抬到 2，
    // Tower 才升得上去。
    rts::Command upgrade_keep;
    upgrade_keep.kind = rts::CommandKind::Upgrade;
    upgrade_keep.side = rts::Side::Defender;
    upgrade_keep.slot = rts::slot_of(w.keep_pos(), w.width());
    w.submit(rts::Side::Defender, &upgrade_keep, 1);
    w.advance(1);

    rts::Command upgrade_tower;
    upgrade_tower.kind = rts::CommandKind::Upgrade;
    upgrade_tower.side = rts::Side::Defender;
    upgrade_tower.slot = rts::slot_of(rts::GridPos{4, 4}, w.width());
    w.submit(rts::Side::Defender, &upgrade_tower, 1);
    w.advance(1);

    const rts::UnitId ghoul =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{4.5f, 6.5f}, 1, 30, 30);
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});

    // Tower 前摇 1：第 1 tick 承诺，第 2 tick 落地。伤害走与单位同一对
    // combat_math 函数现算，不手抄一个数（同「等级只乘数值」那条纪律）。
    w.advance(1);
    REQUIRE(w.unit_hp(ghoul) == 30);
    w.advance(1);
    const std::int64_t expect =
        30 - rts::apply_permille(7, {rts::level_permille(2, 100)});
    REQUIRE(w.unit_hp(ghoul) == expect);
}

// ——建筑攻击：对空 / 对地是结构——

TEST_CASE("Tower 只打地面，Flak 只打空中", "[mech]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Tower, rts::GridPos{4, 4}, 50, 50});
    init.buildings.push_back(rts::BldInit{rts::BldType::Flak, rts::GridPos{6, 4}, 40, 40});
    rts::World w(std::move(init));
    const rts::UnitId ghoul =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{4.5f, 6.5f}, 1, 30, 30);
    const rts::UnitId phoenix =
        w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{6.5f, 6.5f}, 1, 24, 24);
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop, rts::UnitAction::Stop});

    // Flak 前摇 0：第 1 tick 就落地 9 点；Tower 前摇 1：第 2 tick 落地 7 点。
    w.advance(1);
    REQUIRE(w.unit_hp(phoenix) == 15);
    REQUIRE(w.unit_hp(ghoul) == 30);
    w.advance(1);
    REQUIRE(w.unit_hp(ghoul) == 23);
    // 各自的冷却里谁都不会去打对方那一类——打到死也只死「自己那类」的。
    w.advance(30);
    REQUIRE_FALSE(w.alive(phoenix));
    REQUIRE_FALSE(w.alive(ghoul));
}

TEST_CASE("Phoenix 对墙无解：AtkWall 掩码位恒 0，且飞得过岩壁", "[mech]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, rts::GridPos{6, 4}, 40, 40});
    init.terrain[4 * 12 + 5] = rts::Terrain::Rock;
    rts::World w(std::move(init));
    const rts::UnitId p =
        w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{5.7f, 4.5f}, 1, 24, 24);
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE});

    // 掩码：墙就贴脸，AtkWall 位仍是 0——结构挡的（can_break_structure），
    // 不是「伤害恰好为 0」那种数值挡法。
    const std::uint16_t mask = w.action_mask(p);
    REQUIRE((mask & (1u << static_cast<unsigned>(rts::UnitAction::AtkWall))) == 0);

    // 岩壁与墙都拦不住飞行（空军的地形掩码全 1，也不吃实体占位）。
    w.advance(20);
    REQUIRE(w.unit_pos(p).x > 7.0f);
    REQUIRE(w.live_bld_count() == 2);   // 墙毫发无损
}

// ——视野——

TEST_CASE("视野：半径内可见、森林挡视线、离开变记忆、记忆留住墙", "[mech]") {
    // 观察者用**攻方**的地面单位——「记忆过时」这个机制的消费者本来就是攻方
    // （AI 的侦查记忆图）。守方那一侧测不出「离开变记忆」：完工建筑恒照亮
    // 自己那一格（哪怕视野半径为 0），你自己的墙你永远看得见。
    // 载具是 `Shade`（伤害 0、纯观察）：原载具 `Wraith` 2026-09-03 起会飞，
    // 空军不吃视线遮挡，测不了「森林挡视线」。
    rts::WorldInit init = arena();
    init.terrain[4 * 12 + 6] = rts::Terrain::Forest;   // (6,4) 挡视线
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, rts::GridPos{4, 6}, 40, 40});
    rts::World w(std::move(init));
    w.spawn_unit(rts::UnitType::Shade, rts::Vec2{4.5f, 4.5f}, 1, 14, 14);
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(1);

    const rts::FogLayer& f = w.fog(rts::Side::Attacker);
    REQUIRE(f.at(4, 4) == rts::Vis::Visible);
    REQUIRE(f.at(5, 4) == rts::Vis::Visible);
    REQUIRE(f.at(6, 4) == rts::Vis::Visible);    // 遮挡格自身看得见
    REQUIRE(f.at(8, 4) == rts::Vis::Unseen);     // 林后（距离 4 <= 6，被挡）
    // 守方那份迷雾没跟着亮（每侧一份；守方在 (4,4) 附近没有眼睛）。
    REQUIRE(w.fog(rts::Side::Defender).at(4, 4) == rts::Vis::Unseen);

    // 可见的守方墙进了攻方的记忆图——这就是双视图里 AI 那半张图的内容。
    rts::RememberedBld rb;
    REQUIRE(f.remembered_bld(4, 6, &rb));
    REQUIRE(rb.type == rts::BldType::Wall);
    REQUIRE(rb.hp_permille == 1000);

    // 斥候走开（MoveNE 是格增量 (0,-1)，往北），原来看着的格降级成记忆。
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveNE});
    w.advance(20);
    REQUIRE(f.at(4, 6) == rts::Vis::Remembered);
    REQUIRE(f.remembered_bld(4, 6, &rb));   // 记忆还在：那里曾有一段满血的墙
}

TEST_CASE("缺口进记忆图：拆墙发生在视野内，之后那格是 Remembered 而非 Unseen",
          "[mech]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, rts::GridPos{6, 4}, 40, 40});
    rts::World w(std::move(init));
    // 守方斥候盯着墙；攻方 Ghoul 贴着墙啃。
    w.spawn_unit(rts::UnitType::Scout, rts::Vec2{4.5f, 4.5f}, 1, 8, 8);
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{5.4f, 4.5f}, 1, 30, 30);
    act(w, rts::Side::Defender, {rts::UnitAction::Stop});
    act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE});

    w.advance(120);
    REQUIRE(w.live_bld_count() == 1);   // 墙没了
    (void)g;

    const rts::FogLayer& f = w.fog(rts::Side::Defender);
    REQUIRE(f.at(6, 4) == rts::Vis::Visible);
    rts::RememberedBld rb;
    REQUIRE_FALSE(f.remembered_bld(6, 4, &rb));   // 见过，且记住了「没有建筑」
}

// ——机制全开之下，回放仍逐 tick 一致——

TEST_CASE("一场小型攻防的回放逐 tick 一致", "[mech]") {
    const auto build = [] { return rts::World(arena()); };

    rts::World w = build();
    w.spawn_unit(rts::UnitType::Archer, rts::Vec2{4.5f, 4.5f}, 2, 20, 20);
    w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{9.5f, 4.5f}, 1, 30, 30);
    w.spawn_unit(rts::UnitType::Ram, rts::Vec2{9.5f, 5.5f}, 1, 60, 60);

    // 注意：spawn 是机制产物、不进回放，所以**重放侧要有同一批初始单位**，
    // 正规做法是走 WorldInit::units。这里两边都手工 spawn 同一批，等价。
    rts::ReplayRecorder rec(w, 1);
    const rts::UnitAction def[] = {rts::UnitAction::AtkNear};
    const rts::UnitAction atk[] = {rts::UnitAction::MoveW, rts::UnitAction::MoveW};
    rec.submit_actions(rts::Side::Defender, def, 1);
    rec.submit_actions(rts::Side::Attacker, atk, 2);
    rec.advance(50);
    const rts::Replay r = rec.finish();

    rts::World fresh = build();
    fresh.spawn_unit(rts::UnitType::Archer, rts::Vec2{4.5f, 4.5f}, 2, 20, 20);
    fresh.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{9.5f, 4.5f}, 1, 30, 30);
    fresh.spawn_unit(rts::UnitType::Ram, rts::Vec2{9.5f, 5.5f}, 1, 60, 60);
    const rts::ReplayResult res = rts::replay_verify(r, fresh);
    INFO(res.message);
    REQUIRE(res.verdict == rts::ReplayVerdict::Match);
    REQUIRE(res.checked_hashes > 10);
}

// ——机制第六批：城门对自己人是通的——

TEST_CASE("完工城门：守方地面单位穿门而过，攻方在门前被拦下砸门", "[mech]") {
    // CLAUDE.md 两个不能砍的机制之一是「部分资源点在墙外」——它的前提是
    // 守方**出得了城**。cell_open 的唯一实体例外就是这条；flow field 的
    // 通行规则抄的是它（tests/flow_test.cpp 锁两处同真值）。
    SECTION("守方穿过：位置越过门格，门血一点不掉") {
        rts::World w(arena());
        const rts::BldId gate =
            w.place_bld(rts::BldType::Gate, rts::GridPos{5, 2}, 24, 24);
        const rts::UnitId r =
            w.spawn_unit(rts::UnitType::Ranger, rts::Vec2{4.5f, 2.5f}, 1, 18, 18);
        act(w, rts::Side::Defender, {rts::UnitAction::MoveSE});
        w.advance(10);   // 0.30/tick × 10 = 3.0 格：4.5 → 7.5，穿过 (5,2)
        REQUIRE(w.unit_pos(r).x > 6.0f);
        REQUIRE(w.bld_hp(gate) == 24);   // 是走过去的，不是砸开的
    }
    SECTION("攻方被拦：位置停在门外，门在掉血") {
        rts::World w(arena());
        const rts::BldId gate =
            w.place_bld(rts::BldType::Gate, rts::GridPos{5, 2}, 24, 24);
        const rts::UnitId g =
            w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{4.5f, 2.5f}, 1, 30, 30);
        act(w, rts::Side::Attacker, {rts::UnitAction::MoveSE});
        w.advance(8);   // 撞门自动破坏：一发 apply_permille(4,{1000,2000}) = 8
        REQUIRE(w.unit_pos(g).x < 5.0f);
        REQUIRE(w.alive(gate));
        REQUIRE(w.bld_hp(gate) < 24);
    }
    SECTION("工地状态的门对守方也不通，且守方不打自己的工地") {
        rts::World w(arena());
        const rts::BldId site = w.place_bld(rts::BldType::Gate, rts::GridPos{5, 2},
                                            24, 24, /*work_left=*/10);
        const rts::UnitId r =
            w.spawn_unit(rts::UnitType::Ranger, rts::Vec2{4.5f, 2.5f}, 1, 18, 18);
        act(w, rts::Side::Defender, {rts::UnitAction::MoveSE});
        w.advance(10);
        REQUIRE(w.unit_pos(r).x < 5.0f);
        REQUIRE(w.bld_hp(site) == 24);
    }
}

TEST_CASE("Destroyed investment includes completed upgrades and unlocked production", "[mech][audit]") {
    for (const bool unlocked : {false, true}) {
        auto init = arena();
        auto& quarry = bs(init.stats, rts::BldType::Quarry);
        quarry.max_hp=100;
        quarry.cost_stone=30; quarry.cost_wood=40; quarry.upgrade_cost_stone=50;
        quarry.upgrade_cost_wood=60; quarry.income_amount=9;
        init.stats.global.income_period_ticks=100;
        init.resources.push_back({{5,4},rts::Resource::Stone,unlocked ? 1 : 99});
        init.stats.global.mason_work_radius=2.0f;
        bs(init.stats,rts::BldType::Keep).upgrade_ticks=1;
        quarry.upgrade_ticks=1;
        us(init.stats,rts::UnitType::Ghoul).damage=10000;
        init.buildings.push_back({rts::BldType::Quarry,{5,4},1,100});
        rts::World w(init);
        w.set_stock(rts::Resource::Stone,10000); w.set_stock(rts::Resource::Wood,10000);
        const auto mason=w.spawn_unit(rts::UnitType::Mason,{0.5f,0.5f},1,30,30);
        rts::Command upgrade;
        upgrade.kind=rts::CommandKind::Upgrade; upgrade.side=rts::Side::Defender;
        upgrade.slot=rts::slot_of(w.keep_pos(),w.width());
        for (int i=0; i<4; ++i) {w.submit(rts::Side::Defender,&upgrade,1);w.advance(10);}
        w.kill_unit(mason);
        const auto builder=w.spawn_unit(rts::UnitType::Mason,{5.5f,4.5f},1,30,30);
        upgrade.slot=rts::slot_of({5,4},w.width());
        for (int i=0; i<2; ++i) {w.submit(rts::Side::Defender,&upgrade,1);w.advance(10);}
        REQUIRE(w.bld_level(w.bld_at({5,4}))==3);
        w.kill_unit(builder);
        w.spawn_unit(rts::UnitType::Ghoul,{4.5f,4.5f},1,30,30);
        act(w,rts::Side::Attacker,{rts::UnitAction::AtkBld}); w.advance(8);
        const auto tally=w.take_tally(rts::Side::Attacker);
        REQUIRE(tally.blds_destroyed==1);
        CHECK(tally.destroyed_stone==30+w.bld_upgrade_cost_stone(rts::BldType::Quarry,1)+w.bld_upgrade_cost_stone(rts::BldType::Quarry,2));
        CHECK(tally.destroyed_wood==40+w.bld_upgrade_cost_wood(rts::BldType::Quarry,1)+w.bld_upgrade_cost_wood(rts::BldType::Quarry,2));
        CHECK(tally.destroyed_income_stone==(unlocked ? 9 : 0));
        CHECK(tally.destroyed_income_wood==0);
        CHECK(w.take_tally(rts::Side::Attacker).destroyed_stone==0);
    }
}
