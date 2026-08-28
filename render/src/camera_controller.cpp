#include "render/camera_controller.hpp"

#include <algorithm>

namespace render {

CameraController::CameraController() noexcept {
    cam_.zoom = 1.0f;
    cam_.rotation = 0.0f;
    cam_.offset = Vector2{0.0f, 0.0f};
    cam_.target = Vector2{0.0f, 0.0f};
}

void CameraController::set_viewport(Vector2 viewport) noexcept {
    cam_.offset = Vector2{viewport.x * 0.5f, viewport.y * 0.5f};
}

void CameraController::fit(const IsoProjection& proj, int w, int h, Vector2 viewport,
                           float top_margin_tiles) noexcept {
    const Rectangle b = proj.grid_bounds(w, h);
    const float margin = top_margin_tiles * static_cast<float>(proj.tile_h());

    // 需要装进画面的世界矩形：格心包围盒 + 上边距（精灵向上长，不向下）。
    const float need_w = b.width + static_cast<float>(proj.tile_w());
    const float need_h = b.height + static_cast<float>(proj.tile_h()) + margin;

    set_viewport(viewport);
    cam_.target = Vector2{b.x + b.width * 0.5f, b.y + b.height * 0.5f - margin * 0.5f};

    if (need_w <= 0.0f || need_h <= 0.0f) {
        cam_.zoom = 1.0f;
        return;
    }
    // 取两个方向里更严的那个，并留 4% 余量——正好贴边看着很挤，
    // 而且 grid_bounds 不含精灵宽度，边上的树会探出去一点。
    const float z = std::min(viewport.x / need_w, viewport.y / need_h) * 0.96f;
    cam_.zoom = std::clamp(z, min_zoom_, max_zoom_);
}

void CameraController::update(float dt) noexcept {
    // 平移：缩得越远，同样的按键要扫过更多世界像素，否则大图上慢到没法用。
    const float speed = pan_px_per_sec_ * dt / std::max(cam_.zoom, min_zoom_);
    Vector2 d{0.0f, 0.0f};
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) d.x += speed;
    if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) d.x -= speed;
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) d.y += speed;
    if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) d.y -= speed;
    cam_.target.x += d.x;
    cam_.target.y += d.y;

    // 中键拖拽。除以 zoom：拖 1 个屏幕像素应当对应 1/zoom 个世界像素，
    // 不除的话缩远之后拖起来像卡住。
    if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
        const Vector2 md = GetMouseDelta();
        cam_.target.x -= md.x / cam_.zoom;
        cam_.target.y -= md.y / cam_.zoom;
    }

    // 滚轮缩放，**以鼠标位置为锚**：缩放前后鼠标下的那个世界点不动。
    // 不做这一步的话缩放总是以屏幕中心为锚，想放大某个角落要一边缩一边平移。
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        const Vector2 mouse = GetMousePosition();
        const Vector2 before = GetScreenToWorld2D(mouse, cam_);
        cam_.zoom = std::clamp(cam_.zoom * (1.0f + wheel * 0.12f), min_zoom_, max_zoom_);
        const Vector2 after = GetScreenToWorld2D(mouse, cam_);
        cam_.target.x += before.x - after.x;
        cam_.target.y += before.y - after.y;
    }
}

}  // namespace render
