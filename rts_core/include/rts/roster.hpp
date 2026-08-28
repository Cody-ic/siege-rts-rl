// 花名册：11 单位 + 11 建筑 + 3 中立障碍。
//
// **规范来源是 `CLAUDE.md`「兵种与建筑花名册」的三张表**，本文件是它的代码化。
// 中文展示名在 `game/display_names.hpp`——按 CLAUDE.md 命名纪律，
// **中文只出现在展示层**，这里一个汉字都不该有。
//
// ## 为什么现在才建这个头
//
// `rts/types.hpp` 开头原先写着「花名册枚举不在这里，它们仍在变动中（PR #19 正在改）」。
// #19 已合入、标识符已定稿（README「已定 / 未定速查」：11 单位 + 11 建筑），
// 所以现在是把它落进代码的时候。
//
// ## 这个头**只有身份，没有属性**
//
// 血量、伤害、射程、速度、造价、破坏速率、编成位占用**一个都不在这里**，
// 将来也不在这里——CLAUDE.md「关于数值」要求它们以占位常量放在 JSON 数据文件中、
// 调整不触碰代码。本文件只回答「有哪些东西、它属于哪一侧」。
//
// 唯一的例外是三条**结构性**属性（`side_of` / `is_aerial` / `is_combat`）。
// 它们不是数值：CLAUDE.md 把它们逐条写成了「结构性决定，不要改」，
// 而结构约束与数值的区别是本项目反复强调的一条（「关于数值」末尾两条纪律）。
//
// ## 与素材的对应关系是 1:1，且有一道检查
//
// `tools/sprite_gen/out_3d/_sprite_meta.json` 里正好 35 个标识符 =
// 11 单位 + 11 建筑 + 3 障碍 + 10 地形贴图变体。前三组与本文件逐一对应，
// 由 `render::SpriteAtlas::verify_roster_covered()` 钉住（经 `render_verify_assets`）。
// 没有那道检查的话，新增一个单位而忘了出图，最早暴露的时机是它第一次被画出来。

#ifndef RTS_ROSTER_HPP
#define RTS_ROSTER_HPP

#include <cstdint>
#include <string_view>

#include "rts/types.hpp"

namespace rts {

// ——单位——
//
// **守方在前、攻方在后，中间不许插队。** 下面那条 static_assert 把这个分块钉住，
// 因为 `side_of()` 的测试会拿分块边界与逐成员 switch 互相印证：
// 两者都对才叫对，只有一个对说明有人插了队。
//
// 标识符与 CLAUDE.md 花名册逐字一致，且一律 ≤7 字符（命名纪律）。
enum class UnitType : std::uint8_t {
    // 守方（人类王国）
    Archer = 0,   // 戍卫弓手：中程单体，上墙吃高度加成
    Spear,        // 铁壁枪卫：近战抗骑，堵缺口
    Ranger,       // 逐风猎骑：快速近战，出城的执行手段
    Scout,        // 游猎斥候：极快、大视野、无战力
    Mason,        // 工匠：无战力，维修

    // 攻方（亡灵 / 魔物大军）
    Ghoul,        // 亡灵步兵：近战单体、耐打
    Shade,        // 亡灵弓手：中程单体，压制墙头
    Knight,       // 鬼域骑士团：快速冲锋，冲锋伤害 ∝ 助跑距离
    Phoenix,      // 不死鸟：**唯一的空中单位**
    Wraith,       // 幽影窥使：**地面**、极快、大视野、无战力
    Ram,          // 攻城锤：贴身、AOE、对建筑特攻
};

inline constexpr int kUnitTypeCount = 11;
inline constexpr int kDefenderUnitCount = 5;
inline constexpr int kAttackerUnitCount = 6;

static_assert(kDefenderUnitCount + kAttackerUnitCount == kUnitTypeCount);
// 分块边界。守方那一段以 `Mason` 结束、攻方那一段以 `Ghoul` 开始。
static_assert(static_cast<int>(UnitType::Ghoul) == kDefenderUnitCount);
static_assert(static_cast<int>(UnitType::Ram) == kUnitTypeCount - 1);

// ——建筑——
//
// **全部属守方。** 攻方没有经济系统、也没有建筑（CLAUDE.md：攻方改为两个自动给定的
// 预算）。所以建筑不需要 `side_of()`——它恒为 `Side::Defender`，
// 写一个恒定返回值的函数只会让调用方以为这里有一个真的分支。
enum class BldType : std::uint8_t {
    Keep = 0,     // 领主堡垒：丢失即败，产出极少量金币作为兵力地板
    Wall,         // 城墙：可站人（驻守槽位）
    Gate,         // 城门：木制，破坏速率高于城墙——结构上的既定薄弱点
    Tower,        // 镇野箭楼：对地，齐射覆盖
    Flak,         // 蔽空弩楼：**仅对空**，且视野否定半径 > 伤害半径
    Watch,        // 瞭望塔：大视野、零战力
    Barrack,      // 兵营：部队产出点
    Fence,        // 木栅：廉价应急工事，波次进行中可即时放置
    Quarry,       // 采石场
    Lumber,       // 伐木场
    Mine,         // 金矿场：金币的唯一来源（除 Keep 那条地板）
};

inline constexpr int kBldTypeCount = 11;

static_assert(static_cast<int>(BldType::Mine) == kBldTypeCount - 1);

// ——中立可破坏障碍——
//
// 第三类实体，既不是单位也不是建筑（CLAUDE.md 为它单开了一张对照表）。
// **只有一条属性：血量。** 不吃「三轴定位」——那三轴是为参与克制矩阵的战斗单位设的，
// 而障碍不移动、不攻击、不进克制矩阵。
//
// 视觉规则是一维的：**矮而单薄 = 可破坏；高而厚重 = 不可破坏**（`Rock` / `Forest`
// 属后者）。曾用过「木质 / 矮 / 稀疏」三个维度，但它们不共变。
//
// 这三种随提案「无尽模式与地形分层」§6 待议。留位的理由见 `rts/types.hpp`
// 里 `ObstacleId` 那一段——是回放字节布局，不是乐观。
enum class ObstacleType : std::uint8_t {
    Stump = 0,    // 树桩：木质、贴地
    Sapling,      // 幼树：单株细树，明显矮于密林
    Rubble,       // 碎石：一小堆碎石，矮而散
};

inline constexpr int kObstacleTypeCount = 3;

static_assert(static_cast<int>(ObstacleType::Rubble) == kObstacleTypeCount - 1);

// ——等级——
//
// 等级只提升血量与伤害，**绝不改变克制倍率、射程、速度或任何机制**（CLAUDE.md 硬性）。
// 上限不设常量：兵力预算超线性增长而编成位封顶，所以等级没有设计上的天花板。
//
// **`kMinUnitLevel == 1` 是一条会红的检查，不是约定。**
// 观测里「敌方单位等级」通道存的是**和**而不是平均（见 rts/obs.hpp），
// 而那个设计成立的前提正是等级从 1 起：于是「和 = 0」⟺「该格无敌人」，
// 不需要额外的 mask 通道。
//
// 哪天有人把等级改成 0 基——**数组下标就是 0 基，这个改动非常自然**——
// 那条通道会静默失效：不报错、不崩，AI 只是再也分不出「空地」和「一个 1 级兵」。
// 这正是本项目最防的那种失败形态，所以它必须是编译期的。
inline constexpr int kMinUnitLevel = 1;

static_assert(kMinUnitLevel == 1,
              "观测的等级通道存和而非平均，其「和为 0 即无敌人」依赖等级从 1 起。"
              "改成 0 基会让该通道静默失效——见 rts/obs.hpp。");

// ——三条结构性属性——
//
// 全部写成**无 `default:` 的 switch**，于是新增枚举成员时这里编不过。
// 这条依赖 `/w14062`（MSVC 上 C4062 是 off-by-default，而 GCC 的 `-Wswitch` 在
// `-Wall` 里）——见 `cmake/CompilerWarnings.cmake`。**没有那个开关这里就是静默的。**

// 单位属哪一侧。攻守双方阵营不重叠（CLAUDE.md 命名纪律：因此标识符不加
// `Atk`/`Def` 前缀），所以这是一个全函数、没有「两侧都有」的情形。
constexpr Side side_of(UnitType t) noexcept {
    switch (t) {
        case UnitType::Archer:
        case UnitType::Spear:
        case UnitType::Ranger:
        case UnitType::Scout:
        case UnitType::Mason:
            return Side::Defender;
        case UnitType::Ghoul:
        case UnitType::Shade:
        case UnitType::Knight:
        case UnitType::Phoenix:
        case UnitType::Wraith:
        case UnitType::Ram:
            return Side::Attacker;
    }
    return Side::Defender;   // 不可达；只为让编译器闭嘴，switch 已穷举
}

// 是不是空中单位。
//
// **`Phoenix` 是唯一的，`Wraith` 是地面——这是结构性决定，不要改。**
// 若 `Wraith` 会飞，防空建筑就同时具备「否定侦查」这一**每波都稳定生效**的用途，
// 玩家无脑造 AA 即可，于是 AA 的机会成本不再构成两难——而那是本作「智斗」
// 最可读的载体。完整推导见 CLAUDE.md「空中单位」。
//
// 守方没有空军，所以这个函数对守方恒为假，那也是结构性的（凡人不会飞）。
constexpr bool is_aerial(UnitType t) noexcept {
    switch (t) {
        case UnitType::Phoenix:
            return true;
        case UnitType::Archer:
        case UnitType::Spear:
        case UnitType::Ranger:
        case UnitType::Scout:
        case UnitType::Mason:
        case UnitType::Ghoul:
        case UnitType::Shade:
        case UnitType::Knight:
        case UnitType::Wraith:
        case UnitType::Ram:
            return false;
    }
    return false;
}

// 进不进克制二部图。
//
// `Scout` / `Mason` / `Wraith` 三个不进——它们无战力，只作为猎杀目标存在
// （CLAUDE.md：「因此实际的平衡负担小于花名册的单位总数」）。
//
// 这不是「伤害为 0」的同义词，而是**平衡工具的定义域**：成本产出矩阵只在战斗单位
// 之间取值，把三个无战力单位算进去会得到一堆恒为「净亏全部造价」的格子。
constexpr bool is_combat(UnitType t) noexcept {
    switch (t) {
        case UnitType::Scout:
        case UnitType::Mason:
        case UnitType::Wraith:
            return false;
        case UnitType::Archer:
        case UnitType::Spear:
        case UnitType::Ranger:
        case UnitType::Ghoul:
        case UnitType::Shade:
        case UnitType::Knight:
        case UnitType::Phoenix:
        case UnitType::Ram:
            return true;
    }
    return true;
}

// ——机械遍历——
//
// 存在的理由与 `game/map_data.hpp` 那组 count 常量相同：**要能把一个枚举遍历完**。
// 展示名清单、素材覆盖检查、观测的 one-hot 都靠它，而手抄的清单一定会漏。
constexpr UnitType unit_at(int i) noexcept { return static_cast<UnitType>(i); }
constexpr BldType bld_at(int i) noexcept { return static_cast<BldType>(i); }
constexpr ObstacleType obstacle_at(int i) noexcept {
    return static_cast<ObstacleType>(i);
}

// ——代码标识符——
//
// 返回的是**代码标识符**（`"Archer"`），不是中文展示名。两个用途：
//
//   1. 素材键——`_sprite_meta.json` 的 key 就是这个串，`render::SpriteAtlas`
//      按它查图。花名册与素材的 1:1 对应因此可以被机械检查
//   2. 报错与日志。`state_hash` 对不上时要说出是哪个实体，打枚举的整数值
//      等于让读者自己去数第几个
//
// **不用来做 JSON 数值表的 key**：那份表还没有，届时用同一个串即可，但那是数值层的事。
constexpr std::string_view ident_of(UnitType t) noexcept {
    switch (t) {
        case UnitType::Archer:  return "Archer";
        case UnitType::Spear:   return "Spear";
        case UnitType::Ranger:  return "Ranger";
        case UnitType::Scout:   return "Scout";
        case UnitType::Mason:   return "Mason";
        case UnitType::Ghoul:   return "Ghoul";
        case UnitType::Shade:   return "Shade";
        case UnitType::Knight:  return "Knight";
        case UnitType::Phoenix: return "Phoenix";
        case UnitType::Wraith:  return "Wraith";
        case UnitType::Ram:     return "Ram";
    }
    return {};
}

constexpr std::string_view ident_of(BldType t) noexcept {
    switch (t) {
        case BldType::Keep:    return "Keep";
        case BldType::Wall:    return "Wall";
        case BldType::Gate:    return "Gate";
        case BldType::Tower:   return "Tower";
        case BldType::Flak:    return "Flak";
        case BldType::Watch:   return "Watch";
        case BldType::Barrack: return "Barrack";
        case BldType::Fence:   return "Fence";
        case BldType::Quarry:  return "Quarry";
        case BldType::Lumber:  return "Lumber";
        case BldType::Mine:    return "Mine";
    }
    return {};
}

constexpr std::string_view ident_of(ObstacleType t) noexcept {
    switch (t) {
        case ObstacleType::Stump:   return "Stump";
        case ObstacleType::Sapling: return "Sapling";
        case ObstacleType::Rubble:  return "Rubble";
    }
    return {};
}

}  // namespace rts

#endif  // RTS_ROSTER_HPP
