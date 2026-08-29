// 动态场景装配：`rts::WorldView` → 深度排好的绘制列表（每帧一次）。
//
// 与 `SceneModel` 的分工：那边装配**静态**地图（地砖 + 地形叠加物 + 地图里的墙），
// 用于地图查看器；本类装配**活的**世界——建筑与障碍从仿真读（会被拆掉），
// 单位在格间连续移动。两者都不含任何像素（§7），都在默认构建里被测。
//
// 输入取 `WorldView` 而不是 `World&`，是刻意的：本类的产物只喂渲染，
// 让它拿不到可写入口，不变量 2 就多一层编译期保障（也顺带证明只读视图装得下
// 「画一帧」需要的全部信息——装不下的话该改视图，不该开后门）。

#ifndef GAME_BATTLE_SCENE_HPP
#define GAME_BATTLE_SCENE_HPP

#include <vector>

#include "game/map_data.hpp"
#include "game/scene_model.hpp"
#include "rts/world_view.hpp"

namespace game {

class BattleScene {
public:
    // 静态地砖（地形不会变，建局后算一次即可）。
    static std::vector<DrawItem> tiles(const MapData& map);

    // 动态深度序列：地形叠加物（岩/林/桥）+ 活着的建筑与障碍 + 活着的单位，
    // **混在同一个序列里按连续深度排**。每帧重建——项数是数百的量级，
    // 排序便宜；缓存加失效逻辑才是贵的那条路。
    static std::vector<DrawItem> sorted(const MapData& map, const rts::WorldView& view,
                                        rts::Tick now);

    // 单位朝向：从当前动作推。移动看 `move_delta`，出手中看锁定落点的方向，
    // 其余保持 SE。**这是纯展示推导**，仿真里没有朝向这个状态。
    static Facing facing_of(rts::UnitAction a, rts::Vec2 pos, rts::Vec2 aim,
                            bool winding) noexcept;
};

}  // namespace game

#endif  // GAME_BATTLE_SCENE_HPP
