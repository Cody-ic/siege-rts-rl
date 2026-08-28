// 状态哈希：确定性回放测试的比对基元。
//
// CLAUDE.md 把确定性回放列为核心测试形态——"存一串随机种子 + 指令流，重放必须逐 tick
// 状态一致"。要逐 tick 比较状态，就需要把整个世界压成一个可比较的标量，
// 否则每个 tick 都要 diff 全部数组，慢到没人愿意跑。
//
// 用 FNV-1a 64。选它不是因为它是最快或最强的哈希，而是因为它**顺序敏感且实现极短**：
//   * 顺序敏感是硬要求。若哈希对喂入顺序不敏感，它就查不出本项目最要防的那一类 bug
//     ——容器遍历顺序不稳定导致的仿真偏离。那种 bug 下集合内容相同、顺序不同，
//     顺序不敏感的哈希会给出"一致"的假绿灯。
//   * 实现短意味着它自己不会成为 bug 来源，且跨编译器逐位一致（纯整数运算）。

#ifndef RTS_HASH_HPP
#define RTS_HASH_HPP

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace rts {

class StateHash {
public:
    static constexpr std::uint64_t kOffsetBasis = 0xCBF29CE484222325ull;
    static constexpr std::uint64_t kPrime = 0x100000001B3ull;

    void feed(const void* data, std::size_t n) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t k = 0; k < n; ++k) {
            h_ ^= static_cast<std::uint64_t>(p[k]);
            h_ *= kPrime;
        }
    }

    // 按字节喂入一个平凡类型。
    //
    // 这里的 static_assert 拦的是一个会随机失败、且极难定位的 bug：**含填充字节的结构体
    // 不能按字节哈希**。填充字节的值是不确定的（未初始化、或被前一个对象的残留占据），
    // 于是两个"值完全相同"的对象会算出不同的哈希，回放测试变成偶发失败。
    // has_unique_object_representations 恰好就是"值相同 ⇒ 字节相同"这个性质。
    template <class T>
    void feed_pod(const T& v) noexcept {
        static_assert(std::has_unique_object_representations_v<T>,
                      "该类型含填充字节或存在多种字节表示，不能按字节哈希——"
                      "会让同一状态算出不同哈希，回放测试将偶发失败。"
                      "请逐字段 feed，浮点用 feed_f32 / feed_f64。");
        feed(&v, sizeof(T));
    }

    // 浮点按**位**喂入。
    //
    // 浮点不满足上面那个 trait（+0.0 与 -0.0 值相等但字节不同，NaN 有多种编码），
    // 所以必须显式走这条路径，并接受其语义：位不同即视为状态不同。
    // 对回放比对而言这正是想要的——我们要查的就是"位级是否一致"。
    void feed_f32(float v) noexcept {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        feed_pod(bits);
    }

    void feed_f64(double v) noexcept {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        feed_pod(bits);
    }

    std::uint64_t value() const noexcept { return h_; }

    void reset() noexcept { h_ = kOffsetBasis; }

private:
    std::uint64_t h_ = kOffsetBasis;
};

}  // namespace rts

#endif  // RTS_HASH_HPP
