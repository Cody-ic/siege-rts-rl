// 相机：平移与缩放。
//
// 缩放初值刻意**不写死一个数**，而是由「让整张地图入画」算出来——地图尺寸待标定
// （`地图与场景设计.md` 第 3 节只锁了不变量、没锁数字），写死的 zoom 换一张图就不对。
// 这与 CLAUDE.md「关于数值」是同一条：能推导的就不要拍。

#ifndef RENDER_CAMERA_CONTROLLER_HPP
#define RENDER_CAMERA_CONTROLLER_HPP

#include "raylib.h"
#include "render/iso_projection.hpp"

namespace render {

class CameraController {
public:
    CameraController() noexcept;

    // 让 w×h 的地图整张入画。`viewport` 是窗口尺寸，`top_margin_tiles` 是给
    // 高个子精灵留的上边距（以格边长为单位）——`grid_bounds` 只是格心的包围盒，
    // 而堡垒能高到 4 格边长，不留边距的话它的塔顶会被裁掉。
    void fit(const IsoProjection& proj, int w, int h, Vector2 viewport,
             float top_margin_tiles = 4.0f) noexcept;

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
