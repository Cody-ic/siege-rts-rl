// 素材自动发现（`game::asset_paths.hpp`）——「双击 exe 也能开局」的那一层。
//
// 它值得被测的地方不是「找得到」，而是**找不到时要真的找不到**：一个只查
// `game/` 存不存在的判据会在半个仓库上误判成功，而那种失败的症状是
// 「我改了地图怎么没生效」（读的是另一份）。

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/asset_paths.hpp"
#include "rts/utf8_path.hpp"

TEST_CASE("从 game/data 逐级向上能找到仓库根", "[assets]") {
    // 起点用编译期烤进来的 `GAME_DATA_DIR`（= <仓库根>/game/data），
    // **不依赖 ctest 的工作目录**——同 tests/CMakeLists.txt 里那条理由。
    const std::vector<std::string> starts = {std::string(GAME_DATA_DIR)};
    const auto found = game::discover_assets(starts);
    REQUIRE(found.has_value());

    // 三条路径都必须真的存在（`assets_under` 本来就是这么判的，这里再确认一次
    // 它返回的是**拼好的**路径而不是相对片段）。
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
    REQUIRE_FALSE(game::discover_assets(starts, /*max_up=*/0).has_value());
    // 两级刚好到仓库根（data → game → 根）。
    REQUIRE(game::discover_assets(starts, /*max_up=*/2).has_value());
}

TEST_CASE("不存在的目录与空串都安全地返回「没有」", "[assets]") {
    REQUIRE_FALSE(game::assets_under("").has_value());
    REQUIRE_FALSE(game::assets_under(std::string(GAME_DATA_DIR) + "/no_such_dir")
                      .has_value());
    // 起点全是空串时不该崩，也不该从当前工作目录开始猜。
    REQUIRE_FALSE(game::discover_assets({std::string(), std::string()}).has_value());
    REQUIRE_FALSE(game::discover_assets({}).has_value());
}

TEST_CASE("从 exe 路径推目录：给的是文件也要能拿到它所在的目录", "[assets]") {
    const std::string f = std::string(GAME_DATA_DIR) + "/demo_skirmish.json";
    const std::string dir = game::parent_dir_of(f);
    REQUIRE(dir == std::string(GAME_DATA_DIR));
    REQUIRE(game::parent_dir_of("").empty());

    // 真实用法：`args[0]` 是 exe 的绝对路径，从它的目录向上找。这里用数据文件
    // 代替 exe（构建目录的位置随生成器变，写死它反而不可靠）。
    REQUIRE(game::discover_assets({dir}).has_value());
}

TEST_CASE("工作目录取得到，且能作为发现的起点", "[assets]") {
    const std::string cwd = game::current_dir();
    REQUIRE_FALSE(cwd.empty());
    // **刻意不断言它能找到仓库根**：ctest 的工作目录是构建目录，而构建目录
    // 完全可以在仓库外（`cmake -B ../build`）。断言它命中就等于给这条测试
    // 加了一个「构建目录必须在仓库里」的隐藏前提——那种前提迟早让人对着一条
    // 与它无关的红色测试查半天。
    (void)game::discover_assets({cwd});
}
