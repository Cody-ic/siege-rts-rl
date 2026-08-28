// 仿真用的伪随机数发生器。
//
// CLAUDE.md：所有随机性走一个可播种的 PRNG，禁止 C 库的全局随机函数。
// 本文件是那条规定的唯一合法出口——仿真里其他地方不得再引入随机源。
//
// 为什么自己写而不用 <random>：
//   引擎本身（如 mt19937）的输出是标准规定的，但**分布适配器的实现是各标准库自定的**。
//   同一个引擎、同一个种子，MSVC 与 libstdc++ 的整数/实数分布会给出不同的数列。
//   本项目本来只要求同平台可复现，但手写三十行就能换来跨标准库一致的随机流，
//   而这让"训练时在服务器上观察到的行为"与"本地复现"之间少一层疑点，白拿。
//
// 算法是 xoshiro128**（Blackman & Vigna，公有领域）：状态 16 字节、易于序列化，
// 统计质量对本项目远超所需。**回放需要保存与恢复状态**，故 state() / set_state() 是
// 接口的一部分，不是调试便利。

#ifndef RTS_RNG_HPP
#define RTS_RNG_HPP

#include <array>
#include <cstdint>

namespace rts {

class Rng {
public:
    using State = std::array<std::uint32_t, 4>;

    // 从 64 位种子播种（内部经 splitmix64 铺开成 128 位状态）。
    explicit Rng(std::uint64_t seed) noexcept;

    // 从既有状态恢复，用于回放与 BatchedEnv 里给每局单独发种。
    explicit Rng(State s) noexcept : s_(s) {}

    std::uint32_t next_u32() noexcept;

    // 均匀取 [0, bound)。bound <= 1 时返回 0。
    // 用拒绝采样去偏，而不是直接取模——直接取模会让小的余数出现得更频繁，
    // 在"选择攻击目标""挑集结点"这类小 bound 的场合偏差可观测。
    std::uint32_t below(std::uint32_t bound) noexcept;

    // 均匀取 [0, 1)。取高 24 位喂给 float 的 24 位有效位数，故每个可表示值等概率。
    float unit_float() noexcept;

    State state() const noexcept { return s_; }
    void set_state(State s) noexcept { s_ = s; }

private:
    State s_{};
};

}  // namespace rts

#endif  // RTS_RNG_HPP
