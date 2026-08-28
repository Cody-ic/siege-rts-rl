#include "rts/rng.hpp"

namespace rts {
namespace {

constexpr std::uint32_t rotl32(std::uint32_t x, unsigned k) noexcept {
    return static_cast<std::uint32_t>((x << k) | (x >> (32u - k)));
}

// splitmix64：只用于把 64 位种子铺开成 128 位状态。
// 直接把种子塞进状态是不行的——xoshiro 家族对"几乎全零"的初始状态需要相当多次迭代
// 才能摆脱低熵，用小整数当种子（比如 seed=1）时前若干个输出会明显不随机。
constexpr std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace

Rng::Rng(std::uint64_t seed) noexcept {
    std::uint64_t st = seed;
    for (int i = 0; i < 2; ++i) {
        const std::uint64_t v = splitmix64(st);
        s_[static_cast<std::size_t>(2 * i)] = static_cast<std::uint32_t>(v & 0xFFFFFFFFull);
        s_[static_cast<std::size_t>(2 * i + 1)] = static_cast<std::uint32_t>(v >> 32);
    }
    // 全零状态是 xoshiro 的不动点，会永远输出 0。splitmix64 实际上不会产生它，
    // 但这条兜底比"相信不会发生"便宜得多。
    if (s_[0] == 0u && s_[1] == 0u && s_[2] == 0u && s_[3] == 0u) {
        s_[0] = 1u;
    }
}

std::uint32_t Rng::next_u32() noexcept {
    const std::uint32_t result = rotl32(s_[1] * 5u, 7) * 9u;
    const std::uint32_t t = static_cast<std::uint32_t>(s_[1] << 9);
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl32(s_[3], 11);
    return result;
}

std::uint32_t Rng::below(std::uint32_t bound) noexcept {
    if (bound <= 1u) {
        return 0u;
    }
    // threshold = 2^32 mod bound。写成 (0u - bound) % bound 而不是 -bound % bound：
    // 后者会让 MSVC 报 C4146（对无符号量取一元负号），而 /WX 下那是错误。
    const std::uint32_t threshold = (0u - bound) % bound;
    for (;;) {
        const std::uint32_t r = next_u32();
        if (r >= threshold) {
            return r % bound;
        }
    }
}

float Rng::unit_float() noexcept {
    // 2^-24。float 只有 24 位有效位数，取更多位不会提高分辨率，只会引入舍入。
    constexpr float kScale = 1.0f / 16777216.0f;
    return static_cast<float>(next_u32() >> 8) * kScale;
}

}  // namespace rts
