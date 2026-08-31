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
// **hint 是提示，不是裁决。** 真正的裁决在 `World` 的解算里——它拒绝时是
// **静默**丢弃（`break`，什么都不说），所以 hint 骗一次，玩家看到的就是
// 「点了没反应」而查不出原因。两层刻意不共用代码（hint 错了顶多虚影颜色骗
// 一下，命令该拒还是拒），但 hint 也**不能只查一半**——凡是前端查得动的
// 规则都要查，资源点归属（结构规则）与**造价够不够**（数值规则）是**同一条
// 要求的两个来源**，不能只补结构那一半。
//
// 造价查询与结构查询**分成两个函数**（`can_place_hint`/`can_afford_build`
// 这一对，`can_train_hint`/`can_afford_train`，`can_repair_hint`/
// `can_afford_repair`），理由是弹窗一次列出**好几个选项**、每项造价不同
// （建几种建筑、征哪个兵种），而「这一格能不能弹出菜单」在还不知道玩家会
// 选哪一项时就要判断——两者粒度不同，压成一个函数会把其中一层判断丢掉。
// 调用方（`render/` 的弹窗）对每一项把两者取 **AND** 才是完整的绿框判据；
// 只查结构不查造价，绿框在钱不够时仍然亮着，就是这一层曾经的 bug（一次
// 试玩报出来的：招兵/建筑/维修弹窗只查了位置合法性，没查资源够不够）。

#ifndef GAME_PLAYER_INPUT_HPP
#define GAME_PLAYER_INPUT_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "game/iso_projection.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
#include "rts/types.hpp"
#include "rts/world_view.hpp"

namespace game {

// 右键点击 → 命令。`cell` 必须在界内（越界归拾取层挡，这里 assert）。
//
// **`force` 只用来填 `Command::force`，不参与「这一格该翻成哪种命令」的
// 判断**——判断只看格上的东西（墙/门 → Garrison，活障碍 → Clear，其余 →
// MoveForce）。框选流程因此可以拿它当纯粹的「判语义」工具：传一个占位
// force（比如 0），只读返回值的 `.kind`/`.slot`，`.force` 由调用方按框选
// 到的编队重新决定（见 `distinct_forces`）——不必另写一份判断表。
rts::Command command_for_click(const rts::WorldView& view, std::uint8_t force,
                               rts::GridPos cell);

// 框选：给一个世界像素矩形（`IsoProjection::world_to_screen` 那套坐标系，
// 与鼠标拖拽经 `GetScreenToWorld2D` 得到的坐标同一套），返回落在其中的
// 己方单位。`ids` 必须是 `enumerate_units(Side::Defender, …)` 的产物——
// 这里只做几何判断，不重新枚举（枚举的时机与顺序归调用方）。
std::vector<rts::UnitId> units_in_rect(const rts::WorldView& view,
                                       std::span<const rts::UnitId> ids,
                                       const IsoProjection& proj, Rect rect);

// `ids` 里出现过的、互不相同的编队号（跳过 `kNoForce`）。**驻守要按编队
// 提交**——`rts_core` 的登墙机制（`World::tick_garrison`）天生按编队记账，
// 框选选出的是单位而不是编队，提交前要先把这批单位映射回它们各自的编队。
std::vector<std::uint8_t> distinct_forces(const rts::WorldView& view,
                                          std::span<const rts::UnitId> ids);

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

// 造价够不够（石 + 木）。**与 `can_place_hint` 分开查**：结构合法性与买不买得起
// 是两件独立的事——弹窗里同时列着好几种建筑，每种造价不同，两者混在一起会把
// 「这一格能不能盖东西」与「这一种买不买得起」这两条不同粒度的判断压成一条。
// 调用方（弹窗）把它与 `can_place_hint` 取 AND 才是完整的绿框判据。
bool can_afford_build(const rts::WorldView& view, rts::BldType bt);

rts::Command build_command(rts::BldType bt, rts::GridPos cell, int map_width);

// ——征兵——
//
// 可征的兵种清单（守方全部五种），顺序即 TAB 的循环顺序。
const std::vector<rts::UnitType>& trainable_types();

// 这一格能不能出兵：**完工**的 `Barrack` 或 `Keep`，且没有正在练的。
// 「`Keep` 也行」不是宽松，是 CLAUDE.md 花名册里那条兜底（兵营可退化为
// 从堡垒出兵），机制侧同一条。**不查造价**——这一格能不能弹出征兵菜单，
// 与「菜单里哪个兵种买得起」是两个不同粒度的问题，后者见 `can_afford_train`。
bool can_train_hint(const rts::WorldView& view, rts::GridPos cell);

// 造价够不够（金）。同 `can_afford_build` 的理由：弹窗里每个兵种一行，
// 造价各不相同，只有知道具体兵种才能判断买不买得起。
bool can_afford_train(const rts::WorldView& view, rts::UnitType ut);

// 征兵命令。`force` 决定新兵进哪支编队——**这是玩家唯一能编队的入口**
// （命令枚举里没有「把单位编入编队」，#57 组内已定维持 12 种不变），
// 所以「往打薄的那支里补兵」就是这条路。
rts::Command train_command(rts::UnitType u, std::uint8_t force, rts::GridPos cell,
                           int map_width);

// ——维修——
//
// 这一格能不能修：己方**完工**的建筑、掉了血、且没有正在进行的工时
// （施工或上一次维修）。工地不「修」——工地是往前盖，那是 `Cancel` 的事。
// **不查造价**，理由同上——见 `can_afford_repair`。
bool can_repair_hint(const rts::WorldView& view, rts::GridPos cell);

// 造价够不够（木，按缺口血量折算——公式与 `World` 的 `Repair` 解算逐字相同：
// `ceil(缺口 × repair_wood_per_1000hp ÷ 1000)`，两处算法分叉是「绿框骗人」这
// 类 bug 的另一个来源，所以照抄，不重新推一遍）。
bool can_afford_repair(const rts::WorldView& view, rts::GridPos cell);

// 修这一格要花多少木材——`can_afford_repair` 内部就是算这个数再比大小，
// 单独露出来是因为**弹窗要把价钱印在「维修」两个字后面**：光有红绿框，
// 玩家看不出到底差多少（一次试玩报出来的：维修选项只有「维修」两个字，
// 不知道要花什么）。这一层与建造/征兵的价钱标签同一个来源，理由见
// `render/` 的弹窗代码。这一格无效或没有缺口时返回 0。
std::int64_t repair_wood_cost(const rts::WorldView& view, rts::GridPos cell);

rts::Command repair_command(rts::GridPos cell, int map_width);

// ——升级（建筑等级上限，守方升级轴第一个输出）——
//
// 这一格**为什么**不能升级。`can_upgrade_hint` 就是它 `== None`，两者
// 共用同一份判定、不各写一遍（同 `can_afford_repair` 内部调
// `repair_wood_cost` 的做法）。
//
// **露出理由而不只给一个 bool，是因为其中一种原因在画面上没有任何线索。**
// 血条看得见掉血、施工进度看得见在忙，而「这座建筑几级」与「当前上限是
// 几级」画面上一个字都没有。于是 `LevelCap` 那一种若只表现为「菜单里没
// 有升级这一行」，玩家不会知道存在升级这件事，更不会想到该先去升堡垒
// ——而占位系数 `building_level_cap_divisor = 2` 下，1 级堡垒的上限就是
// 1 级，**开局每一座建筑都是 `LevelCap`**。所以弹窗对己方完工建筑
// **恒显示**升级行（`!= NoBuilding` 即显示），靠这个理由说明为什么灰着。
enum class UpgradeBlock : int {
    None = 0,      // 能升（还要另查造价，见 `can_afford_upgrade`）
    NoBuilding,    // 这一格没有己方活着的建筑：升级行不该出现
    Unbuilt,       // 是工地：工地往前盖，不谈升级（同 `can_repair_hint`）
    Busy,          // 在建 / 在修 / 已经在升——同一时刻只能有一件工程在推进
    LevelCap,      // 顶到 `building_level_cap()`（`Keep` 永远不会是这个）
};
UpgradeBlock upgrade_block(const rts::WorldView& view, rts::GridPos cell);

// 这一格的建筑现在几级——弹窗要把 `Lv2 → 3` 印出来，理由同上（等级在
// 画面上没有别的来源）。这一格没有活着的建筑时返回 0。
std::int32_t bld_level_at(const rts::WorldView& view, rts::GridPos cell);

// 这一格能不能升级：己方**完工**建筑、没有在建/在修/在升、没顶到
// `WorldView::building_level_cap()`（`Keep` 本身不受这条上限约束——
// 「堡垒等级本身不设上限」，CLAUDE.md）。**不查造价**，理由同
// `can_repair_hint`。
bool can_upgrade_hint(const rts::WorldView& view, rts::GridPos cell);

// 升一级要花多少石/木——判定逻辑只在这里查一遍 `view.stats()`，不重新推
// `rts_core/src/world.cpp` 的 `Upgrade` 解算（同 `repair_wood_cost` 的纪律：
// 两处算法分叉是「绿框骗人」这类 bug 的来源）。这一格无效时返回 0。
std::int64_t upgrade_cost_stone(const rts::WorldView& view, rts::GridPos cell);
std::int64_t upgrade_cost_wood(const rts::WorldView& view, rts::GridPos cell);

// 造价够不够（石 + 木都要够）。同 `can_afford_build`/`can_afford_repair`
// 的理由：与 `can_upgrade_hint` 分开查，粒度不同。
bool can_afford_upgrade(const rts::WorldView& view, rts::GridPos cell);

rts::Command upgrade_command(rts::GridPos cell, int map_width);

}  // namespace game

#endif  // GAME_PLAYER_INPUT_HPP
