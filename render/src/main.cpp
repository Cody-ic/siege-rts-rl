// 前端入口。
//
// 现在它是「地图查看器」：读一张地图文件、画出来、能平移缩放、能查看光标所在格。
// 实体渲染、建造放置等接口定稿后再加（见 render/README.md 的分期）。
//
// 有两种运行方式：
//
//   * **窗口模式**（默认）：正常开窗，方向键/WASD 平移，滚轮缩放，中键拖拽
//   * **截图模式**（`--screenshot <路径>`）：渲一帧到离屏纹理、导出、退出，
//     **不显示窗口**。存在的理由不是省事——它让渲染结果**不必靠人盯着看**才能验：
//     评审可以跑一遍对着 PNG 看，回归也可以（见 render/CMakeLists.txt 的几条 ctest）。
//     #16 的「双视图」将来也要用同一套离屏渲染。
//
// 截图模式**刻意把窗口模式里的每一层都走一遍**（字体、信息条、格高亮），
// 而不是只渲个地形——只渲地形的话 `render_smoke` 就只是一条「地砖没崩」的测试，
// 而字体那三个坑（见 render/text.hpp）恰好全在它覆盖不到的地方。

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "raylib.h"

#include "game/display_names.hpp"
#include "game/iso_projection.hpp"
#include "game/map_loader.hpp"
#include "game/scene_model.hpp"
#include "render/camera_controller.hpp"
#include "render/cli.hpp"
#include "render/scene_overlay.hpp"
#include "render/scene_renderer.hpp"
#include "render/sprite_atlas.hpp"
#include "render/text.hpp"
#include "rts/utf8_path.hpp"

namespace {

// 字体的烘焙字号。界面上用到的最大字号即此，比它小的靠双线性过滤缩下来。
constexpr int kFontBakeSize = 32;
constexpr float kHudSize = 20.0f;
constexpr float kHudLine = 26.0f;
constexpr float kReadoutSize = 20.0f;

struct Options {
    std::string map_path;
    std::string sprite_dir;
    std::string screenshot;      // 非空 = 截图模式
    std::string font_path;       // 非空 = 只用这个字体，不试候选
    bool verify_assets = false;  // 只校验素材，不渲场景
    int width = 1600;
    int height = 900;
};

void print_usage(const char* argv0) {
    std::printf(
        "用法: %s --map <地图.json> --sprites <精灵目录> [选项]\n"
        "\n"
        "  --map <路径>          地图文件。仓库里有一张临时夹具：game/testdata/fixture_min.json\n"
        "  --sprites <目录>      精灵成品目录，通常是 tools/sprite_gen/out_3d\n"
        "  --screenshot <路径>   渲一帧导出成 PNG 后退出，不开窗口\n"
        "  --verify-assets       把元数据声明的每一张精灵都载入一遍，缺的全报出来后退出。\n"
        "                        不渲场景、不需要 --map\n"
        "  --font <路径>         中文字体，必须是**纯 TTF**（.ttc 字体集合不行，理由见\n"
        "                        render/text.hpp）。不给则依次试 simhei.ttf、Deng.ttf\n"
        "  --size <宽> <高>      画面尺寸，默认 1600x900\n"
        "\n"
        "窗口模式下：方向键 / WASD 平移，滚轮缩放（以鼠标为锚），中键拖拽，F 重新入画。\n"
        "            光标所在格会高亮，旁边显示这一格上有什么。\n",
        argv0);
}

// 手写参数解析而不引第三方库：五个选项不值得一个依赖，
// 而 CLAUDE.md 对第三方库的要求是「优先 header-only 且要在两套工具链上调通」。
//
// **收 `vector<string>` 而不是 `char**`。** 不是风格问题：Windows 上的 `argv` 是
// ANSI 代码页，必须在入口处转成 UTF-8（见文件末尾），而转完自然就是字符串了。
// 让这一层拿不到 `char**`，也就写不出「忘了转就直接用」这种代码。
bool parse(const std::vector<std::string>& args, Options& out) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        const auto next = [&](const char* what) -> const std::string* {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "%s 后面缺少参数\n", what);
                return nullptr;
            }
            return &args[++i];
        };
        if (a == "--map") {
            const std::string* v = next("--map");
            if (!v) return false;
            out.map_path = *v;
        } else if (a == "--sprites") {
            const std::string* v = next("--sprites");
            if (!v) return false;
            out.sprite_dir = *v;
        } else if (a == "--screenshot") {
            const std::string* v = next("--screenshot");
            if (!v) return false;
            out.screenshot = *v;
        } else if (a == "--font") {
            const std::string* v = next("--font");
            if (!v) return false;
            out.font_path = *v;
        } else if (a == "--size") {
            const std::string* w = next("--size");
            if (!w) return false;
            const std::string* h = next("--size");
            if (!h) return false;
            out.width = std::atoi(w->c_str());
            out.height = std::atoi(h->c_str());
            if (out.width <= 0 || out.height <= 0) {
                std::fprintf(stderr, "--size 必须为正\n");
                return false;
            }
        } else if (a == "--verify-assets") {
            out.verify_assets = true;
        } else if (a == "-h" || a == "--help") {
            return false;
        } else {
            std::fprintf(stderr, "不认识的选项: %s\n", a.c_str());
            return false;
        }
    }
    if (out.sprite_dir.empty()) {
        std::fprintf(stderr, "--sprites 是必需的\n");
        return false;
    }
    // `--verify-assets` 不看地图，所以不强求 `--map`。
    // 分开判而不是合成一句，是为了让报错说出**缺的是哪一个**。
    if (!out.verify_assets && out.map_path.empty()) {
        std::fprintf(stderr, "--map 是必需的（除 --verify-assets 外）\n");
        return false;
    }
    return true;
}

// 只校验素材。**要 GL 上下文**（纹理），所以照样开窗——但隐藏。
//
// 这是一条与 `check_assets.py` 互补而非重复的检查，理由见
// `SpriteAtlas::verify_all_declared()` 的注释：那个脚本查 `assets.json` 的模型，
// 这个查渲染产物，而且跑的是**读取器这一侧**的文件名规则。
int run_verify(const Options& opt) {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(64, 64, "verify-assets");
    if (!IsWindowReady()) {
        std::fprintf(stderr, "开不了窗口（没有可用的 OpenGL 上下文？）\n");
        return 2;
    }
    int rc = 0;
    try {
        render::SpriteAtlas atlas(opt.sprite_dir);
        // 两条方向相反的检查，都要跑：
        //   * verify_roster_covered —— 花名册里的实体，元数据里有没有条目
        //   * verify_all_declared   —— 元数据声明的条目，PNG 是不是真载得上
        // 只跑后者会漏掉「加了单位但没出图」，那是更常见的那一半。
        //
        // 先跑前者：它不碰 GPU、几微秒就出结果，而后者要把 476 张纹理传上去。
        const std::size_t entities = atlas.verify_roster_covered();
        const std::size_t n = atlas.verify_all_declared();
        std::printf("素材校验通过：花名册 %zu 个实体全部有图，共 %zu 张，px_per_tile = %d\n",
                    entities, n, atlas.px_per_tile());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "失败：%s\n", e.what());
        rc = 4;
    }
    CloseWindow();
    return rc;
}

// 字体要覆盖的全部串。
//
// **地图的 `name` 也在里面，这一条容易漏**：它是来自数据文件的任意中文
// （夹具那张叫「最小夹具」），不是代码里的字面量，所以没法从任何枚举推导。
// 漏掉它的后果不是崩，是标题渲成一串 `?`——正是 render/text.hpp 的第 2 个坑。
// 将来单位属性表从 JSON 读进来时同一条约束照样成立：
// **任何要显示的数据串，都必须在建字体之前登记。**
std::vector<std::string_view> font_coverage(const game::MapData& map) {
    std::vector<std::string_view> cover = game::all_display_strings();
    const std::vector<std::string_view>& ui = render::ui_strings();
    cover.insert(cover.end(), ui.begin(), ui.end());
    cover.push_back(map.name());
    cover.push_back(map.map_id());
    return cover;
}

// 左上角的场景信息 + 操作提示。**每一项都是真数据**，没有占位数字。
//
// 原先的排期表写的是「面板先接假数据」，这里刻意没那么做：硬编码的数字在界面上
// 和真数据长得一模一样，于是「这个面板到底接没接上」变成一个要读代码才能回答的问题。
// 宁可少显示几项。
void draw_hud(const render::FontSet& font, const game::MapData& map,
              const game::DrawLists& lists) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "地图 %s (%s)   尺寸 %d×%d", map.name().c_str(),
                  map.map_id().c_str(), map.width(), map.height());
    font.draw(buf, rts::Vec2{14.0f, 12.0f}, kHudSize, Color{225, 225, 235, 255});

    std::snprintf(buf, sizeof(buf), "地砖 %zu   深度序列 %zu   码点 %zu",
                  lists.tiles.size(), lists.sorted.size(), font.codepoint_count());
    font.draw(buf, rts::Vec2{14.0f, 12.0f + kHudLine}, kHudSize,
              Color{170, 175, 190, 255});

    font.draw("方向键平移   滚轮缩放   中键拖拽   F 重新入画",
              rts::Vec2{14.0f, 12.0f + kHudLine * 2.0f}, kHudSize,
              Color{140, 145, 160, 255});
}

int run(const Options& opt) {
    // 地图与场景装配**完全不碰图形**，所以放在开窗之前——地图坏了不该先弹一个窗。
    const game::MapData map = game::MapLoader::from_file(opt.map_path);
    const game::DrawLists lists = game::SceneModel::build(map);
    std::printf("地图 %s（%s）%dx%d，地砖 %zu 张，深度序列 %zu 项\n",
                map.map_id().c_str(), map.name().c_str(), map.width(), map.height(),
                lists.tiles.size(), lists.sorted.size());

    if (!opt.screenshot.empty()) SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(opt.width, opt.height, "siege-rts-rl — 地图查看器");
    if (!IsWindowReady()) {
        std::fprintf(stderr, "开不了窗口（没有可用的 OpenGL 上下文？）\n");
        return 2;
    }
    // 纹理与字体都要 GL 上下文，所以两者都在 InitWindow 之后才建。
    render::SpriteAtlas atlas(opt.sprite_dir);
    const game::IsoProjection proj(atlas.px_per_tile());
    render::SceneRenderer renderer(atlas, proj);

    // 缺素材要**在渲之前**一次性全部报出来，而不是渲到一半才炸。
    renderer.preload(lists);

    const std::vector<std::string_view> cover = font_coverage(map);
    const std::unique_ptr<render::FontSet> font =
        render::FontSet::open(opt.font_path, kFontBakeSize, cover);
    // 字体路径只打到控制台，**不画到屏幕上**：它可能含中文（`--font C:/字体/x.ttf`），
    // 而屏幕上的每个字符都要已登记，路径显然登记不了。控制台没有这个约束。
    std::printf("字体 %s，码点 %zu 个\n", font->path().c_str(), font->codepoint_count());

    const render::SceneOverlay overlay(*font, proj);

    const Vector2 viewport{static_cast<float>(opt.width), static_cast<float>(opt.height)};
    render::CameraController cam;
    cam.fit(proj, map.width(), map.height(), viewport);

    const Color bg{30, 30, 38, 255};
    const Color hover_line{255, 214, 120, 235};

    if (!opt.screenshot.empty()) {
        // 渲到离屏纹理再导出。**隐藏窗口的默认帧缓冲在 Windows 上读回来是黑的**
        // （实测过），而 RenderTexture 不依赖窗口可见性。
        RenderTexture2D rt = LoadRenderTexture(opt.width, opt.height);
        // 截图里没有光标，所以拿地图中心那格当「被指着的格」。
        // 这不是为了好看：不这么做，格高亮与信息条这两条路径在回归测试里一次都不走。
        const rts::GridPos probe{static_cast<std::int16_t>(map.width() / 2),
                                 static_cast<std::int16_t>(map.height() / 2)};

        BeginTextureMode(rt);
        ClearBackground(bg);
        BeginMode2D(cam.camera());
        renderer.draw(lists);
        overlay.draw_cell_outline(probe, hover_line, 3.0f);
        EndMode2D();
        draw_hud(*font, map, lists);
        overlay.draw_cell_readout(
            map, probe,
            rts::Vec2{14.0f, static_cast<float>(opt.height) - kReadoutSize - 20.0f},
            kReadoutSize);
        EndTextureMode();

        Image img = LoadImageFromTexture(rt.texture);
        ImageFlipVertical(&img);      // 离屏纹理的 y 是反的
        // **不用 `ExportImage(img, path)`。** raylib 的文件写入同样走窄 `fopen`，
        // 路径含非 ASCII 字符时会失败（见 rts/utf8_path.hpp）。而截图路径极可能含中文
        // ——它是用户随手给的。所以先导到内存、再自己写字节。
        int png_size = 0;
        unsigned char* png = ExportImageToMemory(img, ".png", &png_size);
        bool ok = false;
        if (png != nullptr && png_size > 0) {
            ok = rts::write_file_bytes(opt.screenshot, png, static_cast<std::size_t>(png_size));
        }
        if (png != nullptr) MemFree(png);
        UnloadImage(img);
        UnloadRenderTexture(rt);
        CloseWindow();
        if (!ok) {
            std::fprintf(stderr, "写不出 %s\n", opt.screenshot.c_str());
            return 3;
        }
        std::printf("已导出 %s\n", opt.screenshot.c_str());
        return 0;
    }

    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        if (IsWindowResized()) {
            cam.set_viewport(Vector2{static_cast<float>(GetScreenWidth()),
                                     static_cast<float>(GetScreenHeight())});
        }
        if (IsKeyPressed(KEY_F)) {
            cam.fit(proj, map.width(), map.height(),
                    Vector2{static_cast<float>(GetScreenWidth()),
                            static_cast<float>(GetScreenHeight())});
        }
        cam.update(GetFrameTime());

        // 光标所在格。**这里是 `screen_to_grid` 的目视验证手段**：移动鼠标，
        // 信息条里的坐标应当与高亮的那一格一致。单元测试钉的是往返恒等
        // （纯数学），这里钉的是「相机变换有没有接错」——那是测试够不到的一层。
        const Vector2 mouse = GetMousePosition();
        const Vector2 world = GetScreenToWorld2D(mouse, cam.camera());
        const rts::GridPos cell = proj.screen_to_grid(rts::Vec2{world.x, world.y});

        BeginDrawing();
        ClearBackground(bg);
        BeginMode2D(cam.camera());
        renderer.draw(lists);
        if (map.in_bounds(cell.i, cell.j)) {
            // 线宽除以 zoom：描边在世界空间里画，不这么做缩远之后它会变成一团。
            overlay.draw_cell_outline(cell, hover_line, 2.0f / cam.camera().zoom);
        }
        EndMode2D();
        draw_hud(*font, map, lists);
        // 界外也画信息条（内容是「光标不在地图内」）——空着的话你会以为
        // 是信息条坏了，而不是光标真的出界了。
        overlay.draw_cell_readout(map, cell, rts::Vec2{mouse.x + 18.0f, mouse.y + 20.0f},
                                  kReadoutSize);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}


// 真正的入口。**约定：进来的每一条参数都是 UTF-8**，由 `cli_entry.cpp` 保证。
int cli_main_impl(const std::vector<std::string>& args) {
    Options opt;
    if (!parse(args, opt)) {
        print_usage(args.empty() ? "rts_render" : args[0].c_str());
        return 1;
    }
    try {
        return opt.verify_assets ? run_verify(opt) : run(opt);
    } catch (const std::exception& e) {
        // 地图格式错、素材缺失、字体缺字都走这里。**打完整信息再退非零**——
        // 这几类失败的报错里带着「哪个文件、哪个字段、哪个字符、哪些可选值」，
        // 那正是查起来最省时间的部分，吞掉它等于白写。
        std::fprintf(stderr, "失败：%s\n", e.what());
        return 4;
    }
}

}  // namespace

namespace render {

int cli_main(const std::vector<std::string>& args) { return cli_main_impl(args); }

}  // namespace render
