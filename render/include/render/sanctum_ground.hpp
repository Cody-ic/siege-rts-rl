#ifndef RENDER_SANCTUM_GROUND_HPP
#define RENDER_SANCTUM_GROUND_HPP
#include <array>
#include <vector>
#include "game/scene_model.hpp"
#include "game/iso_projection.hpp"
#include "raylib.h"
namespace render {
// Decorative surfaces only. Paths never grant movement, vision or buildability.
class SanctumGround {
public:
    ~SanctumGround();
    SanctumGround() = default;
    SanctumGround(const SanctumGround&) = delete;
    SanctumGround& operator=(const SanctumGround&) = delete;
    void prepare(const std::vector<game::DrawItem>& tiles,
                 const std::vector<game::DrawItem>& objects);
    bool draw(const game::DrawItem& tile, const game::IsoProjection& projection) const;
private:
    struct Cell { unsigned char surface=0; std::array<Color,4> colors{}; };
    int width_=0,height_=0;
    std::vector<Cell> cells_;
    Texture2D grass_{},stone_{};
};
}
#endif
