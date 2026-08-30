// 数值表：进入路径与指纹。
//
// 这一批测试防的是 `rts_core 接口契约.md` §1.1.2 查出的那个洞的**回归**：
// 数值表是外生输入，改一个数字必须让回放以「数值表变了」的名义失效，
// 而不是以「第 N tick 状态不一致」的名义——后者会让人去查一个不存在的
// 确定性缺陷。所以三层各测一条：
//
//   * 指纹本身：值变则变、值同则同（它是「变没变」的唯一判据）
//   * `state_hash`：表变了，**第 0 tick** 就分歧（早报）
//   * `replay_verify`：分歧被翻译成 `StatsMismatch`，不落进 `Diverged`（诊断）
//
// 载入器那几条测的是另一件事：**JSON 里的手误不得静默落回默认值**
// （漏一个兵种 / 拼错一个键的症状会是「能跑但打不动」，离病因极远）。

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/stats_loader.hpp"
#include "rts/replay.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace {

// 与 replay_test.cpp 的 demo_init 同族：一张最小地图 + 一个单位。
// 数值全是驱动测试的占位。
rts::WorldInit tiny_init(const rts::StatsTable& stats) {
    rts::WorldInit init;
    init.width = 4;
    init.height = 3;
    init.terrain.assign(12, rts::Terrain::Plain);
    init.keep = rts::GridPos{1, 1};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 10, 10});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{2.5f, 1.5f}, 1, 12, 12});
    init.seed = 7;
    init.map_id = "stats-demo";
    init.nominal_level = 1;
    init.stats = stats;
    return init;
}

// 一份「每格都不同于默认值」的表，用来确认指纹真的读到了每个分组。
rts::StatsTable filled_table() {
    rts::StatsTable t;
    for (std::size_t k = 0; k < t.unit.size(); ++k) {
        t.unit[k].max_hp = 100 + static_cast<std::int64_t>(k);
        t.unit[k].damage = 10 + static_cast<std::int64_t>(k);
        t.unit[k].range = 1.0f + static_cast<float>(k);
        t.unit[k].speed = 0.1f;
        t.unit[k].vision = 6.0f;
        t.unit[k].windup_ticks = 4;
        t.unit[k].cooldown_ticks = 20;
        t.unit[k].vs_structure_permille = 500;
    }
    for (std::size_t k = 0; k < t.bld.size(); ++k) {
        t.bld[k].max_hp = 500 + static_cast<std::int64_t>(k);
    }
    for (std::size_t k = 0; k < t.obstacle.size(); ++k) {
        t.obstacle[k].max_hp = 60;
        t.obstacle[k].yield_amount = 20;
    }
    t.global.hp_permille_per_level = 150;
    t.global.dmg_permille_per_level = 150;
    return t;
}

}  // namespace

// ——指纹——

TEST_CASE("数值表指纹：值同则同，任一格变则变", "[stats]") {
    const rts::StatsTable a = filled_table();
    const rts::StatsTable b = filled_table();
    REQUIRE(a.fingerprint() == b.fingerprint());

    // 四个分组各改一格，都必须被指纹看到——漏喂一个分组的症状正是
    // 「那一组改了而指纹不变」，即本文件要防的洞换了个位置。
    {
        rts::StatsTable c = filled_table();
        c.unit[3].damage += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    {
        rts::StatsTable c = filled_table();
        c.bld[7].max_hp += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    {
        rts::StatsTable c = filled_table();
        c.obstacle[2].yield_amount += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    {
        rts::StatsTable c = filled_table();
        c.global.dmg_permille_per_level += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    // 第二批新增的字段也要被指纹看到——每个结构抽一格。改造价而指纹不动，
    // 意味着「涨价后的表」能对上「涨价前录的回放」，正是本文件要防的洞。
    {
        rts::StatsTable c = filled_table();
        c.unit[1].cost_gold += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    {
        rts::StatsTable c = filled_table();
        c.bld[2].cost_stone += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    {
        rts::StatsTable c = filled_table();
        c.global.income_period_ticks += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    // 第三批（驻守与高度优势）的四个全局字段。整数抽一格、浮点单独一格——
    // 浮点走的是 feed_f32 那条按位喂入的路，漏喂时症状与整数那组不同。
    {
        rts::StatsTable c = filled_table();
        c.global.high_ground_miss_permille += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    {
        rts::StatsTable c = filled_table();
        c.global.high_ground_range_bonus += 0.5f;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    // 第四批（冲锋与齐射）：建筑的 AOE 半径与冲锋参数各抽一格。
    {
        rts::StatsTable c = filled_table();
        c.bld[3].aoe_radius += 0.5f;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    {
        rts::StatsTable c = filled_table();
        c.global.anti_charge_permille += 1;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    // 第五批（在途弹丸）：两张子表各自的弹丸速度。改它而指纹不动，意味着
    // 「箭变快后的表」能对上「变快前录的回放」——同 cost 那组的洞。
    {
        rts::StatsTable c = filled_table();
        c.unit[0].proj_speed += 0.25f;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
    {
        rts::StatsTable c = filled_table();
        c.bld[4].proj_speed += 0.25f;
        REQUIRE(c.fingerprint() != a.fingerprint());
    }
}

TEST_CASE("表变了，第 0 tick 的 state_hash 就分歧（早报）", "[stats]") {
    rts::StatsTable a = filled_table();
    rts::StatsTable b = filled_table();
    b.unit[0].damage += 1;   // 「平衡性调整：弓手伤害 +1」——正是最会被漏掉的那种改动

    const rts::World wa(tiny_init(a));
    const rts::World wb(tiny_init(b));
    const rts::World wa2(tiny_init(a));

    REQUIRE(wa.state_hash() == wa2.state_hash());
    REQUIRE(wa.state_hash() != wb.state_hash());
    REQUIRE(wa.stats_fingerprint() == a.fingerprint());
}

// ——回放诊断——

TEST_CASE("回放对上数值表不同的世界：报 StatsMismatch，不报跑歪了", "[stats]") {
    const rts::StatsTable a = filled_table();
    rts::StatsTable b = filled_table();
    b.unit[0].damage += 1;

    // 用表 A 录一段。
    rts::World w(tiny_init(a));
    rts::ReplayRecorder rec(w, 1);
    rec.advance(3);
    const rts::Replay r = rec.finish();

    // 用表 B 验证：**当场拦**，与 MapMismatch 同一契约——两局跑在不同的规则下，
    // 比对没有意义。verdict 不允许落进 Diverged，那正是本字段要消掉的误判。
    {
        rts::World fresh(tiny_init(b));
        const rts::ReplayResult res = rts::replay_verify(r, fresh);
        REQUIRE(res.verdict == rts::ReplayVerdict::StatsMismatch);
        REQUIRE_FALSE(res.stats_matches);
        REQUIRE_FALSE(res.ok());
    }
    // 用表 A 验证：一切照旧。
    {
        rts::World fresh(tiny_init(a));
        const rts::ReplayResult res = rts::replay_verify(r, fresh);
        REQUIRE(res.verdict == rts::ReplayVerdict::Match);
        REQUIRE(res.stats_matches);
    }
}

TEST_CASE("指纹经字节往返不丢", "[stats]") {
    rts::World w(tiny_init(filled_table()));
    rts::ReplayRecorder rec(w, 1);
    rec.advance(1);
    const rts::Replay r = rec.finish();

    const std::vector<unsigned char> bytes = r.to_bytes();
    rts::Replay back;
    std::string err;
    REQUIRE(rts::Replay::from_bytes(bytes.data(), bytes.size(), &back, &err));
    REQUIRE(back.stats_fp() == r.stats_fp());
    REQUIRE(back.stats_fp() == w.stats_fingerprint());
}

// ——载入器——

TEST_CASE("占位数值表能载入，且铺满花名册", "[stats]") {
    const rts::StatsTable t = game::StatsLoader::from_file(
        std::string(GAME_DATA_DIR) + "/stats_placeholder.json");

    // 逐格 >= 1 由载入器保证；这里抽查几条「结构约束的占位实现」没有被写反。
    for (const rts::UnitStats& s : t.unit) REQUIRE(s.max_hp >= 1);
    for (const rts::BldStats& s : t.bld) REQUIRE(s.max_hp >= 1);

    // `Ram` 的压倒性拆除效率是设计（结构上要求它显著高于步兵）。
    const rts::UnitStats& ram = t.of(rts::UnitType::Ram);
    const rts::UnitStats& ghoul = t.of(rts::UnitType::Ghoul);
    REQUIRE(ram.vs_structure_permille > ghoul.vs_structure_permille);
    // `Flak` 的视野否定半径 > 伤害半径（CLAUDE.md「空中单位」，结构性不等号）。
    const rts::BldStats& flak = t.of(rts::BldType::Flak);
    REQUIRE(flak.vision > flak.range);
    // 无战力三单位伤害为 0（与 is_combat 一致的诚实默认）。
    REQUIRE(t.of(rts::UnitType::Scout).damage == 0);
    REQUIRE(t.of(rts::UnitType::Mason).damage == 0);
    REQUIRE(t.of(rts::UnitType::Wraith).damage == 0);
    // 门比墙薄（既定薄弱点的占位实现）。
    REQUIRE(t.of(rts::BldType::Gate).max_hp < t.of(rts::BldType::Wall).max_hp);
    // `Keep` 的金币地板存在（数额待定，**存在**是结构——它兜的是「金矿被点掉
    // 后连补兵都做不到」那种死亡螺旋，CLAUDE.md 单列一节）。
    REQUIRE(t.of(rts::BldType::Keep).income_amount >= 1);
    // 三座采集建筑都有产出（收入的唯一来源）。
    REQUIRE(t.of(rts::BldType::Quarry).income_amount >= 1);
    REQUIRE(t.of(rts::BldType::Lumber).income_amount >= 1);
    REQUIRE(t.of(rts::BldType::Mine).income_amount >= 1);
}

TEST_CASE("载入器：手误不得静默落回默认值", "[stats]") {
    // 一份最小合法表当底稿：从占位表出发改坏一处，比手搭一份完整 JSON 稳。
    const std::string path = std::string(GAME_DATA_DIR) + "/stats_placeholder.json";
    const rts::StatsTable good = game::StatsLoader::from_file(path);
    (void)good;

    // 漏一个兵种：报错点名 `units.Ram`，不是「能跑但 Ram 打不动」。
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(R"({
        "schema": "stats/5",
        "units": {}, "buildings": {}, "obstacles": {}, "global": {}
    })"),
                      game::StatsFormatError);

    // schema 不认识——包括全部旧格（旧表缺新批字段，静默补默认值正是
    // 「能跑但打不动」那种坑，所以刻意不做向后兼容）。
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(R"({"schema": "stats/999"})"),
                      game::StatsFormatError);
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(R"({"schema": "stats/1"})"),
                      game::StatsFormatError);
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(R"({"schema": "stats/2"})"),
                      game::StatsFormatError);
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(R"({"schema": "stats/3"})"),
                      game::StatsFormatError);
    REQUIRE_THROWS_AS(game::StatsLoader::from_string(R"({"schema": "stats/4"})"),
                      game::StatsFormatError);

    // 认不出的键（拼错）：`cooldown_tick` 少个 s。静默忽略的话它落回默认值 1。
    REQUIRE_THROWS_AS(
        game::StatsLoader::from_string(R"({
        "schema": "stats/5",
        "units": { "Archer": { "max_hp": 1, "damage": 0, "range": 0, "speed": 0,
                               "vision": 0, "windup_ticks": 0, "cooldown_tick": 5,
                               "vs_structure_permille": 0, "aoe_radius": 0, "proj_speed": 0,
                               "cost_gold": 0, "train_ticks": 0 } },
        "buildings": {}, "obstacles": {}, "global": {}
    })"),
        game::StatsFormatError);

    // 下界：max_hp 0 会让 spawn 恒抛，在载入这一层就拦。
    // （新字段要写全——否则先撞上的是「缺少字段」，测的就不是下界了。）
    REQUIRE_THROWS_AS(
        game::StatsLoader::from_string(R"({
        "schema": "stats/5",
        "units": { "Archer": { "max_hp": 0, "damage": 0, "range": 0, "speed": 0,
                               "vision": 0, "windup_ticks": 0, "cooldown_ticks": 1,
                               "vs_structure_permille": 0, "aoe_radius": 0, "proj_speed": 0,
                               "cost_gold": 0, "train_ticks": 0 } },
        "buildings": {}, "obstacles": {}, "global": {}
    })"),
        game::StatsFormatError);
}
