// 相机：平移与缩放。
//
// 对局开场聚焦堡垒；全图视角按地图尺寸推导缩放。两者都随窗口尺寸适配。

#ifndef RENDER_CAMERA_CONTROLLER_HPP
#define RENDER_CAMERA_CONTROLLER_HPP

#include "raylib.h"
#include "game/iso_projection.hpp"

namespace render {

class CameraController {
public:
    CameraController() noexcept;

    // 让 w×h 的地图整张入画。`viewport` 是窗口尺寸，`top_margin_tiles` 是给
    // 高个子精灵留的上边距（以格边长为单位）——`grid_bounds` 只是格心的包围盒，
    // 而堡垒能高到 4 格边长，不留边距的话它的塔顶会被裁掉。
    void fit(const game::IsoProjection& proj, int w, int h, Vector2 viewport,
             float top_margin_tiles = 4.0f) noexcept;

    void focus_keep(const game::IsoProjection& proj, rts::GridPos keep,
                    Vector2 viewport) noexcept;

    // 每帧调一次。读键鼠：方向键 / WASD 平移，滚轮缩放，中键拖拽。
    void update(float dt) noexcept;

    // 窗口尺寸变了要跟着改，否则缩放中心会偏。
    void set_viewport(Vector2 viewport) noexcept;

    const Camera2D& camera() const noexcept { return cam_; }

private:
    Camera2D cam_{};
    // 平移速度按**世界像素每秒**给，并随缩放反比调整：缩得越远，同样的按键
    // 应当扫过更多格子，否则在大图上平移慢到没法用。
    float pan_px_per_sec_ = 1200.0f;
    float min_zoom_ = 0.02f;
    float max_zoom_ = 2.0f;
};

}  // namespace render

#endif  // RENDER_CAMERA_CONTROLLER_HPP
