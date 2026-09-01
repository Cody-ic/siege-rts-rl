// 玩家级 / 宏观级命令。**这一套枚举同时是三样东西**，而那不是巧合：
//
//   1. 人类玩家能下的全部指令（`game/` 把鼠标键盘翻译成它）
//   2. **守方 RL 决策层的动作空间**
//   3. 回放文件里存的指令流（`CLAUDE.md`：回放 = 种子 + 指令流）
//
// 第 2 条来自 #46 落地的结论：**守方 RL 只在决策层，单兵由参数化脚本驱动。**
// 因为守方 AI 是人类玩家的替身，它的动作空间**应当恰好等于玩家能下的指令**——
// 多出来的那部分能力没有对应的真人来源，而那正是「攻方过拟合到超人类微操」
// 那个坑的入口。所以这里只需要一套枚举，不是两套。
//
// 攻方的宏观层（兵种配比、集结点选择）也在这里，按 `Side` 参数化。
// 它是 bandit 尺度的小决策（一波一次），与守方的短序列共用同一个提交通道。
//
// ## 与战术动作的分界
//
// `rts/action.hpp` 是「一个单位这 4–8 tick 干什么」，本文件是「玩家按了什么」。
// 分界线上有一个刻意的判决：**「驻守墙段槽位」归本文件，不进 `UnitAction`。**
// 理由是 CLAUDE.md 的「没有兵种专属动作」——登墙是守方专属，塞进战术枚举
// 就等于给动作空间开了一个按兵种分叉的口子，而攻方本来就没有登墙手段。
//
// ## 动作头怎么粗化，是 train/ 的事，不是本文件的事
//
// `守方AI与协同演化.md` 第 3 节给的动作头是「操作类型（建墙 / 建塔 / 建防空 /
// 建采集 / 维修 / 造兵 / 调兵 / 跳过，约 8 个）× 目标（槽位 index，约 60 个）」，
// 而那是一个**粗化**：11 种建筑被并成 4 组。
//
// **本文件刻意不采用那个粗化**，`Build` 带完整的 `BldType`。因为第 1 条用途要求
// 它是玩家指令集的**完备**表达——若把粗化烤进来，人类玩家就真的没法指定
// 造哪一座建筑了，而那是荒谬的。从粗动作头映射到具体 `Command` 属于策略侧，
// 放在 `train/`；仿真接受的是完整信息。
//
// ## 掩码不在这里
//
// 「买不起 / 槽位已占 / 完好的墙段无需维修」这些一律要在 logits 上屏蔽
// （否则 agent 会把大量样本浪费在学习规则本身）。但掩码要读世界状态，
// 所以它是 `World` 的方法，随数据布局那一版给出，不是一个能光凭枚举定义的东西。

#ifndef RTS_COMMAND_HPP
#define RTS_COMMAND_HPP

#include <cassert>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "rts/roster.hpp"
#include "rts/types.hpp"

namespace rts {

// **`None` 必须是 0，且它是「跳过」而不是「没有命令」。**
//
// 这个区分是实打实的：跳过是 RL 的一个**合法动作**（`守方AI与协同演化.md` 第 3 节
// 的操作类型表里就有它），一波之内策略常常该什么都不做。若把 0 当成哨兵
// 「这一格没填」，那么「策略选择了跳过」与「有人忘了填」在字节上不可区分，
// 而回放里两者的含义完全不同。
//
// 所以：0 = 策略/玩家**主动**决定这一步不动作。缺失用「不提交这条命令」表达，
// 不用哨兵值。
enum class CommandKind : std::uint8_t {
    None = 0,

    // ——守方：建造与经济——
    Build,        // slot = 槽位；what = BldType
    Repair,       // slot = 槽位
    Cancel,       // slot = 槽位；撤销施工中的工地（工地在杀伤区里，撤是真决策）
    Train,        // what = UnitType；在 slot 指定的兵营（Barrack）出兵
    Summon,       // 提前召唤下一波。CLAUDE.md 明写「必须提供」，附奖励

    // ——攻方：宏观（一波一次，bandit 尺度）——
    Composition,  // what = UnitType；param = 该兵种占的编成位数
    PickSpawn,    // slot = 集结点 index；可对多个集结点各下一条 = 分兵佯攻

    // ——守方：清野。**它按分组本该排在 `Cancel` 旁边，放在这里是被迫的**，见下——
    Clear,        // slot = 格线性下标；清掉该格的可破坏障碍

    // ——守方：建筑升级。**同理该排在 `Repair` 旁边，追加在末尾是同一条纪律**——
    Upgrade,      // slot = 槽位；`Keep` 不受 `building_level_cap()` 约束
};

inline constexpr int kCommandKindCount = 10;

static_assert(static_cast<int>(CommandKind::Upgrade) == kCommandKindCount - 1);

// **枚举值就是回放的线路编码，所以新增一律追加在末尾，不按语义分组插入。**
//
// `Clear` 是守方的建造/经济类命令，读起来该跟在 `Cancel` 后面。但插在那里会让
// `Train`..`PickSpawn` 全部后移一位，于是**旧回放里那些字节会被重新解释成别的命令**
// ——`Clear` 变 `Composition` 之类。那种失效不报错，只是回放出一局不同的仗。
//
// 追加则只有一个后果：旧回放里不会出现 `Clear`，而它本来也不会出现。
//
// 附带受影响的还有 `command_mask()` 的位序（`train/` 那侧读它），追加同样让旧的
// 11 个位不动。**下一个加命令的人照此办理：往末尾加，并把这段注释留着。**
//
// **这条纪律有过一次例外，值得记在这里**：编队系统整体移除时（2026-09），
// `SelectForce` / `MoveForce` / `Garrison` / `UpgradeForce` 四个枚举被**删除**，
// 其后的枚举值前移——线路编码当场全变。那次能这么做，是因为哈希口径同时进格
// （`World/11` → `World/12`）、全部已录回放本来就要重录；「删枚举」与「改口径」
// 必须绑在同一次提交里，否则旧回放的字节会被静默重解释。
//
// `Clear` 是玩家级命令而不是战术动作（没有 `UnitAction::AtkObst`），
// 理由见 CLAUDE.md「RL 设计决策」那两条：攻方不需要它（寻路已把障碍当高代价可通行，
// 撞上去自动破坏），而守方清野是波次间的决策、不是逐 tick 微操。

// 「无槽位」哨兵。
//
// 用 0xFFFF 而不是 0：0 是一个合法的槽位下标，拿它当哨兵会让
// 「没指定槽位」与「指定了第 0 个槽位」不可区分——而第 0 个槽位在地图文件里
// 是真实存在的一个。
inline constexpr std::uint16_t kNoSlot = 0xFFFFu;

// 一条命令。
//
// **字段顺序是为了零填充**：uint16 在前，uint8 们在后，于是 sizeof 恰好是
// 对齐（2）的整数倍、没有编译器插入的填充字节。这条对回放不是锦上添花——
// 填充字节的内容是不确定的，若直接把结构体喂进 `StateHash`（`World::state_hash()`
// 对待排空命令队列就是这么干的：`h.feed(q.data(), q.size() * sizeof(Command))`）
// 或写进文件，**同一条命令可能哈希出两个值**。下面有 static_assert 钉住。
//
// `what` 一个字段兼放 `BldType` 与 `UnitType`，语义由 `kind` 决定。
// 这是刻意的紧凑，代价是类型安全，所以取值一律经下面两个访问器（带断言），
// 不要直接读 `what`。
//
// **`level`（兵种等级上限落地时追加）只有 `Train` 读**——征兵时选等级，
// 1..`unit_level_cap()` 任选。`_reserved` 不是笔误：四个 uint8 字段合计
// 4 字节，加上 `slot` 的 2 字节是 6，不是 2 的整数倍，编译器会在结构体末尾
// 插内容不确定的填充字节——正是上一段要挡的那种。补两个显式、恒为 0
// 的字段把总字节数凑回 8（2 的整数倍），于是「有没有填充」不再取决于
// 编译器的选择，是结构体自己保证的。
struct Command {
    std::uint16_t slot = kNoSlot;
    CommandKind   kind = CommandKind::None;
    Side          side = Side::Defender;
    std::uint8_t  what = 0;
    std::uint8_t  level = kMinUnitLevel;
    std::uint8_t  _reserved[2] = {0, 0};

    friend constexpr bool operator==(Command, Command) noexcept = default;

    constexpr BldType bld() const noexcept {
        assert(kind == CommandKind::Build);
        return static_cast<BldType>(what);
    }

    constexpr UnitType unit() const noexcept {
        assert(kind == CommandKind::Train || kind == CommandKind::Composition);
        return static_cast<UnitType>(what);
    }
};

// `param` 只有 `Composition` 用（该兵种占多少编成位），而它需要的位宽超过 uint8
// 吗？不需要：编成位**硬性封顶约 40**（CLAUDE.md，且那是结构约束不是数值），
// 所以 uint16 有充足余量。
//
// 于是 `Composition` 的权重放在 `slot` 里（uint16，肯定够），而它不用槽位。
// 这一处「字段换用途」写在这里而不是靠注释散落：
constexpr std::uint16_t composition_slots(Command c) noexcept {
    assert(c.kind == CommandKind::Composition);
    return c.slot;
}

static_assert(sizeof(Command) == 8,
              "回放要按字节存命令流；有填充字节的话同一条命令可能哈希出两个值");
static_assert(alignof(Command) == 2);
static_assert(std::is_trivially_copyable_v<Command>);

// 一条命令属于哪一侧的动作空间。
//
// **不是 `c.side`。** `c.side` 是「谁下的」，本函数是「这个 kind 只有哪一侧能下」。
// 两者不同：`Composition` 是攻方专有的，一条 `side == Defender` 的
// `Composition` 是**格式合法但语义非法**的命令，而回放读进来的字节可以是任何东西。
// 校验因此需要一个与 `side` 字段无关的判据。
constexpr Side owner_of(CommandKind k) noexcept {
    switch (k) {
        case CommandKind::Composition:
        case CommandKind::PickSpawn:
            return Side::Attacker;
        case CommandKind::None:
        case CommandKind::Build:
        case CommandKind::Repair:
        case CommandKind::Cancel:
        case CommandKind::Train:
        case CommandKind::Summon:
        case CommandKind::Clear:
        case CommandKind::Upgrade:
            return Side::Defender;
    }
    return Side::Defender;
}

// `None`（跳过）两侧都能下——它是每一侧动作空间里那个「兜底一定合法」的动作。
// 上面 `owner_of` 把它归给守方只是因为函数要有返回值；判「这一侧能不能下」用这个。
constexpr bool is_legal_for(CommandKind k, Side s) noexcept {
    return k == CommandKind::None || owner_of(k) == s;
}

constexpr std::string_view ident_of(CommandKind k) noexcept {
    switch (k) {
        case CommandKind::None:        return "None";
        case CommandKind::Build:       return "Build";
        case CommandKind::Repair:      return "Repair";
        case CommandKind::Cancel:      return "Cancel";
        case CommandKind::Train:       return "Train";
        case CommandKind::Summon:      return "Summon";
        case CommandKind::Composition: return "Composition";
        case CommandKind::PickSpawn:   return "PickSpawn";
        case CommandKind::Clear:       return "Clear";
        case CommandKind::Upgrade:     return "Upgrade";
    }
    return {};
}

constexpr CommandKind command_kind_at(int i) noexcept {
    return static_cast<CommandKind>(i);
}

}  // namespace rts

#endif  // RTS_COMMAND_HPP
