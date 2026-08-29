// 观测通道注册表。**带版本号与布局指纹，Python 侧一律从这里读，不硬编码。**
//
// 规范来源是 `CLAUDE.md`「RL 设计决策」：
//
//   > **观测**：以单位自身为中心的 K×K 局部视野多通道张量（我方/敌方单位密度与血量、
//   > 墙、门、可通行地形、目标方向场）+ 自身状态向量 + 少量全局标量。
//
// ## 为什么是「注册表」而不是几个常量
//
// 地形枚举本周刚从 3 变 5、还可能再动；通道也会随训练调整增减。若 Python 侧
// 硬编码通道数与顺序，那么两侧不一致时**不会有任何东西报错**——`reshape` 照样成功
// （总元素数对得上），网络照样收敛（收敛到一个把「墙」当成「敌方血量」的表示上）。
// 这是本项目最防的失败形态：不红、不崩、只是学不动，而排查方向完全错。
//
// 所以三件事一起给：
//
//   * `kObsVersion` —— 人手维护的版本号，语义变化时 +1
//   * `kObsLayoutFingerprint` —— **编译期**算出的通道名折叠值，重排即变
//   * `kObsChannels` —— 名字与归一化方式的表，Python 侧照它解包
//
// 版本号会被忘记改，指纹不会。两者的分工：指纹管**布局**（顺序、增删、改名），
// 版本号管**语义**（同一个名字的含义变了，比如 `enemy_level` 从存和改成存平均）。
// 指纹抓不到后者，所以两个都要。
//
// **权重存盘时要把指纹一并存进去。** 载入一个指纹不符的 checkpoint 必须是硬失败——
// 那是训练侧唯一能在事前拦住这类错位的地方。
//
// ## 这一版是**战术层**的观测，宏观 / 决策层的不在这里
//
// 守方决策层与攻方宏观层吃的是**全局摘要**（槽位逐项状态、三种资源存量、
// 各编队位置与兵力），不是 K×K 局部视野——`守方AI与协同演化.md` 第 4 节。
// 那份观测依赖「地图给出的槽位列表」，而槽位列表随 `World` 的数据布局一起落地，
// 所以它是下一版的事。**这里明说而不是静默省略**：读到本文件的人不该以为
// 宏观层复用这套通道。

#ifndef RTS_OBS_HPP
#define RTS_OBS_HPP

#include <array>
#include <cstdint>
#include <string_view>

#include "rts/hash.hpp"
#include "rts/roster.hpp"

namespace rts {

// 语义版本。**改通道含义时手动 +1。** 增删或重排通道不必靠它——指纹会变。
//
// **版本 2：`visible` 从两档变三档。** 这一改动同时是「为什么指纹不够、
// 必须再有一个版本号」的现成例子——通道名、归一化方式、成对标记一个字都没动，
// 所以 `kObsLayoutFingerprint` **完全不变**，而含义变了。
// 只靠指纹的话，一个用版本 1 权重的 checkpoint 会被判为兼容，然后学出来的
// 「0 = 看不见」在新语义下是「从未见过」——不报错、不崩、只是策略是错的。
//
// 改动的理由见 `rts/fog.hpp` 文件头「三态，不是两态」：两档时
// 「从未侦查过」与「侦查过、那里是个缺口」在张量里是同一串字节，
// 而后者正是核心评估指标「AI 是否发现并利用已有缺口」要的东西。
inline constexpr int kObsVersion = 2;

static_assert(kObsVersion >= 2,
              "visible 通道是三档（见 rts/fog.hpp 的 vis_value）。"
              "把版本号退回 1 意味着有人把它改回了布尔——那会让「从未见过」"
              "与「记忆里是缺口」再次不可区分。");

// ——K×K 局部视野的边长——
//
// **这是占位值，标注在此以免被当成已定值引用**（CLAUDE.md「关于数值」明文要求
// 举例用的数字必须标注）。它必须是奇数，自身才能落在正中。
//
// 定它需要先标定**射程**，而射程未标定。约束是 `kObsK / 2 ≥ 最大射程`：
// 看不见自己能打到的地方，等于把射程优势从观测里删掉。
// 地图边长量级 64–128（`地图与场景设计.md` 第 3 节），所以 K 不该接近那个量级——
// 那就退化成全局视野，卷积的平移不变性白费。
//
// 改它不需要改任何代码，但**要改版本号**：K 变了张量形状就变了，
// 而形状不匹配 PyTorch 会当场报错，所以这一项是三个里最不容易静默出错的。
inline constexpr int kObsK = 15;

static_assert(kObsK % 2 == 1, "K 必须是奇数，否则自身单位不在视野正中");
static_assert(kObsK >= 3);

// ——归一化方式——
//
// 每条通道都必须说明自己怎么归一化，因为**归一化选错会静默毁掉课程学习**，
// 而那是我们省掉手工 curriculum 的唯一依据（CLAUDE.md：「波数即难度轴，
// 天然构成课程学习，不需要手工设计 curriculum」）。
//
// 具体地：攻方兵力预算超线性增长，所以任何**固定常数**做分母的通道，
// 在第 10 波与第 100 波之间的输入尺度会漂移两个数量级，
// 低波次学到的表示在高波次直接不可用。
enum class ObsNorm : std::uint8_t {
    // 已经落在 [0, 1]：血量比例、可通行性、可见性。
    Frac01,
    // 计数 ÷ 每格容量上限。单位有体积，一格里挤得下的数量是有界的。
    Count,
    // **等级和 ÷ 本波名义等级 L(w)**，其中 L(w) = 兵力预算 ÷ 编成位。
    //
    // 量纲上这是「这一格有多少个本波标准兵当量」，与 Count 同量纲，
    // 两条通道因此可以相加、相减、共享同一组卷积核。
    //
    // 两侧共用同一个分母（不是各按自己的预算），否则守方与攻方的同一格
    // 会有两个尺度。
    LevelSum,
    // [−1, 1]，方向场的两个分量。
    Signed,
};

// ——通道——
//
// **成对的通道必须相邻，且顺序恒为 (ally, enemy)。** 这条不是排版偏好：
// Python 侧按 `obs[..., 2k]` / `obs[..., 2k+1]` 取一对来算差值（我方优势），
// 插一条不成对的进去会让后面所有对**错位一位**——而那不报错。
//
// `ally` / `enemy` 是**相对于观测者**的，不是 `Defender` / `Attacker`：
// 策略网络是共享的（parameter sharing），两侧共用一套权重，
// 所以观测必须以观测者为原点。写成绝对阵营会让同一份权重在两侧看到镜像输入。
//
// 中立可破坏障碍**不占一对**，它单独一条。若给它加 `Side::Neutral`
// 让它也成对，成对通道数就变成奇数——见 `rts/types.hpp` 里 `ObstacleId` 那一段。
enum class ObsChannel : std::uint8_t {
    // —— 成对（3 对 = 6 条）——
    AllyDensity = 0,
    EnemyDensity,
    AllyHp,            // Σ (hp / max_hp)，不是 Σ hp——见下面第 ⑩ 条那段
    EnemyHp,
    AllyLevel,         // Σ level ÷ L(w)，不是平均——见下面第 ⑧ 条那段
    EnemyLevel,

    // —— 不成对（8 条）——
    WallHp,            // 墙段血量比例
    GateHp,            // 城门单独一条：破坏速率高于城墙，是结构上的既定薄弱点，
                       // 与墙混在一条通道里 AI 就分不出「这里更好打」
    BldHp,             // 非墙建筑（塔、防空、兵营、采集……）血量比例
    ObstacleHp,        // 中立可破坏障碍
    Passable,          // 可通行地形
    Visible,           // **观测者自己的迷雾状态，三档**：0 从未见过 / 0.5 记忆 / 1 可见
                       // 取值一律经 `rts::vis_value()`（`rts/fog.hpp`），
                       // 不要在打包器里写 `vis != Unseen` 之类——那就压回两档了
    FlowDi,            // 目标方向场，格坐标 i 分量
    FlowDj,            // 同上，j 分量
};

inline constexpr int kObsChannelCount = 14;
inline constexpr int kObsPairedCount = 6;   // 前 6 条，3 对

static_assert(static_cast<int>(ObsChannel::FlowDj) == kObsChannelCount - 1);
static_assert(kObsPairedCount % 2 == 0);
static_assert(static_cast<int>(ObsChannel::WallHp) == kObsPairedCount,
              "成对通道必须是**前缀**，不许与不成对的交错");

// `Visible` 这一条是硬要求，不是可选项。CLAUDE.md「RL 侧两条硬要求」第 1 条：
//
//   > **AI 的观测必须是它自己的迷雾状态，不得是 ground truth。**
//
// 而只给密度通道是不够的：没有 `Visible`，「那一格没有敌人」与「那一格看不见」
// 在张量里是同一个 0。整套侦查博弈（佯攻诱饵、藏不死鸟、屏蔽集结区）
// 都建立在这两者的区别上——分不开的话，AI 会把所有迷雾格当成空地，
// 于是它永远不会学「先派斥候」。
//
// **而它必须是三档，两档不够**——同一条推理再走一步：两档时
// 「从未见过」与「见过、那里是个缺口」又撞成同一串字节（两者的 `visible`
// 与 `wall_hp` 全为 0）。完整推导在 `rts/fog.hpp` 文件头。

struct ObsChannelSpec {
    std::string_view name;   // Python 侧的通道名，ASCII，snake_case
    ObsNorm norm;
    bool paired;             // 是否属于 (ally, enemy) 成对通道
};

// 名字用 snake_case 而不是照抄枚举的 CamelCase：它们要变成 Python 的标识符
// （`obs.channels.ally_density`），而 Python 侧的惯例是 snake_case。
// 转换写在这里一次，比让每个读它的人各转一次好。
inline constexpr std::array<ObsChannelSpec, kObsChannelCount> kObsChannels{{
    {"ally_density",  ObsNorm::Count,    true},
    {"enemy_density", ObsNorm::Count,    true},
    {"ally_hp",       ObsNorm::Frac01,   true},
    {"enemy_hp",      ObsNorm::Frac01,   true},
    {"ally_level",    ObsNorm::LevelSum, true},
    {"enemy_level",   ObsNorm::LevelSum, true},
    {"wall_hp",       ObsNorm::Frac01,   false},
    {"gate_hp",       ObsNorm::Frac01,   false},
    {"bld_hp",        ObsNorm::Frac01,   false},
    {"obstacle_hp",   ObsNorm::Frac01,   false},
    {"passable",      ObsNorm::Frac01,   false},
    {"visible",       ObsNorm::Frac01,   false},
    {"flow_di",       ObsNorm::Signed,   false},
    {"flow_dj",       ObsNorm::Signed,   false},
}};

// ——自身状态向量——
//
// 与上面的 K×K 张量分开喂：它是逐单位的标量，没有空间结构，
// 塞进 K×K 的中心格会浪费 K²−1 份存储、还让卷积核去学「只看中心」。
enum class ObsSelfField : std::uint8_t {
    // 兵种 one-hot 占 kUnitTypeCount 条。**共享策略网络靠它区分兵种**
    // （CLAUDE.md：「所有单位共用一个网络，兵种以 one-hot 输入区分」）。
    TypeOneHot = 0,
    // 自身等级 ÷ L(w)。与 `AllyLevel` 同一个分母，理由同。
    LevelNorm = kUnitTypeCount,
    // 自身血量比例。
    HpFrac,
    // **攻击前摇的剩余比例**（1 = 刚开始摇，0 = 已可出手）。
    //
    // 它必须来自仿真、不能由前端或策略自己数帧：前摇时长是**待定数值**，
    // 精灵只给关键姿势、不编码时长（`_sprite_meta.json` 的 note 明写这一条）。
    // 而「弓手有攻击前摇，被贴脸即废」是 CLAUDE.md 列出的机制性克制之一，
    // 观测里没有它，AI 就学不出「贴脸打断」。
    WindupLeft,
};

inline constexpr int kObsSelfCount = kUnitTypeCount + 3;

static_assert(static_cast<int>(ObsSelfField::WindupLeft) == kObsSelfCount - 1);

// ——全局标量——
//
// 「少量」是 CLAUDE.md 的用词，这里当真了：只有两条，且两条都是硬要求。
// 想加第三条时先问它是不是能从上面两组里推出来。
enum class ObsGlobal : std::uint8_t {
    // **波数**，CLAUDE.md 硬要求（「观测中必须包含波数与玩家当前的防御布局」；
    // 后半句由 WallHp / GateHp / BldHp 三条通道承担）。
    //
    // 喂 `log2(1 + wave)` 而不是 `wave`：与 LevelSum 那条同病同治，
    // 线性的波数在第 100 波是第 10 波的 10 倍，输入尺度漂移会毁掉课程迁移。
    // 取对数之后第 1000 波也只有 10 左右，**不需要一个待标定的分母**——
    // 这一点是刻意的：任何形如 `wave / 训练截断波数` 的写法都把观测
    // 绑在一个还没定的数上。
    WaveLog = 0,
    // **当前存活的空军数 ÷ 当前波次的空军上限**，CLAUDE.md 明写
    // 「观测中需包含当前存活空军数量」。
    //
    // 存比例而非绝对数：上限本身随波数缓慢放开（也是待标定的曲线），
    // 存绝对数就又把观测绑在那条曲线上。
    AerialAliveFrac,
};

inline constexpr int kObsGlobalCount = 2;

static_assert(static_cast<int>(ObsGlobal::AerialAliveFrac) == kObsGlobalCount - 1);

// **本波进度（已过 tick ÷ 预期波长）刻意没有加进来。**
// 它的分母是预期波长，而那是待定数值；用一个错的分母比没有这一项更坏——
// 前者会让 AI 学到一个基于假时间轴的策略，而且看不出来。
// 等波长标定了再加，那时改版本号。

// 自身向量与全局标量的名字。**与上面两个枚举同序**，理由同 kObsChannels：
// Python 侧要按名字取，而名字表与枚举分开写就会漂移。
//
// `type_onehot` 是一个**块**，占 kUnitTypeCount 条而不是 1 条——它在这张表里
// 只占一个名字，解包时按块展开。这一条容易读错，所以写下来。
inline constexpr std::array<std::string_view, 4> kObsSelfNames{
    "type_onehot", "level_norm", "hp_frac", "windup_left"};

inline constexpr std::array<std::string_view, kObsGlobalCount> kObsGlobalNames{
    "wave_log", "aerial_alive_frac"};

// 自身向量的**块**数（4），与元素数 kObsSelfCount（= 11 + 3）不是一回事。
// 两个数放在一起，就是为了让下一个读它的人不必再算一遍。
inline constexpr int kObsSelfBlockCount = 4;
static_assert(kObsSelfNames.size() == static_cast<std::size_t>(kObsSelfBlockCount));

constexpr std::string_view ident_of(ObsNorm n) noexcept {
    switch (n) {
        case ObsNorm::Frac01:   return "Frac01";
        case ObsNorm::Count:    return "Count";
        case ObsNorm::LevelSum: return "LevelSum";
        case ObsNorm::Signed:   return "Signed";
    }
    return {};
}

// ——布局指纹——
//
// 把全部通道名、归一化方式、成对标记、自身向量与全局标量的名字，
// 折进一个 64 位值。**重排、改名、增删、改归一化方式都会让它变。**
//
// 存在的理由见文件头：版本号靠人记得改，指纹不靠。
// 训练侧把它写进 checkpoint，载入时不符即**硬失败**——
// 那是唯一能在事前拦住「按旧顺序解包新张量」的地方。
//
// 分隔符不是装饰：没有它，`{"ab","c"}` 与 `{"a","bc"}` 会折出同一个值。
constexpr std::uint64_t obs_layout_fingerprint() noexcept {
    StateHash h;
    for (const ObsChannelSpec& c : kObsChannels) {
        h.feed_text(c.name);
        h.feed_text("\x1f");
        h.feed_text(ident_of(c.norm));
        h.feed_text(c.paired ? "\x1fpaired\x1e" : "\x1fsingle\x1e");
    }
    h.feed_text("\x1dself\x1d");
    for (const std::string_view n : kObsSelfNames) {
        h.feed_text(n);
        h.feed_text("\x1e");
    }
    h.feed_text("\x1dglobal\x1d");
    for (const std::string_view n : kObsGlobalNames) {
        h.feed_text(n);
        h.feed_text("\x1e");
    }
    return h.value();
}

inline constexpr std::uint64_t kObsLayoutFingerprint = obs_layout_fingerprint();

// 指纹不能是 offset basis——那意味着一个字节都没喂进去（表空了、或循环写错了），
// 而那时它是一个**稳定的**值，看起来完全正常。
static_assert(kObsLayoutFingerprint != StateHash::kOffsetBasis);

}  // namespace rts

#endif  // RTS_OBS_HPP
