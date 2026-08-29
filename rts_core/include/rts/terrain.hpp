// 地形枚举，以及它在仿真里展开成的三张位图。
//
// **规范来源是 `地图与场景设计.md` 4.1（标志位表）与 4.2（展开成三张位图）**，
// 本文件是它的代码化。
//
// ## 为什么它从 `game/` 搬到了这里
//
// 枚举原先定义在 `game/map_data.hpp`，而依赖方向是反的：
//
//   * `地图与场景设计.md` 开头把地图格式列为**第二个跨模块契约**，
//     第一句就是「`rts_core` 要吃它」
//   * 4.2 明写「`rts_core` 载入时展开成 `passable` / `buildable` / `blocks_vision`
//     三张按格的位图」——展开发生在这一侧
//   * 而 `game` 链接 `rts_core`（`game/CMakeLists.txt`），反过来不行。
//     `World` 若要吃地形，枚举就必须在这一层，否则只能在 `rts_core` 里再抄一份，
//     而「同一件事写在两处然后漂移」是本仓库反复吃过的亏
//
// `game::Terrain` 现在是本枚举的**别名**（`game/map_data.hpp`），
// 所以 `game/` 与 `render/` 的调用点一个字都没改。
//
// ## 这个头**只有结构，没有数值**
//
// 移动速度、视野半径、破坏速率一个都不在这里。三条谓词都是 4.1 那张表里的
// **标志位**，即结构约束——`Forest` 不设移动减速、`Road` 不做，都是 4.1 明写的
// 结构性决定，不是待标定的旋钮。

#ifndef RTS_TERRAIN_HPP
#define RTS_TERRAIN_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rts {

// 五种。**顺序即 `layers.terrain.palette` 的下标**，与 `地图与场景设计.md` 4.1
// 及 `tools/map_gen/mapfile.py` 的 `TERRAIN_PALETTE` 一致——**三处必须同序**。
//
// 4.1：「五种恰好对应五个互不相同的标志位组合，没有冗余项。」
// 那句话是可检查的，而下面三条谓词让它真的被检查（见 tests/terrain_test.cpp：
// 5 种地形的 (passable, buildable, blocks_vision) 三元组必须两两不同）。
enum class Terrain : std::uint8_t {
    Plain = 0,    // 默认地形
    Rock = 1,     // 封死周长、夹出隘口
    Forest = 2,   // 侦查博弈的地形载体
    Water = 3,    // **可见但不可达**：唯一「挡路不挡视野」的地形
    Bridge = 4,   // 跨水的唯一通路，最窄的隘口
};

inline constexpr int kTerrainCount = 5;

static_assert(static_cast<int>(Terrain::Bridge) == kTerrainCount - 1);

// 移动层。**只有两层，且第二层只有 `Phoenix`**（`rts/roster.hpp` 的 `is_aerial`）。
//
// 存在的理由是 4.1 末尾那句：「空中单位的位图里水是可通行的，这与『空军无视墙』
// 用的是同一套按兵种类别的地形掩码。」把它做成参数，那套掩码就只有一处定义。
enum class Mobility : std::uint8_t {
    Ground = 0,
    Aerial = 1,
};

inline constexpr int kMobilityCount = 2;

// ——4.1 那张表的三列——
//
// 全部写成**无 `default:` 的 switch**，于是新增地形时这里编不过。
// 依赖 `/w14062`（MSVC 上 C4062 是 off-by-default），见 `cmake/CompilerWarnings.cmake`。

// 地面单位能不能走。**墙不在这里**——墙是「高代价可通行」而不是障碍
// （`地图与场景设计.md` 5.1），它的代价由寻路按血量 ÷ 破坏速率算，与地形无关。
constexpr bool is_passable(Terrain t) noexcept {
    switch (t) {
        case Terrain::Plain:
        case Terrain::Forest:
        case Terrain::Bridge:
            return true;
        case Terrain::Rock:
        case Terrain::Water:
            return false;
    }
    return false;
}

// 按移动层。
//
// **空中层对全部地形都通**，这不是偷懒的恒真分支而是结构性事实：
// `Phoenix` 的唯一克制手段是位置性的防空（CLAUDE.md「空中单位」），
// 地形不参与。写成参数化的一个函数，是为了让「空军的地形掩码是全 1」
// 只有一处定义——寻路那侧（1c）不必自己判，也就不会各判一遍。
constexpr bool is_passable(Terrain t, Mobility m) noexcept {
    switch (m) {
        case Mobility::Ground:
            return is_passable(t);
        case Mobility::Aerial:
            return true;
    }
    return false;
}

// 能不能在这一格建造。
//
// **注意这只是地形那一半。** 4.2 的组合规则是
// 「格子可建造 ⇔ `terrain.buildable` AND NOT `no_build`」，
// 两者的合成在 `TerrainMasks` 里做——那条规则不明写的话，
// `terrain=Plain + no_build=1` 与 `terrain=Bridge` 谁管谁只能靠猜。
constexpr bool is_buildable(Terrain t) noexcept {
    switch (t) {
        case Terrain::Plain:
            return true;
        // `Bridge` 不可建造是结构性的：否则玩家能在桥上砌墙把唯一通路封死，
        // 而想要的形态是「要守只能在桥头设防」（4.1）。
        case Terrain::Bridge:
        case Terrain::Rock:
        case Terrain::Forest:
        case Terrain::Water:
            return false;
    }
    return false;
}

// 挡不挡视野。**`Water` 不挡**——它是四种「通行 × 视野」组合里
// 「挡路但不挡视野」那一格的唯一填充者，也正是它存在的全部理由：
// 「你看得见他在对岸集结，但打不着，他也过不来，直到他上桥。」
constexpr bool blocks_vision(Terrain t) noexcept {
    switch (t) {
        case Terrain::Rock:
        case Terrain::Forest:
            return true;
        case Terrain::Plain:
        case Terrain::Water:
        case Terrain::Bridge:
            return false;
    }
    return false;
}

// ——名字与机械遍历——
//
// 返回**代码标识符**。它同时是精灵元数据（`_sprite_meta.json`）的键前缀，
// 所以与 `rts/roster.hpp` 的 `ident_of` 同族、同用途。
constexpr std::string_view ident_of(Terrain t) noexcept {
    switch (t) {
        case Terrain::Plain:  return "Plain";
        case Terrain::Rock:   return "Rock";
        case Terrain::Forest: return "Forest";
        case Terrain::Water:  return "Water";
        case Terrain::Bridge: return "Bridge";
    }
    return {};
}

constexpr std::string_view ident_of(Mobility m) noexcept {
    switch (m) {
        case Mobility::Ground: return "Ground";
        case Mobility::Aerial: return "Aerial";
    }
    return {};
}

constexpr Terrain terrain_at(int i) noexcept { return static_cast<Terrain>(i); }

// ——4.2：展开成三张按格的位图——
//
// 「仿真热路径查的是位图，不是枚举 + 查表」，且「观测打包退化成 memcpy」。
//
// **每格一字节而不是按位打包**，这是刻意的：观测通道要的是每格一个 0/1 的值，
// 位打包省下的 7/8 内存换来的是打包时逐位解包，而那正是这一步想避免的。
// 128×128 的地图三张表合计 48 KB，量级上无关紧要。
//
// 存 `std::uint8_t` 而不是 `std::vector<bool>`：后者是位打包的特化，
// `data()` 拿不到、也就没有 memcpy 那条路，而它恰好是这个类存在的理由。
class TerrainMasks {
public:
    // `terrain` 与 `no_build` 都按行主序、长度均为 w×h（`no_build` 允许为空 =
    // 全 0）。越界或长度不符会**断言**——调用方是 `World` 的构造函数，
    // 它拿到的是已经过 `MapLoader` 校验的数据，所以这里是编程错误而非输入错误。
    TerrainMasks(int width, int height, const Terrain* terrain,
                 const std::uint8_t* no_build);

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    bool in_bounds(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    Terrain at(int x, int y) const noexcept { return terrain_[idx(x, y)]; }

    // 地面可通行。查表，不走上面的 switch。
    bool passable(int x, int y) const noexcept { return passable_[idx(x, y)] != 0; }

    // 按移动层。空中层恒真（见上面 `is_passable(Terrain, Mobility)`）。
    bool passable(int x, int y, Mobility m) const noexcept {
        return m == Mobility::Aerial || passable(x, y);
    }

    // **已经与 `no_build` 合成过**——这就是 4.2 那条组合规则的唯一实现处。
    bool buildable(int x, int y) const noexcept { return buildable_[idx(x, y)] != 0; }

    bool blocks_vision(int x, int y) const noexcept {
        return blocks_vision_[idx(x, y)] != 0;
    }

    // 供观测打包用的连续视图。**顺序是行主序 y*w+x**，与地图文件的 `rows` 同序。
    const std::uint8_t* passable_bytes() const noexcept { return passable_.data(); }
    const std::uint8_t* buildable_bytes() const noexcept { return buildable_.data(); }
    const std::uint8_t* blocks_vision_bytes() const noexcept {
        return blocks_vision_.data();
    }

    std::size_t cell_count() const noexcept { return passable_.size(); }

    // 地形是**静态**的（`Bridge` 不可被摧毁，见 4.1），所以它不进 `state_hash`
    // 的逐 tick 部分。但回放开局要确认「两边吃的是同一张图」，
    // 而那由地图文件的 `content_hash` 承担（6.3），不是这个类的事。
    //
    // 这个函数存在只为一件事：把「地图确实被展开成了这三张表」变成可比对的一个值，
    // 于是 `World` 的构造被改错（例如 x/y 写反）会在测试里以哈希不符的形式出现。
    std::uint64_t layout_hash() const noexcept;

private:
    std::size_t idx(int x, int y) const noexcept;

    int width_ = 0;
    int height_ = 0;
    std::vector<Terrain> terrain_;
    std::vector<std::uint8_t> passable_;
    std::vector<std::uint8_t> buildable_;
    std::vector<std::uint8_t> blocks_vision_;
};

}  // namespace rts

#endif  // RTS_TERRAIN_HPP
