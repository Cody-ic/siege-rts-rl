// 菜单与操作说明的**像素那一半**：面板画在哪、条目多高、鼠标落在第几项。
//
// 与 `game::MenuModel` 的分工见那个头。这里只补一条**必须写下来的约束**：
//
// > **画与拾取必须共用同一份版面。** 两处各算一遍的症状是「看起来点在这一项上、
// > 实际选中的是上一项」，而它随窗口尺寸变化而变——最难复现的那类交互 bug。
//
// 所以本文件里 `draw*` 与 `hit_test*` 成对出现，各自都从同一个私有的版面函数
// 取几何，谁都不自己算 y 坐标。改版面只改那一个函数。
//
// 面板的样式刻意保守（半透明深色底 + 一道边 + 选中项一条高亮条），没有渐变、
// 没有动画：`render/` 的预算是 5–8 人日的**正式前端**，而那点预算要花在
// 「看得懂、点得中」上，不是花在过场效果上。

#ifndef RENDER_MENU_VIEW_HPP
#define RENDER_MENU_VIEW_HPP

#include <cstdint>
#include <string_view>

#include "game/menu_model.hpp"
#include "raylib.h"
#include "render/text.hpp"

namespace render {

class MenuView {
public:
    // 一屏的固定文案。**做成一个结构体而不是三个参数**，是为了让
    // `draw` 与 `hit_test` 收下的是同一个东西：副标题在不在会改变条目的 y 坐标，
    // 而两处各传一次就有可能只改了一处——那正好是头文件那条约束要防的错。
    // 面板横向落在哪儿。**主菜单靠左、其余居中**：主菜单背后那张地图是居中
    // 摆的（相机让整张图入画），面板也居中就正好把最值得看的城墙一带遮住。
    // 暂停与败局屏相反——那时玩家的注意力就该在面板上。
    enum class Align : std::uint8_t { Center, Left };

    struct Chrome {
        std::string_view title;
        std::string_view subtitle;   // 空串则不占位
        std::string_view footer;     // 空串则不占位
        Align align = Align::Center;
    };

    explicit MenuView(const FontSet& font) noexcept : font_(&font) {}

    // 全屏压暗。菜单叠在战场之上时先调它——不压暗的话深色面板与地图的
    // 对比度不够，而这层灰同时告诉玩家「后面那个世界现在停着」。
    void dim(Vector2 viewport, unsigned char alpha) const;

    // 一屏菜单。**必须在 `BeginMode2D` 之外调用**：它是屏幕空间的东西，
    // 不随相机缩放。
    void draw(const game::MenuModel& menu, const Chrome& chrome,
              Vector2 viewport) const;

    // 鼠标压在第几项上（-1 = 不在任何项上）。与 `draw` 同一份版面。
    int hit_test(const game::MenuModel& menu, const Chrome& chrome, Vector2 mouse,
                 Vector2 viewport) const;

    // 操作说明：两栏（按键 / 说明）+ 底部的「返回」项。内容取自
    // `game::help_entries()`，本层一个字都不自己写——那是「按键绑定与说明
    // 不可能对不上」的唯一实现方式。`chrome.subtitle` 在这一屏不用。
    void draw_help(const game::MenuModel& menu, const Chrome& chrome,
                   Vector2 viewport) const;

    int hit_test_help(const game::MenuModel& menu, const Chrome& chrome, Vector2 mouse,
                      Vector2 viewport) const;

    void scroll_help(float pixels) const noexcept;
private:
    mutable float help_scroll_ = 0, help_max_scroll_ = 0;
    const FontSet* font_;
};

}  // namespace render

#endif  // RENDER_MENU_VIEW_HPP
