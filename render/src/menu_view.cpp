#include "render/menu_view.hpp"

#include <cstddef>
#include <string>

#include "rts/types.hpp"

namespace render {
namespace {

constexpr float kTitleSize = 44.0f;
constexpr float kSubSize = 22.0f;
constexpr float kItemSize = 28.0f;
constexpr float kFootSize = 18.0f;
constexpr float kHelpSize = 20.0f;
constexpr float kPad = 30.0f;
constexpr float kItemH = 46.0f;
constexpr float kHelpRowH = 30.0f;
constexpr float kHelpKeyCol = 190.0f;   // 按键栏宽度（两栏对齐靠它，不靠空格）

const Color kPanel{16, 17, 24, 232};
const Color kPanelEdge{92, 98, 124, 255};
const Color kTitle{240, 236, 220, 255};
const Color kSub{150, 156, 175, 255};
const Color kItem{214, 219, 234, 255};
const Color kItemOn{250, 250, 255, 255};
const Color kItemOff{104, 107, 120, 255};
const Color kBar{52, 62, 96, 240};
const Color kAccent{236, 190, 96, 255};

float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// 面板几何。**`draw*` 与 `hit_test*` 都只从这里取坐标**（见头文件那条约束）。
struct Rows {
    Rectangle panel{};
    float title_y = 0.0f;
    float sub_y = 0.0f;
    float body_y = 0.0f;    // 说明面板的两栏正文起点；菜单屏不用
    float items_y = 0.0f;
    float foot_y = 0.0f;
    float item_h = kItemH;
};

// 面板外框：给定内容高度，居中放置，并对**极小窗口**收敛。
//
// 极小窗口不是假想的：`render/CMakeLists.txt` 里有几条 ctest 用 `--size 64 64`
// （它们本来就该失败，但要失败在素材或字体上，不该先在这里算出一个负宽度矩形）。
Rectangle frame_of(Vector2 vp, float want_w, float max_w, float content_h,
                   MenuView::Align align) {
    // 下界 560 是量出来的，不是拍的：主菜单副标题「不对称波次生存 · 人类王国
    // vs 亡灵大军」在 22 px 下约 450 px 宽，再窄两边就贴着面板边了。
    // 面板不按内容自适应宽度是刻意的——那会让面板在切屏时忽宽忽窄。
    float w = clampf(want_w, 560.0f, max_w);
    if (w > vp.x - 16.0f) w = vp.x - 16.0f;
    if (w < 40.0f) w = 40.0f;
    float h = content_h;
    if (h > vp.y - 16.0f) h = vp.y - 16.0f;
    if (h < 40.0f) h = 40.0f;
    // 靠左时留出一段边距，但**不小于居中时的位置也不越界**：窗口很窄时
    // 靠左与居中会收敛到同一个位置，那是对的——没有空间可让。
    float x = (vp.x - w) * 0.5f;
    if (align == MenuView::Align::Left) {
        const float margin = vp.x * 0.07f;
        if (margin < x) x = margin;
    }
    return Rectangle{x, (vp.y - h) * 0.5f, w, h};
}

Rows menu_rows(int n, Vector2 vp, bool has_sub, bool has_foot, MenuView::Align align) {
    const float title_h = kTitleSize + 16.0f;
    const float sub_h = has_sub ? kSubSize + 18.0f : 0.0f;
    const float foot_h = has_foot ? kFootSize + 24.0f : 0.0f;
    const float items_h = static_cast<float>(n) * kItemH;
    Rows r;
    r.panel = frame_of(vp, vp.x * 0.46f, 720.0f,
                       kPad * 2.0f + title_h + sub_h + items_h + foot_h, align);
    float y = r.panel.y + kPad;
    r.title_y = y;
    y += title_h;
    r.sub_y = y;
    y += sub_h;
    r.items_y = y;
    y += items_h;
    r.foot_y = y + 6.0f;
    return r;
}

Rows help_rows(int entries, int n, Vector2 vp) {
    const float title_h = kTitleSize + 16.0f;
    const float body_h = static_cast<float>(entries) * kHelpRowH + 14.0f;
    const float items_h = static_cast<float>(n) * kItemH;
    const float foot_h = kFootSize + 24.0f;
    Rows r;
    // 说明面板比菜单宽、上限也更高：右栏是整句中文（最长那句 30 余字），
    // 挤在菜单那个 720 px 里右边会贴着框。**它的高度要把脚注算进去**——
    // 不算的话脚注落在下边距里，看起来像溢出了一行。
    r.panel = frame_of(vp, vp.x * 0.72f, 1040.0f,
                       kPad * 2.0f + title_h + body_h + items_h + foot_h,
                       MenuView::Align::Center);   // 说明屏永远居中
    float y = r.panel.y + kPad;
    r.title_y = y;
    y += title_h;
    r.body_y = y;
    y += body_h;
    r.items_y = y;
    y += items_h;
    r.foot_y = y + 6.0f;
    return r;
}

int hit_row(const Rows& r, int n, Vector2 mouse) {
    if (n <= 0) return -1;
    if (mouse.x < r.panel.x || mouse.x > r.panel.x + r.panel.width) return -1;
    const float dy = mouse.y - r.items_y;
    if (dy < 0.0f) return -1;
    const int k = static_cast<int>(dy / r.item_h);
    return k < n ? k : -1;
}

}  // namespace

void MenuView::dim(Vector2 viewport, unsigned char alpha) const {
    DrawRectangle(0, 0, static_cast<int>(viewport.x), static_cast<int>(viewport.y),
                  Color{8, 8, 12, alpha});
}

// 面板 + 标题 + 副标题 + 脚注的公共部分。条目由各自的 draw 画（说明屏在中间
// 多一段两栏正文），所以这里只画壳。
namespace {

void draw_shell(const FontSet& font, const Rows& r, std::string_view title,
                std::string_view subtitle, std::string_view footer) {
    DrawRectangleRec(r.panel, kPanel);
    DrawRectangleLinesEx(r.panel, 2.0f, kPanelEdge);
    // 标题下面一道细线：面板里有三段（标题 / 内容 / 脚注），没有分隔时
    // 它们的字号差不足以让人一眼看出层级。
    const auto center = [&](std::string_view s, float y, float size, Color c) {
        if (s.empty()) return;
        const std::string owned(s);
        const rts::Vec2 m = font.measure(owned, size);
        font.draw(owned, rts::Vec2{r.panel.x + (r.panel.width - m.x) * 0.5f, y}, size, c);
    };
    center(title, r.title_y, kTitleSize, kTitle);
    DrawRectangle(static_cast<int>(r.panel.x + kPad),
                  static_cast<int>(r.title_y + kTitleSize + 8.0f),
                  static_cast<int>(r.panel.width - kPad * 2.0f), 1,
                  Color{70, 74, 94, 255});
    center(subtitle, r.sub_y, kSubSize, kSub);
    center(footer, r.foot_y, kFootSize, Color{124, 128, 144, 255});
}

void draw_items(const FontSet& font, const Rows& r, const game::MenuModel& menu) {
    const std::vector<game::MenuItem>& items = menu.items();
    for (std::size_t k = 0; k < items.size(); ++k) {
        const float y = r.items_y + static_cast<float>(k) * r.item_h;
        const bool on = static_cast<int>(k) == menu.selected();
        if (on) {
            // 高亮条铺满面板宽度 + 左侧一道竖的强调色。
            // **选中态不靠颜色深浅**（那在投影仪上常常看不出来），
            // 靠「多了一条条与一根竖线」这个形状差异。
            DrawRectangle(static_cast<int>(r.panel.x + 6.0f), static_cast<int>(y + 3.0f),
                          static_cast<int>(r.panel.width - 12.0f),
                          static_cast<int>(r.item_h - 6.0f), kBar);
            DrawRectangle(static_cast<int>(r.panel.x + 6.0f), static_cast<int>(y + 3.0f),
                          4, static_cast<int>(r.item_h - 6.0f), kAccent);
        }
        const std::string label(items[k].label);
        const rts::Vec2 m = font.measure(label, kItemSize);
        const Color c = !items[k].enabled ? kItemOff : (on ? kItemOn : kItem);
        font.draw(label,
                  rts::Vec2{r.panel.x + (r.panel.width - m.x) * 0.5f,
                            y + (r.item_h - kItemSize) * 0.5f},
                  kItemSize, c);
    }
}

}  // namespace

void MenuView::draw(const game::MenuModel& menu, const Chrome& chrome,
                    Vector2 viewport) const {
    const Rows r = menu_rows(static_cast<int>(menu.items().size()), viewport,
                             !chrome.subtitle.empty(), !chrome.footer.empty(),
                             chrome.align);
    draw_shell(*font_, r, chrome.title, chrome.subtitle, chrome.footer);
    draw_items(*font_, r, menu);
}

int MenuView::hit_test(const game::MenuModel& menu, const Chrome& chrome, Vector2 mouse,
                       Vector2 viewport) const {
    // 与 draw 逐字同一份入参（这是 `Chrome` 存在的全部理由）：副标题或脚注
    // 在不在会把条目整体挪十几像素，而那足以让点击错行。
    const Rows r = menu_rows(static_cast<int>(menu.items().size()), viewport,
                             !chrome.subtitle.empty(), !chrome.footer.empty(),
                             chrome.align);
    return hit_row(r, static_cast<int>(menu.items().size()), mouse);
}

void MenuView::draw_help(const game::MenuModel& menu, const Chrome& chrome,
                         Vector2 viewport) const {
    const std::vector<game::HelpEntry>& entries = game::help_entries();
    const Rows r = help_rows(static_cast<int>(entries.size()),
                             static_cast<int>(menu.items().size()), viewport);
    draw_shell(*font_, r, chrome.title, /*subtitle=*/{}, chrome.footer);
    for (std::size_t k = 0; k < entries.size(); ++k) {
        const float y = r.body_y + static_cast<float>(k) * kHelpRowH;
        font_->draw(std::string(entries[k].keys), rts::Vec2{r.panel.x + kPad, y},
                    kHelpSize, kAccent);
        font_->draw(std::string(entries[k].what),
                    rts::Vec2{r.panel.x + kPad + kHelpKeyCol, y}, kHelpSize, kItem);
    }
    draw_items(*font_, r, menu);
}

int MenuView::hit_test_help(const game::MenuModel& menu, const Chrome& chrome,
                            Vector2 mouse, Vector2 viewport) const {
    // 说明屏的版面不看 chrome（标题与脚注的高度是定值），但签名与 draw_help
    // 保持一致：将来若给它加一行副标题，两处会一起改。
    (void)chrome;
    const Rows r = help_rows(static_cast<int>(game::help_entries().size()),
                             static_cast<int>(menu.items().size()), viewport);
    return hit_row(r, static_cast<int>(menu.items().size()), mouse);
}

}  // namespace render
