// 前端入口。
//
// ## 三种模式
//
//   * **游戏**（默认，什么参数都不给就是它）：主菜单 → 对局 → 暂停 / 说明 /
//     败局。屏与屏之间的规则在 `game::GameShell`（可测、GCC 侧也验），
//     这里只做按键、拾取与绘制。
//   * **地图查看器**（给了 `--map` 但没给 `--battle` / `--menu`）：读一张地图、
//     画出来、平移缩放、看光标所在格。它是 `render/` 最早的形态，留着——
//     地图生成器的产物要靠它目视验收，而那条路径不需要数值表与仿真。
//   * **素材校验**（`--verify-assets`）：只把精灵全载一遍，不渲场景。
//
// 每种都可加 `--screenshot <路径>`：渲一帧到离屏纹理、导出、退出，**不显示窗口**。
// 存在的理由不是省事——它让渲染结果**不必靠人盯着看**才能验：评审可以跑一遍
// 对着 PNG 看，回归也可以（见 render/CMakeLists.txt 的几条 ctest）。
// #16 的「双视图」将来也要用同一套离屏渲染。
//
// 截图模式**刻意把窗口模式里的每一层都走一遍**（字体、信息条、格高亮、菜单面板），
// 而不是只渲个地形——只渲地形的话 `render_smoke` 就只是一条「地砖没崩」的测试，
// 而字体那三个坑（见 render/text.hpp）恰好全在它覆盖不到的地方。
//
// ## 「什么参数都不给」是一条要维护的正式路径，不是省事
//
// 三个必填参数（`--map` / `--stats` / `--sprites`）对开发者是好的，对**双击 exe
// 的人**是不可用的：资源管理器不传参数。而这个程序的读者里有一半是「想看看它长
// 什么样」的人（组内其余三人、答辩现场）。所以缺的路径一律由
// `game::discover_assets()` 补齐（找标记文件、逐级向上），失败时报出**它试过
// 哪些目录**——而不是打一句「--map 是必需的」了事。

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "raylib.h"

#include "game/asset_paths.hpp"
#include "game/battle_scene.hpp"
#include "game/demo_driver.hpp"
#include "game/display_names.hpp"
#include "game/game_shell.hpp"
#include "game/iso_projection.hpp"
#include "game/map_loader.hpp"
#include "game/menu_model.hpp"
#include "game/player_input.hpp"
#include "game/scene_model.hpp"
#include "game/stats_loader.hpp"
#include "render/camera_controller.hpp"
#include "render/cli.hpp"
#include "render/menu_view.hpp"
#include "render/scene_overlay.hpp"
#include "render/scene_renderer.hpp"
#include "render/sprite_atlas.hpp"
#include "render/text.hpp"
#include "rts/utf8_path.hpp"

namespace {

// 字体的烘焙字号。**界面上用到的最大字号即此**，比它小的靠双线性过滤缩下来，
// 比它大会糊（见 render/text.hpp）。菜单标题是 44 px，所以这里从 32 抬到 48
// ——抬的时候要连带想到它是「最大字号」而不是「常用字号」。
constexpr int kFontBakeSize = 48;
constexpr float kHudSize = 20.0f;
constexpr float kHudLine = 26.0f;
constexpr float kReadoutSize = 20.0f;

// 最近一次致命错误。**它存在只为双击启动那条路径**：那时控制台是隐藏的，
// stderr 没人看得见，`cli_entry.cpp` 要把这段文本弹成对话框。见 render/cli.hpp。
std::string g_fatal;

// 报一次致命错误：stderr 一份（命令行下），全局一份（对话框用）。
//
// **两处都要**，不是二选一：命令行下的人要能把报错贴进 issue，
// 双击的人要能看见它。
void fatal(const std::string& msg) {
    g_fatal = msg;
    std::fprintf(stderr, "失败：%s\n", msg.c_str());
}

struct Options {
    std::string map_path;
    std::string sprite_dir;
    std::string screenshot;      // 非空 = 截图模式
    std::string font_path;       // 非空 = 只用这个字体，不试候选
    std::string stats_path;      // 游戏模式必需：JSON 数值表
    bool verify_assets = false;  // 只校验素材，不渲场景
    bool battle = false;         // 游戏模式，且**跳过主菜单**直接开局
    bool menu = false;           // 游戏模式，停在主菜单（截图用；窗口下同默认）
    std::string screen;          // 开局前先切到哪一屏（main / help / paused），截图用
    int ticks = 0;               // 截图模式下先推进这么多 tick 再拍
    int width = 1600;
    int height = 900;
};

void print_usage(const char* argv0) {
    std::printf(
        "用法: %s [选项]        （什么都不给 = 开始玩，路径自动找）\n"
        "\n"
        "  --battle              直接开局，跳过主菜单\n"
        "  --menu                停在主菜单（窗口模式下与默认相同；给截图用）\n"
        "  --screen <名>         先切到哪一屏再拍：main / help / paused。只给截图用\n"
        "  --map <路径>          地图文件。只给它（不给 --battle/--menu）= 地图查看器\n"
        "  --stats <路径>        JSON 数值表，通常是 game/data/stats_placeholder.json\n"
        "  --sprites <目录>      精灵成品目录，通常是 tools/sprite_gen/out_3d\n"
        "  --screenshot <路径>   渲一帧导出成 PNG 后退出，不开窗口\n"
        "  --verify-assets       把元数据声明的每一张精灵都载入一遍，缺的全报出来后退出。\n"
        "                        不渲场景、不需要 --map\n"
        "  --font <路径>         中文字体，必须是**纯 TTF**（.ttc 字体集合不行，理由见\n"
        "                        render/text.hpp）。不给则依次试 simhei.ttf、Deng.ttf\n"
        "  --size <宽> <高>      画面尺寸，默认 1600x900\n"
        "  --ticks <N>           与 --screenshot 连用：先推进 N 个 tick 再拍（20 tick = 1 秒）\n"
        "\n"
        "上面三条路径不给时，会从「工作目录」与「exe 所在目录」逐级向上找仓库根，\n"
        "用 %s 与 %s。\n"
        "\n"
        "游戏里的按键以 `game::help_entries()` 为准（游戏内「操作说明」那一屏就是它），\n"
        "**这里刻意不抄一份**——两处各写一份必然漂移。\n",
        argv0, std::string(game::kDefaultMapRel).c_str(),
        std::string(game::kDefaultSpritesRel).c_str());
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
        } else if (a == "--battle") {
            out.battle = true;
        } else if (a == "--menu") {
            out.menu = true;
        } else if (a == "--screen") {
            const std::string* v = next("--screen");
            if (!v) return false;
            // 名字**在这里就校验**，不留到 run_game 里去悄悄忽略：拼错一个屏名
            // 而截图照样出来（拍的是主菜单），正是那种「该红却绿」。
            if (*v != "main" && *v != "help" && *v != "paused") {
                std::fprintf(stderr, "--screen 只能是 main / help / paused，收到: %s\n",
                             v->c_str());
                return false;
            }
            out.screen = *v;
            out.menu = true;   // 它蕴含游戏模式
        } else if (a == "--stats") {
            const std::string* v = next("--stats");
            if (!v) return false;
            out.stats_path = *v;
        } else if (a == "--ticks") {
            const std::string* v = next("--ticks");
            if (!v) return false;
            out.ticks = std::atoi(v->c_str());
            if (out.ticks < 0) {
                std::fprintf(stderr, "--ticks 不得为负\n");
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
    // **这里不再判「哪个参数是必需的」。** 缺的路径由 `resolve_paths()` 用素材
    // 自动发现补齐，补不上才报错——而那条报错要说出「试过哪些目录」，
    // 比「--map 是必需的」有用得多（双击启动的人根本没有命令行可给）。
    return true;
}

// 缺的路径用素材自动发现补齐。返回 false 时 `err` 是给人看的完整说明。
//
// 三条路径**各自独立**地补：只给了 `--map` 想换张图、其余用默认，是完全正常的
// 用法，不该因为「要么全给要么全不给」而失败。
bool resolve_paths(Options& opt, const std::string& argv0, bool need_map,
                   bool need_stats, std::string& err) {
    const bool need_sprites = true;   // 三种模式都要精灵（校验模式尤其）
    const bool missing = (need_map && opt.map_path.empty()) ||
                         (need_stats && opt.stats_path.empty()) ||
                         (need_sprites && opt.sprite_dir.empty());
    if (!missing) return true;

    // 起点两条：工作目录（从仓库根跑 CLI 时命中）与 exe 所在目录
    //（双击时 Explorer 把工作目录设成 exe 目录，但从别处调起来时不是，
    // 所以两条都要）。顺序即优先级，见 `game::discover_assets()`。
    std::vector<std::string> starts;
    starts.push_back(game::current_dir());
    starts.push_back(game::parent_dir_of(argv0));
    const std::optional<game::AssetPaths> found = game::discover_assets(starts);
    if (!found) {
        err = "找不到游戏用的地图 / 数值表 / 精灵。\n"
              "  自动发现的做法是从下面这些目录逐级向上找仓库根（最多 8 级）：\n";
        for (const std::string& s : starts) {
            err += "    - " + (s.empty() ? std::string("（取不到）") : s) + "\n";
        }
        err += "  判据是这三样同时存在：\n";
        err += "    " + std::string(game::kDefaultMapRel) + "\n";
        err += "    " + std::string(game::kDefaultStatsRel) + "\n";
        err += "    " + std::string(game::kDefaultSpritesRel) + "/" +
               std::string(game::kSpriteMetaName) + "\n";
        err += "  所以：从仓库目录里启动，或者用 --map / --stats / --sprites 显式指定。";
        return false;
    }
    if (opt.map_path.empty()) opt.map_path = found->map;
    if (opt.stats_path.empty()) opt.stats_path = found->stats;
    if (opt.sprite_dir.empty()) opt.sprite_dir = found->sprites;
    std::printf("素材根目录 %s（自动发现）\n", found->root.c_str());
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
        fatal("开不了窗口（没有可用的 OpenGL 上下文？）");
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
        fatal(e.what());
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
    // 菜单条目与操作说明。**这一份是机械推导出来的**（`game::all_menu_strings()`
    // 遍历每个 builder），所以加一项菜单不必回头改这里——而漏改这里的症状是
    // 「打开那一屏就抛」。
    const std::vector<std::string_view>& menu = game::all_menu_strings();
    cover.insert(cover.end(), menu.begin(), menu.end());
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
        fatal("开不了窗口（没有可用的 OpenGL 上下文？）");
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
            fatal("写不出 " + opt.screenshot);
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


// ——游戏模式：主菜单 / 对局 / 暂停 / 说明 / 败局——
//
// 屏与屏之间的规则在 `game::GameShell`（纯逻辑、进默认构建、GCC 侧也验），
// 这一层只做三件事：按键与拾取、把当前状态画出来、按 20 Hz 补 tick。
// **本函数没有任何一行改仿真的代码**——写入只经 `DemoBattle::submit_defender`
// 与 `DemoBattle::update`，两者都属于 `game/`（不变量 2）。

// 对局种子。**不是平衡数值**（那些一律待定），只是一个定值，好让「同一次启动的
// 第 n 局」可复现；每局怎么从它派生见 `game::GameShell`。
constexpr std::uint64_t kBattleSeed = 20260830u;

// 演示对局的 HUD：波次 / 阶段 / tick / 双方存活 / 三种资源存量。全是真数据。
void draw_battle_hud(const render::FontSet& font, const game::MapData& map,
                     const game::DemoBattle& battle, bool paused) {
    const rts::World& w = battle.world();
    char buf[320];
    char phase[48];
    if (battle.defeated()) {
        std::snprintf(phase, sizeof(phase), "堡垒陷落·败");
    } else if (w.phase() == rts::WavePhase::Build) {
        std::snprintf(phase, sizeof(phase), "建造 %d", battle.build_ticks_left());
    } else {
        std::snprintf(phase, sizeof(phase), "进攻中");
    }
    std::snprintf(buf, sizeof(buf), "地图 %s (%s)   波 %d   %s   tick %d%s",
                  map.name().c_str(), map.map_id().c_str(), w.wave(), phase,
                  static_cast<int>(w.now()), paused ? "   已暂停" : "");
    font.draw(buf, rts::Vec2{14.0f, 12.0f}, kHudSize, Color{225, 225, 235, 255});

    std::snprintf(buf, sizeof(buf), "守方 %d   攻方 %d   石 %d   木 %d   金 %d",
                  w.live_unit_count(rts::Side::Defender),
                  w.live_unit_count(rts::Side::Attacker),
                  static_cast<int>(w.stock(rts::Resource::Stone)),
                  static_cast<int>(w.stock(rts::Resource::Wood)),
                  static_cast<int>(w.stock(rts::Resource::Gold)));
    font.draw(buf, rts::Vec2{14.0f, 12.0f + kHudLine}, kHudSize,
              Color{170, 175, 190, 255});

    // **`Esc 菜单` 摆在最前面。** 这一行是玩家唯一会读的操作提示，而「怎么退出」
    // 是他第一个要找的东西——找不到就只能点窗口的叉，那看起来像卡住了。
    font.draw("Esc 菜单   空格暂停   方向键平移   滚轮缩放   F 重新入画",
              rts::Vec2{14.0f, 12.0f + kHudLine * 2.0f}, kHudSize,
              Color{140, 145, 160, 255});
}

// 各屏的标题与脚注（静态串）。副标题另取——它要读对局状态、要拼。
struct ScreenText {
    std::string_view title;
    std::string_view footer;
};

ScreenText screen_text(game::Screen s) {
    // 别在这个 switch 上加 `default:`：完备性靠「没有 default」+ /w14062 与
    // -Wswitch 保证，加了之后漏一屏就变成静默画一个没有标题的面板。
    // 同 game/display_names.cpp 里那一串。
    switch (s) {
        case game::Screen::Main:
            // 标题用仓库名（纯 ASCII，永远不会缺字），中文放副标题。
            return {"siege-rts-rl", "方向键选择   回车确认"};
        case game::Screen::Paused:
            return {"已暂停", "方向键选择   回车确认   Esc 继续对局"};
        case game::Screen::Help:
            return {"操作说明", "Esc 返回"};
        case game::Screen::Defeat:
            return {"堡垒陷落", "方向键选择   回车确认"};
        case game::Screen::Battle:
            return {};
    }
    return {};
}

// 面板靠左还是居中。**只有主菜单靠左**：它背后那张地图是居中摆的，
// 面板也居中就正好把城墙一带遮住；而暂停与败局屏本来就该让人看面板。
render::MenuView::Align align_of(game::Screen s) {
    return s == game::Screen::Main ? render::MenuView::Align::Left
                                   : render::MenuView::Align::Center;
}

// 副标题：主菜单是一句定语，暂停与败局屏是这一局的实况。
std::string screen_subtitle(const game::GameShell& shell) {
    const game::DemoBattle* b = shell.battle();
    char buf[160];
    switch (shell.screen()) {
        case game::Screen::Main:
            return "不对称波次生存 · 人类王国 vs 亡灵大军";
        case game::Screen::Paused:
            if (b == nullptr) return {};
            std::snprintf(buf, sizeof(buf), "波 %d   守方 %d   攻方 %d",
                          b->world().wave(),
                          b->world().live_unit_count(rts::Side::Defender),
                          b->world().live_unit_count(rts::Side::Attacker));
            return buf;
        case game::Screen::Defeat:
            if (b == nullptr) return {};
            // 无尽模式看的是**撑过几波**，不是胜负（CLAUDE.md「评估指标」：
            // 胜率在一个必败的模式里没有定义）。所以败局屏上最该显示的是它。
            std::snprintf(buf, sizeof(buf), "存活 %d 波", b->world().wave());
            return buf;
        case game::Screen::Help:
        case game::Screen::Battle:
            return {};
    }
    return {};
}

int run_game(const Options& opt) {
    // 地图与数值表**完全不碰图形**，所以在开窗之前读——数据坏了不该先弹一个窗。
    const game::MapData map = game::MapLoader::from_file(opt.map_path);
    const rts::StatsTable stats = game::StatsLoader::from_file(opt.stats_path);
    game::GameShell shell(map, stats, kBattleSeed);
    // `--battle` = 跳过主菜单直接开局。旧命令行的行为，截图模式也靠它。
    if (opt.battle) shell.apply(game::MenuAction::StartNew);
    // `--screen` 只是**把状态机走到那一屏**，走的是与玩家一样的那几条边
    //（不是直接给 `screen_` 赋值）。所以它不会造出一个玩家到不了的状态，
    // 拍出来的也就一定是玩家看得到的画面。
    if (opt.screen == "help") {
        shell.apply(game::MenuAction::Help);
    } else if (opt.screen == "paused") {
        shell.apply(game::MenuAction::StartNew);
        shell.on_escape();
    }

    if (!opt.screenshot.empty()) {
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
    } else {
        // 可调整大小。菜单面板与 HUD 都按当前视口现算版面，所以拉窗口不会错位
        //（`MenuView` 的版面函数每帧从视口重算，见那个头）。
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    }
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(opt.width, opt.height, "siege-rts-rl — 不对称波次生存");
    if (!IsWindowReady()) {
        fatal("开不了窗口（没有可用的 OpenGL 上下文？）");
        return 2;
    }
    // **Esc 归暂停菜单。** raylib 默认把 Esc 当「关闭窗口」，不改这一行的话
    // 玩家想打开菜单却把程序关了——而那在他看来就是崩溃。
    SetExitKey(KEY_NULL);
    if (opt.screenshot.empty()) SetWindowMinSize(960, 540);

    render::SpriteAtlas atlas(opt.sprite_dir);
    const game::IsoProjection proj(atlas.px_per_tile());
    render::SceneRenderer renderer(atlas, proj);
    const std::vector<game::DrawItem> tiles = game::BattleScene::tiles(map);
    // 主菜单的背景画的是**地图的静态场景**（地砖 + 城墙 + 林地 + 岩壁），
    // 与地图查看器同一份装配。只画 `tiles` 的话背景是一块空荡荡的草地——
    // 墙、树、石头全是叠加物，不在地砖那一层里。
    const game::DrawLists static_scene = game::SceneModel::build(map);

    const std::vector<std::string_view> cover = font_coverage(map);
    const std::unique_ptr<render::FontSet> font =
        render::FontSet::open(opt.font_path, kFontBakeSize, cover);
    std::printf("字体 %s，码点 %zu 个\n", font->path().c_str(), font->codepoint_count());

    const render::SceneOverlay overlay(*font, proj);
    const render::MenuView menu_view(*font);

    render::CameraController cam;
    cam.fit(proj, map.width(), map.height(),
            Vector2{static_cast<float>(opt.width), static_cast<float>(opt.height)});
    const Color bg{30, 30, 38, 255};

    // ——交互状态——
    // 按键绑定的**唯一说明**是 `game::help_entries()`（游戏内那一屏就是它），
    // 这里不再抄一份注释——两处各写一份必然漂移。
    //
    // 三个「点一格生效」的模式（建造 / 征兵 / 维修）**互斥**，所以是一个枚举而不是
    // 三个 bool：三个 bool 允许「同时在建造又在维修」这种状态存在，而那时左键
    // 该干什么只能靠代码里的先后顺序决定——一个读不出来的规则。
    enum class ActMode : int { None = 0, Build, Train, Repair };
    ActMode mode = ActMode::None;
    int force_sel = 0;
    int build_sel = 0;   // 索引进 game::buildable_types()
    int train_sel = 0;   // 索引进 game::trainable_types()
    const std::vector<rts::BldType>& buildable = game::buildable_types();
    const std::vector<rts::UnitType>& trainable = game::trainable_types();
    bool paused = false;
    int preloaded_attempt = 0;

    // 新的一局开始时把它的实体图全预载一遍：缺素材要在**看见之前**报出来，
    // 而不是等某个单位第一次走进画面（见 SpriteAtlas::preload_idle 的理由）。
    const auto preload_for_battle = [&]() {
        const game::DemoBattle* b = shell.battle();
        if (b == nullptr) {
            renderer.preload(static_scene);
            return;
        }
        renderer.preload(tiles,
                         game::BattleScene::sorted(
                             map, b->world().view(rts::Side::Defender), b->world().now()));
        preloaded_attempt = shell.attempt();
    };
    preload_for_battle();

    // 一帧的绘制。**截图模式与窗口模式共用它**——两份各画一遍的话，
    // 截图回归测试保住的就不是玩家真正看到的那个画面。
    const auto draw_frame = [&](Vector2 vp, bool has_cursor, rts::GridPos cell) {
        const game::DemoBattle* b = shell.battle();
        ClearBackground(bg);
        // 血条按屏幕尺寸画，所以每帧把当前缩放告诉渲染器（见 set_screen_scale）。
        renderer.set_screen_scale(1.0f / cam.camera().zoom);
        BeginMode2D(cam.camera());
        if (b != nullptr) {
            renderer.draw(tiles, game::BattleScene::sorted(
                                     map, b->world().view(rts::Side::Defender),
                                     b->world().now()));
        } else {
            // 主菜单也画地图：那张图本身就是最好的背景，而且它让「素材载入
            // 成功了没有」在第一屏就看得见。
            renderer.draw(static_scene);
        }
        if (shell.screen() == game::Screen::Battle && b != nullptr && has_cursor &&
            map.in_bounds(cell.i, cell.j)) {
            // 三个模式下描边就是合法性提示（绿=点了会生效 / 红=不会）；
            // 不在模式里时是白色悬停框。三个模式各问自己那条 hint——
            // 「绿框点了没反应」比红框更难懂，所以 hint 要与解算的结构性规则一致。
            const rts::WorldView v = b->world().view(rts::Side::Defender);
            bool ok = false;
            switch (mode) {
                case ActMode::Build:
                    ok = game::can_place_hint(
                        v, buildable[static_cast<std::size_t>(build_sel)], cell);
                    break;
                case ActMode::Train: ok = game::can_train_hint(v, cell); break;
                case ActMode::Repair: ok = game::can_repair_hint(v, cell); break;
                case ActMode::None: break;
            }
            const Color line = mode == ActMode::None
                                   ? Color{235, 235, 245, 255}
                                   : (ok ? Color{120, 220, 120, 255}
                                         : Color{230, 90, 90, 255});
            overlay.draw_cell_outline(cell, line, 2.0f / cam.camera().zoom);
        }
        EndMode2D();

        if (shell.screen() == game::Screen::Battle && b != nullptr) {
            draw_battle_hud(*font, map, *b, paused);
            // 模式提示行。**造价写在这里**：三种模式各花不同的资源，而「点了
            // 没反应」最常见的真因就是买不起——把价钱摆在眼前比事后猜便宜。
            char ibuf[320];
            switch (mode) {
                case ActMode::Build: {
                    const rts::BldType bt = buildable[static_cast<std::size_t>(build_sel)];
                    const rts::BldStats& s = b->world().stats().of(bt);
                    std::snprintf(ibuf, sizeof(ibuf),
                                  "建造 %s   石 %d 木 %d   TAB 换型   左键放置   右键退出",
                                  std::string(game::display_name(bt)).c_str(),
                                  static_cast<int>(s.cost_stone),
                                  static_cast<int>(s.cost_wood));
                    break;
                }
                case ActMode::Train: {
                    const rts::UnitType ut = trainable[static_cast<std::size_t>(train_sel)];
                    const rts::UnitStats& s = b->world().stats().of(ut);
                    std::snprintf(ibuf, sizeof(ibuf),
                                  "征兵 %s   金 %d   进编队 %d   TAB 换兵种   "
                                  "左键点兵营或堡垒   右键退出",
                                  std::string(game::display_name(ut)).c_str(),
                                  static_cast<int>(s.cost_gold), force_sel + 1);
                    break;
                }
                case ActMode::Repair:
                    std::snprintf(ibuf, sizeof(ibuf),
                                  "维修   左键点掉了血的建筑   花木材   右键退出");
                    break;
                case ActMode::None:
                    std::snprintf(ibuf, sizeof(ibuf),
                                  "编队 %d   右键下令   B 建造   T 征兵   R 维修   "
                                  "N 召唤下一波",
                                  force_sel + 1);
                    break;
            }
            font->draw(ibuf, rts::Vec2{14.0f, 12.0f + kHudLine * 3.0f}, kHudSize,
                       Color{200, 205, 160, 255});
            return;
        }

        // 菜单几屏：先压暗，再画面板。主菜单压得重一些（后面没有正在发生的事，
        // 压暗让面板成为唯一焦点）；暂停与败局压得轻，好让人还能看清战场。
        menu_view.dim(vp, shell.screen() == game::Screen::Main ? 150 : 140);
        const ScreenText st = screen_text(shell.screen());
        const std::string sub = screen_subtitle(shell);
        const render::MenuView::Chrome chrome{st.title, sub, st.footer,
                                              align_of(shell.screen())};
        if (shell.screen() == game::Screen::Help) {
            menu_view.draw_help(shell.menu(), chrome, vp);
        } else {
            menu_view.draw(shell.menu(), chrome, vp);
        }
    };

    if (!opt.screenshot.empty()) {
        // 截图模式：先把仿真推到要看的那一刻，再渲一帧导出。
        if (game::DemoBattle* b = shell.battle()) b->update(opt.ticks);
        shell.poll();
        if (shell.attempt() != preloaded_attempt) preload_for_battle();
        const Vector2 vp{static_cast<float>(opt.width), static_cast<float>(opt.height)};
        RenderTexture2D rt = LoadRenderTexture(opt.width, opt.height);
        BeginTextureMode(rt);
        draw_frame(vp, /*has_cursor=*/false, rts::GridPos{});
        EndTextureMode();

        Image img = LoadImageFromTexture(rt.texture);
        ImageFlipVertical(&img);   // 离屏纹理的 y 是反的
        // 不用 `ExportImage(img, path)`：raylib 的文件写入走窄 `fopen`，
        // 路径含非 ASCII 字符时会失败（见 rts/utf8_path.hpp）。
        int png_size = 0;
        unsigned char* png = ExportImageToMemory(img, ".png", &png_size);
        bool ok = false;
        if (png != nullptr && png_size > 0) {
            ok = rts::write_file_bytes(opt.screenshot, png,
                                       static_cast<std::size_t>(png_size));
        }
        if (png != nullptr) MemFree(png);
        UnloadImage(img);
        UnloadRenderTexture(rt);
        CloseWindow();
        if (!ok) {
            fatal("写不出 " + opt.screenshot);
            return 3;
        }
        const game::DemoBattle* b = shell.battle();
        std::printf("已导出 %s（tick %d）\n", opt.screenshot.c_str(),
                    b == nullptr ? 0 : static_cast<int>(b->world().now()));
        return 0;
    }

    SetTargetFPS(60);
    double acc = 0.0;
    const double kTickDt = 1.0 / static_cast<double>(rts::kTicksPerSecond);

    while (!WindowShouldClose() && !shell.quitting()) {
        const Vector2 vp{static_cast<float>(GetScreenWidth()),
                         static_cast<float>(GetScreenHeight())};
        if (IsWindowResized()) cam.set_viewport(vp);
        shell.poll();   // 对局败了就转败局屏（每帧问一次，见 GameShell::poll）

        if (shell.attempt() != preloaded_attempt && shell.battle() != nullptr) {
            preload_for_battle();
            // 新的一局：镜头重新入画，交互状态归零。不归零的话上一局留下的
            // 建造模式会跟到新局里，而玩家并不知道自己还在建造模式。
            cam.fit(proj, map.width(), map.height(), vp);
            paused = false;
            mode = ActMode::None;
            build_sel = 0;
            train_sel = 0;
            force_sel = 0;
        }

        const Vector2 mouse = GetMousePosition();
        const bool in_menu = shell.screen() != game::Screen::Battle;
        rts::GridPos cell{};

        if (in_menu) {
            // **拾取与绘制必须用同一份 chrome**（见 MenuView::Chrome 的注释）。
            const ScreenText st = screen_text(shell.screen());
            const std::string sub = screen_subtitle(shell);
            const render::MenuView::Chrome chrome{st.title, sub, st.footer,
                                                  align_of(shell.screen())};
            const int hovered =
                shell.screen() == game::Screen::Help
                    ? menu_view.hit_test_help(shell.menu(), chrome, mouse, vp)
                    : menu_view.hit_test(shell.menu(), chrome, mouse, vp);
            if (hovered >= 0) shell.menu().point_at(hovered);
            if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) shell.menu().move(-1);
            if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) shell.menu().move(1);
            // 三条确认路径（点击 / 回车 / Esc）全部汇到 `GameShell::apply`，
            // 可用性只在那里判一次。
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hovered >= 0) {
                shell.apply(shell.menu().action_at(hovered));
            } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
                       IsKeyPressed(KEY_SPACE)) {
                shell.apply(shell.menu().confirm());
            } else if (IsKeyPressed(KEY_ESCAPE)) {
                shell.on_escape();
            }
        } else if (game::DemoBattle* b = shell.battle()) {
            if (IsKeyPressed(KEY_ESCAPE)) shell.on_escape();   // → 暂停菜单
            if (IsKeyPressed(KEY_F)) cam.fit(proj, map.width(), map.height(), vp);
            if (IsKeyPressed(KEY_SPACE)) paused = !paused;
            if (IsKeyPressed(KEY_ONE)) force_sel = 0;
            if (IsKeyPressed(KEY_TWO)) force_sel = 1;
            if (IsKeyPressed(KEY_THREE)) force_sel = 2;
            if (IsKeyPressed(KEY_FOUR)) force_sel = 3;
            // 三个模式键都是「切到它 / 从它退出」。按 B 再按 T 直接换模式，
            // 不必先退出——否则玩家要按两下，而第一下看起来什么都没发生。
            const auto toggle = [&mode](ActMode m) { mode = mode == m ? ActMode::None : m; };
            if (IsKeyPressed(KEY_B)) toggle(ActMode::Build);
            if (IsKeyPressed(KEY_T)) toggle(ActMode::Train);
            if (IsKeyPressed(KEY_R)) toggle(ActMode::Repair);
            if (IsKeyPressed(KEY_TAB)) {
                if (mode == ActMode::Build) {
                    build_sel = (build_sel + 1) % static_cast<int>(buildable.size());
                } else if (mode == ActMode::Train) {
                    train_sel = (train_sel + 1) % static_cast<int>(trainable.size());
                }
            }
            if (IsKeyPressed(KEY_N)) {
                rts::Command c;
                c.kind = rts::CommandKind::Summon;
                c.side = rts::Side::Defender;
                b->submit_defender(&c, 1);
            }
            cam.update(GetFrameTime());

            // 拾取（与地图查看器同一条链路：screen → world → grid）。
            const Vector2 wpos = GetScreenToWorld2D(mouse, cam.camera());
            cell = proj.screen_to_grid(rts::Vec2{wpos.x, wpos.y});
            const bool in_map = map.in_bounds(cell.i, cell.j);
            const rts::WorldView view = b->world().view(rts::Side::Defender);
            if (in_map && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                if (mode != ActMode::None) {
                    mode = ActMode::None;   // RTS 惯例：右键退出当前模式
                } else {
                    const rts::Command c = game::command_for_click(
                        view, static_cast<std::uint8_t>(force_sel), cell);
                    b->submit_defender(&c, 1);
                }
            }
            if (in_map && mode != ActMode::None &&
                IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                // 深层裁决（资源够不够、点位规则、兵营在不在练兵）在 World 的
                // 解算里；这里只把命令发出去，虚影颜色是提示不是裁决。
                bool have = true;
                rts::Command c;
                switch (mode) {
                    case ActMode::Build:
                        c = game::build_command(
                            buildable[static_cast<std::size_t>(build_sel)], cell,
                            map.width());
                        break;
                    case ActMode::Train:
                        c = game::train_command(
                            trainable[static_cast<std::size_t>(train_sel)],
                            static_cast<std::uint8_t>(force_sel), cell, map.width());
                        break;
                    case ActMode::Repair:
                        c = game::repair_command(cell, map.width());
                        break;
                    case ActMode::None: have = false; break;
                }
                if (have) b->submit_defender(&c, 1);
            }
        }

        if (shell.should_advance() && !paused) {
            acc += static_cast<double>(GetFrameTime());
            int steps = 0;
            // 单帧最多补 5 个 tick：掉帧时宁可仿真慢下来，也不追出一大步。
            while (acc >= kTickDt && steps < 5) {
                shell.battle()->update(1);
                acc -= kTickDt;
                ++steps;
            }
        } else {
            // **暂停或在菜单里时把余量清掉。** 不清的话回到对局的那一帧会
            // 一次补满 5 个 tick，画面上是「一松手世界猛地跳一下」。
            acc = 0.0;
        }

        BeginDrawing();
        draw_frame(vp, !in_menu, cell);
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
    // 模式判定。**「什么都不给」= 游戏**，这是双击启动那条路径的落点；
    // 只给 `--map` 仍是地图查看器（生成器的产物要靠它目视验收）。
    const bool game_mode = opt.battle || opt.menu ||
                           (!opt.verify_assets && opt.map_path.empty());
    const std::string argv0 = args.empty() ? std::string() : args[0];
    std::string err;
    if (!resolve_paths(opt, argv0, /*need_map=*/!opt.verify_assets,
                       /*need_stats=*/game_mode, err)) {
        fatal(err);
        return 1;
    }
    try {
        if (opt.verify_assets) return run_verify(opt);
        return game_mode ? run_game(opt) : run(opt);
    } catch (const std::exception& e) {
        // 地图格式错、素材缺失、字体缺字都走这里。**打完整信息再退非零**——
        // 这几类失败的报错里带着「哪个文件、哪个字段、哪个字符、哪些可选值」，
        // 那正是查起来最省时间的部分，吞掉它等于白写。
        fatal(e.what());
        return 4;
    }
}

}  // namespace

namespace render {

int cli_main(const std::vector<std::string>& args) { return cli_main_impl(args); }

const std::string& last_fatal_message() { return g_fatal; }

}  // namespace render
