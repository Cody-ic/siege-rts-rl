// 素材自动发现（`game::asset_paths.hpp`）——「双击 exe 也能开局」的那一层。
//
// 它值得被测的地方不是「找得到」，而是**找不到时要真的找不到**：一个只查
// `game/` 存不存在的判据会在半个仓库上误判成功，而那种失败的症状是
// 「我改了地图怎么没生效」（读的是另一份）。

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/asset_paths.hpp"
#include "rts/rng.hpp"
#include "rts/utf8_path.hpp"

namespace {

// 池子随机选图消耗 `rng` 的状态，所以每个用例都要一份**自己的**固定种子实例，
// 不能共用一个全局的——共用的话用例执行顺序会悄悄影响各自选到的文件。
rts::Rng fixed_rng() { return rts::Rng(1); }

}  // namespace

TEST_CASE("从 game/data 逐级向上能找到仓库根", "[assets]") {
    // 起点用编译期烤进来的 `GAME_DATA_DIR`（= <仓库根>/game/data），
    // **不依赖 ctest 的工作目录**——同 tests/CMakeLists.txt 里那条理由。
    const std::vector<std::string> starts = {std::string(GAME_DATA_DIR)};
    rts::Rng rng = fixed_rng();
    const auto found = game::discover_assets(starts, rng);
    REQUIRE(found.has_value());

    // 三条路径都必须真的存在（`assets_under` 本来就是这么判的，这里再确认一次
    // 它返回的是**拼好的**路径而不是相对片段）。地图来自池子，具体是哪一张
    // 不重要，重要的是它真的存在、真的能读。
    bool ok = false;
    rts::read_file_bytes(found->map, &ok);
    REQUIRE(ok);
    rts::read_file_bytes(found->stats, &ok);
    REQUIRE(ok);
    rts::read_file_bytes(found->sprites + "/" + std::string(game::kSpriteMetaName), &ok);
    REQUIRE(ok);
}

TEST_CASE("向上的级数是有限的：走不到就得放弃", "[assets]") {
    // `max_up = 0` 只看起点自己。`game/data` 不是仓库根，所以必须找不到——
    // 这条同时证明上一条不是「碰巧哪一级都返回真」。
    const std::vector<std::string> starts = {std::string(GAME_DATA_DIR)};
    rts::Rng rng0 = fixed_rng();
    REQUIRE_FALSE(game::discover_assets(starts, rng0, /*max_up=*/0).has_value());
    // 两级刚好到仓库根（data → game → 根）。
    rts::Rng rng2 = fixed_rng();
    REQUIRE(game::discover_assets(starts, rng2, /*max_up=*/2).has_value());
}

TEST_CASE("不存在的目录与空串都安全地返回「没有」", "[assets]") {
    rts::Rng rng = fixed_rng();
    REQUIRE_FALSE(game::assets_under("", rng).has_value());
    REQUIRE_FALSE(game::assets_under(std::string(GAME_DATA_DIR) + "/no_such_dir", rng)
                      .has_value());
    // 起点全是空串时不该崩，也不该从当前工作目录开始猜。
    REQUIRE_FALSE(game::discover_assets({std::string(), std::string()}, rng).has_value());
    REQUIRE_FALSE(game::discover_assets({}, rng).has_value());
}

TEST_CASE("从 exe 路径推目录：给的是文件也要能拿到它所在的目录", "[assets]") {
    const std::string f = std::string(GAME_DATA_DIR) + "/demo_skirmish.json";
    const std::string dir = game::parent_dir_of(f);
    REQUIRE(dir == std::string(GAME_DATA_DIR));
    REQUIRE(game::parent_dir_of("").empty());

    // 真实用法：`args[0]` 是 exe 的绝对路径，从它的目录向上找。这里用数据文件
    // 代替 exe（构建目录的位置随生成器变，写死它反而不可靠）。
    rts::Rng rng = fixed_rng();
    REQUIRE(game::discover_assets({dir}, rng).has_value());
}

TEST_CASE("工作目录取得到，且能作为发现的起点", "[assets]") {
    const std::string cwd = game::current_dir();
    REQUIRE_FALSE(cwd.empty());
    // **刻意不断言它能找到仓库根**：ctest 的工作目录是构建目录，而构建目录
    // 完全可以在仓库外（`cmake -B ../build`）。断言它命中就等于给这条测试
    // 加了一个「构建目录必须在仓库里」的隐藏前提——那种前提迟早让人对着一条
    // 与它无关的红色测试查半天。
    rts::Rng rng = fixed_rng();
    (void)game::discover_assets({cwd}, rng);
}

TEST_CASE("地图从池子随机选：选中的必须真是池子里的文件", "[assets]") {
    const std::string pool = std::string(GAME_DATA_DIR) + "/maps/pool";
    rts::Rng rng = fixed_rng();
    const auto picked = game::pick_pool_map(pool, rng);
    REQUIRE(picked.has_value());
    bool ok = false;
    rts::read_file_bytes(*picked, &ok);
    REQUIRE(ok);
    // 选中的路径必须落在 pool 目录下（而不是随便返回点别的东西）。
    REQUIRE(picked->find("pool") != std::string::npos);
}

TEST_CASE("地图从池子随机选：不同种子能选出不同的文件", "[assets]") {
    const std::string pool = std::string(GAME_DATA_DIR) + "/maps/pool";
    rts::Rng rng_a(1);
    rts::Rng rng_b(2);
    const auto a = game::pick_pool_map(pool, rng_a);
    const auto b = game::pick_pool_map(pool, rng_b);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    // 池子里现在有一批文件，不同种子的下标大概率不同——但不强求"必须不同"
    // （那样测试会依赖池子的具体大小），只验证"至少能选出多个不同的下标"：
    // 拿一串种子扫一遍，出现过至少两个不同结果。
    std::vector<std::string> seen;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        rts::Rng r(seed);
        const auto p = game::pick_pool_map(pool, r);
        REQUIRE(p.has_value());
        if (std::find(seen.begin(), seen.end(), *p) == seen.end()) {
            seen.push_back(*p);
        }
    }
    REQUIRE(seen.size() > 1);
}

TEST_CASE("地图池：目录不存在或是空目录都安全返回「没有」", "[assets]") {
    rts::Rng rng = fixed_rng();
    REQUIRE_FALSE(game::pick_pool_map("", rng).has_value());
    REQUIRE_FALSE(game::pick_pool_map(std::string(GAME_DATA_DIR) + "/no_such_pool", rng)
                      .has_value());
}
