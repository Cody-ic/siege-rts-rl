// 玩家输入的**语义层**：把「点了哪一格」翻成命令（`game/` 管输入，CLAUDE.md
// 架构表），render/ 只负责拾取（screen_to_grid）与绘制（虚影、高亮）。
//
// 放在 game/ 而不是 render/ 的理由与 BattleScene 相同：这一层不含像素、
// 且要在默认构建里被测——「点墙下的是驻守不是开拔」是一条真断言，GCC 侧也验。
//
// ## 右键的语义：一个键，按格上的东西分三种命令
//
//   * 己方**完工**的墙 / 门 → `Garrison`（驻守——「墙段」的判据是 Wall‖Gate，
//     与机制第三批同一条）
//   * 活着的障碍 → `Clear`（清野）
//   * 其余 → `MoveForce`（开拔到那一格；执行层脚本按 flow field 走）
//
// 这不是要长成完整的交互设计——它是「基本可玩」的最小命令面：12 种命令里
// 玩家逐格下达的恰好这三种，其余（Build / Train / Summon…）各有自己的入口。

#ifndef GAME_PLAYER_INPUT_HPP
#define GAME_PLAYER_INPUT_HPP

#include <cstdint>

#include "rts/command.hpp"
#include "rts/types.hpp"
#include "rts/world_view.hpp"

namespace game {

// 右键点击 → 命令。`cell` 必须在界内（越界归拾取层挡，这里 assert）。
rts::Command command_for_click(const rts::WorldView& view, std::uint8_t force,
                               rts::GridPos cell);

// 建造虚影的合法性**提示**（绿 / 红）。只查前端查得动的三条：地形可建、
// 格上没有建筑、没有障碍。**真正的裁决在 World 的 Build 解算**（资源够不够、
// 点位规则）——提示错了顶多虚影颜色骗一下，命令该拒还是拒，两层不会打架。
bool can_place_hint(const rts::WorldView& view, rts::GridPos cell);

}  // namespace game

#endif  // GAME_PLAYER_INPUT_HPP
