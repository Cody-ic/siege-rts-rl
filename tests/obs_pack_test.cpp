// 观测打包（`rts/obs_pack.hpp`）。
//
// **这份测试的主体不是「打包器算得对不对」，是硬要求 1。**
//
// `CLAUDE.md`「RL 侧两条硬要求」第 1 条是「AI 的观测必须是它自己的迷雾状态，
// 不得是 ground truth」。而 `WorldView` 是 god 视角（它文件头明说，那是不变量 2
// 的落点），所以那一条**只能由打包器保证**——`rts_core 接口契约.md` §5.5 因此
// 长期把它标成「要靠**评审**而非编译器把关」。
//
// 打包器一落地，那句话就该作废：它是可测的。下面三条就是它的可执行形式。
//
// ## 破坏性验证（每条都做过，改这份文件前请重做）
//
// | 把 `obs_pack.cpp` 改成 | 应当红的用例 |
// |---|---|
// | 敌方单位那道 `vis_of(...) != Vis::Visible` 摘掉 | 「迷雾之外的敌人不进张量」 |
// | 敌方单位那道门放宽成 `!= Vis::Unseen`（认记忆） | 「记忆记建筑、不记单位」 |
// | 己方单位也过迷雾 | 「己方不受迷雾影响」 |
// | `vis_value(v)` 换成 `v != Unseen ? 1 : 0` | 「visible 是三档」 |
//
// 数值全部是测试自带的占位表，与 `mechanics_test.cpp` 同一套路数。

#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/obs.hpp"
#include "rts/obs_pack.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

namespace {

rts::UnitStats& us(rts::StatsTable& t, rts::UnitType u) {
    return t.unit[static_cast<std::size_t>(u)];
}

rts::StatsTable pack_stats() {
    rts::StatsTable t;
    for (rts::UnitStats& u : t.unit) {
        u.max_hp = 20;
        u.vision = 3.0f;      // 小视野，方便把敌人放到雾外
        u.cooldown_ticks = 3;
        u.windup_ticks = 4;   // 前摇 4，好让 windup_left 有几档可看
    }
    us(t, rts::UnitType::Archer).max_hp = 20;
    us(t, rts::UnitType::Ghoul).max_hp = 40;
    t.bld[static_cast<std::size_t>(rts::BldType::Keep)].max_hp = 200;
    t.bld[static_cast<std::size_t>(rts::BldType::Wall)].max_hp = 50;
    t.global = {100, 100};
    return t;
}

rts::WorldInit arena(int w = 40, int h = 40) {
    rts::WorldInit init;
    init.width = w;
    init.height = h;
    init.terrain.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                        rts::Terrain::Plain);
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.seed = 7;
    init.map_id = "obspack-arena";
    init.nominal_level = 2;
    init.stats = pack_stats();
    return init;
}

// 三块缓冲区打包成一个结构，省得每条用例各写三个 vector。
//
// **必须用圆括号，不能用花括号。** `std::vector<float>{N, 0.0f}` 走的是
// `initializer_list` 构造，得到的是**两个元素**（值 `(float)N` 与 `0.0f`），
// 不是 N 个 0——初版就是这么写的，GCC 的 `-Warray-bounds` 在编译期抓出来
// （报「array subscript 1652 is outside array bounds of 'float [2]'」，
// 那个 `[2]` 正是 initializer_list 的长度）。
//
// 顺带印证了 `pack_unit_obs` 那道长度检查的价值：即便编译器没抓到，
// 它也会在运行时抛——而**没有那道检查的版本会让 Python 侧 reshape 照样成功**，
// 那正是 `obs.hpp` 文件头列为「本项目最防的失败形态」的一类。
struct Buf {
    std::vector<float> cells =
        std::vector<float>(static_cast<std::size_t>(rts::kObsCellFloats), 0.0f);
    std::vector<float> self =
        std::vector<float>(static_cast<std::size_t>(rts::kObsSelfFloats), 0.0f);
    std::vector<float> glob =
        std::vector<float>(static_cast<std::size_t>(rts::kObsGlobalFloats), 0.0f);

    void pack(const rts::WorldView& v, rts::UnitId id,
              const rts::FlowField* flow = nullptr,
              const rts::ObsNorms& n = {}) {
        rts::pack_unit_obs(v, id, flow, n, cells, self, glob);
    }
    // 窗口坐标 (dx, dy) 上某条通道的值。
    float at(int dx, int dy, rts::ObsChannel ch) const {
        return cells[(static_cast<std::size_t>(dy) *
                          static_cast<std::size_t>(rts::kObsK) +
                      static_cast<std::size_t>(dx)) *
                         static_cast<std::size_t>(rts::kObsChannelCount) +
                     static_cast<std::size_t>(ch)];
    }
};

constexpr int kHalf = rts::kObsK / 2;

}  // namespace

TEST_CASE("缓冲区长度必须恰好——长度对不上时 Python 侧 reshape 照样成功", "[obspack]") {
    rts::World w(arena());
    const rts::UnitId a =
        w.spawn_unit(rts::UnitType::Archer, rts::Vec2{20.5f, 20.5f}, 1, 20, 20);
    const rts::WorldView v = w.view(rts::Side::Defender);
    Buf b;
    std::vector<float> shortv(static_cast<std::size_t>(rts::kObsCellFloats) - 1, 0.0f);
    REQUIRE_THROWS_AS(rts::pack_unit_obs(v, a, nullptr, {}, shortv, b.self, b.glob),
                      rts::ContractError);
}

TEST_CASE("拿别人那一侧的单位打包必须抛——那是把 god 视角换个形状", "[obspack]") {
    rts::World w(arena());
    const rts::UnitId g =
        w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{20.5f, 20.5f}, 1, 40, 40);
    Buf b;
    // 守方的视图 + 攻方的单位。
    REQUIRE_THROWS_AS(b.pack(w.view(rts::Side::Defender), g), rts::ContractError);
    // 反过来是合法的。
    REQUIRE_NOTHROW(b.pack(w.view(rts::Side::Attacker), g));
}

// ——硬要求 1，三条——

TEST_CASE("硬要求 1：迷雾之外的敌人一个字节都不进张量", "[obspack]") {
    rts::World w(arena());
    const rts::UnitId a =
        w.spawn_unit(rts::UnitType::Archer, rts::Vec2{20.5f, 20.5f}, 1, 20, 20);
    // 视野是 3 格，把敌人放到窗口内但视野外（距离 6 格）。
    // **这两件事必须同时成立**，否则测的就不是「过滤」而是「窗口裁剪」。
    w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{26.5f, 20.5f}, 1, 40, 40);
    w.advance(1);   // 让视野解算跑一遍，写出迷雾

    Buf b;
    b.pack(w.view(rts::Side::Defender), a);
    const int dx = kHalf + 6;
    const int dy = kHalf;
    REQUIRE(dx < rts::kObsK);   // 确认它真在窗口里
    CHECK(b.at(dx, dy, rts::ObsChannel::EnemyDensity) == 0.0f);
    CHECK(b.at(dx, dy, rts::ObsChannel::EnemyHp) == 0.0f);
    CHECK(b.at(dx, dy, rts::ObsChannel::EnemyLevel) == 0.0f);
    // 而 `visible` 必须是 0（从未见过）——**这一条是上面三个 0 的意义所在**：
    // 没有它，「那格没有敌人」与「那格看不见」在张量里是同一串字节，
    // 于是 AI 会把所有迷雾格当空地，永远不学「先派斥候」。
    CHECK(b.at(dx, dy, rts::ObsChannel::Visible) == 0.0f);

    // 对照：把敌人挪进视野，同样三条必须有值——否则上面的 0 可能只是
    // 打包器根本不写敌方通道。
    rts::World w2(arena());
    const rts::UnitId a2 =
        w2.spawn_unit(rts::UnitType::Archer, rts::Vec2{20.5f, 20.5f}, 1, 20, 20);
    w2.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{22.5f, 20.5f}, 1, 40, 40);
    w2.advance(1);
    Buf b2;
    b2.pack(w2.view(rts::Side::Defender), a2);
    CHECK(b2.at(kHalf + 2, kHalf, rts::ObsChannel::EnemyDensity) > 0.0f);
    CHECK(b2.at(kHalf + 2, kHalf, rts::ObsChannel::EnemyHp) > 0.0f);
    CHECK(b2.at(kHalf + 2, kHalf, rts::ObsChannel::Visible) == 1.0f);
}

TEST_CASE("硬要求 1：记忆记建筑、不记单位", "[obspack]") {
    // 设计依据是 `CLAUDE.md`「迷雾记建筑、不记单位」——建筑不动，
    // 而记住的单位位置会**主动误导**，那份误导正是玩家佯攻的手段。
    rts::WorldInit init = arena();
    init.buildings.push_back(
        rts::BldInit{rts::BldType::Wall, rts::GridPos{24, 20}, 50, 50});
    rts::World w(init);
    const rts::UnitId scout =
        w.spawn_unit(rts::UnitType::Scout, rts::Vec2{24.5f, 20.5f}, 1, 20, 20);
    // 斥候站在墙上那一格旁边先看一眼，于是墙进了记忆。
    w.advance(1);
    // 再放一个敌人在同一格附近，然后把斥候撤走 —— 这样那一片变成「记忆」。
    w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{24.5f, 21.5f}, 1, 40, 40);
    w.advance(1);
    w.kill_unit(scout);
    // 换一个远处的观测者：它看不见那一片，但队友的记忆是**共享**的（每侧一份雾）。
    const rts::UnitId far =
        w.spawn_unit(rts::UnitType::Archer, rts::Vec2{20.5f, 20.5f}, 1, 20, 20);
    w.advance(1);

    Buf b;
    b.pack(w.view(rts::Side::Defender), far);
    const int dxw = kHalf + 4;   // 墙在 (24,20)，观测者在 (20,20)
    const int dyw = kHalf;
    const rts::Vis vw = w.fog(rts::Side::Defender).at(24, 20);
    if (vw == rts::Vis::Remembered) {
        // 建筑**要**从记忆里出来
        CHECK(b.at(dxw, dyw, rts::ObsChannel::WallHp) > 0.0f);
        CHECK(b.at(dxw, dyw, rts::ObsChannel::Visible) == 0.5f);
        // 而同一片上的**单位**必须是 0：那个食尸鬼当时被看见过。
        CHECK(b.at(dxw, dyw + 1, rts::ObsChannel::EnemyDensity) == 0.0f);
        CHECK(b.at(dxw, dyw + 1, rts::ObsChannel::EnemyHp) == 0.0f);
    } else {
        // 视野解算的半径 / 时序若与预期不同，这条用例就没在测它想测的东西。
        // **失败而不是静默跳过**——静默跳过的守卫等于没有守卫。
        FAIL("(24,20) 应当处于「记忆」态，实测 " << rts::ident_of(vw)
             << "；这条用例的前提没成立，请调整站位或视野半径");
    }
}

TEST_CASE("硬要求 1 的另一半：己方单位不受迷雾影响", "[obspack]") {
    // 不对称是设计：自己的部队自己知道在哪，过滤它只会让策略瞎。
    rts::World w(arena());
    const rts::UnitId a =
        w.spawn_unit(rts::UnitType::Archer, rts::Vec2{20.5f, 20.5f}, 1, 20, 20);
    // 队友放在视野外（6 格 > vision 3）。
    w.spawn_unit(rts::UnitType::Spear, rts::Vec2{26.5f, 20.5f}, 1, 20, 20);
    w.advance(1);
    Buf b;
    b.pack(w.view(rts::Side::Defender), a);
    CHECK(b.at(kHalf + 6, kHalf, rts::ObsChannel::AllyDensity) > 0.0f);
    CHECK(b.at(kHalf + 6, kHalf, rts::ObsChannel::AllyHp) > 0.0f);
}

TEST_CASE("visible 是三档，不是两档", "[obspack]") {
    // 两档时「从未侦查过」与「记忆里那段墙是破的」撞成同一串字节，
    // 而后者正是核心评估指标「AI 是否发现并利用已有缺口」要的东西
    // （`rts/fog.hpp` 文件头）。所以取值只可能是 0 / 0.5 / 1 三个。
    rts::World w(arena());
    const rts::UnitId a =
        w.spawn_unit(rts::UnitType::Archer, rts::Vec2{20.5f, 20.5f}, 1, 20, 20);
    w.advance(2);
    Buf b;
    b.pack(w.view(rts::Side::Defender), a);
    bool saw_visible = false, saw_unseen = false;
    for (int dy = 0; dy < rts::kObsK; ++dy) {
        for (int dx = 0; dx < rts::kObsK; ++dx) {
            const float v = b.at(dx, dy, rts::ObsChannel::Visible);
            CHECK((v == 0.0f || v == 0.5f || v == 1.0f));
            if (v == 1.0f) saw_visible = true;
            if (v == 0.0f) saw_unseen = true;
        }
    }
    // 视野 3、窗口 15：两档都该出现，否则这条用例没在测东西。
    CHECK(saw_visible);
    CHECK(saw_unseen);
}

// ——归一化与自身向量——

TEST_CASE("自身向量：兵种 one-hot 恰好一个 1，前摇是剩余比例", "[obspack]") {
    rts::World w(arena());
    const rts::UnitId a =
        w.spawn_unit(rts::UnitType::Archer, rts::Vec2{20.5f, 20.5f}, 1, 10, 20);
    w.spawn_unit(rts::UnitType::Ghoul, rts::Vec2{21.5f, 20.5f}, 1, 40, 40);
    Buf b;
    b.pack(w.view(rts::Side::Defender), a);

    int ones = 0;
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        if (b.self[static_cast<std::size_t>(i)] == 1.0f) ++ones;
    }
    REQUIRE(ones == 1);
    REQUIRE(b.self[static_cast<std::size_t>(rts::UnitType::Archer)] == 1.0f);
    // 血量 10/20
    CHECK(b.self[static_cast<std::size_t>(rts::ObsSelfField::HpFrac)] == 0.5f);
    // 等级 1 ÷ 名义等级 2
    CHECK(b.self[static_cast<std::size_t>(rts::ObsSelfField::LevelNorm)] == 0.5f);
    // 还没出手，前摇剩余是 0
    CHECK(b.self[static_cast<std::size_t>(rts::ObsSelfField::WindupLeft)] == 0.0f);

    // 让它承诺一次攻击，前摇应当变成正数且 ≤ 1。
    w.advance(1);
    Buf b2;
    b2.pack(w.view(rts::Side::Defender), a);
    const float wl = b2.self[static_cast<std::size_t>(rts::ObsSelfField::WindupLeft)];
    CHECK(wl > 0.0f);
    CHECK(wl <= 1.0f);
}

TEST_CASE("密度按 cell_capacity 归一，同格叠加", "[obspack]") {
    // 单位**互不阻挡**（`mechanics.cpp` 的 `cell_open` 只判地形 / 障碍 / 建筑），
    // 所以同格多人是正常局面，密度必须是加起来的。
    rts::World w(arena());
    const rts::UnitId a =
        w.spawn_unit(rts::UnitType::Archer, rts::Vec2{20.2f, 20.2f}, 1, 20, 20);
    w.spawn_unit(rts::UnitType::Spear, rts::Vec2{20.8f, 20.8f}, 1, 20, 20);
    Buf b;
    rts::ObsNorms n;
    n.cell_capacity = 4.0f;
    b.pack(w.view(rts::Side::Defender), a, nullptr, n);
    // 两个都在 (20,20)：2 ÷ 4
    CHECK(b.at(kHalf, kHalf, rts::ObsChannel::AllyDensity) == 0.5f);
}

TEST_CASE("全局标量：wave 取 log2(1+w)，空军比例当前恒为 0", "[obspack]") {
    rts::World w(arena());
    Buf b;
    rts::pack_globals(w.view(rts::Side::Attacker), {}, b.glob);
    // 第 1 波 → log2(2) = 1
    CHECK(b.glob[static_cast<std::size_t>(rts::ObsGlobal::WaveLog)] == 1.0f);
    // 没有空军 —— 这条通道现在恒 0，是「空军还没进 demo」而不是打包器坏了
    CHECK(b.glob[static_cast<std::size_t>(rts::ObsGlobal::AerialAliveFrac)] == 0.0f);

    // 放一只不死鸟，比例应当变成 1/cap。
    w.spawn_unit(rts::UnitType::Phoenix, rts::Vec2{5.5f, 5.5f}, 1, 20, 20);
    rts::ObsNorms n;
    n.aerial_cap = 4.0f;
    rts::pack_globals(w.view(rts::Side::Attacker), n, b.glob);
    CHECK(b.glob[static_cast<std::size_t>(rts::ObsGlobal::AerialAliveFrac)] == 0.25f);
}

TEST_CASE("地形不过迷雾，越界格全 0", "[obspack]") {
    // 地形是免费信息（`CLAUDE.md` 的情报划分里它不在「需要侦查」那一列），
    // 所以 `passable` 不该被迷雾遮住——遮住的话 AI 连地图形状都学不到。
    rts::World w(arena());
    // 观测者贴着地图左上角，于是窗口一半在图外。
    const rts::UnitId a =
        w.spawn_unit(rts::UnitType::Archer, rts::Vec2{1.5f, 1.5f}, 1, 20, 20);
    w.advance(1);
    Buf b;
    b.pack(w.view(rts::Side::Defender), a);
    // 图外：全 0
    CHECK(b.at(0, 0, rts::ObsChannel::Passable) == 0.0f);
    CHECK(b.at(0, 0, rts::ObsChannel::Visible) == 0.0f);
    // 图内但视野外（右下方远处）：`passable` 有值而 `visible` 是 0
    CHECK(b.at(kHalf + 6, kHalf + 6, rts::ObsChannel::Passable) == 1.0f);
    CHECK(b.at(kHalf + 6, kHalf + 6, rts::ObsChannel::Visible) == 0.0f);
}
