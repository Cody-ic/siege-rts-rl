#ifndef RENDER_CHRONICLE_VIEW_HPP
#define RENDER_CHRONICLE_VIEW_HPP
#include <vector>
#include <string_view>
#include "raylib.h"
#include "render/text.hpp"
namespace render {
class ChronicleView {
public:
    bool open = false;
    void update(Vector2 viewport, int reached_wave);
    void draw(const FontSet& font, Vector2 viewport, int reached_wave);
    void preview(int chapter) noexcept;
    static std::vector<std::string_view> strings();
private:
    int chapter_ = 0;
    int ending_ = 0;
    float scroll_ = 0, max_scroll_ = 0;
};
}
#endif
