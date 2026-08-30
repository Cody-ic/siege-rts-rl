// 场景装配：地图 -> 一份**绘制列表**（格坐标 + 精灵标识符 + 朝向 + 深度键）。
//
// **本文件不含任何像素，也不依赖 raylib。** 这是刻意的，理由有两条：
//
//   1. `地图与场景设计.md` 第 7 节要求像素几何只有一个来源（`_sprite_meta.json`），
//      所以「一格多少像素」只能是渲染层的知识。本层只回答「哪一格画哪张图、
//      按什么顺序画」。
//   2. **`render/` 默认不参与构建**（CMakeLists.txt 的 RTS_BUILD_RENDER=OFF），
//      挂在它下面的测试在默认构建里根本不存在、ctest 照样全绿。而 4.2.1 那四条规则
//      恰恰是「只值一行代码但错了才发现」的那种。把它们放在这个**始终构建**的库里，
//      它们就一定被测到。
//
// 规范来源是 `tools/sprite_gen/preview_map.py` 的「契约」段（4.2.1 明文声明），
// 本文件是它的 C++ 移植。两者不一致时以那份 Python 为准并回来修这里。

#ifndef GAME_SCENE_MODEL_HPP
#define GAME_SCENE_MODEL_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "game/map_data.hpp"
#include "rts/types.hpp"

namespace game {

// 四个朝向，取值与精灵文件名里的后缀一致（`<标识符>_<状态>_<朝向>.png`）。
enum class Facing : std::uint8_t { SE, SW, NE, NW };

std::string_view to_string(Facing f) noexcept;

// 绘制列表里的一项。
//
// `sprite` 是**精灵标识符**（"Plain" / "Forest" / "Wall" …），不是文件名——
// 拼文件名要知道状态与帧号，那是渲染层的事。
struct DrawItem {
    rts::GridPos pos{};
    std::string_view sprite;
    Facing facing = Facing::SE;

    // ——实体扩展（机制第一批之后，`BattleScene` 用；静态场景保持默认值）——
    std::string_view state = "idle";   // 精灵状态。渲染侧对没有该状态的实体回落 idle
    int anim = 0;          // 动画相位。渲染侧对帧数取模——帧数是素材侧知识（§7）
    bool continuous = false;           // true ⇒ 用 world 定位（单位在格间移动）
    rts::Vec2 world{};                 // 连续世界坐标（格单位）
    float hp_frac = -1.0f;             // >= 0 ⇒ 渲染侧画血条
    // 屏幕竖直提升，单位是**格高的倍数**（像素换算归渲染侧，§7 的分界）。
    // 驻守登顶的单位用它画成「站在墙上」；深度键不动——人在墙那一格，
    // 排序上就该与墙同格（插入序让单位画在墙之后 = 之上）。
    float lift = 0.0f;
    // 飞行目的地（世界坐标，格单位）。**只对弹丸精灵有意义**：弹丸是单张
    // FREE 图，方向由渲染侧按「投影后的 aim − world」算屏幕角 2D 旋转
    // （§8.3——等距下世界角 ≠ 屏幕角，所以这里给的是点、不是角）。
    rts::Vec2 aim{};

    // 画家算法的深度键：gi + gj。**叠加物与实体必须在同一个序列里排**，
    // 否则站在岩壁前面的单位不会遮住岩壁。
    int depth() const noexcept { return pos.i + pos.j; }

    // 连续深度键，静态项按**格心**折算（i+j+1 = 格心的 x+y），
    // 于是单位与建筑/景物可以混在同一个序列里排，量纲一致。
    float depth_f() const noexcept {
        return continuous ? world.x + world.y
                          : static_cast<float>(pos.i + pos.j) + 1.0f;
    }
};

// 一格展开成的两张图。**一格要画两张，不是一张**（4.2.1）。
struct TerrainSprites {
    std::string_view tile;      // 底衬地砖，永远非空
    std::string_view overlay;   // 叠加物件，空表示没有
};

// 两遍绘制的两份列表。分开给出，是因为它们的性质不同：
// 地砖是平的、永远不遮挡任何东西，可以先整片铺完；叠加物与实体必须排序。
struct DrawLists {
    std::vector<DrawItem> tiles;    // 第一遍，顺序无所谓
    std::vector<DrawItem> sorted;   // 第二遍，已按深度排好，从前往后画
};

// 把地图装配成绘制列表。无状态，因此做成一组静态函数而不是一个对象——
// 它没有任何需要跨调用保持的东西。
class SceneModel {
public:
    // 地形枚举 -> (底衬地砖, 叠加物)。
    //
    // 只有 Plain* 与 Water 自己就是地砖（元数据里 kind == "tile"：画布正好占满菱形、
    // 锚点在格心、不描边不投影）。Rock / Forest / Bridge 是立体物件——锚点在脚底、
    // 带描边与投影、画布远大于一格，单独贴上去底下是透明的。
    //
    // **桥的底衬是 Water 而不是 Plain**：铺平地会让河在桥这一格视觉上断掉，
    // 读作「水断了一格换成桥」而不是「桥架在水上」。
    static TerrainSprites expand(Terrain t) noexcept;

    // `Plain` 的贴图变体按坐标挑。变体**不占枚举位**（4.1），所以地图文件里没有它，
    // 只能由渲染侧决定；而它必须是**确定**的，否则同一格每帧换一张会闪。
    static std::string_view plain_variant(rts::GridPos p) noexcept;

    // 哪些东西是「线性结构」——它们的朝向由**走向**决定。
    // 岩壁与密林是团块，朝向无所谓，因此不在此列。
    enum class RunKind {
        Wall,    // 墙与门，走向看相邻的墙段
        Bridge,  // 桥，走向看相邻的 Bridge 地形格
    };

    // 线性结构的朝向由它的**走向**决定，不是固定 SE。
    //
    // 这条极易漏：单位的朝向是「它面朝哪」，而墙的朝向是「它沿哪个方向铺」。
    // 同一段墙沿 gi 铺和沿 gj 铺要用不同的精灵，否则相邻墙段接不上，
    // 一条边在画面上读作一排分开的板子。
    //
    // 哪个朝向无缝是**实测出来的**（4.2.1.1）：沿 gi 走用 SW，沿 gj 走用 NW。
    // 换素材后要重测。
    static Facing run_direction(const MapData& map, rts::GridPos p,
                                RunKind kind) noexcept;

    // 装配。地砖一遍、叠加物与墙段混排一遍。
    static DrawLists build(const MapData& map);
};

}  // namespace game

#endif  // GAME_SCENE_MODEL_HPP
