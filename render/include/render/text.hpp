// 中文文字渲染。**这个类存在的全部理由是：中文渲错的三种方式都不会自己报错。**
//
// raylib 的内置字体没有 CJK 字形，所以要自己载入一份。而载入这件事有三个坑，
// 全部实测过（spike 第三轮），**三个的共同点是「看起来在正常工作」**：
//
//   1. **码点在集合里、字体没有那个字** → 渲成**零宽的什么都没有**。
//      连豆腐块都没有，就是凭空少了一个字。
//   2. **字体有那个字、但码点没进集合** → 渲成 `?`（集合含 ASCII 时）或
//      **图集里的第一个字形**（不含时）。后者最坏：实测「镇野箭楼」渲成
//      「平平平平」——四个看着完全合理的汉字，肉眼都不一定看得出来。
//   3. **传了 `.ttc`（字体集合）** → `LoadFontEx` **静默退化成 raylib 内置字体**，
//      而且 `texture.id != 0`，所以「载入成功」这条判断为真。实测 simsun.ttc 与
//      msyh.ttc 都是这样：要 131 个码点，`glyphCount` 回来 224（内置字体的字形数）。
//
// 三个坑对应三道检查。第 1、3 个在构造期（第 3 个另有一道专门的检查，
// 因为「全部中文都缺字」这个报错说不出真正的原因是「你给的是字体集合」）；
// **第 2 个必须在画字时查**，因为它取决于传进来的串，构造期无从知道。
//
// 另有第四道检查，是上面第 1 道的豁免边界：空白类字符本来就没有位图，
// 于是被免掉了位图检查——而全角空格 U+3000 在 simhei 下**宽度也是 0**，
// 一路绿过之后各项在界面上糊成一片。所以豁免收紧成「可以没有墨，不能没有宽度」。
//
// 由此推出一条设计：**码点集合必须由「我们真的要显示哪些字」机械推导**，
// 不能手抄一个 Unicode 区间。输入是 `game::all_display_strings()`（枚举展示名，
// 按 count 遍历得出）加上本文件的 `ui_strings()`（界面字面量）。

#ifndef RENDER_TEXT_HPP
#define RENDER_TEXT_HPP

#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "raylib.h"
#include "rts/types.hpp"

namespace render {

// 字体装不上、缺字、或要画一个没登记的字符。
//
// 与 `AssetError` 分开是因为**处置不同**：素材缺了要重跑精灵流水线，
// 字体的问题是「换一个字体」或「把这个串登记进去」，两回事。
class FontError : public std::runtime_error {
public:
    explicit FontError(const std::string& what) : std::runtime_error(what) {}
};

class FontSet {
public:
    // `must_cover` 里出现的每一个字符都必须能显示，否则构造失败。
    // `base_size` 是烘焙字号；画得比它小可以（已开双线性过滤），画得大会糊。
    FontSet(const std::string& font_path, int base_size,
            const std::vector<std::string_view>& must_cover);
    ~FontSet();

    FontSet(const FontSet&) = delete;
    FontSet& operator=(const FontSet&) = delete;

    // 打开字体。`explicit_path` 非空时**只**试它（`--font` 给的，试别的等于无视用户）；
    // 为空时依次试候选清单，第一个通过全部检查的就用。
    //
    // 全都不行时抛，报错里列出**每一个试过的路径与它各自的失败原因**，并提示 `--font`。
    // 逐条列出是有必要的：失败原因不同（不存在 / 是 .ttc / 缺字），
    // 只说一句「找不到中文字体」会让人去装字体，而真正要做的可能是换个路径。
    static std::unique_ptr<FontSet> open(const std::string& explicit_path, int base_size,
                                        const std::vector<std::string_view>& must_cover);

    // 候选字体路径，按优先级。**只放纯 TTF**——见文件头第 3 个坑。
    static std::vector<std::string> font_candidates();

    // 画一行字。**串里出现任何未登记的字符就抛**，而不是渲一个 `?` 出来。
    //
    // 这是第 2 个坑的唯一出路：它取决于传进来的串，构造期查不到。
    // 抛在渲染循环里确实粗暴（这一帧画不完、`EndDrawing()` 不会被调用），
    // 但它只会在**第一帧**发生，而 `main()` 会把消息打全再退非零。
    // 相比之下静默渲成别的汉字可以一路混到答辩现场。
    void draw(const std::string& text, rts::Vec2 pos, float size, Color tint) const;

    // 一行字占多少像素。摆版面要用。同样会查未登记字符。
    rts::Vec2 measure(const std::string& text, float size) const;

    int base_size() const noexcept { return base_size_; }
    const std::string& path() const noexcept { return path_; }
    std::size_t codepoint_count() const noexcept { return loaded_.size(); }

private:
    // 逐码点核对集合成员。不在集合里就抛，消息里给出 U+XXXX、该字符本身、
    // 以及整个出问题的串——三样都要，因为「哪个字」和「哪一句」都得能定位。
    void verify(const std::string& text) const;

    // 字号对应的字间距。raylib 的 DrawTextEx 要显式给，给 0 会挤在一起。
    static float spacing_for(float size) noexcept { return size / 16.0f; }

    Font font_{};
    std::string path_;
    int base_size_ = 0;
    // 有序集合：报错时列出「已登记哪些码点」时有序输出好读，
    // 且与 SpriteAtlas 用 std::map 的理由相同（CLAUDE.md 对无序容器的口径）。
    std::set<int> loaded_;
};

// 界面上会出现的中文字面量。**在这里登记，否则画的时候会抛。**
//
// 这份清单与 `game::all_display_strings()` 一起构成字体的码点来源。
// 它是手写的（界面文案没法从枚举推导），所以漏登记是可能的——
// 兜底就是 `FontSet::draw()` 的那道检查：漏了会在第一帧红，不会静默渲错。
const std::vector<std::string_view>& ui_strings();

}  // namespace render

#endif  // RENDER_TEXT_HPP
