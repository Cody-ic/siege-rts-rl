// 中文展示名。**这是「中文只出现在展示层」这条命名纪律的落点。**
//
// CLAUDE.md「世界观与命名」：代码中一律使用英文标识符，中文名只出现在展示层。
// 于是必须有**一个**地方把枚举翻成中文，否则每个用到它的地方各译一份，
// 同一个东西在 HUD 上叫「岩壁」、在提示里叫「山石」。
//
// 为什么在 `game/` 而不是 `render/`：
//
//   1. 它没有像素、不需要图形库，而放在 `game/` 就进默认构建、能被测。
//      要测的东西是**完备性与互异性**（见下），这正是「加了个枚举值忘了加名字」
//      那类会静默通过的错。
//   2. `all_display_strings()` 是前端推导字体码点集合的输入，而那条推导**不能手抄**
//      ——实测过：字体里有、但没进码点集合的字符会渲成 `?`（集合含 ASCII 时）
//      或图集里的第一个字形（不含时）。后者尤其坏：「镇野箭楼」渲成「平平平平」，
//      是四个看着完全合理的汉字，肉眼都不一定看得出来。详见 render/text.hpp。
//
// **完备性靠编译器，不靠测试。** 下面每个函数都是一个**没有 default 分支**的 switch，
// 于是漏掉一个枚举值会触发 MSVC C4062 / GCC -Wswitch，而两套工具链都开着
// 警告即错误——加枚举值忘了加名字**编不过**。这比「写一条测试提醒自己」强一档，
// 属 CLAUDE.md「结构封死，而非数值劝退」的同类做法。
//
// 但这条有个前提，是写这个文件时才发现的：**C4062 在 MSVC 上是 off-by-default**，
// GCC 的 `-Wswitch` 却含在 `-Wall` 里。所以在加上 `/w14062` 之前（见
// `cmake/CompilerWarnings.cmake`），漏一个枚举值在本地一条警告都没有、到服务器上
// 才编译失败。**改动那条编译选项等于改动本文件的正确性保证**，别顺手删掉。

#ifndef GAME_DISPLAY_NAMES_HPP
#define GAME_DISPLAY_NAMES_HPP

#include <string>
#include <string_view>
#include <vector>

#include "game/map_data.hpp"
#include "rts/roster.hpp"
#include "rts/types.hpp"

namespace game {

std::string_view display_name(Terrain t) noexcept;
std::string_view display_name(ResourceType t) noexcept;
std::string_view display_name(ResourceTier t) noexcept;
std::string_view display_name(CorridorKind k) noexcept;
std::string_view display_name(WallKind k) noexcept;

// 花名册的展示名。**枚举在 `rts_core`，中文在这里**——这正是命名纪律的分工：
// 仿真层只有标识符，中文一律留在展示层。
//
// 这三条与上面五条的一个区别值得留意：`rts::UnitType` 等是**仿真**的枚举，
// 所以这里出现了一个跨层的对应关系（`rts_core` 的枚举 ↔ `game/` 的中文）。
// 没有更好的放法——把中文放进 `rts_core` 会直接违反命名纪律，
// 而在 `render/` 里翻译则让它退出默认构建、失去下面那条完备性保证。
//
// **`display_name(BldType::Wall)` 与 `display_name(WallKind::Wall)` 必须给出同一个词。**
// `game::WallKind` 是 `rts::BldType` 在「地图文件里会出现的那两种」上的一个刻意的
// 限制（于是构造不出 `WallSegment{Tower}`），代价就是同一个东西有两个枚举、
// 各带一份中文。两份不一致的话，同一段墙在信息条与建造菜单里叫两个名字，
// 而两边的代码都没错——所以由 `tests/display_names_test.cpp` 钉住。
std::string_view display_name(rts::UnitType t) noexcept;
std::string_view display_name(rts::BldType t) noexcept;
std::string_view display_name(rts::ObstacleType t) noexcept;

// 「这一格是什么」的一行中文。给光标旁的信息条用。
//
// 在 `game/` 而不是 `render/`，理由与上面第 1 条相同：纯逻辑、无图形依赖，
// 于是进默认构建、能被测。要测的很具体——越界、有墙段、资源点、禁建各自该出什么字。
//
// `p` 在图外时返回一句「光标不在地图内」，**不返回空串**：空串在画面上与
// 「这一格什么都没有」无法区分，而这两种情况的处置完全不同。
std::string describe_cell(const MapData& map, rts::GridPos p);

// 全部展示串。前端用它推导字体要载入哪些码点，而**那条推导不能手抄**——
// 字体里有、却没进码点集合的字符会静默渲成别的字（见 render/text.hpp 的三个坑）。
//
// 两部分：
//
//   * 枚举展示名 —— **机械地**按各自的 count 常量遍历得出，不另抄名单
//   * `describe_cell()` 会用到的字面量 —— 这部分没法从枚举推导，所以是手写的；
//     兜底是 `tests/display_names_test.cpp` 那条测试：它把夹具地图**每一格**
//     （含界外格）的 `describe_cell()` 结果拆成字符，要求每个字符都在本函数
//     的返回值里出现过。于是「加了句文案忘了登记」在默认构建里就会红，
//     不必等到画面上看见一个「?」。
//
// 返回值在首次调用时构建、之后不变。
const std::vector<std::string_view>& all_display_strings();

}  // namespace game

#endif  // GAME_DISPLAY_NAMES_HPP
