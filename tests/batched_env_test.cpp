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
// | 工作线程里那层 `try/catch` 摘掉 | **没有常驻用例会红**——那条路目前通过公开 API 到不了（理由见下面那条用例的注释），只有下面第一条破坏之后才走得到 |
//
// **第一条那次验证顺带查出了一个独立缺陷**，值得记：期待的是一条干净的断言失败，
// 实际拿到的是**退出码 134（SIGABRT）**——工作线程里 `pack_unit_obs` 抛的
// `ContractError` 走到了线程函数外，于是 `std::terminate`。
// 从 Python 调时那是最糟的失败方式：训练器整个死掉、没有 traceback。
// `for_each_env` 因此加了逐环境的 `exception_ptr` 捎回。
// **也就是说那次破坏性验证的产出不是「测试有效」，是「找到了另一个 bug」。**

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
    for (int k = 0; k < attackers; ++k) {
        init.units.push_back(rts::UnitInit{
            rts::UnitType::Ghoul,
            rts::Vec2{12.5f + static_cast<float>(k), 12.5f}, 1, 20, 20});
    }
    init.units.push_back(rts::UnitInit{
        rts::UnitType::Archer, rts::Vec2{9.5f, 10.5f}, 1, 20, 20});
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

TEST_CASE("native reset factory preserves an independent episode clock", "[batchenv]") {
    auto init = make_init(2, 2);
    init.max_ticks_per_episode = 12;
    std::vector<int> calls(2,0);
    init.world_factory = [&calls](rts::WorldInit wi, int i) {
        ++calls[static_cast<std::size_t>(i)];
        auto w = std::make_unique<rts::World>(std::move(wi));
        w->advance(90);
        return w;
    };
    rts::BatchedEnv env(std::move(init));
    REQUIRE(calls == std::vector<int>{1,1});
    REQUIRE(env.world_at(0).now() == 90);
    std::vector<rts::UnitAction> actions(2*rts::BatchedEnv::kMaxUnitsPerEnv);
    std::vector<std::uint8_t> done(2);
    env.step(actions,done);
    REQUIRE(done == std::vector<std::uint8_t>{0,0});
    env.step(actions,done);
    REQUIRE(done == std::vector<std::uint8_t>{1,1});
    env.reset_one(0,one(0,2));
    REQUIRE(calls == std::vector<int>{2,1});
    REQUIRE(env.world_at(0).now() == 90);
    env.step(actions,done);
    REQUIRE(done[0] == 0);

    auto invalid = make_init(1,1);
    invalid.world_factory = [](rts::WorldInit, int) { return std::unique_ptr<rts::World>{}; };
    REQUIRE_THROWS_AS(rts::BatchedEnv(std::move(invalid)),rts::ContractError);
}

TEST_CASE("terminal cause is latched, timeout cannot turn into victory", "[batchenv]") {
    using End = rts::BatchedEnv::EpisodeEnd;
    for (int threads : {1, 2}) {
        auto win = one(0, 1);
        win.units.resize(1);
        win.units[0].pos = rts::Vec2{3.5f, 2.5f};
        win.buildings[0].hp = 1;
        rts::BatchedEnvInit init;
        init.worlds = {win, one(1, 1)};
        init.threads = threads;
        init.ticks_per_step = 6;
        init.max_ticks_per_episode = 6;
        rts::BatchedEnv env(std::move(init));
        std::vector<rts::UnitAction> acts(2*rts::BatchedEnv::kMaxUnitsPerEnv,
                                         rts::UnitAction::AtkBld);
        Bufs bufs(2);
        REQUIRE(env.episode_ends()[0] == End::Running);
        env.step(acts, bufs.done);
        CHECK(env.episode_ends()[0] == End::KeepDestroyed); // tie beats timeout
        CHECK(env.episode_ends()[1] == End::Timeout);
        CHECK(bufs.done == std::vector<std::uint8_t>{1, 1});
        const auto hash0 = env.world_at(0).state_hash();
        const auto hash1 = env.world_at(1).state_hash();
        std::vector<float> tally(2*rts::BatchedEnv::kTallyFields);
        env.take_tally(tally);
        env.step(acts, bufs.done);
        CHECK(env.world_at(0).state_hash() == hash0);
        CHECK(env.world_at(1).state_hash() == hash1);
        env.take_tally(tally);
        for (float value : tally) CHECK(value == 0.0f);
        env.reset_one(0, one(2, 1));
        CHECK(env.episode_ends()[0] == End::Running);
        CHECK(env.episode_ends()[1] == End::Timeout);
    }
}

TEST_CASE("episode stops at the exact tick budget", "[batchenv]") {
    auto init = make_init(1, 1);
    init.max_ticks_per_episode = 7;
    rts::BatchedEnv env(std::move(init));
    Bufs bufs(1);
    std::vector<rts::UnitAction> acts(rts::BatchedEnv::kMaxUnitsPerEnv,
                                     rts::UnitAction::Stop);
    env.step(acts, bufs.done);
    CHECK(bufs.done[0] == 0);
    CHECK(env.world_at(0).now() == 6);
    env.step(acts, bufs.done);
    CHECK(env.world_at(0).now() == 7);
    CHECK(env.episode_ends()[0] == rts::BatchedEnv::EpisodeEnd::Timeout);
}

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

    // 第 2、3 段（下标 1、2）必须整段是 0。只有一局，所以段偏移就是 `u × 每段`
    // ——不需要 `per_self` 那个跨局跨度（初版算了一个然后 `(void)` 掉，已删）。
    for (int u = 1; u < 3; ++u) {
        for (int f = 0; f < rts::kObsSelfFloats; ++f) {
            const std::size_t o = static_cast<std::size_t>(u) *
                                      static_cast<std::size_t>(rts::kObsSelfFloats) +
                                  static_cast<std::size_t>(f);
            CHECK(b.self[o] == 0.0f);
        }
    }
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

TEST_CASE("长度检查在进线程之前就抛，抛完这一批仍然可用", "[batchenv]") {
    // **这条用例的名字曾经写的是「工作线程里的异常要捎回调用线程」，那是一句
    // 它没有兑现的承诺**：`observe` 的长度检查在 `for_each_env` **之前**、
    // 在调用线程上就抛了，所以下面这两行一个工作线程都没进过。
    //
    // 而「捎回」那层 `try/catch` 确实是需要的——只是**目前通过公开 API 到不了**：
    // `pack_unit_obs` 的三种抛出条件（缓冲区长度、self 不活、self 不同侧）在
    // `observe` 里都不可能成立（子 span 长度恒精确，`ids` 每步 refresh 一次），
    // 而 `step` 的切片被补齐到恰好等于活单位数。也就是说它是**防御性代码**，
    // 由文件头那张表里的破坏性验证验过一次（那次实测：没有捎回时退出码
    // 134/SIGABRT，加上之后是 42 + 一条干净的断言失败），此外没有常驻覆盖。
    //
    // **把这件事写清楚比留一个好听的名字重要**：读到旧名字的人会以为
    // 「线程里抛异常」这条路有测试钉着，于是哪天把 `try/catch` 删了、
    // 看见全绿就以为没事——而症状要到从 Python 调的时候才出现，且是进程直接死。
    //
    // 它能变成真覆盖的那一天：`observe` 或 `step` 里出现一个**逐局**才知道
    // 合不合法的输入（比如按局给不同的 `ObsNorms`、或者允许某局单位数超过
    // `kMaxUnitsPerEnv` 而不补齐）。那时请把这条用例改回去。
    //
    // 下面这两行仍然有值：多线程构型下，一次被拒绝的 `observe` 之后这一批
    // 还能正常打包（清零与偏移都没被那次失败弄脏）。
    rts::BatchedEnv e(make_init(4, 4));
    Bufs b(4);
    std::vector<float> shortv(b.cells.size() - 1, 0.0f);
    REQUIRE_THROWS_AS(e.observe(shortv, b.self, b.glob), rts::ContractError);
    // 进程还活着，且批仍然可用。
    REQUIRE_NOTHROW(e.observe(b.cells, b.self, b.glob));
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

// ═══════════════════════════════════════════════════════════════════════
// agent = 编队（2026-09-05，`World/13 → World/14`）
//
// 编队号此前只活在 `game/` 里，而 `BatchedEnv` 在 `rts_core`、看不见它 ⇒
// 观测按**单位**摊行、`kMaxUnitsPerEnv` 还停在过期的 40，于是攻方场上
// ~70 个单位里有 30 个**存在、会挨打、但完全不受控且不被观测**（超出的
// 被补 `Stop`）。而「攻方 RL 控制的是每一支编队」是 CLAUDE.md 定的。
// ═══════════════════════════════════════════════════════════════════════

namespace {

// 6 个 Ghoul 编成 2 支（编队 0 与 1，各 3 个）+ 1 名守方弓手（散兵）。
rts::WorldInit squads_world() {
    rts::WorldInit init = one(0, 0);
    for (int q = 0; q < 2; ++q) {
        for (int m = 0; m < 3; ++m) {
            init.units.push_back(rts::UnitInit{
                rts::UnitType::Ghoul,
                rts::Vec2{12.5f + static_cast<float>(m), 12.5f + static_cast<float>(q)},
                1, 20, 20, static_cast<std::uint16_t>(q)});
        }
    }
    return init;
}

}   // namespace

TEST_CASE("编队：观测按编队摊行，一支编队一行", "[batchenv]") {
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(squads_world());
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    e.observe(b.cells, b.self, b.glob);

    // 6 个单位、2 支编队 ⇒ **只有 2 行非零**。若打包仍按单位摊行，
    // 这里会是 6 行——那正是这条测试要钉的东西。
    int rows = 0;
    for (int u = 0; u < rts::BatchedEnv::kMaxUnitsPerEnv; ++u) {
        const std::size_t so = static_cast<std::size_t>(u) *
                               static_cast<std::size_t>(rts::kObsSelfFloats);
        bool nz = false;
        for (int f = 0; f < rts::kObsSelfFloats; ++f) {
            if (b.self[so + static_cast<std::size_t>(f)] != 0.0f) { nz = true; break; }
        }
        if (nz) ++rows;
    }
    CAPTURE(rows);
    CHECK(rows == 2);
}

TEST_CASE("编队：一个动作发给整队，队里每个成员都动", "[batchenv]") {
    // 这是「agent = 编队」在**动作面**的另一半。张量的第二维是编队，而
    // `submit_actions` 要逐单位 ⇒ `step` 必须把编队的动作摊给队里每个成员。
    // 不摊的话后两个成员拿到的是别的编队的动作（或越界读）。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(squads_world());
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);

    std::vector<rts::UnitAction> acts(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::Stop);
    // 编队 0 往北走、编队 1 停住。
    acts[0] = rts::UnitAction::MoveN;
    acts[1] = rts::UnitAction::Stop;

    const rts::World& w0 = e.world_at(0);
    std::vector<rts::UnitId> before;
    w0.enumerate_units(rts::Side::Attacker, before);
    std::vector<rts::Vec2> p0;
    for (const rts::UnitId id : before) p0.push_back(w0.unit_pos(id));

    e.step(acts, b.done);

    const rts::World& w1 = e.world_at(0);
    std::vector<rts::UnitId> after;
    w1.enumerate_units(rts::Side::Attacker, after);
    REQUIRE(after.size() == before.size());

    int moved_q0 = 0, moved_q1 = 0;
    for (std::size_t k = 0; k < after.size(); ++k) {
        const rts::Vec2 p = w1.unit_pos(after[k]);
        const bool moved = (p.x != p0[k].x || p.y != p0[k].y);
        if (w1.unit_squad(after[k]) == 0 && moved) ++moved_q0;
        if (w1.unit_squad(after[k]) == 1 && moved) ++moved_q1;
    }
    CAPTURE(moved_q0, moved_q1);
    CHECK(moved_q0 == 3);   // 整队都动了
    CHECK(moved_q1 == 0);   // 另一队没动
}

TEST_CASE("编队：散兵各自成一支，于是守方那一侧退化成逐单位", "[batchenv]") {
    // 守方单兵不上 RL（参数化脚本驱动），所以它们一律 `kNoSquad`。
    // `enumerate_squads` 让每个散兵自成一支 ⇒ 调用方不必分两种情况写。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(squads_world());
    bi.side = rts::Side::Defender;   // 这一侧只有 1 名弓手，且是散兵
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));

    const rts::World& w = e.world_at(0);
    std::vector<rts::UnitId> units, leaders;
    w.enumerate_units(rts::Side::Defender, units);
    w.enumerate_squads(rts::Side::Defender, leaders);
    CAPTURE(units.size(), leaders.size());
    CHECK(units.size() == leaders.size());   // 退化：一人一支
    for (const rts::UnitId id : units) CHECK(w.unit_squad(id) == rts::kNoSquad);
}

TEST_CASE("编队：编队号进 state_hash，改了它世界就不同", "[batchenv]") {
    // 编队号是外生输入、影响仿真（它决定 RL 的动作怎么摊），所以必须进哈希
    // ——否则两个「编成一样但分队不同」的世界会算出同一个哈希，而回放
    // 对不上时查不出原因。`kWorldHashTag` 因此从 World/13 进到 World/14。
    rts::World a(squads_world());
    rts::WorldInit alt = squads_world();
    // 把最后一个 Ghoul 从编队 1 挪到编队 0：编成、位置、血量全不变。
    REQUIRE(alt.units.size() >= 2);
    alt.units.back().squad = 0;
    rts::World b(std::move(alt));
    CHECK(a.state_hash() != b.state_hash());
}

TEST_CASE("战果计数：伤害与击杀按造成方记，读走即清", "[batchenv]") {
    // 奖励此前**整个不存在**（绑定层零引用，`World` 也没有可读的战果计数），
    // 而 PPO 循环没有它就写不了。形状由 CLAUDE.md 定：摧毁建筑的即时奖励 =
    // 该建筑的重建成本，击杀 `Scout` 必须给即时奖励。
    //
    // **权重刻意不在 C++ 侧**：那是训练超参。这一层只给「发生了什么」。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(one(0, 3));   // 3 个 Ghoul 打一面 50 血的墙
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    std::vector<float> tally(
        static_cast<std::size_t>(rts::BatchedEnv::kTallyFields), 0.0f);

    // 开局什么都没发生。
    e.take_tally(tally);
    for (const float v : tally) CHECK(v == 0.0f);

    // **先走到墙边再打。** `one()` 把 Ghoul 摆在 (12.5, 12.5)、墙在 (10, 10)
    // ——隔着两三格，而 `AtkWall` 只有贴着时才在掩码里（`submit_actions` 会
    // 按掩码拒掉非法动作）。我第一版直接发 `AtkWall` 跑 40 步，`dmg_to_blds`
    // 恒 0 —— 那是**用例设计错**（没走过去），不是埋点没生效。
    std::vector<rts::UnitAction> go(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::MoveN);   // (−1,−1)：从 (12.5,12.5) 朝 (10,10) 去
    std::vector<rts::UnitAction> hit(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::AtkWall);
    float dmg_total = 0.0f, value_total = 0.0f;
    for (int k = 0; k < 120; ++k) {
        // 走一段、试着砸一下。掩码不亮时 `AtkWall` 会被拒（那是设计），
        // 所以两种动作交替发就不需要自己判「到没到」。
        e.step(k % 2 == 0 ? go : hit, b.done);
        e.take_tally(tally);
        dmg_total += tally[1];    // dmg_to_blds
        value_total += tally[4];  // bld_value
    }
    CAPTURE(dmg_total, value_total);
    CHECK(dmg_total > 0.0f);   // 真的打到了墙

    // **读走即清**：紧接着再读一次必须全零（上一轮循环末尾刚读过）。
    e.take_tally(tally);
    for (const float v : tally) CHECK(v == 0.0f);
}

TEST_CASE("战果计数：不进 state_hash——它是旁观计数器", "[batchenv]") {
    // 同 `volley_hits()` 那条先例：没有任何一条仿真推演读它，喂进哈希只会让
    // 「改了一个纯诊断字段」表现为「回放对不上」，把查 bug 的人引向一个
    // 不存在的确定性缺陷。
    rts::World a(one(0, 3));
    rts::World c(one(0, 3));
    const std::uint64_t h0 = a.state_hash();
    REQUIRE(h0 == c.state_hash());

    // 在 `a` 上读走战果（清零），`c` 不读 —— 两者的哈希必须仍然相同。
    (void)a.take_tally(rts::Side::Attacker);
    CHECK(a.state_hash() == h0);
    CHECK(a.state_hash() == c.state_hash());
}

TEST_CASE("episode 时间上界：打不动也要收场", "[batchenv]") {
    // 没有它，`done` 只在「Keep 被拆」时置位 ⇒ 打不动的策略把一局无限拖下去。
    // 实测：随机策略在 170×170 图上推 **18000 tick 仍未终局**，于是一个
    // rollout（384 tick）里一次奖励都收不到，PPO 学不动。
    //
    // 而 CLAUDE.md 要的是「一波 = 一个 RL episode」「**短 episode** 让 credit
    // assignment 链条足够短，是训练可行的关键」。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(one(0, 1));
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    bi.ticks_per_step = 6;
    bi.max_ticks_per_episode = 60;   // 10 步就该到点
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    std::vector<rts::UnitAction> acts(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::Stop);   // 什么都不做 ⇒ Keep 永远不会掉

    int steps_to_done = -1;
    for (int k = 0; k < 40; ++k) {
        e.step(acts, b.done);
        if (b.done[0]) { steps_to_done = k + 1; break; }
    }
    CAPTURE(steps_to_done);
    CHECK(steps_to_done == 10);   // 60 tick / 6 = 10 步

    // **重置要把计时清零**，否则新 episode 一开始就是「已超时」。
    e.reset_one(0, one(1, 1));
    e.step(acts, b.done);
    CHECK(b.done[0] == 0u);
}

TEST_CASE("episode 时间上界：置 0 = 不设（旧行为）", "[batchenv]") {
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(one(0, 1));
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    bi.max_ticks_per_episode = 0;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    std::vector<rts::UnitAction> acts(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::Stop);
    for (int k = 0; k < 60; ++k) {
        e.step(acts, b.done);
        CHECK(b.done[0] == 0u);   // Keep 还在、且不设上界 ⇒ 永不终局
    }
}

TEST_CASE("Native macro goals are isolated, deterministic and reset safely", "[batchenv][batchgoals]") {
    const auto build = [](int threads, int mode) {
        auto init=make_init(2,threads);
        if(mode) init.goal_hook=[mode](const rts::WorldView&, std::span<const rts::UnitId> ids, int) {
            rts::BatchedGoals g;
            g.groups.resize(ids.size());
            for(std::size_t i=0;i<ids.size();++i) g.groups[i]=static_cast<std::uint8_t>(i%2);
            if(mode!=2) g.economy.push_back({22,12});
            if(mode==3) g.groups.push_back(0);
            if(mode==4) g.economy[0]={24,12};
            return g;
        };
        return std::make_unique<rts::BatchedEnv>(std::move(init));
    };
    auto legacy=build(1,0), serial=build(1,1), parallel=build(2,1), fallback=build(2,2);
    Bufs a(2),b(2),c(2),d(2);
    legacy->observe(a.cells,a.self,a.glob);
    serial->observe(b.cells,b.self,b.glob);
    parallel->observe(c.cells,c.self,c.glob);
    fallback->observe(d.cells,d.self,d.glob);
    REQUIRE(b.cells==c.cells);REQUIRE(b.self==c.self);REQUIRE(b.glob==c.glob);
    REQUIRE(a.cells==d.cells);REQUIRE(a.self==d.self);REQUIRE(a.glob==d.glob);
    REQUIRE(a.self==b.self);REQUIRE(a.glob==b.glob);
    const auto center=static_cast<std::size_t>((rts::kObsK/2*rts::kObsK+rts::kObsK/2)*rts::kObsChannelCount);
    const auto di=static_cast<std::size_t>(rts::ObsChannel::FlowDi);
    REQUIRE(a.cells[center+di]==b.cells[center+di]);
    REQUIRE(a.cells[rts::kObsCellFloats+center+di]<0);
    REQUIRE(b.cells[rts::kObsCellFloats+center+di]>0);
    // Goal conditioning may change only the two registered flow channels.
    bool unchanged=true;
    for(std::size_t i=0;i<a.cells.size();++i)
        if(i%rts::kObsChannelCount<di && a.cells[i]!=b.cells[i]) unchanged=false;
    REQUIRE(unchanged);
    serial->reset_one(0,one(0,2));
    serial->observe(d.cells,d.self,d.glob);
    REQUIRE(d.cells==b.cells);
    for(int mode:{3,4}) {
        auto invalid=build(2,mode);
        REQUIRE_THROWS_AS(invalid->observe(d.cells,d.self,d.glob),rts::ContractError);
    }
}

TEST_CASE("观测必须喂方向场——否则策略不知道该往哪走", "[batchenv]") {
    // `observe` 此前给 `pack_unit_obs` 传 `nullptr`（`obs_pack.hpp` 说
    // 「训练早期没有宏观目标时是正常形态，不是缺陷」）。**在有宏观目标的
    // 时候它就是缺陷**，而实测代价是：14 条通道里只有 4 条非零，攻方在
    // 50 格外、视野半径只有 7 格、`enemy_*` 全 0 —— 观测里**没有任何东西
    // 指向目标**。40 万步 PPO 回报恒 0.00 就是这么来的，而同一个局面用
    // 「一路朝 keep 走」的定向策略能打出 8515 点建筑伤害。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(one(0, 3));
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    e.observe(b.cells, b.self, b.glob);

    // 找 `flow_di` / `flow_dj` 那两条通道的下标（**按名字找，不按位置猜**）。
    int di = -1, dj = -1;
    for (int c = 0; c < rts::kObsChannelCount; ++c) {
        if (rts::kObsChannels[static_cast<std::size_t>(c)].name == "flow_di") di = c;
        if (rts::kObsChannels[static_cast<std::size_t>(c)].name == "flow_dj") dj = c;
    }
    REQUIRE(di >= 0);
    REQUIRE(dj >= 0);

    // 第 0 个 agent 的那一格窗口里，方向场必须有非零值。
    int nz = 0;
    for (int y = 0; y < rts::kObsK; ++y) {
        for (int x = 0; x < rts::kObsK; ++x) {
            const std::size_t base =
                (static_cast<std::size_t>(y) * rts::kObsK + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(rts::kObsChannelCount);
            if (b.cells[base + static_cast<std::size_t>(di)] != 0.0f) ++nz;
            if (b.cells[base + static_cast<std::size_t>(dj)] != 0.0f) ++nz;
        }
    }
    CAPTURE(nz);
    CHECK(nz > 0);
}

TEST_CASE("战果计数：progress 是「离堡垒近了几格」，站着不动就是 0", "[batchenv]") {
    // **这一列是 2026-09-06 一次长跑坍缩之后加的。** 只有前 7 列时，
    // 「站着不动」是**局部最优**：攻方在集结点离堡垒 40 格，随机游走 400 个
    // 决策的期望位移只有 9.6 格 ⇒ 前 6 列恒 0 够不着，而 `losses`（负权重）
    // 够得着——不动就不死。实测长跑到 50 万步之后「建筑伤」与「自损」
    // **同时**归零并再不回升。
    //
    // 本项只验证位移诊断；折扣奖励与终局条件在 training_math 中验证。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(one(0, 3));
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    std::vector<float> tally(
        static_cast<std::size_t>(rts::BatchedEnv::kTallyFields), 0.0f);
    const std::size_t ip = 7;   // = kTallyNames 里 "progress" 的下标
    REQUIRE(rts::BatchedEnv::kTallyNames[ip] == "progress");

    // ——一、站着不动 ⇒ 恒 0。这是那次坍缩的**局面**本身——
    std::vector<rts::UnitAction> stay(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::Stop);
    float idle = 0.0f;
    for (int k = 0; k < 20; ++k) {
        e.step(stay, b.done);
        e.take_tally(tally);
        idle += tally[ip];
    }
    CAPTURE(idle);
    CHECK(idle == 0.0f);

    // ——二、朝堡垒走 ⇒ 正。`keep` 在 (2,2)、Ghoul 在 (12.5,12.5) ⇒ 西北——
    std::vector<rts::UnitAction> go(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::MoveN);
    float toward = 0.0f;
    for (int k = 0; k < 20; ++k) {
        e.step(go, b.done);
        e.take_tally(tally);
        toward += tally[ip];
    }
    CAPTURE(toward);
    CHECK(toward > 0.0f);

    // ——三、读走即清——
    e.take_tally(tally);
    CHECK(tally[ip] == 0.0f);
}

TEST_CASE("战果计数：progress 在换局时清零，不把上一局的位移算进新局", "[batchenv]") {
    // **不清就是一笔凭空的大额奖励**：新局的单位在集结点、距离是满的，
    // 于是「上一局最后一步的位移」会被记在新局第一步头上。而 episode 的第一
    // 步恰好是 PPO 最看重的那一段（回报要靠它 bootstrap）。
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(one(0, 3));
    bi.side = rts::Side::Attacker;
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    std::vector<float> tally(
        static_cast<std::size_t>(rts::BatchedEnv::kTallyFields), 0.0f);

    // 先攒一点位移，**故意不读走**（模拟「终局那一步没来得及读」）。
    std::vector<rts::UnitAction> go(
        static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::MoveN);
    for (int k = 0; k < 10; ++k) e.step(go, b.done);

    // 换局。此后第一次读必须是 0 ——攒着那份被清掉了。
    e.reset_one(0, one(1, 3));
    e.take_tally(tally);
    CAPTURE(tally[7]);
    CHECK(tally[7] == 0.0f);
}

TEST_CASE("episode 时长：构造和全批重置都保留完整任务时限", "[batchenv]") {
    rts::BatchedEnvInit bi;
    for (int i = 0; i < 8; ++i) bi.worlds.push_back(one(i, 1));
    bi.threads = 1;
    bi.max_ticks_per_episode = 240;
    bi.ticks_per_step = 6;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(8);
    std::vector<rts::UnitAction> stay(8 * rts::BatchedEnv::kMaxUnitsPerEnv,
                                       rts::UnitAction::Stop);
    for (int round = 0; round < 2; ++round) {
        for (int step = 1; step <= 40; ++step) {
            e.step(stay, b.done);
            for (const auto d : b.done) CHECK(d == (step == 40 ? 1u : 0u));
        }
        for (int i = 0; i < 8; ++i) e.reset_one(i, one(i + 8, 1));
    }
}

TEST_CASE("训练势：同一批的移动、死亡和重置都按当前状态读", "[batchenv]") {
    auto init = one(0, 1);
    // BatchedEnv 不运行守方单兵脚本，用自动开火的塔验证真实死亡。
    init.units[0].pos = rts::Vec2{9.5f, 11.5f};
    init.buildings.push_back(rts::BldInit{rts::BldType::Tower, {8, 11}, 100, 100});
    auto& tower = init.stats.bld[static_cast<std::size_t>(rts::BldType::Tower)];
    tower.max_hp = 100;
    tower.damage = 1000;
    tower.range = 3.0f;
    tower.vision = 4.0f;
    rts::BatchedEnvInit bi;
    bi.worlds.push_back(init);
    bi.threads = 1;
    rts::BatchedEnv e(std::move(bi));
    Bufs b(1);
    CHECK(e.potentials()[0] == -9.0);
    CHECK(e.potentials()[0] == -9.0);  // 只读，不像 take_tally 那样清零
    std::vector<rts::UnitAction> stay(rts::BatchedEnv::kMaxUnitsPerEnv,
                                       rts::UnitAction::Stop);
    for (int step = 0; step < 100 && e.unit_counts()[0] != 0; ++step) e.step(stay, b.done);
    REQUIRE(e.unit_counts()[0] == 0);
    CHECK(e.potentials()[0] == 0.0);
    e.reset_one(0, one(1, 1));
    CHECK(e.potentials()[0] == -10.0);
    stay[0] = rts::UnitAction::MoveSE;
    e.step(stay, b.done);
    CHECK(e.potentials()[0] < -10.0);
}

TEST_CASE("对侧钩子：在 advance 之前被调、每步每局恰好一次", "[batchenv]") {
    // `opponent_hook` 是 2026-09-07 加的，用来**把真正的守方接进训练回路**
    // （实现在 `bindings/src/scripted_defender.hpp`，因为 `rts_core` 不能
    // 依赖 `game/`）。这里测的是**钩子这个机制**，不是守方逻辑。
    //
    // 三条契约各测一条：调用次数、时序（在 `advance` 之前）、以及
    // **下标正确**——最后那条是并行安全的前提，钩子的实现按下标取自己那
    // 一份状态，下标错了就是跨局写。
    rts::BatchedEnvInit bi;
    for (int k = 0; k < 4; ++k) bi.worlds.push_back(one(k, 2));
    bi.side = rts::Side::Attacker;
    bi.threads = 1;   // 计数用共享 vector，所以这条用例单线程（并行那条见下）
    std::vector<int> calls(4, 0);
    std::vector<rts::Tick> saw_tick(4, -1);
    bi.opponent_hook = [&calls, &saw_tick](rts::World& w, int i) {
        ++calls[static_cast<std::size_t>(i)];
        // **在 `advance` 之前**：第一次被调时世界还在 tick 0。
        if (saw_tick[static_cast<std::size_t>(i)] < 0) {
            saw_tick[static_cast<std::size_t>(i)] = w.now();
        }
    };
    rts::BatchedEnv e(std::move(bi));
    Bufs b(4);
    std::vector<rts::UnitAction> stay(
        4 * static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
        rts::UnitAction::Stop);
    for (int k = 0; k < 5; ++k) e.step(stay, b.done);

    for (int i = 0; i < 4; ++i) {
        CAPTURE(i, calls[static_cast<std::size_t>(i)],
                saw_tick[static_cast<std::size_t>(i)]);
        CHECK(calls[static_cast<std::size_t>(i)] == 5);   // 每步每局恰好一次
        CHECK(saw_tick[static_cast<std::size_t>(i)] == 0);  // 在 advance 之前
    }
}

TEST_CASE("对侧钩子：真的能改变对侧行为——不接就一枪不放", "[batchenv]") {
    // **这一条是那个洞的回归**：光把守方单位摆进 `World` 是不够的
    // ——单位出生动作是 `Stop`，而攻击阶段只处理攻击类动作 ⇒ 没人替它们
    // 下令的话它们会站在原地被打死而一枪不放。40M 那轮就是这个局面
    // （`杀` 全程 0），而它**不会让任何测试变红**，所以补这一条。
    //
    // 判据取**攻方掉了多少血**：攻方全程 `Stop`，`one()` 那张图没有塔，
    // 所以攻方受到的伤害只可能来自守方单位。
    const auto attacker_hp_after = [](bool hook) {
        rts::WorldInit init = one(0, 3);
        // 一名弓手贴着攻方（`one()` 把 Ghoul 摆在 (12.5,12.5) 起）。
        init.units.push_back(rts::UnitInit{
            rts::UnitType::Archer, rts::Vec2{13.0f, 12.5f}, 1, 200, 200});
        rts::BatchedEnvInit bi;
        bi.worlds.push_back(std::move(init));
        bi.side = rts::Side::Attacker;
        bi.threads = 1;
        if (hook) {
            // **最小的「会开枪」钩子**：给每个守方单位发 `AtkNear`。
            // 真正的守方（`ScriptedDefender`）做的事多得多，但这一条要测的
            // 是「钩子能不能改变对侧行为」，所以刻意最小。
            bi.opponent_hook = [](rts::World& w, int) {
                std::vector<rts::UnitId> ids;
                w.enumerate_units(rts::Side::Defender, ids);
                std::vector<rts::UnitAction> a(ids.size(), rts::UnitAction::AtkNear);
                w.submit_actions(rts::Side::Defender, a.data(), a.size());
            };
        }
        rts::BatchedEnv e(std::move(bi));
        Bufs b(1);
        std::vector<rts::UnitAction> stay(
            static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
            rts::UnitAction::Stop);
        for (int k = 0; k < 40; ++k) e.step(stay, b.done);
        const rts::WorldView v = e.world_at(0).view(rts::Side::Attacker);
        const auto ut = v.unit_type();
        const auto ua = v.unit_alive();
        const auto hp = v.unit_hp();
        std::int64_t total = 0;
        for (std::size_t k = 0; k < ut.size(); ++k) {
            if (ua[k] && rts::side_of(ut[k]) == rts::Side::Attacker) total += hp[k];
        }
        return total;
    };

    const std::int64_t idle = attacker_hp_after(false);
    const std::int64_t armed = attacker_hp_after(true);
    CAPTURE(idle, armed);
    // 不接钩子 ⇒ 守方一枪不放 ⇒ 攻方满血。**这一半是对照**，
    // 少了它下面那条可能只是「地图上有塔在打」。
    CHECK(armed < idle);
}

TEST_CASE("对侧钩子：结果与线程数无关", "[batchenv]") {
    // 钩子在工作线程里被调 ⇒ 它是「结果与线程数无关」这条不变量的新入口。
    // 一个**只碰自己那一局**的钩子必须仍然满足它；而这条用例也是那句纪律
    // （写在 `opponent_hook` 声明处）的可执行版本。
    const auto hash_after = [](int threads) {
        rts::BatchedEnvInit bi;
        for (int k = 0; k < 6; ++k) bi.worlds.push_back(one(k, 2));
        bi.side = rts::Side::Attacker;
        bi.threads = threads;
        // 按**下标**决定动作：下标传错就会算出不同的哈希。
        bi.opponent_hook = [](rts::World& w, int i) {
            std::vector<rts::UnitId> ids;
            w.enumerate_units(rts::Side::Defender, ids);
            std::vector<rts::UnitAction> a(
                ids.size(), i % 2 == 0 ? rts::UnitAction::AtkNear
                                       : rts::UnitAction::MoveN);
            w.submit_actions(rts::Side::Defender, a.data(), a.size());
        };
        rts::BatchedEnv e(std::move(bi));
        Bufs b(6);
        std::vector<rts::UnitAction> go(
            6 * static_cast<std::size_t>(rts::BatchedEnv::kMaxUnitsPerEnv),
            rts::UnitAction::MoveN);
        for (int k = 0; k < 30; ++k) e.step(go, b.done);
        std::vector<std::uint64_t> hs;
        for (int i = 0; i < 6; ++i) hs.push_back(e.world_at(i).state_hash());
        return hs;
    };
    const std::vector<std::uint64_t> one_thread = hash_after(1);
    const std::vector<std::uint64_t> many = hash_after(6);
    REQUIRE(one_thread.size() == many.size());
    for (std::size_t k = 0; k < many.size(); ++k) {
        CAPTURE(k, one_thread[k], many[k]);
        CHECK(one_thread[k] == many[k]);
    }
}

TEST_CASE("训练全灭立即终局并冻结，重置后可继续", "[batchenv]") {
    auto init = one(0, 0);
    rts::BatchedEnvInit bi;
    bi.worlds = {init};
    bi.ticks_per_step = 1;
    bi.max_ticks_per_episode = 100;
    rts::BatchedEnv env(std::move(bi));
    Bufs buffers(1);
    std::vector<rts::UnitAction> actions(rts::BatchedEnv::kMaxUnitsPerEnv, rts::UnitAction::Stop);
    env.step(actions, buffers.done);
    CHECK(buffers.done[0] == 1);
    CHECK(env.episode_ends()[0] == rts::BatchedEnv::EpisodeEnd::AttackersEliminated);
    const auto frozen = env.world_at(0).state_hash();
    env.step(actions, buffers.done);
    CHECK(env.world_at(0).state_hash() == frozen);
    env.reset_one(0, one(1, 1));
    CHECK(env.episode_ends()[0] == rts::BatchedEnv::EpisodeEnd::Running);
    env.step(actions, buffers.done);
    CHECK(buffers.done[0] == 0);
}

TEST_CASE("编队身份在队长替换与观测压缩后仍可匹配", "[batchenv]") {
    auto init = one(0, 3);
    init.units[0].squad = 2;
    init.units[1].squad = 2;
    init.units[2].squad = 7;
    rts::BatchedEnvInit bi;
    bi.worlds = {init};
    rts::BatchedEnv env(std::move(bi));
    std::vector<std::int64_t> keys(rts::BatchedEnv::kMaxUnitsPerEnv);
    env.agent_keys(keys);
    CHECK(keys[0] == 3);
    CHECK(keys[1] == 8);
    CHECK(keys[2] == 0);
    // Remove the original squad leader; the remaining member represents same agent.
    init.units.erase(init.units.begin());
    env.reset_one(0, init);
    env.agent_keys(keys);
    CHECK(keys[0] == 3);
    CHECK(keys[1] == 8);
    // Remove the whole first squad; second squad compacts into row zero.
    init.units.erase(init.units.begin());
    env.reset_one(0, init);
    env.agent_keys(keys);
    CHECK(keys[0] == 8);
    CHECK(keys[1] == 0);
    CHECK_THROWS_AS(env.agent_keys(std::span<std::int64_t>{}), rts::ContractError);
}

TEST_CASE("攻方全灭仍等待已发射的致胜弹丸", "[batchenv]") {
    auto init = one(0, 0);
    init.buildings[0].hp = 1;
    init.units = {
        rts::UnitInit{rts::UnitType::Shade, {4.5f, 2.5f}, 1, 1, 1},
        rts::UnitInit{rts::UnitType::Spear, {4.6f, 2.5f}, 1, 20, 20}};
    auto& shade = init.stats.unit[static_cast<std::size_t>(rts::UnitType::Shade)];
    shade.damage = 500;
    shade.range = 6.0f;
    shade.windup_ticks = 0;
    shade.proj_speed = 0.1f;
    auto& spear = init.stats.unit[static_cast<std::size_t>(rts::UnitType::Spear)];
    spear.damage = 100;
    spear.windup_ticks = 3;
    rts::BatchedEnvInit bi;
    bi.worlds = {init};
    bi.ticks_per_step = 1;
    bi.opponent_hook = [](rts::World& w, int) {
        const rts::UnitAction a = rts::UnitAction::AtkNear;
        w.submit_actions(rts::Side::Defender, &a, 1);
    };
    rts::BatchedEnv env(std::move(bi));
    Bufs buffers(1);
    std::vector<rts::UnitAction> actions(rts::BatchedEnv::kMaxUnitsPerEnv, rts::UnitAction::AtkBld);
    bool saw_pending_after_death = false;
    for (int i = 0; i < 80 && !buffers.done[0]; ++i) {
        env.step(actions, buffers.done);
        if (env.unit_counts()[0] == 0 && !buffers.done[0]) saw_pending_after_death = true;
    }
    CHECK(saw_pending_after_death);
    CHECK(env.episode_ends()[0] == rts::BatchedEnv::EpisodeEnd::KeepDestroyed);
}
