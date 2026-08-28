// 地图的内存表示。**纯数据 + 只读访问器，不含任何 I/O、不含任何像素。**
//
// 两条边界，都是刻意的：
//
//   * **不含 I/O**：JSON 依赖到 `MapLoader` 为止。`rts_core` 与 `render/` 都只看见
//     这个类型，于是它们都不必引入 JSON 库。对 `rts_core` 尤其重要——它的编译面越小，
//     两套工具链的分歧就越少（CLAUDE.md「关于第三方库」）。
//   * **不含像素**：`地图与场景设计.md` 第 7 节要求像素几何只有一个来源
//     （`_sprite_meta.json`），所以本层只有格坐标。
//
// 字段与 6.2 的结构一一对应。**坐标约定：`pos` 是 `[x, y]`，而 `rows[y][x]`**
// ——这条写反了在方形地图上不报错、只是整张图转置，所以由非方形夹具钉住
// （`tests/map_loader_test.cpp`，同 `tools/map_gen/mapfile.py` 的做法）。

#ifndef GAME_MAP_DATA_HPP
#define GAME_MAP_DATA_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "rts/types.hpp"

namespace game {

// 五种地形。顺序即 `layers.terrain.palette` 的下标，与 `地图与场景设计.md` 4.1 一致，
// 也与 `tools/map_gen/mapfile.py` 的 TERRAIN_PALETTE 一致。**三处必须同序。**
enum class Terrain : std::uint8_t {
    Plain = 0,
    Rock = 1,
    Forest = 2,
    Water = 3,
    Bridge = 4,
};

inline constexpr int kTerrainCount = 5;

// `rows` 是逐**字符**解码的，所以调色板项数超过 10 时下标 ≥10 无法用单字符表达。
// 五种远够，但把它写成编译期断言而不是注释——Python 侧同样的注释曾经声称会报错、
// 实际不会（已在 #33 提出）。
static_assert(kTerrainCount <= 10,
              "rows 逐字符解码，palette 超过 10 项时下标 >=10 无法用单字符表达");

enum class ResourceType : std::uint8_t { Stone, Wood, Gold };

// `inner` 在城内、是保底收入；`outer` 在墙外、要派兵争夺（CLAUDE.md「资源分布形态」）。
enum class ResourceTier : std::uint8_t { Inner, Outer };

// 走廊种类。**这是拼写检查用的枚举，不是「必须四种都有」**——2.2 明写走廊清单是候选
// 而非定数，把它当定数正是 #17 订正过的那类错误。
enum class CorridorKind : std::uint8_t { Open, Defile, Forest, Economy };

enum class WallKind : std::uint8_t { Wall, Gate };

// 各枚举的取值个数。存在的理由是**要能机械地遍历一个枚举**：
// `display_names.cpp` 的 `all_display_strings()` 靠它们把全部展示串枚举出来，
// 而那份清单一旦改成手抄就会漏，漏掉的字符会静默渲成别的字（见 game/display_names.hpp）。
//
// **这里没有编译期保证，也没有测试能抓到它落后。** 加了枚举值忘了 +1 时：
// `display_name()` 那侧编不过（switch 无 default，见 display_names.hpp），
// 所以名字一定会被加上；但 `all_display_strings()` 会漏掉它，字体因此少载几个码点。
// 兜住这一步的是**画字的时候**——`render::FontSet::draw()` 逐码点核对集合成员，
// 未登记的字符直接抛并指名是哪个字。所以这个洞的兜底在渲染侧，不在这里。
inline constexpr int kResourceTypeCount = 3;
inline constexpr int kResourceTierCount = 2;
inline constexpr int kCorridorKindCount = 4;
inline constexpr int kWallKindCount = 2;

struct SpawnPoint {
    int id = 0;
    rts::GridPos pos{};
    CorridorKind corridor = CorridorKind::Open;
};

struct ResourceNode {
    ResourceType type = ResourceType::Stone;
    rts::GridPos pos{};
    ResourceTier tier = ResourceTier::Inner;
};

struct WallSegment {
    WallKind kind = WallKind::Wall;
    rts::GridPos pos{};
    // 残血比例。2.3 要求初始城圈是**残破**的，所以它不恒为 1。
    float hp_frac = 1.0f;
};

// 一张地图。**只能由 `MapLoader` 构造**——它的不变量（terrain 与 no_build 的长度
// 都等于 w×h、全部坐标在界内、走廊种类不重复）在载入时建立，之后不再变。
// 把构造权收在一处，是为了让「一个 MapData 存在」就等价于「它是合法的」。
class MapData {
public:
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    const std::string& map_id() const noexcept { return map_id_; }
    const std::string& name() const noexcept { return name_; }

    bool in_bounds(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    // 越界即 UB 的接口在这里不划算：调用方是渲染循环与校验逻辑，
    // 传进来的坐标常常来自相邻格遍历。断言 + Release 下钳位太隐蔽，
    // 所以直接要求调用方先 in_bounds()，并用断言把违反挡在 Debug 下。
    Terrain terrain_at(int x, int y) const noexcept;
    bool no_build_at(int x, int y) const noexcept;

    rts::GridPos keep() const noexcept { return keep_; }
    const std::vector<SpawnPoint>& spawns() const noexcept { return spawns_; }
    const std::vector<ResourceNode>& resources() const noexcept { return resources_; }
    const std::vector<WallSegment>& walls() const noexcept { return walls_; }

    // 某格上有没有墙段。`SceneModel` 推导墙的**走向**要用它（4.2.1.1）。
    // 线性查找：初始墙段量级在数百，而这条只在装配场景时走一遍，
    // 换成哈希表反而要面对「迭代顺序影响结果」那条确定性禁令。
    const WallSegment* wall_at(int x, int y) const noexcept;

private:
    // 只有 MapLoader 能填。见类注释：构造权收在一处 = 存在即合法。
    friend class MapLoader;
    MapData() = default;

    int width_ = 0;
    int height_ = 0;
    std::string map_id_;
    std::string name_;
    std::vector<Terrain> terrain_;   // 长度 = width_ * height_，索引 y * width_ + x
    std::vector<bool> no_build_;     // 同上
    rts::GridPos keep_{};
    std::vector<SpawnPoint> spawns_;
    std::vector<ResourceNode> resources_;
    std::vector<WallSegment> walls_;
};

}  // namespace game

#endif  // GAME_MAP_DATA_HPP
