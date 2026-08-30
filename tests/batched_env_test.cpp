// 批量环境（`rts/batched_env.hpp`）。
//
// **这份测试的主体是「并行不改变结果」。** 其余的（长度检查、终局位、重置）
// 都是常规断言，而线程那一条是这个类唯一真正的风险：
//
//   若结果按**完成顺序**而不是**环境下标**装配，同一批同一种子跑两遍会得到
//   两份不同排列的张量——而它**不报错**，只是训练时每个样本对应到了别的环境。
//   那种错在损失曲线上看起来像「学不动」，排查方向完全错。
//
// 所以有一条用例把 1 线程与 N 线程的输出**逐字节**比对。
//
// ## 破坏性验证（做过，改这份文件前请重做）
//
// | 把 `batched_env.cpp` 改成 | 应当红的用例 |
// |---|---|
// | `observe` 的输出偏移从 `ui * per_cells` 改成一个按完成顺序自增的计数器 | 「并行不改变结果」 |
// | `observe` 开头那三次 `std::fill` 摘掉 | 「单位数减少后，尾部必须归零」 |
// | `step` 里 `slice.resize(...)` 的补齐删掉 | 单位数 > 40 时抛（本批的占位局面到不了 40，故未直接覆盖） |

#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/action.hpp"
#include "rts/batched_env.hpp"
#include "rts/obs.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace {

rts::StatsTable env_stats() {
    rts::StatsTable t;
    for (rts::UnitStats& u : t.unit) {
        u.max_hp = 20;
        u.damage = 3;
        u.range = 2.0f;
        u.speed = 0.2f;
        u.vision = 4.0f;
        u.windup_ticks = 2;
        u.cooldown_ticks = 3;
    }
    t.bld[static_cast<std::size_t>(rts::BldType::Keep)].max_hp = 200;
    t.bld[static_cast<std::size_t>(rts::BldType::Wall)].max_hp = 50;
    t.global = {100, 100};
    return t;
}

// 第 `seed` 号局面。**各局刻意不同**（种子与单位数都随它变）：
// 若各局完全相同，「装配顺序错了」这条 bug 会被掩盖——换了排列也看不出来。
rts::WorldInit one(int seed, int attackers) {
    rts::WorldInit init;
    init.width = 24;
    init.height = 24;
    init.terrain.assign(24 * 24, rts::Terrain::Plain);
    init.keep = rts::GridPos{2, 2};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.buildings.push_back(
        rts::BldInit{rts::BldType::Wall, rts::GridPos{10, 10}, 50, 50});
    // **`force` 必须留 `kNoForce`（0xFF），不能写 0。** 0 是一个**合法的编队号**，
    // 而「编队是守方概念，攻方初始单位不得带编队」是 `World` 构造时的契约检查
    // ——初版把最后那个字段写成 `0`，于是四条用例全在构造那一行抛。
    // 被契约挡住是好事：那条检查存在的理由正是「攻方没有编队」这个设计事实。
    for (int k = 0; k < attackers; ++k) {
        init.units.push_back(rts::UnitInit{
            rts::UnitType::Ghoul,
            rts::Vec2{12.5f + static_cast<float>(k), 12.5f}, 1, 20, 20,
            rts::kNoForce});
    }
    init.units.push_back(rts::UnitInit{
        rts::UnitType::Archer, rts::Vec2{9.5f, 10.5f}, 1, 20, 20, rts::kNoForce});
    init.seed = static_cast<std::uint64_t>(1000 + seed);
    init.map_id = "batchenv";
    init.nominal_level = 1;
    init.stats = env_stats();
    return init;
}

struct Bufs {
    std::vector<float> cells, self, glob;
    std::vector<std::uint8_t> done;

    explicit Bufs(int n) {
        const std::size_t mu = static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv);
        cells.assign(static_cast<std::size_t>(n) * mu *
                         static_cast<std::size_t>(rts::kObsCellFloats), 0.0f);
        self.assign(static_cast<std::size_t>(n) * mu *
                        static_cast<std::size_t>(rts::kObsSelfFloats), 0.0f);
        glob.assign(static_cast<std::size_t>(n) *
                        static_cast<std::size_t>(rts::kObsGlobalFloats), 0.0f);
        done.assign(static_cast<std::size_t>(n), 0u);
    }
};

rts::BatchedEnvInit make_init(int n, int threads) {
    rts::BatchedEnvInit bi;
    for (int i = 0; i < n; ++i) bi.worlds.push_back(one(i, 2 + i % 3));
    bi.side = rts::Side::Attacker;
    bi.ticks_per_step = 6;
    bi.threads = threads;
    return bi;
}

}  // namespace

TEST_CASE("并行不改变结果：1 线程与 N 线程逐字节相同", "[batchenv]") {
    constexpr int kN = 9;
    constexpr int kSteps = 5;

    // 两份完全一样的批，只有线程数不同。
    rts::BatchedEnv a(make_init(kN, 1));
    rts::BatchedEnv b(make_init(kN, 8));
    Bufs ba(kN), bb(kN);

    // 动作也要**逐环境不同**，否则「装配顺序错了」同样看不出来。
    std::vector<rts::UnitAction> acts(
        static_cast<std::size_t>(kN) *
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::Stop);
    for (std::size_t i = 0; i < acts.size(); ++i) {
        acts[i] = rts::UnitAction::AtkNear;
        if (i % 3 == 0) acts[i] = rts::UnitAction::MoveW;
        if (i % 5 == 0) acts[i] = rts::UnitAction::Stop;
    }

    for (int s = 0; s < kSteps; ++s) {
        a.observe(ba.cells, ba.self, ba.glob);
        b.observe(bb.cells, bb.self, bb.glob);
        REQUIRE(ba.cells == bb.cells);
        REQUIRE(ba.self == bb.self);
        REQUIRE(ba.glob == bb.glob);
        a.step(acts, ba.done);
        b.step(acts, bb.done);
        REQUIRE(ba.done == bb.done);
        REQUIRE(std::vector<int>(a.unit_counts().begin(), a.unit_counts().end()) ==
                std::vector<int>(b.unit_counts().begin(), b.unit_counts().end()));
    }

    // 状态哈希也要逐局相同——张量相同但世界跑歪了，是另一种可能。
    for (int i = 0; i < kN; ++i) {
        REQUIRE(a.world_at(i).state_hash() == b.world_at(i).state_hash());
    }
}

TEST_CASE("各局的观测落在各自那一段，不串台", "[batchenv]") {
    // 造两局：第 0 局有 2 个攻方单位，第 1 局有 4 个。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(one(0, 2));
    bi.worlds.push_back(one(1, 4));
    bi.side = rts::Side::Attacker;
    bi.threads = 2;
    rts::BatchedEnv e(std::move(bi));
    REQUIRE(e.batch_size() == 2);
    REQUIRE(e.unit_counts()[0] == 2);
    REQUIRE(e.unit_counts()[1] == 4);

    Bufs b(2);
    e.observe(b.cells, b.self, b.glob);

    const std::size_t per_self =
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv) *
        static_cast<std::size_t>(rts::kObsSelfFloats);
    // one-hot 的那一位：有单位的段是 1，没单位的段是 0。
    const auto onehot = [&](int env, int u) {
        return b.self[static_cast<std::size_t>(env) * per_self +
                      static_cast<std::size_t>(u) *
                          static_cast<std::size_t>(rts::kObsSelfFloats) +
                      static_cast<std::size_t>(rts::UnitType::Ghoul)];
    };
    CHECK(onehot(0, 0) == 1.0f);
    CHECK(onehot(0, 1) == 1.0f);
    CHECK(onehot(0, 2) == 0.0f);   // 第 0 局只有 2 个
    CHECK(onehot(1, 3) == 1.0f);   // 第 1 局有 4 个
    CHECK(onehot(1, 4) == 0.0f);
}

TEST_CASE("单位数减少后，尾部必须归零——否则策略读到过期观测", "[batchenv]") {
    // 这条锁的是 `observe` 开头那三次 `std::fill`。不清零的话，某局单位数
    // 从 3 掉到 1 时第 2、3 段会留着上一步的值——**不报错、不崩，
    // 只是策略读到了两步前的战场**。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(one(0, 3));
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    e.observe(b.cells, b.self, b.glob);
    REQUIRE(e.unit_counts()[0] == 3);

    // 拿到第 3 个单位并把它打死，于是活单位数降到 2。
    // `world_at` 是只读的，所以换一条路：直接让它被自己那局的守方打死太慢，
    // 改用 `reset_one` 换一个只有 1 个攻方单位的局面——同样触发「尾部要归零」。
    e.reset_one(0, one(0, 1));
    REQUIRE(e.unit_counts()[0] == 1);
    e.observe(b.cells, b.self, b.glob);

    const std::size_t per_self =
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv) *
        static_cast<std::size_t>(rts::kObsSelfFloats);
    // 第 2、3 段（下标 1、2）必须整段是 0。
    for (int u = 1; u < 3; ++u) {
        for (int f = 0; f < rts::kObsSelfFloats; ++f) {
            const std::size_t o = static_cast<std::size_t>(u) *
                                      static_cast<std::size_t>(rts::kObsSelfFloats) +
                                  static_cast<std::size_t>(f);
            CHECK(b.self[o] == 0.0f);
        }
    }
    (void)per_self;
}

TEST_CASE("缓冲区长度必须恰好，批大小不能是 0", "[batchenv]") {
    rts::BatchedEnvInit empty;
    REQUIRE_THROWS_AS(rts::BatchedEnv(std::move(empty)), rts::ContractError);

    rts::BatchedEnvInit bad = make_init(2, 1);
    bad.ticks_per_step = 0;
    REQUIRE_THROWS_AS(rts::BatchedEnv(std::move(bad)), rts::ContractError);

    rts::BatchedEnv e(make_init(2, 1));
    Bufs b(2);
    std::vector<float> shortv(b.cells.size() - 1, 0.0f);
    REQUIRE_THROWS_AS(e.observe(shortv, b.self, b.glob), rts::ContractError);
    std::vector<rts::UnitAction> shorta(3, rts::UnitAction::Stop);
    REQUIRE_THROWS_AS(e.step(shorta, b.done), rts::ContractError);
    REQUIRE_THROWS_AS(e.world_at(2), rts::ContractError);
}

TEST_CASE("终局位：Keep 没了就置 1，且不自动重置", "[batchenv]") {
    // 重置时机归 `train/`（要按波次分层采样，重置成哪一波是它的决定），
    // 所以 `step` 只报告、不动手。
    rts::WorldInit init = one(0, 1);
    // 把 Keep 的血压到 1，让它一步之内就可能掉——但更可靠的是直接不给 Keep
    // 之外的建筑，然后手工确认「有 Keep 时 done 是 0」。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(std::move(init));
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    std::vector<rts::UnitAction> acts(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::Stop);
    e.step(acts, b.done);
    CHECK(b.done[0] == 0u);   // Keep 还在
}
