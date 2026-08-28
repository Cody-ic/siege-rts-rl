#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include "rts/rng.hpp"

// 这一组黄金向量来自**一份独立的 Python 实现**——照 xoshiro128** 与 splitmix64 的定义
// 另写一遍，而不是从这份 C++ 的输出反抄回来。区别是本质的：反抄只能测出"行为有没有
// 被改动"，独立实现才能测出"行为对不对"，从而抓住转写时的笔误（移位数写错、
// 状态更新顺序写反之类）。生成脚本的算法与常量都写在 rng.cpp 的注释里，可复算。

TEST_CASE("Rng 与独立实现逐位一致", "[rng][determinism]") {
    SECTION("seed = 12345") {
        rts::Rng r(12345);
        const rts::Rng::State expect_state{0xA9D111A0u, 0x22118258u, 0xF713F8EDu, 0x346EDCE5u};
        REQUIRE(r.state() == expect_state);

        const std::array<std::uint32_t, 8> expect{
            0x89F4BEFDu, 0x94E95A78u, 0x7A8293BCu, 0xF0F3CCF8u,
            0x4B9122D4u, 0x1E0A0912u, 0x56075C69u, 0xCD7786E3u};
        for (std::size_t k = 0; k < expect.size(); ++k) {
            REQUIRE(r.next_u32() == expect[k]);
        }
    }

    SECTION("seed = 0 —— 全零种子不得退化") {
        // xoshiro 的全零状态是不动点，会永远输出 0。种子 0 是最容易被随手用到的值，
        // 必须确认 splitmix64 铺开后状态不是全零。
        rts::Rng r(0);
        const std::array<std::uint32_t, 8> expect{
            0xDEC9045Du, 0x9A089D75u, 0xAB77D362u, 0xC3E16405u,
            0x5C95A8DAu, 0x60DEA056u, 0xC25A5140u, 0xA4290614u};
        for (std::size_t k = 0; k < expect.size(); ++k) {
            REQUIRE(r.next_u32() == expect[k]);
        }
    }
}

TEST_CASE("below 的取值序列与独立实现一致", "[rng][determinism]") {
    rts::Rng r(12345);
    const std::array<std::uint32_t, 12> expect{5u, 0u, 2u, 2u, 4u, 4u, 5u, 1u, 0u, 4u, 3u, 2u};
    for (std::size_t k = 0; k < expect.size(); ++k) {
        REQUIRE(r.below(6) == expect[k]);
    }
}

TEST_CASE("unit_float 取高 24 位，与黄金 u32 对应", "[rng]") {
    rts::Rng r(12345);
    // 0x89F4BEFD >> 8 == 0x89F4BE。乘 2^-24 再乘回来必须精确复原，
    // 因为 24 位整数在 float 里可精确表示。
    REQUIRE(static_cast<std::uint32_t>(r.unit_float() * 16777216.0f) == 0x89F4BEu);
    REQUIRE(static_cast<std::uint32_t>(r.unit_float() * 16777216.0f) == 0x94E95Au);
}

TEST_CASE("同种子两个实例给出相同序列", "[rng][determinism]") {
    rts::Rng a(0xDEADBEEFull);
    rts::Rng b(0xDEADBEEFull);
    for (int k = 0; k < 1000; ++k) {
        REQUIRE(a.next_u32() == b.next_u32());
    }
}

TEST_CASE("状态可保存与恢复", "[rng][determinism]") {
    // 回放依赖这条：存档点要能精确复原随机流，否则回放从中途接不上。
    rts::Rng r(7);
    for (int k = 0; k < 13; ++k) {
        (void)r.next_u32();
    }
    const rts::Rng::State snapshot = r.state();

    std::array<std::uint32_t, 16> first{};
    for (auto& v : first) {
        v = r.next_u32();
    }

    r.set_state(snapshot);
    for (auto expected : first) {
        REQUIRE(r.next_u32() == expected);
    }

    // 用状态直接构造也应等价
    rts::Rng from_state(snapshot);
    for (auto expected : first) {
        REQUIRE(from_state.next_u32() == expected);
    }
}

TEST_CASE("below 的边界与范围", "[rng]") {
    rts::Rng r(1);
    REQUIRE(r.below(0) == 0u);
    REQUIRE(r.below(1) == 0u);

    for (std::uint32_t bound : {2u, 3u, 7u, 40u, 9216u}) {
        for (int k = 0; k < 500; ++k) {
            REQUIRE(r.below(bound) < bound);
        }
    }
}

TEST_CASE("unit_float 落在 [0, 1)", "[rng]") {
    rts::Rng r(2);
    for (int k = 0; k < 10000; ++k) {
        const float v = r.unit_float();
        REQUIRE(v >= 0.0f);
        REQUIRE(v < 1.0f);
    }
}

TEST_CASE("below 无明显偏斜", "[rng]") {
    // 这条不是在检验统计质量（xoshiro 的质量远超本项目所需），而是在检验**去偏逻辑
    // 没写错**。直接取模会让小余数偏多，bound 与 2^32 不整除时偏差可观测。
    constexpr std::uint32_t kBound = 6;
    constexpr int kDraws = 120000;
    std::array<int, kBound> bucket{};
    rts::Rng r(99);
    for (int k = 0; k < kDraws; ++k) {
        ++bucket[r.below(kBound)];
    }
    constexpr int kExpected = kDraws / static_cast<int>(kBound);
    for (int count : bucket) {
        // ±5% 是很宽的界；写坏的取模会偏得多得多，而正常的随机波动远小于它。
        REQUIRE(count > kExpected * 95 / 100);
        REQUIRE(count < kExpected * 105 / 100);
    }
}
