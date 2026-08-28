#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "rts/hash.hpp"
#include "rts/types.hpp"

// 期望值同样来自独立的 Python 实现（FNV-1a 64 的标准常量与递推）。

TEST_CASE("FNV-1a 64 与标准值一致", "[hash]") {
    SECTION("空输入等于 offset basis") {
        rts::StateHash h;
        REQUIRE(h.value() == 0xCBF29CE484222325ull);
    }

    SECTION("\"abc\"") {
        rts::StateHash h;
        h.feed("abc", 3);
        REQUIRE(h.value() == 0xE71FA2190541574Bull);
    }

    SECTION("分段喂入与整段等价") {
        rts::StateHash whole;
        whole.feed("abc", 3);

        rts::StateHash split;
        split.feed("ab", 2);
        split.feed("c", 1);

        REQUIRE(split.value() == whole.value());
    }
}

TEST_CASE("哈希对顺序敏感", "[hash][determinism]") {
    // 这是选 FNV-1a 的**理由本身**，不是附带性质。本项目最要防的一类 bug 是
    // 容器遍历顺序不稳定导致仿真偏离；那种情形下集合内容相同、只有顺序不同，
    // 顺序不敏感的哈希会给出"一致"的假绿灯，回放测试就白做了。
    rts::StateHash ab;
    ab.feed_pod(std::uint32_t{1});
    ab.feed_pod(std::uint32_t{2});

    rts::StateHash ba;
    ba.feed_pod(std::uint32_t{2});
    ba.feed_pod(std::uint32_t{1});

    REQUIRE(ab.value() != ba.value());
}

TEST_CASE("reset 回到初始状态", "[hash]") {
    rts::StateHash h;
    h.feed("perturb", 7);
    REQUIRE(h.value() != rts::StateHash::kOffsetBasis);
    h.reset();
    REQUIRE(h.value() == rts::StateHash::kOffsetBasis);
}

TEST_CASE("浮点按位喂入", "[hash][determinism]") {
    SECTION("+0.0 与 -0.0 视为不同") {
        // 这是 feed_f32 的既定语义，写成测试是为了让它成为契约而不是意外：
        // 回放比对要查的就是位级一致，值相等但位不同必须体现出来。
        rts::StateHash pos;
        pos.feed_f32(0.0f);
        rts::StateHash neg;
        neg.feed_f32(-0.0f);
        REQUIRE(pos.value() != neg.value());
    }

    SECTION("相同的值给出相同的哈希") {
        rts::StateHash a;
        a.feed_f32(1.5f);
        rts::StateHash b;
        b.feed_f32(1.5f);
        REQUIRE(a.value() == b.value());
    }
}

TEST_CASE("句柄可直接按字节喂入", "[hash]") {
    // UnitId 只含一个 uint32、无填充字节，因此 feed_pod 的 static_assert 能通过。
    // 若日后有人往句柄里加字段导致出现填充，这里会在编译期失败——那正是想要的。
    const auto a = rts::UnitId::make(3, 1);
    const auto b = rts::UnitId::make(3, 2);

    rts::StateHash ha;
    ha.feed_pod(a);
    rts::StateHash hb;
    hb.feed_pod(b);

    REQUIRE(ha.value() != hb.value());
}
