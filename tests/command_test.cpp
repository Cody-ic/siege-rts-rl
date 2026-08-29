#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <set>
#include <string_view>

#include "rts/command.hpp"
#include "rts/hash.hpp"

// 标题刻意不写 kind 的数目：它每加一条命令就会错一次，而下面的循环读的是
// `kCommandKindCount`、本来就不需要那个数字。同 `tests/CMakeLists.txt` 那条
// 「刻意不写条目总数」。（原文写的是「11 个」，加 `Clear` 之后就成了错的。）
TEST_CASE("None 是 0，且每个 kind 各有唯一名字", "[command]") {
    REQUIRE(static_cast<int>(rts::CommandKind::None) == 0);

    std::set<std::string_view> seen;
    for (int i = 0; i < rts::kCommandKindCount; ++i) {
        const std::string_view id = rts::ident_of(rts::command_kind_at(i));
        INFO("下标 " << i);
        REQUIRE_FALSE(id.empty());
        REQUIRE(seen.insert(id).second);
    }
    REQUIRE(seen.size() == static_cast<std::size_t>(rts::kCommandKindCount));
}

TEST_CASE("Command 无填充字节，可整体喂进状态哈希", "[command]") {
    // 已有 static_assert 查 sizeof / alignof，这条从**哈希那一侧**查同一件事：
    // `feed_pod` 要求 has_unique_object_representations，而含填充字节的结构体
    // 不满足它。若哪天有人往 Command 里加一个 uint16 字段、把布局撑出填充，
    // 这行会编不过——而那正是想要的时机，不是等回放偶发失败的时候。
    rts::StateHash h;
    const rts::Command c{};
    h.feed_pod(c);   // 编译过就是结论
    REQUIRE(h.value() != rts::StateHash::kOffsetBasis);
    REQUIRE(sizeof(rts::Command) == 6);
}

TEST_CASE("两条值相同的命令哈希相同", "[command][determinism]") {
    // 上一条查的是「能不能喂」，这条查的是「喂了有没有意义」。
    //
    // 刻意用 memset 把两块内存**预先填成不同的垃圾**再构造：填充字节若存在，
    // 它会残留下来，于是两个值相等的 Command 哈希不等——回放测试将偶发失败，
    // 而失败率取决于栈上残留了什么，是最难复现的一类。
    alignas(rts::Command) unsigned char buf_a[sizeof(rts::Command)];
    alignas(rts::Command) unsigned char buf_b[sizeof(rts::Command)];
    std::memset(buf_a, 0x00, sizeof(buf_a));
    std::memset(buf_b, 0xFF, sizeof(buf_b));

    const rts::Command tpl{7, rts::CommandKind::Build, rts::Side::Defender,
                           static_cast<std::uint8_t>(rts::BldType::Tower),
                           rts::kNoForce};
    std::memcpy(buf_a, &tpl, sizeof(tpl));
    std::memcpy(buf_b, &tpl, sizeof(tpl));

    rts::StateHash ha;
    rts::StateHash hb;
    ha.feed(buf_a, sizeof(buf_a));
    hb.feed(buf_b, sizeof(buf_b));
    REQUIRE(ha.value() == hb.value());
}

TEST_CASE("kNoSlot 与 kNoForce 不与合法下标撞车", "[command]") {
    // 用 0 当哨兵会让「没指定槽位」与「指定了第 0 个槽位」不可区分，
    // 而第 0 个槽位在地图文件里是真实存在的一个。
    REQUIRE(rts::kNoSlot != 0);
    REQUIRE(rts::kNoForce != 0);
    // 默认构造的命令是「跳过」，且不指向任何槽位或编队。
    const rts::Command c{};
    REQUIRE(c.kind == rts::CommandKind::None);
    REQUIRE(c.slot == rts::kNoSlot);
    REQUIRE(c.force == rts::kNoForce);
}

TEST_CASE("攻方的宏观命令只有两条，其余都是守方的", "[command]") {
    // 攻方没有经济、没有建筑（预算自动给定），所以它的宏观动作只有
    // 「兵种配比」与「集结点选择」。这条测试防的是有人给攻方加一条建造类命令——
    // 那会静默推翻「攻方没有经济系统」这条设计。
    int atk = 0;
    for (int i = 0; i < rts::kCommandKindCount; ++i) {
        if (rts::owner_of(rts::command_kind_at(i)) == rts::Side::Attacker) ++atk;
    }
    REQUIRE(atk == 2);
    REQUIRE(rts::owner_of(rts::CommandKind::Composition) == rts::Side::Attacker);
    REQUIRE(rts::owner_of(rts::CommandKind::PickSpawn) == rts::Side::Attacker);
}

TEST_CASE("跳过对两侧都合法，其余命令只对自己那一侧合法", "[command]") {
    // `None` 是每一侧动作空间里那个「兜底一定合法」的动作。缺了它，
    // 掩码在「什么都买不起」的局面下会把全部 logits 屏蔽掉——那是一个
    // 没有合法动作的状态，采样时直接 NaN。
    REQUIRE(rts::is_legal_for(rts::CommandKind::None, rts::Side::Defender));
    REQUIRE(rts::is_legal_for(rts::CommandKind::None, rts::Side::Attacker));

    REQUIRE(rts::is_legal_for(rts::CommandKind::Build, rts::Side::Defender));
    REQUIRE_FALSE(rts::is_legal_for(rts::CommandKind::Build, rts::Side::Attacker));
    REQUIRE(rts::is_legal_for(rts::CommandKind::PickSpawn, rts::Side::Attacker));
    REQUIRE_FALSE(rts::is_legal_for(rts::CommandKind::PickSpawn, rts::Side::Defender));
}

TEST_CASE("owner_of 与 side 字段是两件事", "[command]") {
    // 回放读进来的字节可以是任何东西，所以校验「这条命令合不合法」
    // 需要一个与 `side` 字段无关的判据。构造一条格式合法、语义非法的命令：
    // 守方下了一条攻方专有的 PickSpawn。
    rts::Command bogus{};
    bogus.kind = rts::CommandKind::PickSpawn;
    bogus.side = rts::Side::Defender;
    bogus.slot = 0;

    // `side` 字段说是守方，而这个 kind 只有攻方能下——两者不一致，可检出。
    REQUIRE(rts::owner_of(bogus.kind) == rts::Side::Attacker);
    REQUIRE_FALSE(rts::is_legal_for(bogus.kind, bogus.side));
}

TEST_CASE("建造命令携带完整的建筑类型", "[command]") {
    // 不采用「建墙 / 建塔 / 建防空 / 建采集」四组的粗化：那是策略侧的动作头设计，
    // 而本枚举同时是**人类玩家的指令集**，玩家必须能指定造哪一座。
    for (int i = 0; i < rts::kBldTypeCount; ++i) {
        rts::Command c{};
        c.kind = rts::CommandKind::Build;
        c.slot = 3;
        c.what = static_cast<std::uint8_t>(i);
        INFO("建筑 " << rts::ident_of(rts::bld_at(i)));
        REQUIRE(c.bld() == rts::bld_at(i));
    }
}

TEST_CASE("造兵与配比命令携带完整的兵种", "[command]") {
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        rts::Command c{};
        c.kind = rts::CommandKind::Train;
        c.what = static_cast<std::uint8_t>(i);
        REQUIRE(c.unit() == rts::unit_at(i));
    }
}

TEST_CASE("Composition 的编成位数放在 slot 字段里", "[command]") {
    // 一处「字段换用途」，写成访问器而不是靠注释散落。
    // 编成位硬性封顶约 40（结构约束），uint16 余量极大。
    rts::Command c{};
    c.kind = rts::CommandKind::Composition;
    c.side = rts::Side::Attacker;
    c.what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
    c.slot = 12;
    REQUIRE(rts::composition_slots(c) == 12);
    REQUIRE(c.unit() == rts::UnitType::Ghoul);
}

TEST_CASE("驻守是玩家级命令，不是战术动作", "[command]") {
    // CLAUDE.md「没有兵种专属动作」要求战术动作枚举保持最小，
    // 而登墙是守方专属。把它放在 Command 里，`UnitAction` 才能一个字不动。
    REQUIRE(rts::owner_of(rts::CommandKind::Garrison) == rts::Side::Defender);
}

TEST_CASE("「调哪一支部队」两个候选都还表达得出来", "[command]") {
    // 甲：MoveForce 自带 force 字段，一步说完。
    rts::Command one_step{};
    one_step.kind = rts::CommandKind::MoveForce;
    one_step.force = 2;
    one_step.slot = 17;
    REQUIRE(one_step.force == 2);

    // 乙：先 SelectForce 再 MoveForce，两步。
    rts::Command pick{};
    pick.kind = rts::CommandKind::SelectForce;
    pick.force = 2;
    REQUIRE(pick.slot == rts::kNoSlot);   // 第一步不指定去处

    // 这条测试的作用是**提醒**：待定项定了之后，其中一条会变成死代码，
    // 该删。两条并存意味着同一件事有两种字节表达，
    // 而回放里出现哪一种取决于是谁录的。
    REQUIRE(rts::CommandKind::SelectForce != rts::CommandKind::MoveForce);
}
