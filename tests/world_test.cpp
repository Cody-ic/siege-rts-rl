// `World` / `WorldView` / 槽位池 / 掩码 / 状态哈希。
//
// 这一批里有相当一部分测的是**「本版刻意没做」这件事本身**：
// 六种命令被记账而不是被丢弃、攻击掩码默认允许、`advance()` 不是空操作。
// 那些不是占位测试——它们锁住的是「没做」与「静默吞掉」的区别，
// 而 1c 实现它们时正是要把这些断言翻过来。

#include <array>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/map_loader.hpp"
#include "game/world_builder.hpp"
#include "rts/action.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
#include "rts/terrain.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

namespace {

// 一张最小的能用地图：全平地，中间一座 Keep，两个集结点。
// **一切数值都是驱动测试用的占位值**（CLAUDE.md「关于数值」）。
rts::WorldInit tiny_init(int w = 5, int h = 4) {
    rts::WorldInit init;
    init.width = w;
    init.height = h;
    init.terrain.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                        rts::Terrain::Plain);
    init.keep = rts::GridPos{2, 2};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 10, 10});
    init.spawns.push_back(rts::SpawnSite{rts::GridPos{0, 0}});
    init.spawns.push_back(rts::SpawnSite{rts::GridPos{4, 3}});
    init.seed = 0x5EED;
    init.map_id = "tiny";
    init.nominal_level = 1;
    return init;
}

rts::Command cmd(rts::CommandKind k, rts::Side s) {
    rts::Command c;
    c.kind = k;
    c.side = s;
    return c;
}

bool has_bit(std::uint16_t mask, rts::UnitAction a) {
    return (mask & static_cast<std::uint16_t>(1u << static_cast<unsigned>(a))) != 0;
}

bool has_bit(std::uint16_t mask, rts::CommandKind k) {
    return (mask & static_cast<std::uint16_t>(1u << static_cast<unsigned>(k))) != 0;
}

}  // namespace

// ——建局校验——

TEST_CASE("建局参数不合法一律抛，而不是勉强建出一个坏世界", "[world]") {
    SECTION("尺寸为零") {
        rts::WorldInit init = tiny_init();
        init.width = 0;
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("terrain 长度与 w×h 不符") {
        rts::WorldInit init = tiny_init();
        init.terrain.pop_back();
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("no_build 长度不符") {
        rts::WorldInit init = tiny_init();
        init.no_build.assign(3, 0);
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("keep 越界") {
        rts::WorldInit init = tiny_init();
        init.keep = rts::GridPos{99, 99};
        init.buildings[0].pos = init.keep;
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("一座 Keep 都没有") {
        // 「丢失即败」这个败北条件否则无从表达，而不会有任何别处报错。
        rts::WorldInit init = tiny_init();
        init.buildings.clear();
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("两座 Keep") {
        rts::WorldInit init = tiny_init();
        init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 10, 10});
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("Keep 的位置与 WorldInit::keep 不一致") {
        rts::WorldInit init = tiny_init();
        init.buildings[0].pos = rts::GridPos{0, 0};
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("血量违反 0 < hp <= max_hp") {
        rts::WorldInit init = tiny_init();
        init.buildings[0].hp = 20;   // > max_hp
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("nominal_level 小于 kMinUnitLevel") {
        rts::WorldInit init = tiny_init();
        init.nominal_level = 0;
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
}

TEST_CASE("合法建局之后世界处在开局状态", "[world]") {
    rts::World w(tiny_init());
    REQUIRE(w.now() == 0);
    REQUIRE(w.wave() == 1);
    REQUIRE(w.phase() == rts::WavePhase::Build);
    REQUIRE(w.width() == 5);
    REQUIRE(w.height() == 4);
    REQUIRE(w.live_bld_count() == 1);         // 那座 Keep
    REQUIRE(w.live_unit_count() == 0);
    REQUIRE(w.live_obstacle_count() == 0);
    REQUIRE(w.spawns().size() == 2);
    REQUIRE_FALSE(w.spawn_chosen(0));
    REQUIRE(w.map_id() == "tiny");
    for (int i = 0; i < rts::kResourceCount; ++i) {
        REQUIRE(w.stock(rts::resource_at(i)) == 0);
    }
}

// ——句柄与槽位复用——

TEST_CASE("句柄在实体死后失效，槽位复用不会让旧句柄复活", "[world]") {
    rts::World w(tiny_init());
    const rts::UnitId a = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
    REQUIRE(w.alive(a));
    REQUIRE(w.unit_type(a) == rts::UnitType::Ghoul);
    REQUIRE(w.unit_side(a) == rts::Side::Attacker);

    w.kill_unit(a);
    REQUIRE_FALSE(w.alive(a));
    // 读一个失效句柄必须抛，而不是返回槽位里的残留值。
    REQUIRE_THROWS_AS(w.unit_hp(a), rts::ContractError);

    // 复用同一个槽位：下标相同、代数不同，于是旧句柄仍然是死的。
    const rts::UnitId b = w.spawn_unit(rts::UnitType::Shade, rts::Vec2{2.5f, 2.5f}, 1, 5, 5);
    REQUIRE(b.index() == a.index());
    REQUIRE(b.generation() != a.generation());
    REQUIRE(w.alive(b));
    REQUIRE_FALSE(w.alive(a));
    REQUIRE(w.unit_type(b) == rts::UnitType::Shade);
}

TEST_CASE("三组句柄互不相通", "[world]") {
    rts::World w(tiny_init());
    const rts::UnitId u = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
    const rts::BldId b = w.place_bld(rts::BldType::Wall, rts::GridPos{0, 0}, 3, 3);
    const rts::ObstacleId o =
        w.place_obstacle(rts::ObstacleType::Stump, rts::GridPos{1, 0}, 2, 2);
    // 下标可以撞（三组各是独立的扁平数组），但类型系统不让它们互相传递
    // ——那一条由 `tests/types_test.cpp` 的 static_assert 管。
    REQUIRE(w.alive(u));
    REQUIRE(w.alive(b));
    REQUIRE(w.alive(o));
    REQUIRE(w.live_bld_count() == 2);   // Keep + 这段墙
    REQUIRE(w.live_obstacle_count() == 1);
}

TEST_CASE("单位等级不得低于 kMinUnitLevel", "[world]") {
    // 观测的等级通道存**和**，「和 = 0 ⟺ 该格无敌人」依赖等级从 1 起。
    // 0 基等级会让那条通道静默失效，所以这里必须抛。
    rts::World w(tiny_init());
    REQUIRE_THROWS_AS(
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 0, 5, 5),
        rts::ContractError);
}

// ——输入——

TEST_CASE("命令的归属由 kind 决定，不由 side 字段决定", "[world]") {
    rts::World w(tiny_init());

    // 攻方下建造命令：格式上完全成立（字段都填了），语义非法。
    rts::Command build = cmd(rts::CommandKind::Build, rts::Side::Attacker);
    build.slot = 0;
    build.what = static_cast<std::uint8_t>(rts::BldType::Wall);
    REQUIRE_THROWS_AS(w.submit(rts::Side::Attacker, &build, 1), rts::ContractError);

    // 守方下编成配比同理。
    rts::Command comp = cmd(rts::CommandKind::Composition, rts::Side::Defender);
    comp.what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
    REQUIRE_THROWS_AS(w.submit(rts::Side::Defender, &comp, 1), rts::ContractError);

    // `None`（跳过）两侧都合法——它是每一侧那个兜底一定合法的动作。
    const rts::Command skip_d = cmd(rts::CommandKind::None, rts::Side::Defender);
    const rts::Command skip_a = cmd(rts::CommandKind::None, rts::Side::Attacker);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &skip_d, 1));
    REQUIRE_NOTHROW(w.submit(rts::Side::Attacker, &skip_a, 1));
}

TEST_CASE("Command::side 与 submit 的参数不一致要抛", "[world]") {
    // 回放读进来的字节可以是任何东西，所以这条必须查。
    rts::World w(tiny_init());
    const rts::Command c = cmd(rts::CommandKind::None, rts::Side::Attacker);
    REQUIRE_THROWS_AS(w.submit(rts::Side::Defender, &c, 1), rts::ContractError);
}

TEST_CASE("Composition 只能配比攻方单位，Train 只能产出守方单位", "[world]") {
    rts::World w(tiny_init());

    rts::Command comp = cmd(rts::CommandKind::Composition, rts::Side::Attacker);
    comp.what = static_cast<std::uint8_t>(rts::UnitType::Archer);   // 守方
    REQUIRE_THROWS_AS(w.submit(rts::Side::Attacker, &comp, 1), rts::ContractError);
    comp.what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
    comp.slot = 7;
    REQUIRE_NOTHROW(w.submit(rts::Side::Attacker, &comp, 1));

    rts::Command train = cmd(rts::CommandKind::Train, rts::Side::Defender);
    train.slot = 0;
    train.what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);   // 攻方
    REQUIRE_THROWS_AS(w.submit(rts::Side::Defender, &train, 1), rts::ContractError);
    train.what = static_cast<std::uint8_t>(rts::UnitType::Archer);
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &train, 1));
}

TEST_CASE("槽位越界要抛，且集结点用的是集结点下标而不是格下标", "[world]") {
    rts::World w(tiny_init());   // 5×4 = 20 格，2 个集结点

    rts::Command repair = cmd(rts::CommandKind::Repair, rts::Side::Defender);
    repair.slot = 20;            // 格数正好越界
    REQUIRE_THROWS_AS(w.submit(rts::Side::Defender, &repair, 1), rts::ContractError);
    repair.slot = 19;
    REQUIRE_NOTHROW(w.submit(rts::Side::Defender, &repair, 1));

    rts::Command pick = cmd(rts::CommandKind::PickSpawn, rts::Side::Attacker);
    pick.slot = 2;               // 只有 2 个集结点（0、1）
    REQUIRE_THROWS_AS(w.submit(rts::Side::Attacker, &pick, 1), rts::ContractError);
    pick.slot = 1;
    REQUIRE_NOTHROW(w.submit(rts::Side::Attacker, &pick, 1));
}

TEST_CASE("一批命令是全有或全无", "[world]") {
    // 半批入队会让「这次提交失败了」与「前几条生效了」不可区分，
    // 而回放里两者的后果完全不同。
    rts::World w(tiny_init());
    std::array<rts::Command, 2> batch{};
    batch[0] = cmd(rts::CommandKind::Composition, rts::Side::Attacker);
    batch[0].what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
    batch[0].slot = 5;
    batch[1] = cmd(rts::CommandKind::PickSpawn, rts::Side::Attacker);
    batch[1].slot = 99;   // 越界，整批都不该入队

    REQUIRE_THROWS_AS(w.submit(rts::Side::Attacker, batch.data(), 2), rts::ContractError);
    w.advance(1);
    REQUIRE(w.composition(rts::UnitType::Ghoul) == 0);
}

TEST_CASE("动作数组长度必须恰好等于该侧活着的单位数", "[world]") {
    // 长度错位的症状是「每个单位都拿到邻居的动作」——不报错、不崩、只是学不动。
    // 这条检查是 `submit_actions` 存在的主要理由。
    rts::World w(tiny_init());
    w.spawn_unit(rts::UnitType::Archer, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);   // 守方
    w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{2.5f, 1.5f}, 1, 5, 5);
    w.spawn_unit(rts::UnitType::Shade, rts::Vec2{3.5f, 1.5f}, 1, 5, 5);

    REQUIRE(w.live_unit_count() == 3);
    REQUIRE(w.live_unit_count(rts::Side::Attacker) == 2);
    REQUIRE(w.live_unit_count(rts::Side::Defender) == 1);

    const std::array<rts::UnitAction, 3> three{rts::UnitAction::MoveN,
                                               rts::UnitAction::MoveS,
                                               rts::UnitAction::Stop};
    // 攻方只有 2 个，给 3 个要抛；给 1 个也要抛。
    REQUIRE_THROWS_AS(w.submit_actions(rts::Side::Attacker, three.data(), 3),
                      rts::ContractError);
    REQUIRE_THROWS_AS(w.submit_actions(rts::Side::Attacker, three.data(), 1),
                      rts::ContractError);
    REQUIRE_NOTHROW(w.submit_actions(rts::Side::Attacker, three.data(), 2));
}

TEST_CASE("动作按槽位升序配给该侧单位，与 enumerate_units 同序", "[world]") {
    rts::World w(tiny_init());
    const rts::UnitId d = w.spawn_unit(rts::UnitType::Archer, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
    const rts::UnitId a0 = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{2.5f, 1.5f}, 1, 5, 5);
    const rts::UnitId a1 = w.spawn_unit(rts::UnitType::Shade, rts::Vec2{3.5f, 1.5f}, 1, 5, 5);

    std::vector<rts::UnitId> order;
    w.enumerate_units(rts::Side::Attacker, order);
    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == a0);
    REQUIRE(order[1] == a1);

    const std::array<rts::UnitAction, 2> acts{rts::UnitAction::AtkWall,
                                              rts::UnitAction::MoveNW};
    w.submit_actions(rts::Side::Attacker, acts.data(), 2);
    REQUIRE(w.unit_action(a0) == rts::UnitAction::AtkWall);
    REQUIRE(w.unit_action(a1) == rts::UnitAction::MoveNW);
    // 守方那一个没被碰到——新单位的默认动作是 Stop。
    REQUIRE(w.unit_action(d) == rts::UnitAction::Stop);
}

TEST_CASE("动作枚举值越界要抛，且整批不生效", "[world]") {
    rts::World w(tiny_init());
    const rts::UnitId a = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
    const std::array<rts::UnitAction, 1> bad{
        static_cast<rts::UnitAction>(rts::kUnitActionCount)};
    REQUIRE_THROWS_AS(w.submit_actions(rts::Side::Attacker, bad.data(), 1),
                      rts::ContractError);
    REQUIRE(w.unit_action(a) == rts::UnitAction::Stop);
}

// ——advance 与命令应用——

TEST_CASE("submit 只入队，advance 才应用", "[world]") {
    // 决定 ① 的两段式：人类输入异步到达、RL 一次提交一批，两者走同一条路。
    rts::World w(tiny_init());
    rts::Command comp = cmd(rts::CommandKind::Composition, rts::Side::Attacker);
    comp.what = static_cast<std::uint8_t>(rts::UnitType::Knight);
    comp.slot = 12;

    w.submit(rts::Side::Attacker, &comp, 1);
    REQUIRE(w.composition(rts::UnitType::Knight) == 0);   // 还没 advance
    w.advance(1);
    REQUIRE(w.composition(rts::UnitType::Knight) == 12);
    REQUIRE(w.now() == 1);
}

TEST_CASE("PickSpawn 是置位而不是赋值——分兵佯攻要能选多个", "[world]") {
    rts::World w(tiny_init());
    std::array<rts::Command, 2> both{};
    both[0] = cmd(rts::CommandKind::PickSpawn, rts::Side::Attacker);
    both[0].slot = 0;
    both[1] = cmd(rts::CommandKind::PickSpawn, rts::Side::Attacker);
    both[1].slot = 1;
    w.submit(rts::Side::Attacker, both.data(), 2);
    w.advance(1);
    REQUIRE(w.spawn_chosen(0));
    REQUIRE(w.spawn_chosen(1));
    REQUIRE_THROWS_AS(w.spawn_chosen(2), rts::ContractError);
}

TEST_CASE("Summon 提前结束建造阶段", "[world]") {
    // CLAUDE.md 明写「必须提供『提前召唤下一波』」——TAB 的一个明确缺陷是
    // 玩家经济起飞后只能干等。
    rts::World w(tiny_init());
    REQUIRE(w.phase() == rts::WavePhase::Build);
    REQUIRE(has_bit(w.command_mask(rts::Side::Defender), rts::CommandKind::Summon));

    const rts::Command s = cmd(rts::CommandKind::Summon, rts::Side::Defender);
    w.submit(rts::Side::Defender, &s, 1);
    w.advance(1);
    REQUIRE(w.phase() == rts::WavePhase::Assault);
    // 已经在进攻阶段，这条命令不再有意义，掩码要关掉它。
    REQUIRE_FALSE(has_bit(w.command_mask(rts::Side::Defender), rts::CommandKind::Summon));
}

TEST_CASE("六种需要数值的命令被记账，而不是静默丢弃", "[world]") {
    // 「本版没解算」与「悄悄吞掉」的区别就在这个计数器上。
    // 1c 实现它们时把 `deferred_command_count` 一并删掉。
    rts::World w(tiny_init());
    const std::array<rts::CommandKind, 6> deferred{
        rts::CommandKind::Build,     rts::CommandKind::Repair,
        rts::CommandKind::Cancel,    rts::CommandKind::Train,
        rts::CommandKind::MoveForce, rts::CommandKind::Garrison};

    for (const rts::CommandKind k : deferred) {
        rts::Command c = cmd(k, rts::Side::Defender);
        c.slot = 1;
        c.force = 0;
        c.what = static_cast<std::uint8_t>(
            k == rts::CommandKind::Train ? rts::UnitType::Archer : rts::UnitType::Archer);
        if (k == rts::CommandKind::Build) {
            c.what = static_cast<std::uint8_t>(rts::BldType::Tower);
        }
        w.submit(rts::Side::Defender, &c, 1);
    }
    w.advance(1);
    for (const rts::CommandKind k : deferred) {
        REQUIRE(w.deferred_command_count(k) == 1);
    }
    // 能完整应用的那几种不该被记进这里。
    REQUIRE(w.deferred_command_count(rts::CommandKind::None) == 0);
    REQUIRE(w.deferred_command_count(rts::CommandKind::Summon) == 0);
    REQUIRE(w.deferred_command_count(rts::CommandKind::PickSpawn) == 0);
}

TEST_CASE("advance 递减前摇与施工进度，到 0 就停", "[world]") {
    // 这两个计数器不需要任何数值就能推进（初值来自数值表，递减不来自任何表），
    // 所以本版做完。也正因为有它们，`advance()` 不是空操作。
    rts::World w(tiny_init());
    const rts::BldId site = w.place_bld(rts::BldType::Tower, rts::GridPos{0, 0}, 1, 8, 3);
    REQUIRE_FALSE(w.bld_complete(site));

    w.advance(2);
    REQUIRE_FALSE(w.bld_complete(site));
    w.advance(1);
    REQUIRE(w.bld_complete(site));
    w.advance(5);
    REQUIRE(w.bld_complete(site));   // 不会减成负数
    REQUIRE(w.now() == 8);
}

TEST_CASE("advance(0) 是合法空操作，负数要抛", "[world]") {
    rts::World w(tiny_init());
    const std::uint64_t before = w.state_hash();
    REQUIRE_NOTHROW(w.advance(0));
    REQUIRE(w.state_hash() == before);
    REQUIRE(w.now() == 0);
    REQUIRE_THROWS_AS(w.advance(-1), rts::ContractError);
}

TEST_CASE("波次推进重置集结点选择但保留编成位", "[world]") {
    rts::World w(tiny_init());
    rts::Command pick = cmd(rts::CommandKind::PickSpawn, rts::Side::Attacker);
    pick.slot = 0;
    rts::Command comp = cmd(rts::CommandKind::Composition, rts::Side::Attacker);
    comp.what = static_cast<std::uint8_t>(rts::UnitType::Ram);
    comp.slot = 3;
    w.submit(rts::Side::Attacker, &pick, 1);
    w.submit(rts::Side::Attacker, &comp, 1);
    w.advance(1);
    w.begin_assault();

    w.begin_next_wave(4);
    REQUIRE(w.wave() == 2);
    REQUIRE(w.phase() == rts::WavePhase::Build);
    REQUIRE(w.nominal_level() == 4);
    REQUIRE_FALSE(w.spawn_chosen(0));            // 逐波重新决定
    REQUIRE(w.composition(rts::UnitType::Ram) == 3);   // 由命令重设，不在这里清
    REQUIRE_THROWS_AS(w.begin_next_wave(0), rts::ContractError);
}

// ——状态哈希——

TEST_CASE("同种子同指令流给同一个哈希", "[world]") {
    auto run = []() {
        rts::World w(tiny_init());
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 2, 7, 9);
        rts::Command comp = cmd(rts::CommandKind::Composition, rts::Side::Attacker);
        comp.what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
        comp.slot = 6;
        w.submit(rts::Side::Attacker, &comp, 1);
        w.advance(3);
        return w.state_hash();
    };
    REQUIRE(run() == run());
}

TEST_CASE("改一条命令哈希就变", "[world]") {
    auto run = [](std::uint16_t weight) {
        rts::World w(tiny_init());
        rts::Command comp = cmd(rts::CommandKind::Composition, rts::Side::Attacker);
        comp.what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
        comp.slot = weight;
        w.submit(rts::Side::Attacker, &comp, 1);
        w.advance(1);
        return w.state_hash();
    };
    REQUIRE(run(6) != run(7));
}

TEST_CASE("哈希绑定地图身份", "[world]") {
    // 6.3：「地图改一个格子，旧回放就会静默偏离」。让 content_hash 与地形位图
    // 都进状态哈希，那种偏离在**第 0 个 tick** 就能被发现。
    rts::WorldInit a = tiny_init();
    rts::WorldInit b = tiny_init();
    b.map_content_hash[0] = 0x01;
    REQUIRE(rts::World(std::move(a)).state_hash() !=
            rts::World(std::move(b)).state_hash());

    rts::WorldInit c = tiny_init();
    rts::WorldInit d = tiny_init();
    d.terrain[3] = rts::Terrain::Rock;
    REQUIRE(rts::World(std::move(c)).state_hash() !=
            rts::World(std::move(d)).state_hash());

    rts::WorldInit e = tiny_init();
    rts::WorldInit f = tiny_init();
    f.map_id = "tiny2";
    REQUIRE(rts::World(std::move(e)).state_hash() !=
            rts::World(std::move(f)).state_hash());
}

TEST_CASE("空闲表的顺序进哈希——否则分叉会被推迟发现", "[world]") {
    // 两个世界的活实体集合、代数、全部字段都相同，只有「空闲槽的排队顺序」不同。
    // 而那个顺序决定下一次 spawn 落在哪个槽，于是它们从下一次 spawn 起分叉。
    auto build = [](bool kill_in_order) {
        rts::World w(tiny_init());
        const rts::UnitId a = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
        const rts::UnitId b = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{2.5f, 1.5f}, 1, 5, 5);
        const rts::UnitId c = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{3.5f, 1.5f}, 1, 5, 5);
        (void)a;
        if (kill_in_order) {
            w.kill_unit(b);
            w.kill_unit(c);
        } else {
            w.kill_unit(c);
            w.kill_unit(b);
        }
        return w;
    };
    rts::World in_order = build(true);
    rts::World reversed = build(false);

    REQUIRE(in_order.live_unit_count() == reversed.live_unit_count());
    REQUIRE(in_order.state_hash() != reversed.state_hash());

    // 而分叉确实是真的：下一次 spawn 落在不同的槽。
    const rts::UnitId x = in_order.spawn_unit(rts::UnitType::Shade, rts::Vec2{0.5f, 0.5f}, 1, 5, 5);
    const rts::UnitId y = reversed.spawn_unit(rts::UnitType::Shade, rts::Vec2{0.5f, 0.5f}, 1, 5, 5);
    REQUIRE(x.index() != y.index());
}

TEST_CASE("死掉的槽位不把残留带进哈希", "[world]") {
    // 销毁时把字段清成规范值，于是「走到同一状态的两个世界」哈希相同。
    // 不清的话回放测试会变成偶发失败——最难查的一种。
    rts::World a(tiny_init());
    const rts::UnitId ua = a.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 3, 9, 9);
    a.kill_unit(ua);

    rts::World b(tiny_init());
    const rts::UnitId ub = b.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{3.5f, 2.5f}, 7, 4, 4);
    b.kill_unit(ub);

    REQUIRE(a.state_hash() == b.state_hash());
}

TEST_CASE("迷雾进哈希", "[world]") {
    rts::World a(tiny_init());
    rts::World b(tiny_init());
    REQUIRE(a.state_hash() == b.state_hash());

    b.fog_mut(rts::Side::Attacker).mark_visible(1, 1, 0);
    REQUIRE(a.state_hash() != b.state_hash());

    // 两侧的迷雾是分开的：给守方标同一格，得到的是第三个值。
    rts::World c(tiny_init());
    c.fog_mut(rts::Side::Defender).mark_visible(1, 1, 0);
    REQUIRE(c.state_hash() != b.state_hash());
    REQUIRE(c.state_hash() != a.state_hash());
}

// ——掩码——

TEST_CASE("Stop 位恒为 1，攻击四位默认允许", "[world]") {
    // 掩码里 0 通常是「兜底一定合法的那一个」。停住永远合法。
    // 攻击位默认**允许**：错误地允许只是浪费样本，错误地禁止会让 agent
    // 永远学不到那个动作，而后者不会有任何东西提示。
    rts::WorldInit init = tiny_init(3, 3);
    for (rts::Terrain& t : init.terrain) t = rts::Terrain::Rock;
    init.terrain[1 * 3 + 1] = rts::Terrain::Plain;   // 只有正中间能站
    init.keep = rts::GridPos{1, 1};
    init.buildings.clear();
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 10, 10});
    init.spawns.clear();

    rts::World w(std::move(init));
    const rts::UnitId u = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
    const std::uint16_t mask = w.action_mask(u);

    REQUIRE(has_bit(mask, rts::UnitAction::Stop));
    REQUIRE(has_bit(mask, rts::UnitAction::AtkNear));
    REQUIRE(has_bit(mask, rts::UnitAction::AtkWeak));
    REQUIRE(has_bit(mask, rts::UnitAction::AtkBld));
    REQUIRE(has_bit(mask, rts::UnitAction::AtkWall));
    // 八面都是岩壁，一个移动方向都不该开。
    for (int d = 0; d < rts::kMoveDirCount; ++d) {
        REQUIRE_FALSE(has_bit(mask, rts::move_of(d)));
    }
}

TEST_CASE("空中单位不受地形限制", "[world]") {
    // Phoenix 的唯一克制手段是位置性的防空，地形不参与。
    rts::WorldInit init = tiny_init(3, 3);
    for (rts::Terrain& t : init.terrain) t = rts::Terrain::Rock;
    init.terrain[1 * 3 + 1] = rts::Terrain::Plain;
    init.keep = rts::GridPos{1, 1};
    init.buildings.clear();
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 10, 10});
    init.spawns.clear();

    rts::World w(std::move(init));
    const rts::UnitId p = w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
    const std::uint16_t mask = w.action_mask(p);
    for (int d = 0; d < rts::kMoveDirCount; ++d) {
        REQUIRE(has_bit(mask, rts::move_of(d)));
    }
}

TEST_CASE("斜向穿角：格坐标的斜向要两个正交邻格都通", "[world]") {
    // **注意「斜向」指格坐标的斜向，也就是屏幕上的正北东南西**——按名字猜会恰好
    // 猜反（`rts/action.hpp` 文件头那张表）。所以这里走 `is_grid_diagonal`。
    //
    // 3×3 全平地，只把 (2,1) 改成岩壁。单位站 (1,1)：
    //   * `MoveSE` 是格轴向，目标就是 (2,1) ⇒ 被地形挡住
    //   * `MoveS` 是格斜向，目标 (2,2) 可通行，但正交邻格之一 (2,1) 不通
    //     ⇒ kDiagonalNeedsBothOrthogonal 为真时也要被挡
    rts::WorldInit init = tiny_init(3, 3);
    init.terrain[1 * 3 + 2] = rts::Terrain::Rock;
    init.keep = rts::GridPos{0, 0};
    init.buildings.clear();
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 10, 10});
    init.spawns.clear();

    rts::World w(std::move(init));
    const rts::UnitId u = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
    const std::uint16_t mask = w.action_mask(u);

    REQUIRE(rts::move_delta(rts::UnitAction::MoveSE) == rts::GridDelta{+1, 0});
    REQUIRE_FALSE(rts::is_grid_diagonal(rts::UnitAction::MoveSE));
    REQUIRE(rts::is_grid_diagonal(rts::UnitAction::MoveS));

    REQUIRE_FALSE(has_bit(mask, rts::UnitAction::MoveSE));   // 目标就是岩壁
    REQUIRE(rts::kDiagonalNeedsBothOrthogonal);
    REQUIRE_FALSE(has_bit(mask, rts::UnitAction::MoveS));    // 穿角被禁
    // 另一侧不受影响。
    REQUIRE(has_bit(mask, rts::UnitAction::MoveNW));
    REQUIRE(has_bit(mask, rts::UnitAction::MoveN));
}

TEST_CASE("命令掩码只开本侧能下的", "[world]") {
    rts::World w(tiny_init());
    const std::uint16_t d = w.command_mask(rts::Side::Defender);
    const std::uint16_t a = w.command_mask(rts::Side::Attacker);

    REQUIRE(has_bit(d, rts::CommandKind::Build));
    REQUIRE_FALSE(has_bit(d, rts::CommandKind::Composition));
    REQUIRE(has_bit(a, rts::CommandKind::Composition));
    REQUIRE(has_bit(a, rts::CommandKind::PickSpawn));
    REQUIRE_FALSE(has_bit(a, rts::CommandKind::Build));
    // `None`（跳过）两侧都开——每一侧都要有一个兜底合法的动作。
    REQUIRE(has_bit(d, rts::CommandKind::None));
    REQUIRE(has_bit(a, rts::CommandKind::None));
}

// ——只读视图——

TEST_CASE("WorldView 的数组长度是槽位数，含空槽", "[world]") {
    // 读错的症状是「偶尔渲染出一个血量为 0 的幽灵单位」：槽位会被复用，
    // 死掉的单位留下的洞就在数组中间，不能假设前 N 个活着。
    rts::World w(tiny_init());
    const rts::UnitId a = w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{1.5f, 1.5f}, 1, 5, 5);
    w.spawn_unit(rts::UnitType::Shade, rts::Vec2{2.5f, 1.5f}, 1, 5, 5);
    w.kill_unit(a);

    const rts::WorldView v = w.view(rts::Side::Attacker);
    REQUIRE(v.unit_alive().size() == 2);
    REQUIRE(v.unit_type().size() == v.unit_alive().size());
    REQUIRE(v.unit_pos().size() == v.unit_alive().size());
    REQUIRE(v.unit_alive()[0] == 0);
    REQUIRE(v.unit_alive()[1] != 0);
    REQUIRE(w.live_unit_count() == 1);

    REQUIRE(v.side() == rts::Side::Attacker);
    REQUIRE(v.now() == w.now());
    REQUIRE(v.wave() == w.wave());
    REQUIRE(v.width() == w.width());
    REQUIRE(v.stock().size() == static_cast<std::size_t>(rts::kResourceCount));
}

TEST_CASE("WorldView 给的是本侧的迷雾", "[world]") {
    rts::World w(tiny_init());
    w.fog_mut(rts::Side::Attacker).mark_visible(1, 1, 0);

    REQUIRE(w.view(rts::Side::Attacker).fog().at(1, 1) == rts::Vis::Visible);
    REQUIRE(w.view(rts::Side::Defender).fog().at(1, 1) == rts::Vis::Unseen);
}

// ——坐标与槽位编码——

TEST_CASE("格心不是格角", "[world]") {
    // 拿 (i, j) 当格心会让每个单位稳定偏半格，而半格偏移在等距投影下看起来
    // 像「精灵锚点没对准」——第一反应会去查渲染。
    REQUIRE(rts::center_of(rts::GridPos{3, 5}) == rts::Vec2{3.5f, 5.5f});
    REQUIRE(rts::grid_of(rts::Vec2{3.5f, 5.5f}) == rts::GridPos{3, 5});
    // 一格覆盖 [i, i+1)：左闭右开。
    REQUIRE(rts::grid_of(rts::Vec2{3.0f, 5.0f}) == rts::GridPos{3, 5});
    REQUIRE(rts::grid_of(rts::Vec2{3.999f, 5.999f}) == rts::GridPos{3, 5});
    REQUIRE(rts::grid_of(rts::Vec2{4.0f, 6.0f}) == rts::GridPos{4, 6});
    // floor 而不是向零取整：−0.5 属于格 −1。
    REQUIRE(rts::grid_of(rts::Vec2{-0.5f, -0.5f}) == rts::GridPos{-1, -1});
}

TEST_CASE("槽位编码是格线性下标，两向可逆", "[world]") {
    const int w = 7;
    for (std::int16_t y = 0; y < 5; ++y) {
        for (std::int16_t x = 0; x < 7; ++x) {
            const rts::GridPos p{x, y};
            REQUIRE(rts::pos_of_slot(rts::slot_of(p, w), w) == p);
        }
    }
    // 与「无槽位」哨兵不能撞——这条由 World 构造时的格数上界检查兜住。
    REQUIRE(rts::slot_of(rts::GridPos{6, 4}, 7) != rts::kNoSlot);
}

TEST_CASE("格数不得触到 kNoSlot", "[world]") {
    // 地图边长待标定；256×256 = 65536 正好越过 uint16 的可用范围。
    // 所以这条是运行期检查，不是一条注释。
    rts::WorldInit init;
    init.width = 256;
    init.height = 256;
    init.terrain.assign(256u * 256u, rts::Terrain::Plain);
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 10, 10});
    init.nominal_level = 1;
    REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
}

// ——与真实地图对接——

TEST_CASE("夹具地图能装配成一个合法的世界", "[world]") {
    // 这条测的不是 `make_world_init` 的细节，而是**契约装得下它真正的消费者**。
    // `rts_core` 的接口目前全部由同一个人写、也全部由同一个人消费，
    // 一个能跑通的适配层是这个问题唯一能自己动手补的部分。
    const game::MapData map =
        game::MapLoader::from_file(std::string(GAME_TESTDATA_DIR) + "/fixture_min.json");
    const game::InitialHp hp{100, 200, 150};
    rts::WorldInit init = game::make_world_init(map, hp, 99, 1);

    REQUIRE(init.width == 7);
    REQUIRE(init.height == 5);
    REQUIRE(init.map_id == "fixture_min");
    REQUIRE(init.spawns.size() == 2);
    REQUIRE(init.resources.size() == 4);
    // 6 段初始墙 + 合成出来的那一座 Keep（地图文件里没有它）。
    REQUIRE(init.buildings.size() == 7);

    rts::World w(std::move(init));
    REQUIRE(w.live_bld_count() == 7);
    REQUIRE(w.keep_pos() == rts::GridPos{1, 1});
    // 地形没被转置：(1,0) 是岩壁、(3,0) 是水（terrain rows[0] = "0123300"）。
    REQUIRE(w.terrain().at(1, 0) == rts::Terrain::Rock);
    REQUIRE(w.terrain().at(3, 0) == rts::Terrain::Water);
    REQUIRE_FALSE(w.terrain().passable(3, 0));
    REQUIRE_FALSE(w.terrain().blocks_vision(3, 0));   // 水挡路不挡视野
    // no_build rows[3] = "0000010" ⇒ (5,3) 不可建造，尽管它是平地。
    REQUIRE(w.terrain().at(5, 3) == rts::Terrain::Plain);
    REQUIRE_FALSE(w.terrain().buildable(5, 3));
}

TEST_CASE("残血比例乘成绝对血量，四舍五入且不塌成 0", "[world]") {
    // 夹具里有 hp_frac 0.45 与 0.8 两段（初始城圈是**残破**的，2.3）。
    const game::MapData map =
        game::MapLoader::from_file(std::string(GAME_TESTDATA_DIR) + "/fixture_min.json");
    const game::InitialHp hp{100, 200, 150};
    const rts::WorldInit init = game::make_world_init(map, hp, 0, 1);

    std::set<std::int64_t> wall_hps;
    for (const rts::BldInit& b : init.buildings) {
        if (b.type == rts::BldType::Wall) wall_hps.insert(b.hp);
        // 一律满足 World 的前置条件，否则构造会抛。
        REQUIRE(b.hp > 0);
        REQUIRE(b.hp <= b.max_hp);
    }
    REQUIRE(wall_hps.count(200) == 1);   // hp_frac 1.0
    REQUIRE(wall_hps.count(90) == 1);    // 0.45 × 200
    REQUIRE(wall_hps.count(160) == 1);   // 0.8 × 200
}

TEST_CASE("采集建筑与资源两向可逆", "[world]") {
    // 只写一个方向的话，另一侧会在调用处手写一个 if/else 副本，而那是漂移的起点。
    for (int i = 0; i < rts::kResourceCount; ++i) {
        const rts::Resource r = rts::resource_at(i);
        const rts::BldType b = rts::gatherer_of(r);
        REQUIRE(rts::is_gatherer(b));
        REQUIRE(rts::resource_of(b) == r);
    }
    int gatherers = 0;
    for (int i = 0; i < rts::kBldTypeCount; ++i) {
        if (rts::is_gatherer(rts::bld_at(i))) ++gatherers;
    }
    REQUIRE(gatherers == rts::kResourceCount);
    // 金矿场是金币的唯一来源（除 Keep 那条地板），所以这一对不能错。
    REQUIRE(rts::gatherer_of(rts::Resource::Gold) == rts::BldType::Mine);
}
