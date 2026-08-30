// 「一局游戏的最小输入在哪儿」——素材自动发现。
//
// ## 为什么需要它
//
// 在这之前，开一局的唯一办法是三个必填参数：
//
//     rts_render --battle --map game/data/demo_skirmish.json \
//                --stats game/data/stats_placeholder.json --sprites tools/sprite_gen/out_3d
//
// 那对**开发者**是好的（显式、可换图、可换表），对**双击 exe 的人**是不可用的：
// 资源管理器不传参数，而 `rts_render.exe` 躺在 `build/render/Release/` 里。
// 所以要有一条「什么都不给也能开局」的路径，而它必须自己找到那三样东西。
//
// ## 为什么是「找标记文件 + 逐级向上」，而不是一条相对路径
//
// 相对路径要相对**某个**目录，而那个目录在两种启动方式下不同：
//
//   * 从仓库根跑 CLI  → 工作目录 = 仓库根
//   * 从资源管理器双击 → 工作目录 = **exe 所在目录**（Explorer 的行为）
//
// 于是写死 `"game/data/..."` 只在前一种下成立，写死 `"../../../game/data/..."`
// 只在后一种下成立，而两条都会在别人换个构建目录名（`build-Release/`）时坏掉。
// 逐级向上找**标记文件**对两者都成立，且不关心中间有几层。
//
// ## 为什么在 `game/` 而不是 `render/`
//
// 与 `MapLoader` / `player_input` / `battle_scene` 同一条：它不含像素，
// 于是进默认构建、在 GCC 侧也被编被测。而它**值得被测**——「找不到就退回报错」
// 与「找错了但看起来能跑」这两种失败长得完全不同，后者的症状是
// 「我改了地图怎么没生效」（读的是另一份）。
//
// 素材路径一律走 `rts::path_from_utf8()`：仓库里已经有过一次「路径含中文就打不开」
// 的真实缺陷（见 `rts/utf8_path.hpp`），而这一层恰好要拼路径，是最容易再犯的地方。

#ifndef GAME_ASSET_PATHS_HPP
#define GAME_ASSET_PATHS_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// 一局游戏的最小输入。四条都是 UTF-8 字节串（约定同 `MapLoader::from_file`）。
struct AssetPaths {
    std::string root;      // 仓库根（找到标记文件的那一级）
    std::string map;       // 演示地图 JSON
    std::string stats;     // 数值表 JSON
    std::string sprites;   // 精灵成品目录（`_sprite_meta.json` 所在的目录）
};

// 标记文件，**相对仓库根**。三条一起构成「这是不是仓库根」的判据：
// 只查一条会在某个恰好也有 `game/` 的目录上误判，而三条同时命中的目录只有仓库根。
//
// 它们同时是「默认开局用哪张图、哪份表」的**唯一登记处**——写死在 `render/` 里
// 就等于同一件事写在两处，而那必然漂移（本仓库的常客）。
inline constexpr std::string_view kDefaultMapRel = "game/data/demo_skirmish.json";
inline constexpr std::string_view kDefaultStatsRel = "game/data/stats_placeholder.json";
inline constexpr std::string_view kDefaultSpritesRel = "tools/sprite_gen/out_3d";
// 精灵目录靠**它里面的元数据**判在不在，而不是靠目录存在——一个空目录同样存在，
// 而那会让判据在「素材还没生成」时误判成功，报错随之推迟到载入纹理那一层。
inline constexpr std::string_view kSpriteMetaName = "_sprite_meta.json";

// `dir` 是不是仓库根（三条标记全部命中）。是则给出四条绝对路径。
std::optional<AssetPaths> assets_under(std::string_view dir_utf8);

// 从每个起点逐级向上找，最多 `max_up` 级。第一个命中的就用。
//
// 起点通常是「工作目录」与「exe 所在目录」两条（见文件头）。给一个向量而不是
// 两个参数：调用方还可能想加「用户拖进来的目录」，而那不该改这里的签名。
std::optional<AssetPaths> discover_assets(const std::vector<std::string>& start_dirs,
                                         int max_up = 8);

// `path` 的父目录（UTF-8 进、UTF-8 出）。给「从 `args[0]` 推 exe 目录」用。
// 没有父目录时返回空串。
std::string parent_dir_of(std::string_view path_utf8);

// 当前工作目录（UTF-8）。取不到时返回空串。
std::string current_dir();

}  // namespace game

#endif  // GAME_ASSET_PATHS_HPP
