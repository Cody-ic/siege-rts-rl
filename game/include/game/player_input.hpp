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
// 玩家逐格下达的恰好这三种，其余（Build / Train / Repair / Summon…）
// 各有自己的入口，见下面三个「模式」。
//
// ## 三种「点一格生效」的模式共用一套形状
//
// 建造 / 征兵 / 维修都是「先选模式，再点一格」。三者各有一个 `*_hint()`
// （虚影该绿还是红）与一个 `*_command()`（点下去发什么命令）。
//
// **hint 是提示，不是裁决。** 真正的裁决在 `World` 的解算里（买不买得起、
// 点位规则、兵营是不是正在练兵）。两层刻意不共用代码：hint 错了顶多虚影
// 颜色骗一下，命令该拒还是拒。但 hint 也**不能只查一半**——玩家点了没反应
// 而虚影是绿的，比红框更难懂，所以凡是前端查得动的结构性规则都查
// （资源点归属就是这类：它是「结构封死」的规则，不是数值）。

#ifndef GAME_PLAYER_INPUT_HPP
#define GAME_PLAYER_INPUT_HPP

#include <cstdint>
#include <vector>

#include "rts/command.hpp"
#include "rts/roster.hpp"
#include "rts/types.hpp"
#include "rts/world_view.hpp"

namespace game {

// 右键点击 → 命令。`cell` 必须在界内（越界归拾取层挡，这里 assert）。
rts::Command command_for_click(const rts::WorldView& view, std::uint8_t force,
                               rts::GridPos cell);

// ——建造——
//
// 建造面板上的建筑清单，**顺序就是 TAB 的循环顺序**。放在 `game/` 而不是
// `render/` 的理由与本文件其余部分相同：它要被测（`Keep` 不可在列——
// 「不可再建」是结构，见 `World` 的 Build 解算；采集三座必须在列——
// 少了它们石木两种资源永远没有收入，而那不是「少一个入口」，
// 是**整条经济轴不通**）。
const std::vector<rts::BldType>& buildable_types();

// 建造虚影的合法性**提示**（绿 / 红）。查四条：地形可建、格上没有建筑、
// 没有障碍、**资源点归属**（采集建筑必须落在对应种类的资源点上，其余建筑
// 不得占资源点——`World` 的 Build 解算里那条结构规则）。
//
// 第四条是后加的：少了它，玩家会在草地上看到一个绿框然后点出一个采石场，
// 而命令被静默拒绝。资源点在画面上没有专门的标记，所以这个绿框是唯一的线索。
bool can_place_hint(const rts::WorldView& view, rts::BldType bt, rts::GridPos cell);

rts::Command build_command(rts::BldType bt, rts::GridPos cell, int map_width);

// ——征兵——
//
// 可征的兵种清单（守方全部五种），顺序即 TAB 的循环顺序。
const std::vector<rts::UnitType>& trainable_types();

// 这一格能不能出兵：**完工**的 `Barrack` 或 `Keep`，且没有正在练的。
// 「`Keep` 也行」不是宽松，是 CLAUDE.md 花名册里那条兜底（兵营可退化为
// 从堡垒出兵），机制侧同一条。
bool can_train_hint(const rts::WorldView& view, rts::GridPos cell);

// 征兵命令。`force` 决定新兵进哪支编队——**这是玩家唯一能编队的入口**
// （命令枚举里没有「把单位编入编队」，#57 组内已定维持 12 种不变），
// 所以「往打薄的那支里补兵」就是这条路。
rts::Command train_command(rts::UnitType u, std::uint8_t force, rts::GridPos cell,
                           int map_width);

// ——维修——
//
// 这一格能不能修：己方**完工**的建筑、掉了血、且没有正在进行的工时
// （施工或上一次维修）。工地不「修」——工地是往前盖，那是 `Cancel` 的事。
bool can_repair_hint(const rts::WorldView& view, rts::GridPos cell);

rts::Command repair_command(rts::GridPos cell, int map_width);

}  // namespace game

#endif  // GAME_PLAYER_INPUT_HPP
