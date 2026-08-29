#include "rts/terrain.hpp"

#include <cassert>

#include "rts/hash.hpp"

namespace rts {

TerrainMasks::TerrainMasks(int width, int height, const Terrain* terrain,
                           const std::uint8_t* no_build)
    : width_(width), height_(height) {
    assert(width > 0 && height > 0);
    assert(terrain != nullptr);

    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    terrain_.assign(terrain, terrain + n);
    passable_.resize(n);
    buildable_.resize(n);
    blocks_vision_.resize(n);

    for (std::size_t k = 0; k < n; ++k) {
        const Terrain t = terrain_[k];
        passable_[k] = rts::is_passable(t) ? std::uint8_t{1} : std::uint8_t{0};
        blocks_vision_[k] = rts::blocks_vision(t) ? std::uint8_t{1} : std::uint8_t{0};
        // 4.2 的组合规则，**只在这一处实现**：
        //     格子可建造 ⇔ terrain.buildable AND NOT no_build
        const bool blocked = no_build != nullptr && no_build[k] != 0;
        buildable_[k] =
            (rts::is_buildable(t) && !blocked) ? std::uint8_t{1} : std::uint8_t{0};
    }
}

std::size_t TerrainMasks::idx(int x, int y) const noexcept {
    // 与 `MapData::terrain_at` 同一条纪律：越界是调用方的错，Debug 下拦住，
    // 不在 Release 下悄悄钳位——钳位会把一个坐标 bug 变成一个看起来正常的画面。
    assert(in_bounds(x, y));
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
           static_cast<std::size_t>(x);
}

std::uint64_t TerrainMasks::layout_hash() const noexcept {
    StateHash h;
    h.feed_text("TerrainMasks/1");
    h.feed_pod(width_);
    h.feed_pod(height_);
    // 三张表逐字节喂入。**顺序敏感**是 FNV-1a 被选中的唯一理由（`rts/hash.hpp`），
    // 所以 x/y 写反会让这个值变——那正是这个函数要抓的东西。
    h.feed(passable_.data(), passable_.size());
    h.feed(buildable_.data(), buildable_.size());
    h.feed(blocks_vision_.data(), blocks_vision_.size());
    return h.value();
}

}  // namespace rts
