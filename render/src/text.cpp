#include "render/text.hpp"

#include <cstdio>
#include <vector>

#include "rts/utf8_path.hpp"

namespace render {
namespace {

// 空白类码点：它们**本来就没有位图**，所以不能按「缺字」判。
//
// 但这条豁免曾经开得太宽，被实测打了脸：全角空格 U+3000 在 simhei 里经 raylib
// 载入后 **advance 为 0**，于是它作为分隔符完全不占位——界面上
// 「地砖 35　深度序列 10」渲成「地砖 35深度序列 10」，几项糊成一片。
// 而位图检查恰好豁免了空白类字符，所以它一路绿过。
//
// 所以豁免收紧成「可以没有墨，但**不能没有宽度**」：见构造函数第四步。
// U+3000 因此不再被使用（分隔符改成 ASCII 空格），但它仍列在这里——
// 豁免名单要覆盖「所有可能作为空白出现的字符」，而不是「当前用到的」。
bool is_blank_codepoint(int cp) noexcept {
    return cp == 0x20 || cp == 0x3000;
}

// 把一个码点写成 "U+4E2D(中)" 这样的形式，报错时用。
std::string describe(int cp) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "U+%04X", cp);
    std::string out = buf;
    // raylib 负责码点 → UTF-8；缺字时它也能给出字节，所以这一步总是可行的。
    int n = 0;
    const char* utf8 = CodepointToUTF8(cp, &n);
    if (n > 0) {
        out += "(";
        out.append(utf8, static_cast<std::size_t>(n));
        out += ")";
    }
    return out;
}

}  // namespace

const std::vector<std::string_view>& ui_strings() {
    // **只放 `render/` 自己画的文案。** 中文展示名与光标信息条的字面量在
    // `game::all_display_strings()`，那边与产出它们的代码在同一个文件里
    // ——登记表离产出点越近，越不容易漏。
    //
    // 加新文案时必须加到这里，否则画的时候会抛。漏了不会静默渲错，这是刻意的。
    static const std::vector<std::string_view> kStrings = {
        // 操作提示
        "方向键平移", "滚轮缩放", "中键拖拽", "重新入画", "空格暂停",
        // 左上角的场景信息
        "地图", "尺寸", "地砖", "深度序列", "码点",
        // 演示对局的 HUD（run_battle）
        "波", "已暂停", "守方", "攻方", "石", "木", "金",
        // 波次循环的阶段与败局（draw_battle_hud 的 phase 那一段）
        "建造", "进攻中", "堡垒陷落·败",
        // 交互层（对局里的按键提示行）
        "编队", "右键下令", "建造模式", "换型", "左键放置", "右键退出",
        "召唤下一波", "Esc 菜单",
        // 主菜单 / 暂停 / 说明 / 败局这几屏的**壳**（标题、副标题、脚注）。
        //
        // **菜单条目与操作说明不在这里**——它们在 `game::all_menu_strings()`，
        // 由那边机械地遍历各屏的 builder 得出（登记表离产出点越近越不容易漏，
        // 同本函数开头那条）。这里只放 `render/` 自己拼的那几句。
        "不对称波次生存 · 人类王国 vs 亡灵大军",
        "方向键选择   回车确认", "Esc 继续对局", "Esc 返回",
        "操作说明", "堡垒陷落", "存活",
        // 乘号出现在**拼接出来**的串里（「7×5」），而拼接结果不在这份清单上，
        // 所以要单独登记它的码点。
        //
        // 分隔符用的是 ASCII 空格而不是全角空格：**实测 U+3000 在 simhei 下
        // 宽度为 0**，作为分隔符完全不占位。见本文件顶部 is_blank_codepoint 的注释。
        "×",
    };
    return kStrings;
}

std::vector<std::string> FontSet::font_candidates() {
    // Windows 自带的两个**纯 TTF** 中文字体，两者都实测通过。
    // 按优先级：黑体笔画均匀、小字号下比等线清楚一些。
    //
    // 刻意**不放 .ttc**（msyh.ttc 微软雅黑、simsun.ttc 宋体）——见文件头第 3 个坑，
    // 它们会让 raylib 静默退化成内置字体。
    //
    // 也刻意不放 Linux 路径：`render/` 从不在训练服务器上构建（CLAUDE.md:163），
    // 而 Linux 上常见的 CJK 字体（Noto CJK、文泉驿）**恰好都是 .ttc**，
    // 放上去等于放一条注定失败的路径。那边用 `--font` 显式给一个 TTF。
    return {
        "C:/Windows/Fonts/simhei.ttf",   // 黑体
        "C:/Windows/Fonts/Deng.ttf",     // 等线（Windows 10 起自带）
    };
}

std::unique_ptr<FontSet> FontSet::open(const std::string& explicit_path, int base_size,
                                       const std::vector<std::string_view>& must_cover) {
    std::vector<std::string> paths;
    if (!explicit_path.empty()) {
        // `--font` 给了就只试它。再去试候选等于无视用户的指定，
        // 而那会让「我明明指了字体，怎么用的还是别的」变成一个查不出来的问题。
        paths.push_back(explicit_path);
    } else {
        paths = font_candidates();
    }

    std::string log;
    for (const std::string& p : paths) {
        try {
            return std::make_unique<FontSet>(p, base_size, must_cover);
        } catch (const FontError& e) {
            log += "\n  - " + p + "\n      " + e.what();
        }
    }
    throw FontError(
        std::string("装不上中文字体，试过 ") + std::to_string(paths.size()) + " 个：" + log +
        "\n    用 --font <路径> 显式指定一个**纯 TTF** 的中文字体。"
        "\n    .ttc（字体集合，如 msyh.ttc / simsun.ttc）不行——raylib 会静默退化成内置字体。");
}

FontSet::FontSet(const std::string& font_path, int base_size,
                 const std::vector<std::string_view>& must_cover)
    : path_(font_path), base_size_(base_size) {
    // ——第一步：机械地推出码点集合——
    // ASCII 可打印区间无条件全收：数字、括号、逗号都来自运行时拼出来的串
    // （坐标、计数），没法从 must_cover 里推。95 个字形的代价可以忽略。
    for (int c = 0x20; c <= 0x7E; ++c) loaded_.insert(c);

    for (std::string_view s : must_cover) {
        const std::string owned(s);   // LoadCodepoints 要 NUL 结尾
        int n = 0;
        int* cps = LoadCodepoints(owned.c_str(), &n);
        for (int i = 0; i < n; ++i) loaded_.insert(cps[i]);
        UnloadCodepoints(cps);
    }

    std::vector<int> wanted(loaded_.begin(), loaded_.end());
    if (wanted.empty()) throw FontError("码点集合是空的，没什么可载入");

    // **不用 `LoadFontEx(path, ...)`。** raylib 的文件读取走窄 `fopen`，
    // 路径含非 ASCII 字符时会失败（见 rts/utf8_path.hpp）。而字体路径**很可能**
    // 含中文——`--font C:/字体/我的字体.ttf` 是完全正常的用法。
    // 所以自己读字节，再交给内存版 `LoadFontFromMemory`。
    //
    // 后缀写死 `.ttf`：raylib 只用它选解析器，而 `.ttc` 走同一个解析器
    // （也正是它会退回内置字体的原因，第二步查的就是这个）。
    bool read_ok = false;
    const std::vector<unsigned char> bytes = rts::read_file_bytes(font_path, &read_ok);
    if (!read_ok) throw FontError("打不开这个文件");
    if (bytes.empty()) throw FontError("这个文件是空的");

    font_ = LoadFontFromMemory(".ttf", bytes.data(), static_cast<int>(bytes.size()),
                               base_size_, wanted.data(),
                               static_cast<int>(wanted.size()));
    if (font_.texture.id == 0) {
        throw FontError("不是可用的字体文件");
    }

    // ——第二步：字形数必须**正好**等于要的个数——
    // 这一条专门抓 .ttc：raylib 装不上时会回退到内置字体，而内置字体有 224 个字形。
    // 不查它的话后面每一个中文码点都会各报一次「字体没有这个字」，
    // 报错刷满屏幕却没说出真正的原因（「你给的是字体集合」）。
    if (font_.glyphCount != static_cast<int>(wanted.size())) {
        const int got = font_.glyphCount;
        UnloadFont(font_);
        font_ = Font{};
        throw FontError("要了 " + std::to_string(wanted.size()) + " 个字形，回来 " +
                        std::to_string(got) +
                        " 个——raylib 没能解析这个文件、退回内置字体了。"
                        "最常见的原因是它是 .ttc（字体集合）而不是 .ttf。");
    }

    // ——第三步：逐码点核对字体真的有那个字——
    // 缺字的表现是**零宽、零 advance 的空白**，画面上就是凭空少一个字，
    // 所以必须在这里查掉，不能等看图。
    std::string missing;
    int missing_count = 0;
    for (int cp : wanted) {
        const GlyphInfo& g = font_.glyphs[GetGlyphIndex(font_, cp)];
        const bool absent = (g.value != cp);
        const bool blank = (g.image.width <= 0 || g.image.height <= 0);
        if (absent || (blank && !is_blank_codepoint(cp))) {
            ++missing_count;
            if (missing_count <= 12) missing += " " + describe(cp);
        }
    }
    if (missing_count > 0) {
        UnloadFont(font_);
        font_ = Font{};
        throw FontError("这个字体缺 " + std::to_string(missing_count) + " 个要用到的字符：" +
                        missing + (missing_count > 12 ? " …" : ""));
    }

    // ——第四步：空白类字符必须真的占宽度——
    // 第三步豁免了它们的位图检查（空白没有墨是正常的），而这一步是那条豁免的边界：
    // **没有墨可以，没有宽度不行**。一个宽度为 0 的空格作为分隔符等于不存在，
    // 症状是界面上几项糊成一片——而那看起来像排版没写好，不像字体的问题。
    // 实测 U+3000 在 simhei 下正是这样（advance 为 0），所以这条不是假想的。
    for (int cp : wanted) {
        if (!is_blank_codepoint(cp)) continue;
        int n = 0;
        const char* utf8 = CodepointToUTF8(cp, &n);
        const std::string one(utf8, static_cast<std::size_t>(n > 0 ? n : 0));
        const float w =
            MeasureTextEx(font_, one.c_str(), static_cast<float>(base_size_), 0.0f).x;
        if (w <= 0.0f) {
            UnloadFont(font_);
            font_ = Font{};
            throw FontError("空白字符 " + describe(cp) +
                            " 在这个字体里宽度为 0，不能用作分隔符。"
                            "\n    改用 ASCII 空格，或换一个字体。");
        }
    }

    // 双线性过滤。烘焙字号只有一个，而界面上会用比它小的字号；
    // 默认的点采样在缩小时会把笔画采断，中文笔画密，比拉丁字母明显得多。
    SetTextureFilter(font_.texture, TEXTURE_FILTER_BILINEAR);
}

FontSet::~FontSet() {
    if (font_.texture.id != 0) UnloadFont(font_);
}

void FontSet::verify(const std::string& text) const {
    const char* p = text.c_str();
    while (*p != '\0') {
        int size = 0;
        const int cp = GetCodepointNext(p, &size);
        if (loaded_.find(cp) == loaded_.end()) {
            throw FontError(
                "要画一个没登记的字符 " + describe(cp) + "，整句是「" + text + "」。" +
                "\n    登记处：中文展示名在 game/display_names.cpp，"
                "界面文案在 render/src/text.cpp 的 ui_strings()。"
                "\n    为什么不能不管：没登记的字符会被渲成 `?`，或（集合不含 ASCII 时）"
                "渲成图集里的第一个字形——实测「镇野箭楼」渲成「平平平平」，看不出错。");
        }
        p += (size > 0) ? size : 1;   // size<=0 不该发生，但别在这里死循环
    }
}

void FontSet::draw(const std::string& text, rts::Vec2 pos, float size, Color tint) const {
    verify(text);
    DrawTextEx(font_, text.c_str(), Vector2{pos.x, pos.y}, size, spacing_for(size), tint);
}

rts::Vec2 FontSet::measure(const std::string& text, float size) const {
    verify(text);
    const Vector2 v = MeasureTextEx(font_, text.c_str(), size, spacing_for(size));
    return rts::Vec2{v.x, v.y};
}

}  // namespace render
