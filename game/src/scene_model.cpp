#include "game/scene_model.hpp"

#include <algorithm>
#include <cstdint>

namespace game {
namespace {

// 精灵标识符。写成常量而不是散在代码里的字面量，因为它们同时出现在
// `assets.json`、`_sprite_meta.json` 与这里，改名时要能一处 grep 到。
constexpr std::string_view kPlain = "Plain";
constexpr std::string_view kPlainB = "PlainB";
constexpr std::string_view kWater = "Water";
constexpr std::string_view kRock = "Rock";
constexpr std::string_view kForest = "Forest";
constexpr std::string_view kBridge = "Bridge";
constexpr std::string_view kWall = "Wall";
constexpr std::string_view kGate = "Gate";
constexpr std::string_view kNone = "";

// 排序时的次级键。preview_map.py 用 `sorted(overlays + objs)` + Python 排序的稳定性
// 让**同深度下叠加物排在实体前面**；这里把那条隐含规则写成显式的键，
// 于是排序结果只由内容决定，不再依赖插入顺序。
enum class Layer : std::uint8_t { Overlay = 0, Entity = 1 };

struct Keyed {
    DrawItem item;
    Layer layer;
};

bool is_bridge_cell(const MapData& map, int x, int y) noexcept {
    return map.in_bounds(x, y) && map.terrain_at(x, y) == Terrain::Bridge;
}

bool is_water_cell(const MapData& map, int x, int y) noexcept {
    return map.in_bounds(x, y) && map.terrain_at(x, y) == Terrain::Water;
}

bool is_wall_cell(const MapData& map, int x, int y) noexcept {
    return map.in_bounds(x, y) && map.wall_at(x, y) != nullptr;
}

}  // namespace

std::string_view to_string(Facing f) noexcept {
    switch (f) {
        case Facing::SE: return "SE";
        case Facing::SW: return "SW";
        case Facing::NE: return "NE";
        case Facing::NW: return "NW";
    }
    return "SE";
}

std::string_view SceneModel::resource_marker(rts::Resource kind) noexcept {
    // 与素材键同一条纪律：标识符与所属枚举成员**同前缀**（`Stone*` 属
    // `Resource::Stone`），渲染侧按前缀归类时才有据可依（CLAUDE.md「命名纪律」）。
    // 穷举 switch 无 default，完备性由 /w14062 与 -Wswitch 保证——同本文件
    // `to_string` 的先例。
    switch (kind) {
        case rts::Resource::Stone: return "StonePt";
        case rts::Resource::Wood:  return "WoodPt";
        case rts::Resource::Gold:  return "GoldPt";
    }
    return "";
}

TerrainSprites SceneModel::expand(Terrain t) noexcept {    switch (t) {
        case Terrain::Plain:  return {kPlain, kNone};
        case Terrain::Water:  return {kWater, kNone};
        case Terrain::Rock:   return {kPlain, kRock};
        case Terrain::Forest: return {kPlain, kForest};
        // 桥的底衬是 Water，不是 Plain。铺平地会让河在这一格断掉。
        case Terrain::Bridge: return {kWater, kBridge};
    }
    return {kPlain, kNone};
}

std::string_view SceneModel::plain_variant(rts::GridPos p) noexcept {
    // **只在两个中性草地变体之间挑。** `PlainDirt` / `PlainRoad` / `PlainAsh` 暂不使用：
    // 地图格式里没有任何字段能表达「这一格是路 / 是焦土」（4.1 说变体不占枚举位，
    // 但没说由什么承载），按坐标哈希随机撒路面只会得到一张到处是断头路的图。
    // 这个缺口已报给小组，见本 PR 的描述。
    //
    // 必须**确定**：同一格每次都得到同一个变体，否则画面每帧闪烁。
    const auto ui = static_cast<std::uint32_t>(static_cast<std::uint16_t>(p.i));
    const auto uj = static_cast<std::uint32_t>(static_cast<std::uint16_t>(p.j));
    std::uint32_t h = ui * 73856093u ^ uj * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    // 不用 (i+j) % k 这类线性式：它在等距视角下会排成一条条斜带，一眼就看得出规律。
    return (h % 4u == 0u) ? kPlainB : kPlain;
}

Facing SceneModel::run_direction(const MapData& map, rts::GridPos p,
                                 RunKind kind) noexcept {
    const int x = p.i;
    const int y = p.j;
    bool along_i = false;
    if (kind == RunKind::Wall) {
        along_i = is_wall_cell(map, x - 1, y) || is_wall_cell(map, x + 1, y);
        // 墙板：沿 gi 用 SW、沿 gj 用 SE（实测，见头文件那张表）。
        return along_i ? Facing::SW : Facing::SE;
    }
    {
        // **桥的走向是「过河的方向」，不是「相邻桥格的方向」。**
        //
        // 一条一格宽的河上并排两格桥（demo 就是这样：河沿 gj，桥在 (13,5) 与
        // (13,6)），按相邻同类推出来的走向是**沿着河**的，于是桥板顺着水流铺，
        // 读作「河里漂着两块板」而不是「一座桥」。
        //
        // 所以先问水：河沿哪个轴，桥就沿另一个轴。相邻的**水**格给出河的走向
        // （相邻的桥格不算——它们是并排的另一条车道，正是上面那个陷阱的来源）。
        const bool river_along_i = is_water_cell(map, x - 1, y) || is_water_cell(map, x + 1, y);
        const bool river_along_j = is_water_cell(map, x, y - 1) || is_water_cell(map, x, y + 1);
        if (river_along_i != river_along_j) {
            along_i = river_along_j;   // 河沿 gj ⇒ 桥沿 gi
        } else {
            // 问不出河的走向（桥两头都不挨水，或四面都是水）：回落到相邻桥格。
            along_i = is_bridge_cell(map, x - 1, y) || is_bridge_cell(map, x + 1, y);
        }
    }
    // **桥板与墙板的表是反的**，因为两个模型的长轴本来就不是同一个轴
    // （`wall-narrow.glb` vs `bridge-draw.glb`）。这不是笔误——
    // `tools/sprite_gen/README.md` 那节自己写着「这取决于模型自身的长轴朝向」，
    // 所以这张表只能逐模型实测，不能全局共用一份。
    return along_i ? Facing::SE : Facing::SW;
}

DrawLists SceneModel::build(const MapData& map) {
    DrawLists out;
    const auto cells = static_cast<std::size_t>(map.width()) *
                       static_cast<std::size_t>(map.height());
    out.tiles.reserve(cells);

    std::vector<Keyed> keyed;

    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const rts::GridPos p{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
            const TerrainSprites ts = expand(map.terrain_at(x, y));

            // 第一遍：地砖。地砖是平的、永远不遮挡任何东西，可以先整片铺完。
            const std::string_view tile =
                (ts.tile == kPlain) ? plain_variant(p) : ts.tile;
            out.tiles.push_back(DrawItem{p, tile, Facing::SE});

            if (ts.overlay.empty()) continue;

            // 桥是线性结构，朝向按走向取；岩壁与密林是团块，朝向无所谓。
            const Facing f = (ts.overlay == kBridge)
                                 ? run_direction(map, p, RunKind::Bridge)
                                 : Facing::SE;
            keyed.push_back(Keyed{DrawItem{p, ts.overlay, f}, Layer::Overlay});
        }
    }

    // 墙段是实体，不是地形层的东西，所以单独走一遍——但它们**和叠加物排进同一个
    // 序列**，这正是 4.2.1 那条「不能先画完全部地形再画单位」的要求。
    for (const WallSegment& w : map.walls()) {
        const std::string_view sprite = (w.kind == WallKind::Gate) ? kGate : kWall;
        keyed.push_back(Keyed{
            DrawItem{w.pos, sprite, run_direction(map, w.pos, RunKind::Wall)},
            Layer::Entity});
    }

    // 资源点的地表标记。**与叠加物同层**：同深度下叠加物先画（排序次级键的
    // layer 档），于是采集建筑落成后正好盖住自己脚下那个标记，而不是反过来。
    for (const ResourceNode& r : map.resources()) {
        keyed.push_back(Keyed{DrawItem{r.pos, resource_marker(r.type), Facing::SE},
                              Layer::Overlay});
    }

    // 画家算法：按 gi+gj 从小到大画，后画的自然遮住先画的。
    //
    // 次级键 (layer, i, j) 使结果**只由内容决定、不依赖插入顺序**。
    // preview_map.py 靠 `sorted(overlays + objs)` 加 Python 排序的稳定性得到
    // 「同深度时叠加物先于实体」，那是隐含的；这里把它写成显式的 layer 键，
    // 顺序因此可被测试钉住，而不是「碰巧对」。
    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        const int da = a.item.depth();
        const int db = b.item.depth();
        if (da != db) return da < db;
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.item.pos.i != b.item.pos.i) return a.item.pos.i < b.item.pos.i;
        return a.item.pos.j < b.item.pos.j;
    });

    out.sorted.reserve(keyed.size());
    for (const Keyed& k : keyed) out.sorted.push_back(k.item);
    return out;
}

}  // namespace game
