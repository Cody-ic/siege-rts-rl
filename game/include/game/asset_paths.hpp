// 「一局游戏的最小输入在哪儿」——素材自动发现。
//
// ## 为什么需要它
//
// 在这之前，开一局的唯一办法是三个必填参数：
//
//     rts_render --battle --map game/data/maps/pool/gen_01001000.json
//                --stats game/data/stats_placeholder.json --sprites tools/sprite_gen/out_3d
//
// （上面两行原本用行尾反斜杠续行写成一条命令。**在 `//` 注释里不能那么写**：
//  行尾的 `\` 是续行符，于是 GCC 把下一行也吞进注释并报 `-Werror=comment`
//  ——`main` 因此在 Linux 上编不过，而 MSVC 一声不响。要贴可复制的多行命令，
//  用 ``` 代码块或 `/* */`，不要在 `//` 里用反斜杠。）
//
// 那对**开发者**是好的（显式、可换图、可换表），对**双击 exe 的人**是不可用的：
// 资源管理器不传参数，而 `rts_render.exe` 躺在 `build/render/Release/` 里。
// 所以要有一条「什么都不给也能开局」的路径，而它必须自己找到那三样东西。
//
// ## 不给 `--map` 时用哪张图：从池子随机选一张，不是固定一张
//
// 2026-08-31 起，「实战默认地图」与「答辩用的固定演示图」被拆成了两件事——
// 前者要让玩家每次打开游戏都不一样（持久可玩性），后者要能反复打磨、稳定复现。
// 因此 `kDefaultMapRel`（单个文件）换成了 `kMapPoolDirRel`（一个目录）：
// `game/data/maps/pool/` 下的每一张都经 `tools/map_gen/generate.py` 生成、
// 过完整校验器，`assets_under()` 从目录里随机选一张。手写的参考图
// `game/data/maps/reference_border_keep_01.json` **不在池子里**，永远不会被
// 选中——它只用于展示地图格式（含 `Water`/`Bridge`），需要显式 `--map` 才能打开。
//
// 真随机只允许发生在**一个地方**：`render/src/main.cpp` 里构造的那个
// `rts::Rng`（用挂钟播种）。本文件与其余 `game/` 代码只接受调用方传入的
// `rts::Rng&`，本身不含任何随机源——这样 `pick_pool_map()`/`assets_under()`/
// `discover_assets()` 才能用固定种子测试，且 `tools/check_determinism_bans.py`
// （只扫 `rts_core/` 与 `game/`）不会因为这里出现挂钟或分布适配器而报警；
// `render/` 不在它的扫描范围内，这正是把随机源放在那里而不是这里的理由。
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

#include "rts/rng.hpp"

namespace game {

// 一局游戏的最小输入。四条都是 UTF-8 字节串（约定同 `MapLoader::from_file`）。
struct AssetPaths {
    std::string root;      // 仓库根（找到标记文件的那一级）
    std::string map;       // 本局用的地图 JSON（从池子随机选出的那一张）
    std::string stats;     // 数值表 JSON
    std::string sprites;   // 精灵成品目录（`_sprite_meta.json` 所在的目录）
};

// 标记文件，**相对仓库根**。三条一起构成「这是不是仓库根」的判据：
// 只查一条会在某个恰好也有 `game/` 的目录上误判，而三条同时命中的目录只有仓库根。
//
// 它们同时是「默认开局用哪张图、哪份表」的**唯一登记处**——写死在 `render/` 里
// 就等于同一件事写在两处，而那必然漂移（本仓库的常客）。
inline constexpr std::string_view kMapPoolDirRel = "game/data/maps/pool";
inline constexpr std::string_view kDefaultStatsRel = "game/data/stats_placeholder.json";
inline constexpr std::string_view kDefaultSpritesRel = "tools/sprite_gen/out_3d";
// 精灵目录靠**它里面的元数据**判在不在，而不是靠目录存在——一个空目录同样存在，
// 而那会让判据在「素材还没生成」时误判成功，报错随之推迟到载入纹理那一层。
inline constexpr std::string_view kSpriteMetaName = "_sprite_meta.json";

// 从 `pool_dir_utf8` 下的 `*.json` 里随机选一个（**不递归**），返回其绝对路径。
// 目录不存在或一个 `.json` 都没有时返回空。
//
// 先列出全部文件再**排序**，才用 `rng.below(n)` 取下标——排序消掉了
// `std::filesystem::directory_iterator` 的枚举顺序依赖（该顺序随文件系统/平台
// 变化），否则同一个 `rng` 状态在不同机器上可能选出不同的文件。
std::optional<std::string> pick_pool_map(std::string_view pool_dir_utf8,
                                        rts::Rng& rng);

// `dir` 是不是仓库根（三条标记全部命中，其中地图那一条改为「池子目录非空」）。
// 是则给出四条绝对路径，`map` 是这次调用从池子里随机选中的那一张
// （消耗 `rng` 的状态——同一个 `rng` 实例连续调用两次通常会给出不同的图）。
std::optional<AssetPaths> assets_under(std::string_view dir_utf8, rts::Rng& rng);

// 从每个起点逐级向上找，最多 `max_up` 级。第一个命中的就用。
//
// 起点通常是「工作目录」与「exe 所在目录」两条（见文件头）。给一个向量而不是
// 两个参数：调用方还可能想加「用户拖进来的目录」，而那不该改这里的签名。
//
// `rng` 由调用方构造并传入——**这个函数本身不含任何随机源**，测试因此可以传一个
// 固定种子的 `rng` 得到可复现的结果；真正需要"每次启动不一样"的那个挂钟种子
// 只在 `render/src/main.cpp` 里出现一次。
std::optional<AssetPaths> discover_assets(const std::vector<std::string>& start_dirs,
                                         rts::Rng& rng, int max_up = 8);

// `path` 的父目录（UTF-8 进、UTF-8 出）。给「从 `args[0]` 推 exe 目录」用。
// 没有父目录时返回空串。
std::string parent_dir_of(std::string_view path_utf8);

// 当前工作目录（UTF-8）。取不到时返回空串。
std::string current_dir();

}  // namespace game

#endif  // GAME_ASSET_PATHS_HPP
