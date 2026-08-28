#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "rts/obs.hpp"

TEST_CASE("通道名唯一、非空、且是 ASCII snake_case", "[obs]") {
    // 这些名字要变成 Python 的标识符（`obs.channels.ally_density`），
    // 所以不能有大写、连字符、空格或非 ASCII——否则那一侧要么语法错、
    // 要么被迫改名，而改名会让两侧漂移。
    std::set<std::string_view> seen;
    for (const rts::ObsChannelSpec& c : rts::kObsChannels) {
        INFO("通道 " << c.name);
        REQUIRE_FALSE(c.name.empty());
        REQUIRE(seen.insert(c.name).second);
        for (const char ch : c.name) {
            const bool ok = (ch >= 'a' && ch <= 'z') || ch == '_' ||
                            (ch >= '0' && ch <= '9');
            REQUIRE(ok);
        }
    }
    REQUIRE(seen.size() == static_cast<std::size_t>(rts::kObsChannelCount));
}

TEST_CASE("成对通道是前缀，且严格按 (ally, enemy) 相邻排列", "[obs]") {
    // **这条是决策 ⑤ 那个警告的可执行形式。**
    //
    // Python 侧按 `obs[..., 2k]` / `obs[..., 2k+1]` 取一对来算「我方优势」，
    // 所以插一条不成对的进去会让后面所有的对错位一位——而那不报错、不崩，
    // 只是网络看到的每一对都是错的组合。
    //
    // 最可能的触发方式不是恶意重排，而是「给中立障碍也加上 Side::Neutral」：
    // 那会让成对通道变成奇数个。
    REQUIRE(rts::kObsPairedCount % 2 == 0);

    for (int i = 0; i < rts::kObsChannelCount; ++i) {
        const rts::ObsChannelSpec& c = rts::kObsChannels[static_cast<std::size_t>(i)];
        INFO("下标 " << i << " = " << c.name);
        REQUIRE(c.paired == (i < rts::kObsPairedCount));
    }

    for (int k = 0; k < rts::kObsPairedCount; k += 2) {
        const std::string_view a =
            rts::kObsChannels[static_cast<std::size_t>(k)].name;
        const std::string_view b =
            rts::kObsChannels[static_cast<std::size_t>(k + 1)].name;
        INFO("第 " << (k / 2) << " 对：" << a << " / " << b);
        REQUIRE(a.substr(0, 5) == "ally_");
        REQUIRE(b.substr(0, 6) == "enemy_");
        // 后缀必须相同，否则「一对」只是位置上相邻、语义上无关。
        REQUIRE(a.substr(5) == b.substr(6));
        // 归一化方式也必须一致：一对里两条用不同分母，差值就没有意义。
        REQUIRE(rts::kObsChannels[static_cast<std::size_t>(k)].norm ==
                rts::kObsChannels[static_cast<std::size_t>(k + 1)].norm);
    }
}

TEST_CASE("成对通道用相对观测者的命名，不是绝对阵营", "[obs]") {
    // 策略网络是共享的（parameter sharing），两侧共用一套权重，
    // 所以观测必须以观测者为原点。若通道叫 `defender_*` / `attacker_*`，
    // 同一份权重在两侧看到的是镜像输入——**训练照样收敛**，
    // 收敛到一个把两侧混在一起学的表示上，而那看不出来。
    for (const rts::ObsChannelSpec& c : rts::kObsChannels) {
        INFO("通道 " << c.name);
        REQUIRE(c.name.find("defender") == std::string_view::npos);
        REQUIRE(c.name.find("attacker") == std::string_view::npos);
    }
}

TEST_CASE("迷雾可见性有一条独立通道", "[obs]") {
    // CLAUDE.md「RL 侧两条硬要求」第 1 条：AI 的观测必须是它自己的迷雾状态。
    //
    // 光有密度通道不够：没有 `visible`，「那一格没有敌人」与「那一格看不见」
    // 在张量里是同一个 0。整套侦查博弈（佯攻诱饵、藏不死鸟、屏蔽集结区）
    // 建立在这两者的区别上——分不开的话 AI 会把所有迷雾格当成空地，
    // 于是永远学不到「先派斥候」。
    bool found = false;
    for (const rts::ObsChannelSpec& c : rts::kObsChannels) {
        if (c.name == "visible") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("城门与城墙是两条通道", "[obs]") {
    // 城门破坏速率高于城墙，是结构上的既定薄弱点。混在一条通道里 AI 就分不出
    // 「这里更好打」，而「发现并利用已有薄弱点」正是本项目的头号评估指标。
    bool wall = false;
    bool gate = false;
    for (const rts::ObsChannelSpec& c : rts::kObsChannels) {
        if (c.name == "wall_hp") wall = true;
        if (c.name == "gate_hp") gate = true;
    }
    REQUIRE(wall);
    REQUIRE(gate);
}

TEST_CASE("等级通道用 LevelSum 归一化，血量通道用比例", "[obs]") {
    // 决策 ⑧⑨⑩ 的可执行形式。任何**固定常数**做分母的等级或血量通道，
    // 在第 10 波与第 100 波之间输入尺度会漂移两个数量级，
    // 低波次学到的表示在高波次直接不可用——而「波数即课程」是我们
    // 省掉手工 curriculum 的唯一依据。
    for (const rts::ObsChannelSpec& c : rts::kObsChannels) {
        INFO("通道 " << c.name);
        if (c.name.find("_level") != std::string_view::npos) {
            REQUIRE(c.norm == rts::ObsNorm::LevelSum);
        }
        if (c.name.find("_hp") != std::string_view::npos) {
            REQUIRE(c.norm == rts::ObsNorm::Frac01);
        }
    }
}

TEST_CASE("方向场两个分量都是有符号的", "[obs]") {
    for (const rts::ObsChannelSpec& c : rts::kObsChannels) {
        if (c.name.substr(0, 5) == "flow_") {
            INFO("通道 " << c.name);
            REQUIRE(c.norm == rts::ObsNorm::Signed);
        }
    }
}

TEST_CASE("自身向量含兵种 one-hot，长度对得上", "[obs]") {
    // 共享策略网络靠 one-hot 区分兵种，所以这一块的长度必须等于花名册大小。
    REQUIRE(static_cast<int>(rts::ObsSelfField::LevelNorm) == rts::kUnitTypeCount);
    REQUIRE(rts::kObsSelfCount == rts::kUnitTypeCount + 3);
    REQUIRE(rts::kObsSelfNames[0] == "type_onehot");
}

TEST_CASE("全局标量只有两条，且都不依赖待标定的分母", "[obs]") {
    // 「少量全局标量」当真了。两条都是 CLAUDE.md 的硬要求（波数、存活空军数），
    // 而两条的编码都刻意避开了待定数值：
    //   * 波数取 log2(1 + wave)，不是 wave / 训练截断波数
    //   * 空军存**比例**（÷ 当前上限），不是绝对数
    // 这一条来自决策 ⑫ 那条被推广的规则：格式承诺不得依赖任何待定数值。
    REQUIRE(rts::kObsGlobalCount == 2);
    REQUIRE(rts::kObsGlobalNames[0] == "wave_log");
    REQUIRE(rts::kObsGlobalNames[1] == "aerial_alive_frac");
}

TEST_CASE("K 是奇数", "[obs]") {
    // 自身单位要落在视野正中，偶数边长做不到。
    // K 本身是占位值（要等射程标定），但奇数这条性质与取值无关。
    REQUIRE(rts::kObsK % 2 == 1);
    REQUIRE(rts::kObsK >= 3);
}

TEST_CASE("布局指纹稳定、非退化，且对重排敏感", "[obs]") {
    // 指纹存在的全部理由是「版本号会被忘记改」。所以要验的是它**真的会变**——
    // 一个恒定的指纹与没有指纹等价，而它看起来完全正常。
    REQUIRE(rts::kObsLayoutFingerprint == rts::obs_layout_fingerprint());
    REQUIRE(rts::kObsLayoutFingerprint != rts::StateHash::kOffsetBasis);
    REQUIRE(rts::kObsLayoutFingerprint != 0);

    // 手工折一份**交换了前两条通道**的指纹，必须与真的不同。
    // 直接改 kObsChannels 做不到（它是 constexpr），所以在这里重放一遍折叠过程，
    // 只把前两项对调。这同时把「分隔符不是装饰」那条钉住：
    // 少了分隔符，`{"ab","c"}` 与 `{"a","bc"}` 会折出同一个值。
    const auto fold = [](const std::vector<rts::ObsChannelSpec>& chans) {
        rts::StateHash h;
        for (const rts::ObsChannelSpec& c : chans) {
            h.feed_text(c.name);
            h.feed_text("\x1f");
            h.feed_text(rts::ident_of(c.norm));
            h.feed_text(c.paired ? "\x1fpaired\x1e" : "\x1fsingle\x1e");
        }
        return h.value();
    };

    std::vector<rts::ObsChannelSpec> as_is(rts::kObsChannels.begin(),
                                           rts::kObsChannels.end());
    std::vector<rts::ObsChannelSpec> swapped = as_is;
    std::swap(swapped[0], swapped[1]);
    REQUIRE(fold(as_is) != fold(swapped));
}

TEST_CASE("语义版本号存在且为正", "[obs]") {
    // 指纹管布局，版本号管语义（同一个名字的含义变了）。指纹抓不到后者，
    // 所以两个都要。这条只查它没被误设成 0——0 在 Python 侧常被当作「未设置」。
    REQUIRE(rts::kObsVersion >= 1);
}
