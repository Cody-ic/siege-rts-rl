// 前端入口。
//
// 现在它是「地图查看器」：读一张地图文件、画出来、能平移缩放。实体渲染、HUD、
// 建造放置等接口定稿后再加（见 render/README.md 的分期）。
//
// 有两种运行方式：
//
//   * **窗口模式**（默认）：正常开窗，方向键/WASD 平移，滚轮缩放，中键拖拽
//   * **截图模式**（`--screenshot <路径>`）：渲一帧到离屏纹理、导出、退出，
//     **不显示窗口**。存在的理由不是省事——它让渲染结果**不必靠人盯着看**才能验：
//     评审可以跑一遍对着 PNG 看，回归也可以（见 tests 里的 render_smoke）。
//     #16 的「双视图」将来也要用同一套离屏渲染。

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "raylib.h"

#include "game/map_loader.hpp"
#include "game/scene_model.hpp"
#include "render/camera_controller.hpp"
#include "render/iso_projection.hpp"
#include "render/scene_renderer.hpp"
#include "render/sprite_atlas.hpp"

namespace {

struct Options {
    std::string map_path;
    std::string sprite_dir;
    std::string screenshot;      // 非空 = 截图模式
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
        "  --size <宽> <高>      画面尺寸，默认 1600x900\n"
        "\n"
        "窗口模式下：方向键 / WASD 平移，滚轮缩放（以鼠标为锚），中键拖拽，F 重新入画。\n",
        argv0);
}

// 手写参数解析而不引第三方库：四个选项不值得一个依赖，
// 而 CLAUDE.md 对第三方库的要求是「优先 header-only 且要在两套工具链上调通」。
bool parse(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 后面缺少参数\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--map") {
            const char* v = next("--map");
            if (!v) return false;
            out.map_path = v;
        } else if (a == "--sprites") {
            const char* v = next("--sprites");
            if (!v) return false;
            out.sprite_dir = v;
        } else if (a == "--screenshot") {
            const char* v = next("--screenshot");
            if (!v) return false;
            out.screenshot = v;
        } else if (a == "--size") {
            const char* w = next("--size");
            if (!w) return false;
            const char* h = next("--size");
            if (!h) return false;
            out.width = std::atoi(w);
            out.height = std::atoi(h);
            if (out.width <= 0 || out.height <= 0) {
                std::fprintf(stderr, "--size 必须为正\n");
                return false;
            }
        } else if (a == "-h" || a == "--help") {
            return false;
        } else {
            std::fprintf(stderr, "不认识的选项: %s\n", a.c_str());
            return false;
        }
    }
    if (out.map_path.empty() || out.sprite_dir.empty()) {
        std::fprintf(stderr, "--map 与 --sprites 都是必需的\n");
        return false;
    }
    return true;
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
    // 纹理要 GL 上下文，所以图集在 InitWindow 之后才构造。
    render::SpriteAtlas atlas(opt.sprite_dir);
    const render::IsoProjection proj(atlas.px_per_tile());
    render::SceneRenderer renderer(atlas, proj);

    // 缺素材要**在渲之前**一次性全部报出来，而不是渲到一半才炸。
    renderer.preload(lists);

    const Vector2 viewport{static_cast<float>(opt.width), static_cast<float>(opt.height)};
    render::CameraController cam;
    cam.fit(proj, map.width(), map.height(), viewport);

    const Color bg{30, 30, 38, 255};

    if (!opt.screenshot.empty()) {
        // 渲到离屏纹理再导出。**隐藏窗口的默认帧缓冲在 Windows 上读回来是黑的**
        // （实测过），而 RenderTexture 不依赖窗口可见性。
        RenderTexture2D rt = LoadRenderTexture(opt.width, opt.height);
        BeginTextureMode(rt);
        ClearBackground(bg);
        BeginMode2D(cam.camera());
        renderer.draw(lists);
        EndMode2D();
        EndTextureMode();

        Image img = LoadImageFromTexture(rt.texture);
        ImageFlipVertical(&img);      // 离屏纹理的 y 是反的
        const bool ok = ExportImage(img, opt.screenshot.c_str());
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

        BeginDrawing();
        ClearBackground(bg);
        BeginMode2D(cam.camera());
        renderer.draw(lists);
        EndMode2D();
        // **屏幕文字暂时只能是 ASCII。** raylib 的内置字体没有 CJK 字形，
        // 中文会渲成一串 `?`（实测）。要中文得随包分发一个字体文件并指定码点集合，
        // 那件事与 ImGui 的字体是同一件事，一起做（见 render/README.md 的分期）。
        // 在此之前刻意写英文，而不是渲一行问号出来。
        DrawText("WASD / arrows: pan   wheel: zoom   MMB: drag   F: fit", 12, 12, 18,
                 Color{210, 210, 220, 255});
        EndDrawing();
    }
    CloseWindow();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        print_usage(argv[0]);
        return 1;
    }
    try {
        return run(opt);
    } catch (const std::exception& e) {
        // 地图格式错、素材缺失都走这里。**打完整信息再退非零**——
        // 这两类失败的报错里带着「哪个文件、哪个字段、哪些可选值」，
        // 那正是查起来最省时间的部分，吞掉它等于白写。
        std::fprintf(stderr, "失败：%s\n", e.what());
        return 4;
    }
}
