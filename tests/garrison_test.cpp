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

// 提交登墙意愿：按 `enumerate_units(Defender)` 的顺序一人一愿（kNoSlot = 不想登）。
// 编队系统移除后，驻守**只有这一条通道**——没有 `Garrison` 命令了。
// 意愿是世界状态（进哈希），提交一次持续生效；「每拍重发」是脚本层的纪律
// （它要表达「保持现状」），不是世界会把它丢掉。
void wish(rts::World& w, std::initializer_list<std::uint16_t> wl) {
    std::vector<std::uint16_t> v(wl);
    w.submit_garrison_wishes(rts::Side::Defender, v.data(), v.size());
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

// ——登墙、钉住、下墙——

TEST_CASE("驻守：相邻单位自动登墙，延迟走完落位墙心，此后被钉住", "[garrison]") {
    rts::WorldInit init = arena();
    init.stats.global.garrison_mount_ticks = 4;
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);

    wish(w, {slot(kWall)});
    w.advance(1);
    // 意愿受理：单位开始爬，人还在地面。
    REQUIRE(w.unit_garrison(a) == slot(kWall));
    REQUIRE(w.unit_mount(a) == 4);
    REQUIRE(w.unit_pos(a).x == 5.5f);
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
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{4.5f, 4.5f}, 1, 30, 30});
    rts::World w(std::move(init));
    std::vector<rts::UnitId> foes;
    w.enumerate_units(rts::Side::Attacker, foes);
    const rts::UnitId g = foes[0];

    wish(w, {slot(kWall)});
    w.advance(1);   // 开始爬（mount = 6）
    act(w, rts::Side::Defender, {rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(6);   // 整段攀爬期：目标近在咫尺（距离 2 < 射程 4）也一箭不发
    REQUIRE(w.unit_hp(g) == 30);
    REQUIRE(w.unit_mount(defender_at(w, 0)) == 0);
    w.advance(2);   // 登顶后照常：承诺 + 落地
    REQUIRE(w.unit_hp(g) == 25);
}

TEST_CASE("清空意愿即下墙：落到第一个空邻格（纠偏语义的一半）", "[garrison]") {
    // 「下墙」不再是一条命令（`recall_units` 已删）：意愿与现状不一致即纠偏，
    // 意愿清空 ⇒ 下一拍世界放人。
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);

    wish(w, {slot(kWall)});
    w.advance(1);   // 零延迟：当场登顶
    REQUIRE(w.unit_garrison(a) == slot(kWall));

    wish(w, {rts::kNoSlot});
    w.advance(1);
    REQUIRE(w.unit_garrison(a) == rts::kNoSlot);
    // 第一个空邻格按规范扫描序（dj 外层、di 内层）是 (5,3)。
    REQUIRE(w.unit_pos(a).x == 5.5f);
    REQUIRE(w.unit_pos(a).y == 3.5f);
}

TEST_CASE("逐单位意愿：只撤一个人的意愿，只他一个人下墙", "[garrison]") {
    // 意愿是**逐单位**的输入（与动作同序同长度），这正是旧「单兵召回」
    // 的替代形态：没有编队概念，也就没有「牵连同编队」可言。
    rts::WorldInit init = arena();
    const rts::GridPos other{6, 2};
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, other, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 2.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    wish(w, {slot(kWall), slot(other)});
    w.advance(1);
    const rts::UnitId first = defender_at(w, 0);
    const rts::UnitId second = defender_at(w, 1);
    REQUIRE(w.unit_garrison(first) == slot(kWall));
    REQUIRE(w.unit_garrison(second) == slot(other));

    wish(w, {rts::kNoSlot, slot(other)});   // 只撤第一个人的
    w.advance(1);
    REQUIRE(w.unit_garrison(first) == rts::kNoSlot);
    REQUIRE(w.unit_garrison(second) == slot(other));
}

TEST_CASE("意愿改指别段墙即纠偏：先从旧墙下来，邻近就直接上新墙", "[garrison]") {
    // 「不一致即纠偏」的另一半：不是先撤意愿再发新意愿两拍走完，
    // 而是一拍内下旧墙、落邻格、恰好够着新墙就当场再登（零延迟表下）。
    rts::WorldInit init = arena();
    const rts::GridPos other{6, 2};
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, other, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);

    wish(w, {slot(kWall)});
    w.advance(1);
    REQUIRE(w.unit_garrison(a) == slot(kWall));

    wish(w, {slot(other)});
    w.advance(1);
    // 下墙落点是 kWall 的第一个空邻格 (5,3)，它与 (6,2) 相邻——同一拍的
    // 意愿受理随即把它放上新墙。
    REQUIRE(w.unit_garrison(a) == slot(other));
    REQUIRE(w.unit_pos(a).x == 6.5f);
    REQUIRE(w.unit_pos(a).y == 2.5f);
}

TEST_CASE("对空：只有登上墙段的 Archer 能锁定 Phoenix", "[garrison]") {
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{6.5f, 6.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Phoenix, rts::Vec2{7.5f, 4.5f}, 1, 24, 24});
    rts::World w(std::move(init));
    wish(w, {slot(kWall), rts::kNoSlot});   // 只有第一个弓手想登
    w.advance(1);
    const rts::UnitId wall_archer = defender_at(w, 0);
    const rts::UnitId ground_archer = defender_at(w, 1);

    REQUIRE(has(w.action_mask(wall_archer), rts::UnitAction::AtkNear));
    REQUIRE_FALSE(has(w.action_mask(ground_archer), rts::UnitAction::AtkNear));

    act(w, rts::Side::Defender,
        {rts::UnitAction::AtkNear, rts::UnitAction::AtkNear});
    act(w, rts::Side::Attacker, {rts::UnitAction::Stop});
    w.advance(8);
    std::vector<rts::UnitId> attackers;
    w.enumerate_units(rts::Side::Attacker, attackers);
    REQUIRE(attackers.size() == 1);
    REQUIRE(w.unit_hp(attackers[0]) < 24);

    // 墙掉到半血以下会失去既有高度优势，但人仍在墙上，对空许可不额外读墙血。
    rts::WorldInit broken = arena();
    broken.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 19, 40});
    broken.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    broken.units.push_back(
        rts::UnitInit{rts::UnitType::Phoenix, rts::Vec2{7.5f, 4.5f}, 1, 24, 24});
    rts::World damaged(std::move(broken));
    wish(damaged, {slot(kWall)});
    damaged.advance(1);
    REQUIRE(has(damaged.action_mask(defender_at(damaged, 0)),
                rts::UnitAction::AtkNear));
}

TEST_CASE("墙塌了：人落在缺口里，意愿随墙作废，行动恢复", "[garrison]") {
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);
    const rts::BldId wall = w.place_bld(rts::BldType::Wall, kWall, 40, 40);

    wish(w, {slot(kWall)});
    w.advance(1);
    REQUIRE(w.unit_garrison(a) == slot(kWall));

    w.destroy_bld(wall);
    // 不是「下墙」，是脚下的东西没了：不给延迟，人当场站在缺口那一格。
    // destroy_bld 顺手把指着那格墙的意愿也清了（否则下一拍对着废墟干等）。
    REQUIRE(w.unit_garrison(a) == rts::kNoSlot);
    REQUIRE(w.unit_pos(a).x == 6.5f);
    // 缺口可通行（CLAUDE.md），人也解了钉：能走了。
    act(w, rts::Side::Defender, {rts::UnitAction::MoveE});
    w.advance(4);
    REQUIRE(w.unit_pos(a).x > 6.5f);
}

TEST_CASE("一格一人；驻守者阵亡后，仍指着那段墙的相邻单位自动补位", "[garrison]") {
    // 旧编队时代这是「常设指令」的补位；现在是**意愿是持久输入**给的：
    // b 的意愿一直指着那段墙，格子空出来的那一拍 tick_garrison 就受理它。
    // 世界不持常设指令——「该不该补位」的判断从此在脚本（每拍重发意愿）。
    rts::WorldInit init = arena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{7.5f, 4.5f}, 1, 20, 20});
    rts::World w(std::move(init));
    const rts::UnitId a = defender_at(w, 0);
    const rts::UnitId b = defender_at(w, 1);

    wish(w, {slot(kWall), slot(kWall)});   // 两人都想登同一段
    w.advance(1);
    // 槽位序小的上墙，另一个留在地面——一格一人。
    REQUIRE(w.unit_garrison(a) == slot(kWall));
    REQUIRE(w.unit_garrison(b) == rts::kNoSlot);

    w.kill_unit(a);
    w.advance(1);
    REQUIRE(w.unit_garrison(b) == slot(kWall));
    REQUIRE(w.unit_pos(b).x == 6.5f);
}

TEST_CASE("登墙意愿只认完工的墙段：Gate 算墙；工地、空格、其他建筑不算",
          "[garrison]") {
    // 意愿逐 tick 重验（tick_garrison）：指着的那格必须是**完工**的
    // Wall/Gate，否则不登。脚本读的是迷雾下的记忆，它记得的墙可能已被拆
    // 或从未盖完——世界侧再挡一道，不把「记忆的时效」推给脚本去赌。
    SECTION("Gate：「墙段」的既有判据是 Wall‖Gate，门楼一样能站人") {
        rts::WorldInit init = arena();
        init.buildings.push_back(rts::BldInit{rts::BldType::Gate, kWall, 24, 24});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
        rts::World w(std::move(init));
        const rts::UnitId a = defender_at(w, 0);
        wish(w, {slot(kWall)});
        w.advance(1);
        REQUIRE(w.unit_garrison(a) == slot(kWall));
    }
    SECTION("工地：完工前不是墙，站上去没有「上」可言") {
        rts::WorldInit init = arena();
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
        rts::World w(std::move(init));
        const rts::UnitId a = defender_at(w, 0);
        // 一座还在施工的墙（work_left > 0 ⇒ 未完工）。
        w.place_bld(rts::BldType::Wall, kWall, 1, 40, /*work_left=*/10);
        wish(w, {slot(kWall)});
        w.advance(1);
        REQUIRE(w.unit_garrison(a) == rts::kNoSlot);
    }
    SECTION("空格与 Keep：不是墙") {
        // 空格：kWall 上什么都没有，意愿指着它什么也不会发生。
        rts::WorldInit init = arena();
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
        rts::World w(std::move(init));
        const rts::UnitId a = defender_at(w, 0);
        wish(w, {slot(kWall)});
        w.advance(1);
        REQUIRE(w.unit_garrison(a) == rts::kNoSlot);

        // Keep（(0,0)，arena 恒有）：不是墙段，贴在旁边也登不上去。
        rts::WorldInit init2 = arena();
        init2.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{1.5f, 0.5f}, 1, 20, 20});
        rts::World w2(std::move(init2));
        const rts::UnitId a2 = defender_at(w2, 0);
        wish(w2, {slot(rts::GridPos{0, 0})});
        w2.advance(1);
        REQUIRE(w2.unit_garrison(a2) == rts::kNoSlot);
    }
}

// ——高度优势——

TEST_CASE("高度优势：远程驻守吃射程加成，近战不吃，破损墙不给", "[garrison]") {
    SECTION("远程：墙上打得到地面打不到的目标") {
        rts::WorldInit init = arena();
        init.stats.global.high_ground_range_bonus = 2.0f;
        init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
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

        wish(w, {slot(kWall)});
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
            rts::UnitInit{rts::UnitType::Spear, rts::Vec2{5.5f, 4.5f}, 1, 24, 24});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{8.5f, 4.5f}, 1, 30, 30});
        rts::World w(std::move(init));
        std::vector<rts::UnitId> foes;
        w.enumerate_units(rts::Side::Attacker, foes);
        const rts::UnitId g = foes[0];

        wish(w, {slot(kWall)});
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
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{11.5f, 4.5f}, 1, 30, 30});
        rts::World w(std::move(init));
        std::vector<rts::UnitId> foes;
        w.enumerate_units(rts::Side::Attacker, foes);
        const rts::UnitId g = foes[0];

        wish(w, {slot(kWall)});
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
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{6.5f, 3.5f}, 1, 20, 20});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{5.5f, 4.5f}, 1, 30, 30});
        return init;
    };
    const auto run = [](rts::WorldInit init, int ticks) {
        rts::World w(std::move(init));
        wish(w, {slot(kWall)});
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
            rts::UnitInit{rts::UnitType::Archer, rts::Vec2{6.5f, 3.5f}, 1, 20, 20});
        init.units.push_back(
            rts::UnitInit{rts::UnitType::Phoenix, rts::Vec2{6.5f, 2.5f}, 1, 24, 24});
        rts::World w(std::move(init));
        const rts::UnitId a = defender_at(w, 0);

        wish(w, {slot(kWall)});
        w.advance(1);
        act(w, rts::Side::Defender, {rts::UnitAction::Stop});
        act(w, rts::Side::Attacker, {rts::UnitAction::AtkNear});
        w.advance(2);
        REQUIRE(w.unit_hp(a) == 20 - 8);   // 全额：高度优势挡不住从上面来的
    }
}

// ——契约与确定性——

TEST_CASE("登墙意愿是守方专属通道：错侧或长度不符都当场抛", "[garrison]") {
    // 攻方没有登墙手段（CLAUDE.md，结构）——这条通道对它不存在。
    // 长度检查与 `submit_actions` 同一条纪律：错一位就是每个单位拿到
    // 邻居的意愿，不报错、只是墙头上错了人。
    rts::WorldInit init = arena();
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Archer, rts::Vec2{3.5f, 3.5f}, 1, 20, 20});
    rts::World w(std::move(init));

    const std::uint16_t none = rts::kNoSlot;
    REQUIRE_THROWS_AS(w.submit_garrison_wishes(rts::Side::Attacker, &none, 0),
                      rts::ContractError);
    // 守方活单位是 1 个，长度必须是 1——多一个少一个都抛。
    REQUIRE_THROWS_AS(w.submit_garrison_wishes(rts::Side::Defender, &none, 0),
                      rts::ContractError);
    const std::uint16_t two[2] = {rts::kNoSlot, rts::kNoSlot};
    REQUIRE_THROWS_AS(w.submit_garrison_wishes(rts::Side::Defender, two, 2),
                      rts::ContractError);
    REQUIRE_NOTHROW(w.submit_garrison_wishes(rts::Side::Defender, &none, 1));
    // 越界的格下标同样当场抛（kNoSlot 是唯一合法的超格数值）。
    const std::uint16_t oob = static_cast<std::uint16_t>(12 * 8);
    REQUIRE_THROWS_AS(w.submit_garrison_wishes(rts::Side::Defender, &oob, 1),
                      rts::ContractError);
}

TEST_CASE("驻守状态逐字段进 state_hash（各自隔离验证）", "[garrison]") {
    // 「两个世界不同 ⇒ 哈希不同」的笼统写法在这里会假绿：`u_garrison_` 本来
    // 就在哈希里，漏喂**新**字段照样红不了。所以每个字段单独隔离——
    // 让两个世界的差异恰好只剩它。

    SECTION("u_garrison_target_（意愿本身是输入状态，进哈希）") {
        // 意愿指着一格**没有墙**的空地：逐 tick 重验把它拦下，什么都不会发生
        // ——于是两个世界的全部差异恰好是那份意愿本身（单位、位置、钱全同）。
        const auto fresh = [] {
            rts::WorldInit init = arena();
            init.units.push_back(rts::UnitInit{rts::UnitType::Archer,
                                               rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
            return init;
        };
        rts::World wa(fresh());
        rts::World wb(fresh());
        wish(wb, {slot(kWall)});
        wa.advance(1);
        wb.advance(1);
        REQUIRE(wa.unit_garrison(defender_at(wa, 0)) == rts::kNoSlot);   // 没登成
        REQUIRE(wb.unit_garrison(defender_at(wb, 0)) == rts::kNoSlot);   // 两边都没
        REQUIRE(wa.state_hash() != wb.state_hash());
    }

    SECTION("u_mount_") {
        // 同一份建局、同一条意愿，只是提交时刻差一 tick。推进到同一 tick 时
        // 两边都在爬、人都在地面原位、驻守格全同——唯独倒计时差 1。
        const auto fresh = [] {
            rts::WorldInit init = arena();
            init.stats.global.garrison_mount_ticks = 4;
            init.buildings.push_back(rts::BldInit{rts::BldType::Wall, kWall, 40, 40});
            init.units.push_back(rts::UnitInit{rts::UnitType::Archer,
                                               rts::Vec2{5.5f, 4.5f}, 1, 20, 20});
            return init;
        };
        rts::World wa(fresh());
        rts::World wb(fresh());
        wish(wb, {slot(kWall)});
        wb.advance(1);   // B 第 1 tick 起爬
        wa.advance(1);
        wish(wa, {slot(kWall)});
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
