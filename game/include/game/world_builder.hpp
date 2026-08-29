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
// 血量由调用方经 `InitialHp` 给。地图文件给的是**残血比例**
// （`地图与场景设计.md` 2.3：初始城圈是残破的），把它乘成绝对值需要 `max_hp`,
// 而 `max_hp` 在那份**还不存在的** JSON 数值表里。
// 于是这一层只做乘法，不知道任何数字——契约规则「接口与回放格式的承诺
// 不得依赖任何待定数值」在这里的形式。

#ifndef GAME_WORLD_BUILDER_HPP
#define GAME_WORLD_BUILDER_HPP

#include <array>
#include <cstdint>

#include "game/map_data.hpp"
#include "rts/roster.hpp"
#include "rts/world.hpp"

namespace game {

// 建局要用到的那几个 `max_hp`，**全部由调用方给**（见文件头）。
//
// 默认值刻意是 1 而不是某个"看起来合理"的数：1 是能让 `0 < hp <= max_hp`
// 成立的最小值，**一眼就能看出它不是标定过的**。给个 1000 之类的默认值，
// 半年后一定有人把它当成已定数值引用——那正是 `CLAUDE.md`「关于数值」
// 末尾那两条纪律要防的事。
struct InitialHp {
    std::int64_t keep = 1;
    std::int64_t wall = 1;
    std::int64_t gate = 1;
    // 可破坏障碍。**三种共用一个**，这是刻意的：让它们逐种取值等于在这个结构里
    // 断言「树桩比碎石好打」，而那个比较关系本身还没有人定过。
    // 三种各自的血量是三个待标定数值，而这里只需要一个「不是标定值」的占位。
    std::int64_t obstacle = 1;
};

// 从一张地图装配建局参数。
//
// `content_hash` 由调用方给：`MapLoader` **刻意不算**它（要先证明 C++ 的规范
// 序列化与 `mapfile.py` 逐字节一致，见 `game/map_loader.hpp` 那段），
// 而回放头必须记它（6.3）。全零表示「还没算」，这在本阶段是诚实的；
// 等那条跨语言比对测试写好，这个参数就有真值可填，**接口不必改**。
rts::WorldInit make_world_init(const MapData& map, const InitialHp& hp,
                              std::uint64_t seed, std::int32_t nominal_level,
                              const std::array<unsigned char, 32>& content_hash = {});

}  // namespace game

#endif  // GAME_WORLD_BUILDER_HPP
