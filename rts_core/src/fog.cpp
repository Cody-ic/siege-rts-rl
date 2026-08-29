#include "rts/fog.hpp"

#include <cassert>

namespace rts {

FogLayer::FogLayer(int width, int height) : width_(width), height_(height) {
    assert(width > 0 && height > 0);
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    vis_.assign(n, Vis::Unseen);
    last_seen_.assign(n, kNeverSeen);
    // 未见过时这两张表的内容不该被读到（`remembered_bld` 先查 has_bld_），
    // 但仍显式初始化：它们要进 `state_hash`，而未初始化的字节会让同一个开局
    // 算出不同的哈希——回放测试于是变成偶发失败，最难查的一种。
    bld_.assign(n, BldType::Wall);
    bld_hp_permille_.assign(n, 0);
    has_bld_.assign(n, 0);
}

std::size_t FogLayer::idx(int x, int y) const noexcept {
    assert(in_bounds(x, y));
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
           static_cast<std::size_t>(x);
}

bool FogLayer::remembered_bld(int x, int y, RememberedBld* out) const noexcept {
    const std::size_t k = idx(x, y);
    if (has_bld_[k] == 0) return false;
    if (out != nullptr) {
        out->type = bld_[k];
        out->hp_permille = bld_hp_permille_[k];
    }
    return true;
}

void FogLayer::begin_tick() noexcept {
    for (Vis& v : vis_) {
        if (v == Vis::Visible) v = Vis::Remembered;
    }
}

void FogLayer::mark_visible(int x, int y, Tick now) noexcept {
    const std::size_t k = idx(x, y);
    vis_[k] = Vis::Visible;
    last_seen_[k] = now;
}

void FogLayer::remember_bld(int x, int y, BldType type,
                            std::uint16_t hp_permille) noexcept {
    const std::size_t k = idx(x, y);
    // 记忆只能来自看见。抄 ground truth 进记忆是硬要求第 1 条禁的事，
    // 而它不会有任何东西报错——所以这里必须是一条断言。
    assert(vis_[k] == Vis::Visible);
    assert(hp_permille <= kFullPermille);
    has_bld_[k] = 1;
    bld_[k] = type;
    bld_hp_permille_[k] = hp_permille;
}

void FogLayer::remember_no_bld(int x, int y) noexcept {
    const std::size_t k = idx(x, y);
    assert(vis_[k] == Vis::Visible);
    has_bld_[k] = 0;
    // 清掉残留，理由同构造函数：它们进哈希。
    bld_[k] = BldType::Wall;
    bld_hp_permille_[k] = 0;
}

const std::uint8_t* FogLayer::vis_bytes() const noexcept {
    // `Vis` 的底层类型是 uint8_t，两者大小与对齐相同，所以这一步是重新解释
    // **同一批字节**，不是绕过类型系统。给出的是打包用的连续视图，只读。
    static_assert(sizeof(Vis) == sizeof(std::uint8_t));
    return reinterpret_cast<const std::uint8_t*>(vis_.data());
}

void FogLayer::feed_hash(StateHash& h) const noexcept {
    h.feed_text("FogLayer/1");
    h.feed_pod(width_);
    h.feed_pod(height_);
    h.feed(vis_.data(), vis_.size() * sizeof(Vis));
    h.feed(last_seen_.data(), last_seen_.size() * sizeof(Tick));
    // has_bld_ 必须进哈希，且必须在血量**之前**：「没有建筑」与「有一段血量 0 的建筑」
    // 的其余字节完全相同，只差这一张表。漏掉它，缺口与未探索就再次撞在一起。
    h.feed(has_bld_.data(), has_bld_.size());
    h.feed(bld_.data(), bld_.size() * sizeof(BldType));
    h.feed(bld_hp_permille_.data(), bld_hp_permille_.size() * sizeof(std::uint16_t));
}

}  // namespace rts
