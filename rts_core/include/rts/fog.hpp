// 每侧一份的迷雾 / 侦查记忆图。
//
// 契约里的决定 ⑥（`rts_core 接口契约.md` 3.2）：**它是一等公民，不是渲染的滤镜。**
// 三处硬要求同时落在它身上：
//
//   * CLAUDE.md「RL 侧两条硬要求」第 1 条：**AI 的观测必须是它自己的迷雾状态，
//     不得是 ground truth**。不做这条则「玩家藏防空塔」毫无意义
//   * `演示与可视化清单.md` 的头号演示点「双视图（真实战场 vs AI 的侦查记忆图）」,
//     README 明写**不可砍**
//   * 「AI 是否发现并利用已有缺口」这条核心评估指标要的**现场**证据，
//     就是「AI 的记忆图里那段墙是破的 → 它调头去打那里」
//
// ## 三态，不是两态。这一条是这个文件里最要紧的东西
//
// `Unseen` / `Remembered` / `Visible`。少了中间那一态，「从未侦查过」与
// 「侦查过、而那里是个缺口」在张量里**是同一串字节**：
//
// | | `visible` | `wall_hp` |
// |---|:-:|:-:|
// | 从未见过 | 0 | 0 |
// | 见过，记忆里那段墙已破 | 0 | 0 |
//
// 而上面第三条指标要的恰好是这两者的**区别**。所以 `visible` 通道取三档
// （见下面 `vis_value`），不是布尔——`rts/obs.hpp` 的 `kObsVersion` 因此从 1 升到 2。
//
// 这个缺陷是写本文件时才发现的，此前那一版（PR #48）的 `visible` 是布尔。
// 记在 `rts_core 接口契约.md` §4.3，连同「为什么指纹抓不到它、必须靠版本号」。
//
// ## 记的是建筑，不记单位。这是有意的不对称
//
// 建筑不动，所以「上次看到那里有一段满血的墙」在下一波仍是有用的情报——
// 而这正是 CLAUDE.md 说「波次结构使 AI 的记忆天然过时」时那个「记忆」的所指。
// 单位会走，记住的位置**主动误导**：AI 会朝一个空地包夹。
// 真实 RTS 也是这么做的，理由相同。
//
// ## 本版只给形状与更新原语，视野解算不在这里
//
// 「谁看得见哪一格」要读单位视野半径（待标定的数值）与 `blocks_vision`
// 位图，那是 1c（@ArLiangz 的分片）。本文件给的是**存储 + 三个更新原语 +
// 进哈希的顺序**，因为这三样都必须在回放格式定稿之前定死。

#ifndef RTS_FOG_HPP
#define RTS_FOG_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "rts/hash.hpp"
#include "rts/roster.hpp"
#include "rts/types.hpp"

namespace rts {

// **`Unseen` 必须是 0**：零初始化的迷雾 = 全图未探索，那是唯一正确的开局状态。
enum class Vis : std::uint8_t {
    Unseen = 0,       // 从未进入过视野
    Remembered = 1,   // 曾经可见，现在不可见——记忆图画的就是这一档
    Visible = 2,      // 当前可见
};

inline constexpr int kVisCount = 3;

static_assert(static_cast<int>(Vis::Visible) == kVisCount - 1);

// 「从未见过」的 `last_seen`。
//
// 取 `Tick` 的最小值而不是 0 或 -1：0 是一个合法的 tick（开局那一刻），
// 而 `now - last_seen` 这种算式在别处一定会被写出来，用 0 当哨兵会得到
// 一个看起来合理的「刚刚见过」。
inline constexpr Tick kNeverSeen = -2147483647 - 1;

static_assert(kNeverSeen < 0);

// 观测里 `visible` 通道的取值。**三档，等距。**
//
// 等距而不是「记忆按时间衰减」：衰减需要一个时间尺度做分母，而预期波长是
// **待定数值**——契约规则「接口与回放格式的承诺不得依赖任何待定数值」
// （`rts_core 接口契约.md` §4.1）在这里第三次生效。
// 波长标定之后可以再加一条独立的「记忆新鲜度」通道，那时改指纹与版本号。
constexpr float vis_value(Vis v) noexcept {
    switch (v) {
        case Vis::Unseen:     return 0.0f;
        case Vis::Remembered: return 0.5f;
        case Vis::Visible:    return 1.0f;
    }
    return 0.0f;
}

constexpr std::string_view ident_of(Vis v) noexcept {
    switch (v) {
        case Vis::Unseen:     return "Unseen";
        case Vis::Remembered: return "Remembered";
        case Vis::Visible:    return "Visible";
    }
    return {};
}

// 记忆中某一格上的建筑。
//
// **血量存的是千分比而不是绝对值**，两个理由都是硬的：
//
//   * 观测里血量存**比例**（决定 ⑩），所以绝对值到不了 Python 侧，存它是死重
//   * 存绝对值就要连 `max_hp` 一起存，而 `max_hp` 来自那份**还不存在的数值表**。
//     存比例则整个候选区间都影响不到这个结构的字节布局——同上那条契约规则
//
// 千分比而不是浮点：迷雾要进 `state_hash`，而浮点按位喂入时 `+0.0` 与 `-0.0`
// 会给出两个哈希（`rts/hash.hpp`）。定点整数没有这个问题。
// 1/1000 的分辨率对「那段墙还剩多少」远远够用。
struct RememberedBld {
    BldType type = BldType::Wall;
    std::uint16_t hp_permille = 0;   // 0 = 记忆里这段已经是缺口
};

inline constexpr std::uint16_t kFullPermille = 1000;

// 一侧的迷雾。
//
// 布局是 SoA：`vis_` 单独一张连续表，于是 `visible` 通道的打包是
// 「逐字节查一张 3 项的表」而不是逐格解结构体——`地图与场景设计.md` 4.2
// 说「观测打包退化成 memcpy」时指的就是这种形状。
class FogLayer {
public:
    FogLayer(int width, int height);

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    bool in_bounds(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    Vis at(int x, int y) const noexcept { return vis_[idx(x, y)]; }
    Tick last_seen(int x, int y) const noexcept { return last_seen_[idx(x, y)]; }

    // 记忆里这一格有建筑吗；有的话是什么、还剩多少。
    //
    // 返回 false 有**两种**完全不同的含义，而调用方必须能分开：
    // 「从未见过」（`at() == Unseen`）与「见过，那里没有建筑」。
    // 后者才是「缺口」，也正是三态存在的理由。所以要判缺口请写：
    //
    //     fog.at(x, y) != Vis::Unseen && !fog.remembered_bld(x, y, &b)
    bool remembered_bld(int x, int y, RememberedBld* out) const noexcept;

    // ——三个更新原语。由视野解算（1c）调用，本版没有调用方——
    //
    // 之所以是「原语」而不是一个 `recompute(vision)`：视野解算的形状还没定
    // （逐单位圆形 + `blocks_vision` 遮挡？分兵种半径？），
    // 而这三个动作无论用哪种解算法都是它需要的最小集合。

    // 每 tick 视野重算**之前**调一次：把上一 tick 的 `Visible` 降为 `Remembered`。
    //
    // 单独一步而不是在 `mark_visible` 里做增量：增量做法要区分「这一 tick 已经标过」
    // 与「上一 tick 标过」，于是要么多一张脏表、要么按 tick 号比较——
    // 前者是状态、后者要求调用方永不漏调。降级一遍是 O(格数) 的连续写，
    // 128×128 是 16 K 字节，比任何一次视野解算都便宜。
    void begin_tick() noexcept;

    // 标记「现在看得见」。
    void mark_visible(int x, int y, Tick now) noexcept;

    // 记下「这一格有这么一座建筑」。**只应在该格当前可见时调**（有断言）——
    // 记忆只能来自看见，否则就是把 ground truth 抄进记忆，
    // 而那恰好是硬要求第 1 条禁的事，且不会有任何东西报错。
    void remember_bld(int x, int y, BldType type, std::uint16_t hp_permille) noexcept;

    // 记下「这一格没有建筑」——**缺口**。同上，只应在该格当前可见时调。
    void remember_no_bld(int x, int y) noexcept;

    // ——观测与哈希——

    // `visible` 通道的原始字节（`Vis` 的整数值，0/1/2）。
    // 打包时逐字节过 `vis_value`，不要自己判 `!= 0`——那就把三态压回两态了。
    const std::uint8_t* vis_bytes() const noexcept;

    std::size_t cell_count() const noexcept { return vis_.size(); }

    // 喂进状态哈希。**顺序固定：vis → last_seen → 建筑类型 → 血量千分比。**
    //
    // 迷雾进哈希不是可选的：记忆是**累积**状态（不是每 tick 从世界重算出来的），
    // 所以两个世界可以实体完全一致而记忆不同，此后立刻分叉。
    // 回放的作用是尽早发现分叉，漏掉记忆就等于把分叉推迟到它影响到动作之后。
    void feed_hash(StateHash& h) const noexcept;

private:
    std::size_t idx(int x, int y) const noexcept;

    int width_ = 0;
    int height_ = 0;
    std::vector<Vis> vis_;
    std::vector<Tick> last_seen_;
    std::vector<BldType> bld_;
    std::vector<std::uint16_t> bld_hp_permille_;
    // 「记忆里这格有建筑」。单独一张表而不是拿 `hp_permille == 0` 表示「没有」:
    // 缺口的血量正好是 0，两者会撞在一起——而分开它们是三态设计的全部意义。
    std::vector<std::uint8_t> has_bld_;
};

static_assert(sizeof(Vis) == 1);
static_assert(sizeof(RememberedBld) == 4);

}  // namespace rts

#endif  // RTS_FOG_HPP
