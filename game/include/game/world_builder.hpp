// `MapData` → `rts::WorldInit`。**关卡加载与仿真之间的唯一接缝。**
//
// 为什么它在 `game/` 而不是 `rts_core/`：`rts_core` 看不见 `MapData`
// （依赖方向是 `game` → `rts_core`），而这也正是想要的——仿真不该知道
// 地图文件长什么样，它只吃 `WorldInit` 里那几组数组。
//
// ## 它存在的第二个理由：证明契约装得下它真正的消费者
//
// `rts_core` 的接口目前**全部由同一个人写、也全部由同一个人消费**，
// 独立校验缺失（这一点已在 PR #48 里写明并请人复核）。一个能编译、能跑的适配层
// 是这个问题唯一能自己动手补的那部分：若 `WorldInit` 少了一项、
// 或者某个字段的形状不对，这里会先编不过。
//
// ## 数值一个都不在这里
//
// 血量来自数值表（`rts/stats.hpp`，经 `game::StatsLoader` 从 JSON 进来）。
// 地图文件给的是**残血比例**（`地图与场景设计.md` 2.3：初始城圈是残破的），
// 本层只做「比例 × 表里的 max_hp」这一步乘法，自己不知道任何数字。
//
// > 历史：这里曾有一个 `InitialHp`（keep / wall / gate / obstacle 四个裸数），
// > 那是「数值表还不存在」时期的桥。表落地后它就是**第二个真相来源**
// > （同一个 `max_hp` 有两条来路，迟早只更新一条），所以随数值表 PR 一并删掉。

#ifndef GAME_WORLD_BUILDER_HPP
#define GAME_WORLD_BUILDER_HPP

#include <array>
#include <cstdint>

#include "game/map_data.hpp"
#include "rts/roster.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace game {

// 从一张地图 + 一份数值表装配建局参数。`stats` 同时被塞进 `WorldInit::stats`
// ——它是外生输入，`World` 要从它算指纹（`rts/stats.hpp` 文件头）。
//
// `content_hash` 由调用方给：`MapLoader` **刻意不算**它（要先证明 C++ 的规范
// 序列化与 `mapfile.py` 逐字节一致，见 `game/map_loader.hpp` 那段），
// 而回放头必须记它（6.3）。全零表示「还没算」，这在本阶段是诚实的；
// 等那条跨语言比对测试写好，这个参数就有真值可填，**接口不必改**。
rts::WorldInit make_world_init(const MapData& map, const rts::StatsTable& stats,
                              std::uint64_t seed, std::int32_t nominal_level,
                              const std::array<unsigned char, 32>& content_hash = {});

}  // namespace game

#endif  // GAME_WORLD_BUILDER_HPP
