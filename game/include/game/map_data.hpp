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

#include "rts/roster.hpp"
#include "rts/terrain.hpp"
#include "rts/types.hpp"

namespace game {

// ——地形与资源种类现在定义在 `rts_core`，这里只是别名——
//
// 两者都被**仿真**消费，而依赖方向只能是 `game` → `rts_core`
// （`game/CMakeLists.txt`：`target_link_libraries(game PUBLIC rts_core)`）。
// `地图与场景设计.md` 4.2 明写「`rts_core` 载入时展开成三张按格的位图」，
// 所以枚举住在 `game/` 是反的；搬家的完整理由见 `rts/terrain.hpp` 文件头。
//
// **用别名而不是逐处改名**：`game/` 与 `render/` 有 50 余处 `Terrain` 引用，
// 而它们要表达的东西一个字都没变。别名不是「同一件事写在两处」——
// 定义只有一份，这里只是给它一个本模块的名字。
using Terrain = rts::Terrain;
using ResourceType = rts::Resource;

inline constexpr int kTerrainCount = rts::kTerrainCount;

// `rows` 是逐**字符**解码的，所以调色板项数超过 10 时下标 ≥10 无法用单字符表达。
// 五种远够，但把它写成编译期断言而不是注释——Python 侧同样的注释曾经声称会报错、
// 实际不会（已在 #33 提出）。
//
// **这条留在 `game/` 而不是随枚举搬走**：它约束的是**地图文件的行编码**，
// 而那是 `MapLoader` 的事；`rts_core` 不读文件，那边没有这条约束。
static_assert(kTerrainCount <= 10,
              "rows 逐字符解码，palette 超过 10 项时下标 >=10 无法用单字符表达");

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
inline constexpr int kResourceTypeCount = rts::kResourceCount;
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
    // **影响仿真，随 `make_world_init` 传进 `rts::ResourceSite::unlock_wave`**
    // ——与 `tier` 不同，`tier` 到此为止（见 `world_builder.cpp` 的注释）。
    // 地图文件里的字段名同名（`地图与场景设计.md` 6.2），必填、下界 1。
    int unlock_wave = 1;
};

struct WallSegment {
    WallKind kind = WallKind::Wall;
    rts::GridPos pos{};
    // 残血比例。2.3 要求初始城圈是**残破**的，所以它不恒为 1。
    float hp_frac = 1.0f;
};

// 一处中立可破坏障碍（`地图与场景设计.md` 6.2 的 `obstacles`）。
//
// **它不带血量，连残血比例都不带**，与 `WallSegment` 不同。理由是两者的来源不同：
// 城墙的残血是 2.3 的一条设计要求（初始城圈是残破的），而障碍是地图上的原生景物、
// 没有任何设计要求说它开局就该带伤。满血就是它的初始状态，不需要一个字段来说。
//
// 绝对血量同样不在这里（它在数值表里：`game/data/stats_placeholder.json`，
// 经 `game::StatsLoader` 进来），由 `make_world_init` 在装配时查表乘上
// ——同 `world_builder.hpp` 文件头那条。
struct ObstacleNode {
    rts::ObstacleType type = rts::ObstacleType::Stump;
    rts::GridPos pos{};
};

// 玩家开局已拥有的**其余**建筑（`地图与场景设计.md` 6.2 的 `buildings`，
// 2026-08-31 新增，随大地图重设计一起补的空白）。
//
// `Keep` 与 `Wall`/`Gate` 不走这里——`Keep` 恒一座、有专门的 `keep` 字段；
// `Wall`/`Gate` 带残血比例、有专门的 `initial_walls` 字段（2.3 的"城圈必须
// 残破"是只对城圈成立的独立要求）。这份列表管的是箭塔、兵营、伐木场、
// 采石场一类——此前地图 schema 完全没有承载它们的地方，`demo_driver.cpp`
// 里"玩家开局有什么"是纯代码写死的占位编成，没有一张地图能表达"这里预先
// 摆了一座兵营"。
//
// **不带残血比例**，同 `ObstacleNode`：满血进场，没有设计要求说玩家的
// 初始建筑开局就该带伤。
struct BuildingNode {
    rts::BldType type = rts::BldType::Tower;
    rts::GridPos pos{};
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
    const std::vector<ObstacleNode>& obstacles() const noexcept { return obstacles_; }
    const std::vector<BuildingNode>& buildings() const noexcept { return buildings_; }

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
    std::vector<ObstacleNode> obstacles_;
    std::vector<BuildingNode> buildings_;
};

}  // namespace game

#endif  // GAME_MAP_DATA_HPP
