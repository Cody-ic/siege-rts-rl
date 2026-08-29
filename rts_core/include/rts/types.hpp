// 仿真的基元类型。
//
// 这一层只放"不会因设计讨论而变"的东西：tick、阵营、格坐标、实体句柄。
// 花名册枚举在 `rts/roster.hpp`、动作与命令在 `rts/action.hpp` / `rts/command.hpp`、
// 观测通道在 `rts/obs.hpp`——**刻意分头放，因为它们的变动理由各不相同**：
// 花名册随设计讨论变、观测通道随网络结构变，而本文件这一层不该被它们牵动。
//
// 依据：CLAUDE.md「rts_core 实现约定」与「确定性要求」。

#ifndef RTS_TYPES_HPP
#define RTS_TYPES_HPP

#include <cassert>
// <compare> 是 operator<=> = default 的硬性要求（见下面的 Handle）。
// 这条曾经漏掉，而它一直编得过——三个测试文件都先 include 了 <catch2/...>，
// 把 <compare> 顺带拉进来了。这类漏 include 只在**该头被单独包含**时才暴露，
// 而 libstdc++ 与 MSVC STL 的内部包含图不同，它还可能只在一个平台上炸。
// rts_core/CMakeLists.txt 里的「头文件自足性守卫」现在把这条钉住了。
#include <cmath>
#include <compare>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace rts {

// 固定 20 Hz，不使用可变时间步（CLAUDE.md 硬性规定）。
// 一波 = 一 episode ≈ 1200–2400 tick，int32 足够，且比 uint 更容易在差值运算里
// 不出意外（tick 相减为负是合法的中间结果）。
using Tick = std::int32_t;

inline constexpr int kTicksPerSecond = 20;

// 阵营。**接口对"哪一侧由谁控制"保持中立**：守方在正式游玩时是人类、在训练时是策略
// （见提案 #15），攻方一直是策略。把 Side 做成参数而不是把攻守写死在函数名里，
// 是为了让「谁控制这一侧」变成训练回路的事，而不是接口重写。
enum class Side : std::uint8_t {
    Defender = 0,  // 人类王国
    Attacker = 1,  // 亡灵 / 魔物大军
};

inline constexpr int kSideCount = 2;

inline constexpr Side other(Side s) noexcept {
    return s == Side::Defender ? Side::Attacker : Side::Defender;
}

inline constexpr int index_of(Side s) noexcept {
    return static_cast<int>(s);
}

// 代码标识符，给报错与日志用（同 `rts/roster.hpp` 的 `ident_of` 一族）。
// 打整数值等于让读者自己去数哪一侧是 0。
inline constexpr std::string_view ident_of(Side s) noexcept {
    switch (s) {
        case Side::Defender: return "Defender";
        case Side::Attacker: return "Attacker";
    }
    return {};
}

// 连续世界坐标。用 float 而非定点数：CLAUDE.md 只要求**同平台同编译器**可复现，
// 不要求跨平台位级一致（回放文件在 Windows 与 Linux 之间本来就不可复现）。
// 定点数的实现与调试成本换不来任何本项目需要的性质。
//
// **注意 operator== 与状态哈希对 ±0.0 的判断相反，这是有意的分工，不是疏漏。**
// 下面的 == 是默认逐成员浮点比较，`+0.0 == -0.0` 为真；hash.hpp 的 feed_f32 是按位
// 喂入，两者不同。因此 `a == b` 并不蕴含 `hash(a) == hash(b)`。
// 两个方向的统一都更坏：统一到位语义会让 Vec2{0.0f, y} == Vec2{-0.0f, y} 为假，
// 违反直觉；统一到浮点语义会让回放哈希查不出位级差异，而那正是它唯一要查的东西。
// 所以缺的不是设计而是这段说明——它们迟早会在同一处碰头（回放比对用哈希、
// 单元测试里比较位置用 ==），到时候看到的现象是"状态相等但哈希不等"。
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    friend constexpr bool operator==(Vec2, Vec2) noexcept = default;
};

// 格坐标。地图边长待标定，量级在 64–128 之间（地图与场景设计.md 第 3 节），
// int16 有充足余量，同时让整张地图的坐标数组保持紧凑。
struct GridPos {
    std::int16_t i = 0;
    std::int16_t j = 0;

    friend constexpr bool operator==(GridPos, GridPos) noexcept = default;
};

// ——连续坐标 ↔ 格坐标：约定只有这一份——
//
// 一格 `(i, j)` 覆盖 `[i, i+1) × [j, j+1)`，格心在 `(i + 0.5, j + 0.5)`。
// 于是 `i = floor(x)`、`j = floor(y)`，而**格心不是格角**这一点必须写下来：
// 取 `(i, j)` 当格心会让每个单位稳定偏半格，而半格偏移在等距投影下看起来
// 像是「精灵锚点没对准」，第一反应会去查渲染而不是查这里。
//
// `i = x`、`j = y`：与地图文件的 `pos: [x, y]` 及 `rows[y][x]` 一致
// （`game/map_data.hpp`、`tools/map_gen/mapfile.py`）。
//
// 用 `std::floor` 而不是强制转换：后者向零取整，`x = -0.5` 会得到格 0
// 而正确答案是格 −1。地图内坐标非负，所以这条平时不生效——正因为不生效，
// 它会一直是对的直到某次有人算出一个负的中间坐标。
inline GridPos grid_of(Vec2 p) noexcept {
    return GridPos{static_cast<std::int16_t>(std::floor(p.x)),
                   static_cast<std::int16_t>(std::floor(p.y))};
}

inline Vec2 center_of(GridPos g) noexcept {
    return Vec2{static_cast<float>(g.i) + 0.5f, static_cast<float>(g.j) + 0.5f};
}

// 实体句柄：索引 + 代数（generation），打包进一个 uint32。
//
// 为什么不用裸指针或裸索引：
//   * 裸指针被 CLAUDE.md 明确禁止——"拿地址当 key 或排序依据"是破坏确定性回放最常见的
//     来源，扁平数组 + 整型 ID 从结构上让这类 bug 写不出来。
//   * 纯裸索引则无法区分"这个槽位已经被回收、又装了别的实体"。代数解决这一点：
//     实体死亡时槽位代数 +1，旧句柄随即失配，于是悬空引用变成一次可检出的失败，
//     而不是静默读到另一个实体。
//
// 索引放**高**16 位是刻意的：raw() 的大小顺序因此等于索引顺序，
// 按句柄排序 == 按数组下标排序，天然稳定，不会因为代数变化而重排。
//
// Tag 使 UnitId 与 BldId 成为**不可互换**的两个类型。单位与建筑是两组独立的扁平数组，
// 传错一个会静默索引到错误的数组；编译期挡掉的成本是零。
template <class Tag>
class Handle {
public:
    using Raw = std::uint32_t;

    static constexpr Raw kInvalidRaw = 0xFFFFFFFFu;
    // 索引 0xFFFF 被保留给"无效"，因此有效索引上限是 0xFFFE。
    // 单波单位数硬上限约 40、建筑数量级在数百，余量极大。
    static constexpr std::uint16_t kMaxIndex = 0xFFFEu;

    constexpr Handle() noexcept = default;

    static constexpr Handle make(std::uint16_t index, std::uint16_t generation) noexcept {
        // 上面 kMaxIndex 那条不变量此前只活在注释里。make(0xFFFF, 0xFFFF) 逐位恰好
        // 等于 kInvalidRaw，于是会造出一个 valid() 为假的"有效"句柄——40 单位的规模
        // 够不着，但这是"该红却绿"的形态。Release 下 assert 是空操作，成本为零。
        assert(index <= kMaxIndex);
        // 必须先转成 Raw 再移位：uint16 会被整型提升为 int，index >= 0x8000 时
        // 左移 16 位就是有符号溢出（未定义行为）。
        return Handle{(static_cast<Raw>(index) << 16) | static_cast<Raw>(generation)};
    }

    constexpr std::uint16_t index() const noexcept {
        return static_cast<std::uint16_t>(raw_ >> 16);
    }

    constexpr std::uint16_t generation() const noexcept {
        return static_cast<std::uint16_t>(raw_ & 0xFFFFu);
    }

    constexpr bool valid() const noexcept { return raw_ != kInvalidRaw; }

    constexpr Raw raw() const noexcept { return raw_; }

    // 相等与全序都由 raw 给出，因而完全确定，可安全用作有序容器的 key。
    friend constexpr bool operator==(Handle, Handle) noexcept = default;
    friend constexpr auto operator<=>(Handle, Handle) noexcept = default;

private:
    explicit constexpr Handle(Raw r) noexcept : raw_(r) {}

    Raw raw_ = kInvalidRaw;
};

struct UnitTag {};
struct BldTag {};
struct ObstacleTag {};

using UnitId = Handle<UnitTag>;
using BldId = Handle<BldTag>;

// 中立可破坏障碍（`Stump` / `Sapling` / `Rubble`，见 rts/roster.hpp）的句柄。
//
// **它现在几乎肯定还没有实体，位子仍然必须留。** 两个理由，第二个才是硬的：
//
//   * 不加 `Side::Neutral`。障碍不属于任何一侧，而观测通道是**按侧成对**定义的
//     （rts/obs.hpp），塞一个第三侧进去会让成对通道变成奇数个，
//     Python 侧按 (ally, enemy) 解包时**静默错位**——不报错，只是学不动
//   * **回放的字节布局与 `state_hash` 的喂入顺序都要固定。** 事后新增一组实体
//     会让所有已录回放失效，而回放是本项目主要的防 bug 手段
//
// 代价是一个空 tag 类型。这三种障碍本身随提案「无尽模式与地形分层」§6 待议
// （CLAUDE.md 那张对照表下有同样的注），**即使被否，留着这一组的代价也不变**。
using ObstacleId = Handle<ObstacleTag>;

// 这些断言保护的是回放文件的字节布局与哈希的可比性：一旦某个基元类型的大小变了，
// 旧回放会静默偏离，而"静默"是最坏的失败形态。
static_assert(sizeof(Tick) == 4);
static_assert(sizeof(Vec2) == 8);
static_assert(sizeof(GridPos) == 4);
static_assert(sizeof(UnitId) == 4);
static_assert(sizeof(BldId) == 4);
static_assert(sizeof(ObstacleId) == 4);
static_assert(std::is_trivially_copyable_v<Vec2>);
static_assert(std::is_trivially_copyable_v<GridPos>);
static_assert(std::is_trivially_copyable_v<UnitId>);
// 三种句柄两两不可互相转换——这正是 Tag 的目的。三组实体是三个独立的扁平数组，
// 传错一个会静默索引到另一个数组，而下标恰好合法的概率很高。
static_assert(!std::is_convertible_v<UnitId, BldId>);
static_assert(!std::is_convertible_v<BldId, UnitId>);
static_assert(!std::is_convertible_v<UnitId, ObstacleId>);
static_assert(!std::is_convertible_v<ObstacleId, BldId>);

}  // namespace rts

#endif  // RTS_TYPES_HPP
