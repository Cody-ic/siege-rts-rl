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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <array>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "raylib.h"

#include "game/asset_paths.hpp"
#include "game/selection_cycle.hpp"
#include "game/battle_scene.hpp"
#include "game/demo_driver.hpp"
#include "game/display_names.hpp"
#include "game/game_shell.hpp"
#include "game/save_game.hpp"
#include <charconv>
#include "game/iso_projection.hpp"
#include "game/map_loader.hpp"
#include "game/menu_model.hpp"
#include "game/player_input.hpp"
#include "game/scene_model.hpp"
#include "game/stats_loader.hpp"
#include "render/camera_controller.hpp"
#include "render/chronicle_view.hpp"
#include "render/battle_atmosphere.hpp"
#include "render/battle_audio.hpp"
#include "game/chronicle.hpp"
#include "render/cli.hpp"
#include "render/menu_view.hpp"
#include "render/field_guide.hpp"
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

// 驻守单位在仿真里仍位于墙格中心，画面上却按墙素材抬到了墙头。选中圈与
// 鼠标拾取必须复用同一份抬升量，否则玩家会看见人，却只能点到其脚下的地面。
std::string_view stand_sprite_at(const rts::WorldView& view,
                                 std::uint16_t wall_slot) noexcept {
    const rts::GridPos wall = rts::pos_of_slot(wall_slot, view.width());
    const auto alive = view.bld_alive();
    const auto type = view.bld_type();
    const auto pos = view.bld_pos();
    for (std::size_t k = 0; k < pos.size(); ++k) {
        if (alive[k] && pos[k] == wall) return rts::ident_of(type[k]);
    }
    return {};
}

rts::Vec2 unit_draw_anchor(const rts::WorldView& view, std::size_t unit_slot,
                           const game::IsoProjection& proj,
                           render::SpriteAtlas& atlas) {
    rts::Vec2 p = proj.world_to_screen(view.unit_pos()[unit_slot]);
    const auto garrison = view.unit_garrison();
    const auto mount = view.unit_mount();
    if (garrison[unit_slot] != rts::kNoSlot && mount[unit_slot] == 0) {
        const std::string_view stand = stand_sprite_at(view, garrison[unit_slot]);
        if (!stand.empty()) p.y -= atlas.stand_lift_px(stand,"idle",game::to_string(game::SceneModel::run_direction(view,rts::pos_of_slot(garrison[unit_slot],view.width()))));
    }
    return p;
}

std::optional<rts::UnitId> pick_garrisoned_unit(
    const rts::WorldView& view, const std::vector<rts::UnitId>& defenders,
    const game::IsoProjection& proj, render::SpriteAtlas& atlas, Vector2 world_mouse) {
    const auto type = view.unit_type();
    const auto garrison = view.unit_garrison();
    const auto mount = view.unit_mount();
    std::optional<rts::UnitId> best;
    float best_d2 = 0.0f;

    for (const rts::UnitId id : defenders) {
        const std::size_t k = id.index();
        if (garrison[k] == rts::kNoSlot || mount[k] != 0) continue;
        const rts::Vec2 foot = unit_draw_anchor(view, k, proj, atlas);
        const render::Sprite& sprite =
            atlas.get(rts::ident_of(type[k]), "idle", "SE");

        // 精灵画布包含透明留白，整张画布做 hit box 会让相邻墙段互相抢点击。
        // 拾取区取角色躯干附近；比例随素材画布与锚点缩放，不复制具体像素。
        const float half_w = static_cast<float>(sprite.texture.width) * 0.22f;
        const float top = foot.y - sprite.ground_anchor.y * 0.62f;
        const float bottom = foot.y +
                             (static_cast<float>(sprite.texture.height) -
                              sprite.ground_anchor.y) * 0.20f;
        const Rectangle hit{foot.x - half_w, top, half_w * 2.0f, bottom - top};
        if (!CheckCollisionPointRec(world_mouse, hit)) continue;

        const float cx = foot.x;
        const float cy = (top + bottom) * 0.5f;
        const float dx = world_mouse.x - cx;
        const float dy = world_mouse.y - cy;
        const float d2 = dx * dx + dy * dy;
        if (!best || d2 < best_d2) {
            best = id;
            best_d2 = d2;
        }
    }
    return best;
}

struct Options {
    std::string map_path;
    std::string sprite_dir;
    std::string screenshot;      // 非空 = 截图模式
    std::string font_path;       // 非空 = 只用这个字体，不试候选
    std::string stats_path;      // 游戏模式必需：JSON 数值表
    std::string rl_policy_path;
    std::string defender_policy_path;
    bool verify_assets = false;  // 只校验素材，不渲场景
    bool battle = false;         // 游戏模式，且**跳过主菜单**直接开局
    bool placement_shot = false; // Screenshot-only construction interaction fixture.
    bool menu = false;           // 游戏模式，停在主菜单（截图用；窗口下同默认）
    std::string screen;          // 开局前先切到哪一屏（main / help / paused），截图用
    int ticks = 0;               // 截图模式下先推进这么多 tick 再拍
    int inspect_x=-1,inspect_y=-1;
    int guide_entry = 0,guide_level=1;
    bool guide_bottom=false;
    int developer_wave = 0;
    int journal_page = -1;
    int journal_ending = 0;
    bool journal_bottom = false;
    bool classic_visuals = false;
    bool mute = false;
    std::string save_dir;
    int width = 1600;
    int height = 900;
};

void print_usage(const char* argv0) {
    std::printf(
        "用法: %s [选项]        （什么都不给 = 开始玩，路径自动找）\n"
        "\n"
        "  --battle              直接开局，跳过主菜单\n"
        "  --rl-policy <ONNX>    Load an experimental tactical policy (CPU)\n"
        "  --defender-policy <DIR>  Experimental defender AI (native CPU)\n"
        "  --save-dir <目录>     指定存档目录；截图模式完全不读写存档\n"
        "  --classic-visuals     使用原始画面；游戏中 V 可切换\n"
        "  --journal-page <0..7> 仅配合截图预览指定日记章节\n"
        "  --journal-ending <1..2> 日记截图预览结局；--journal-bottom 预览末尾\n"
        "  --menu                停在主菜单（窗口模式下与默认相同；给截图用）\n"
        "  --screen <名>         先切到哪一屏再拍：main / help / paused / guide / developer。只给截图用\n"
        "  --map <路径>          地图文件。只给它（不给 --battle/--menu）= 地图查看器\n"
        "  --stats <路径>        JSON 数值表，通常是 game/data/stats_placeholder.json\n"
        "  --sprites <目录>      精灵成品目录，通常是 tools/sprite_gen/out_3d\n"
        "  --screenshot <路径>   渲一帧导出成 PNG 后退出，不开窗口\n"
        "  --verify-assets       把元数据声明的每一张精灵都载入一遍，缺的全报出来后退出。\n"
        "                        不渲场景、不需要 --map\n"
        "  --font <路径>         中文字体，必须是**纯 TTF**（.ttc 字体集合不行，理由见\n"
        "                        render/text.hpp）。不给则依次试 simhei.ttf、Deng.ttf\n"
        "  --size <宽> <高>      画面尺寸，默认 1600x900\n"
        "  --inspect <X,Y>      截图时选中该格建筑，需 --battle --screenshot\n"
        "  --ticks <N>           与 --screenshot 连用：先推进 N 个 tick 再拍（20 tick = 1 秒）\n"
        "\n"
        "上面三条路径不给时，会从「工作目录」与「exe 所在目录」逐级向上找仓库根，\n"
        "地图从 %s 随机选一张，数值表用 %s。\n"
        "\n"
        "游戏里的按键以 `game::help_entries()` 为准（游戏内「操作说明」那一屏就是它），\n"
        "**这里刻意不抄一份**——两处各写一份必然漂移。\n",
        argv0, std::string(game::kMapPoolDirRel).c_str(),
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
        if(a=="--guide-bottom") {out.guide_bottom=true;} else if(a=="--guide-level") {
            const auto* value=next("--guide-level");if(!value) return false;
            const auto result=std::from_chars(value->data(),value->data()+value->size(),out.guide_level);
            if(result.ec!=std::errc{} || result.ptr!=value->data()+value->size() || out.guide_level<1 || out.guide_level>9999) return false;
        } else if(a=="--guide-entry") {
            const auto* value=next("--guide-entry");if(!value) return false;
            const auto result=std::from_chars(value->data(),value->data()+value->size(),out.guide_entry);
            if(result.ec!=std::errc{} || result.ptr!=value->data()+value->size() || out.guide_entry<0 || out.guide_entry>24) return false;
            out.screen="guide";
        } else if(a=="--developer-wave") {
            const auto* value=next("--developer-wave");if(!value) return false;
            const auto result=std::from_chars(value->data(),value->data()+value->size(),out.developer_wave);
            if(result.ec!=std::errc{} || result.ptr!=value->data()+value->size() || out.developer_wave<1 || out.developer_wave>9999) return false;
            out.battle=true;
        } else if (a == "--save-dir") {
            const auto* value=next("--save-dir");if(!value) return false;out.save_dir=*value;
        } else if (a == "--classic-visuals") {
            out.classic_visuals = true;
        } else if (a == "--mute") {
            out.mute = true;
        } else if (a == "--journal-bottom") {
            out.journal_bottom=true;
        } else if (a == "--journal-ending") {
            const std::string* v=next("--journal-ending");
            if(!v || (*v!="1" && *v!="2")) return false;
            out.journal_ending=(*v)[0]-'0';
        } else if (a == "--journal-page") {
            const std::string* v = next("--journal-page");
            if(!v || v->size()!=1 || (*v)[0]<'0' || (*v)[0]>'7') return false;
            out.journal_page=(*v)[0]-'0'; out.menu=true;
        } else if (a == "--map") {
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
        } else if(a=="--inspect") {
            const std::string* v=next("--inspect");
            if(!v) return false;
            const auto comma=v->find(',');if(comma==std::string::npos) return false;
            const auto x=std::from_chars(v->data(),v->data()+comma,out.inspect_x);
            const auto y=std::from_chars(v->data()+comma+1,v->data()+v->size(),out.inspect_y);
            if(x.ec!=std::errc{} || y.ec!=std::errc{} || x.ptr!=v->data()+comma || y.ptr!=v->data()+v->size() || out.inspect_x<0 || out.inspect_y<0) return false;
        } else if (a == "--placement-preview") {
            out.placement_shot=true;
            out.battle=true;

        } else if (a == "--rl-policy") {
            const std::string* v=next("--rl-policy");
            if(!v) return false;
            out.rl_policy_path=*v;
        } else if (a == "--defender-policy") {
            const std::string* v=next("--defender-policy");
            if(!v || v->empty()) return false;
            out.defender_policy_path=*v;
        } else if (a == "--battle") {
            out.battle = true;
        } else if (a == "--menu") {
            out.menu = true;
        } else if (a == "--screen") {
            const std::string* v = next("--screen");
            if (!v) return false;
            // 名字**在这里就校验**，不留到 run_game 里去悄悄忽略：拼错一个屏名
            // 而截图照样出来（拍的是主菜单），正是那种「该红却绿」。
            if (*v != "main" && *v != "help" && *v != "paused" && *v != "guide" && *v != "developer") {
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
    if(out.placement_shot && out.screenshot.empty()) return false;
    if(out.inspect_x>=0 && (out.screenshot.empty() || !out.battle)) {std::fprintf(stderr,"--inspect requires --battle --screenshot\n");return false;}
    if(out.developer_wave>0 && out.screenshot.empty()) return false;
    if((out.screen=="developer" || out.guide_entry!=0 || out.guide_level!=1 || out.guide_bottom) && out.screenshot.empty()) return false;
    if(out.journal_page>=0 && out.screenshot.empty()) { std::fprintf(stderr,"--journal-page 仅用于截图预览\n"); return false; }
    if((out.journal_ending>0 || out.journal_bottom) && out.journal_page<0) return false;
    if(out.journal_ending>0 && out.journal_page!=7) return false;
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

    // **本进程唯一的真随机源**：只用来决定这一局从地图池里抽哪一张，发生在
    // `World` 构造之前，不进回放、不影响仿真确定性。`game::discover_assets()`
    // 本身不含随机源（可用固定种子测试），挂钟只在这一行出现——`render/` 不在
    // `tools/check_determinism_bans.py` 的扫描范围内（它只扫 `rts_core/` 与
    // `game/`），这正是把它放在这里而不是 `game/` 里的理由。
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    rts::Rng map_pick_rng(static_cast<std::uint64_t>(now));

    const std::optional<game::AssetPaths> found =
        game::discover_assets(starts, map_pick_rng);
    if (!found) {
        err = "找不到游戏用的地图池 / 数值表 / 精灵。\n"
              "  自动发现的做法是从下面这些目录逐级向上找仓库根（最多 8 级）：\n";
        for (const std::string& s : starts) {
            err += "    - " + (s.empty() ? std::string("（取不到）") : s) + "\n";
        }
        err += "  判据是这三样同时存在：\n";
        err += "    " + std::string(game::kMapPoolDirRel) + "/（至少一张 .json）\n";
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
        // 第三条：可驻守建筑的抬升量落在合理区间（`kStandFrac` 是目视标定的，
        // 这条挡量级错——见 `verify_stand_geometry()`）。同样不碰 GPU，先跑。
        const std::size_t stands = atlas.verify_stand_geometry();
        // 第四条：线性结构真正用到的朝向，内容确实落在锚点上（门与墙才对得齐、
        // 墙才不被画布切掉）。它要载 4 张图，所以排在只读元数据的两条之后。
        const std::size_t linear = atlas.verify_linear_anchor();
        const std::size_t n = atlas.verify_all_declared();
        std::printf("素材校验通过：花名册 %zu 个实体全部有图，共 %zu 张，"
                    "%zu 种可驻守建筑的抬升在区间内，"
                    "%zu 对线性结构朝向的锚点对得上，px_per_tile = %d\n",
                    entities, n, stands, linear, atlas.px_per_tile());
    } catch (const std::exception& e) {
        fatal(e.what());
        rc = 4;
    }
    CloseWindow();
    return rc;
}

std::vector<rts::GridPos> building_sprite_hits(
    const game::MapData& map, const rts::WorldView& view, rts::Tick tick,
    const game::IsoProjection& proj, render::SpriteAtlas& atlas, Vector2 mouse,bool presentation) {
    std::vector<rts::GridPos> hits;
    const auto items = game::BattleScene::sorted(map, view, tick);
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->continuous || atlas.is_projectile(it->sprite)) continue;
        const auto& sprite = atlas.get(it->sprite, "idle", game::to_string(it->facing));
        const auto c = proj.grid_to_screen(it->pos);
        const auto pixel=render::SceneRenderer::sprite_pixel(sprite,{c.x,c.y},mouse,
            render::SceneRenderer::presentation_scale(it->sprite,presentation));
        const int x=static_cast<int>(std::floor(pixel.x)),y=static_cast<int>(std::floor(pixel.y));
        if (!atlas.opaque_at(sprite, x, y)) continue;
        for (std::size_t k = 0; k < view.bld_pos().size(); ++k) {
            if (view.bld_alive()[k] && view.bld_pos()[k] == it->pos &&
                rts::ident_of(view.bld_type()[k]) == it->sprite) {
                if(std::find(hits.begin(),hits.end(),it->pos)==hits.end()) hits.push_back(it->pos);
                break;
            }
        }
    }
    return hits;
}

std::optional<rts::GridPos> pick_building_sprite(
    const game::MapData& map, const rts::WorldView& view, rts::Tick tick,
    const game::IsoProjection& proj, render::SpriteAtlas& atlas, Vector2 mouse,bool presentation) {
    const auto hits=building_sprite_hits(map,view,tick,proj,atlas,mouse,presentation);
    return hits.empty()?std::nullopt:std::optional<rts::GridPos>{hits.front()};
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
    cover.push_back("强制抢修：工匠将忽略危险，工程结束后恢复自主请选择工匠，并指向在建、在修或升级中的建筑建造指令已提交段座库存换建筑换轴退出");
    const std::vector<std::string_view>& ui = render::ui_strings();
    cover.insert(cover.end(), ui.begin(), ui.end());
    // 菜单条目与操作说明。**这一份是机械推导出来的**（`game::all_menu_strings()`
    // 遍历每个 builder），所以加一项菜单不必回头改这里——而漏改这里的症状是
    // 「打开那一屏就抛」。
    const std::vector<std::string_view>& menu = game::all_menu_strings();
    cover.insert(cover.end(), menu.begin(), menu.end());
    const auto story = render::ChronicleView::strings();
    cover.insert(cover.end(), story.begin(), story.end());
    cover.push_back("开发者对局不保存 开发者模式 资源无限 人口无限 下一波 输入波数 回车应用 关闭面板 本局不保存不解锁剧情 窥使侦查成功 情报影响下一波 窥使尚未得手 沿用旧情报 攻城锤 不死鸟 编队调整 本波编队 无调整 敌军将参考当前城防 石木金人口∞");
    for(auto text:render::guide_strings()) cover.push_back(text);
    for(auto text:render::developer_strings) cover.push_back(text);
    cover.push_back(map.name());
    cover.push_back(map.map_id());
    return cover;
}

// 左上角的场景信息 + 操作提示。**每一项都是真数据**，没有占位数字。
//
// 原先的排期表写的是「面板先接假数据」，这里刻意没那么做：硬编码的数字在界面上
// 和真数据长得一模一样，于是「这个面板到底接没接上」变成一个要读代码才能回答的问题。
// 宁可少显示几项。

// 资源点的地表标记不走精灵流水线（`tools/sprite_gen/` 走 Blender，不为它们
// 出图），但**与流水线产物放在同一个目录**（`tools/sprite_gen/out_3d/`，
// `decal_` 前缀区分），不另开一层「decals 是 sprite_dir 的同级」这种目录
// 假设——那条假设在 `render_utf8_path` 测试（`--sprites` 指向复制出来的自定义
// 目录，不是原目录）下不成立。放同一目录后，`sprite_dir` 走到哪、decal 就
// 跟到哪，与花名册精灵同构。登记要在任何 preload/draw 之前——`run` 与
// `run_game` 各调一次。
//
// 2026-09-01：从程序化生成的抽象菱形换成真实素材（树/石堆/矿场）。
void register_resource_decals(render::SpriteAtlas& atlas, const std::string& sprite_dir) {
    atlas.register_decal_image("StonePt", sprite_dir + "/decal_StonePt.png");
    atlas.register_decal_image("WoodPt", sprite_dir + "/decal_WoodPt.png");
    atlas.register_decal_image("GoldPt", sprite_dir + "/decal_GoldPt.png");
}

// HUD 底板：先铺一块半透明深底再写字。等距地图配色偏中间调，浅色文字直绘
// 压在草地/石头/水面上都可能读不出来——2026-09-01 试玩反馈（地图放大到 144
// 之后左上角文字「颜色太浅看不清」）。模式照 `SceneOverlay::draw_cell_readout`
// 那块底（{18,18,24,205} + 浅字），两处用同一组色。
void draw_hud_backing(const render::FontSet& font,
                      const std::initializer_list<std::string>& lines) {
    float max_w = 0.0f;
    for (const std::string& s : lines) max_w = std::max(max_w, font.measure(s, kHudSize).x);
    const float pad = 8.0f;
    DrawRectangleRec(
        Rectangle{14.0f - pad, 12.0f - pad,
                  max_w + pad * 2.0f,
                  kHudLine * static_cast<float>(lines.size()) + pad * 2.0f - 4.0f},
        Color{18, 18, 24, 205});
}

// 侦查面板：**玩家当前看得见的**来袭编成 + 每种被谁克。画在右上角。
//
// 它是迷雾落地之后补的另一半：迷雾把编成藏起来了（那是设计——「编成构成」在
// CLAUDE.md 的情报划分里属**需侦查**那一列），但侦查到之后玩家**没有任何面板
// 可读**，只能靠数精灵。而 CLAUDE.md 同时要求「玩家必须能快速读懂来袭编成才能
// 应对」、且「克制必须在 UI 中完全透明」——不透明会让「被 AI 针对」退化成
// 「被系统坑」（那是它引 AoE2 那条社区批评时点名要避免的）。
//
// **看不见就整块不画**（而不是画一个空框）：「什么都没侦查到」与「敌人还没来」
// 在玩家侧应当是同一个观感——都是「我不知道」，而一个空面板会读作「确认无敌人」。
// 右上角的一块文字面板（侦查面板两条路径共用：成功报告与「侦查失败」）。
// 抽出来只为一个理由：两处若各画各的，宽度、内边距与配色迟早会漂。
void draw_panel_lines(const render::FontSet& font,
                      const std::vector<std::string>& lines, int screen_w) {
    float max_w = 0.0f;
    for (const std::string& s : lines) {
        max_w = std::max(max_w, font.measure(s, kHudSize).x);
    }
    const float pad = 8.0f;
    const float x = std::max(14.0f, static_cast<float>(screen_w) - max_w - 14.0f);
    const float top = x < 730.0f ? 164.0f : 12.0f;
    DrawRectangleRec(Rectangle{x - pad, top - pad, max_w + pad * 2.0f,
                               kHudLine * static_cast<float>(lines.size()) +
                                   pad * 2.0f - 4.0f},
                     Color{18, 18, 24, 205});
    for (std::size_t k = 0; k < lines.size(); ++k) {
        const Color col = (k == 0) ? Color{235, 235, 245, 255}
                                   : Color{225, 195, 195, 255};
        font.draw(lines[k], rts::Vec2{x, top + kHudLine * static_cast<float>(k)},
                  kHudSize, col);
    }
}

void draw_intel_panel(const render::FontSet& font, const game::DemoBattle& battle,
                      int screen_w) {
    // **读的是本波斥候带回来的报告，不是当前视野**（2026-09-04 组内定）。
    //
    // 此前这里调 `sighted_composition`（当前看得见的），于是斥候一撤、一死，
    // 它送回来的情报在面板上当场消失。而侦查是**二元**事件：斥候到达集结点
    // 掷一次死活，死了本轮什么都没有，活着就拿到完整编成——那份报告到手之后
    // 就是玩家的记忆，不该再随视野变化。理由见 `DefenderSetup` 的注释。
    const auto outcome = battle.scout_outcome();
    if (outcome == game::DemoBattle::ScoutOutcome::None) return;
    if (outcome == game::DemoBattle::ScoutOutcome::Killed) {
        // **失败也要报**：「派了但没回来」与「根本没派」是两种处境，
        // 玩家得知道自己那 25 金买到的是一次失败而不是还在路上。
        draw_panel_lines(font, {"侦查失败"}, screen_w);
        return;
    }
    const std::vector<game::SightedType>& seen = battle.scout_report();
    if (seen.empty()) return;

    std::vector<std::string> lines;
    lines.push_back("已侦查");
    for (const game::SightedType& s : seen) {
        std::string line(game::display_name(s.type));
        line += " ×" + std::to_string(s.count);
        const game::CounterHint c = game::counters_of(s.type);
        if (!c.units.empty() || !c.blds.empty()) {
            line += "  克：";
            bool first = true;
            for (const rts::UnitType u : c.units) {
                if (!first) line += "/";
                line += std::string(game::display_name(u));
                first = false;
            }
            for (const rts::BldType b : c.blds) {
                if (!first) line += "/";
                line += std::string(game::display_name(b));
                first = false;
            }
        }
        lines.push_back(line);
    }

    draw_panel_lines(font, lines, screen_w);
}

// 对局警报横幅（顶中）：「窥使入境」常驻警报 + 三种一次性短闪。
//
// 2026-09-03 试玩反馈：前期侦查阶段（敌窥使来探、我方斥候去看）在界面上
// 没有任何专属提示——阶段行从「建造」直接跳到「进攻中」，玩家对侦查博弈
// 感知不到。两条侦查横幅各管一半，同日又加了第三条操作确认；2026-09-04
// rebase 到 #139 后回报闪改读二元判定（见下），并补了失败闪：
//
//   * **窥使入境**（Threat）：敌 `Wraith` 进入我方视野就亮，离开视野或被
//     击落即灭。它亮着的这段时间是玩家唯一的反制窗口（击落 = 敌 AI 这波
//     带不到新情报，`CLAUDE.md`「双向欺骗」），此前这个窗口只有地图上一个
//     不起眼的小精灵在「提醒」。
//   * **斥候回报**（Report）：斥候二元判定（`DemoBattle::scout_outcome()`，
//     #139）跳到 `Success` 时闪几秒——你的斥候活着回来了，并把视线引向
//     右上角的侦查面板（面板读的是同一份报告快照）。
//   * **斥候阵亡**（Failed）：判定跳到 `Killed` 时闪几秒。占位死亡率
//     400‰——失败是常态分支不是边角，「派了但没回来」和「根本没派」是
//     两种处境，玩家得知道那笔斥候钱买到的是一次失败。
//   * **提前召唤确认**（Summon）：N 键生效时闪几秒。此前 Summon 生效
//     毫无反馈，建造期被瞬间掐掉时玩家无从分辨「自然开打」与「自己按的」
//     ——这条闪就是那条分辨线（2026-09-03 的「建造期消失」疑案）。
//
// 警报只报**看得见的**（判据 `game::enemy_wraith_sighted`，与侦查面板同源）：
// 看不见的不报——那不是保守，是迷雾设计本身。
//
// 描边亮度随 tick 脉动（不用墙钟）：截图模式的画面由 tick 完全决定，
// 用 `GetTime()` 会让同一 tick 的截图忽明忽暗、回归基线没法对。
enum class AlertStyle { Threat, Report, Failed, Summon };

void draw_alert_banner(const render::FontSet& font, int screen_w, rts::Tick now,
                       AlertStyle style) {
    std::string line1, line2;
    Color backing{}, ink1{}, ink2{}, edge_rgb{};
    switch (style) {
        case AlertStyle::Threat:
            line1 = "幽影窥使入境";
            line2 = "击落它，别让它看清你的布防";
            backing = Color{46, 20, 54, 225};
            ink1 = Color{240, 205, 255, 255};
            ink2 = Color{215, 180, 230, 255};
            edge_rgb = Color{220, 150, 255, 0};
            break;
        case AlertStyle::Report:
            line1 = "斥候回报：探到敌军集结";
            line2 = "编成与克制见右上侦查面板";
            backing = Color{18, 42, 46, 225};
            ink1 = Color{205, 240, 245, 255};
            ink2 = Color{175, 215, 220, 255};
            edge_rgb = Color{140, 225, 235, 0};
            break;
        case AlertStyle::Failed:
            line1 = "斥候阵亡：侦查失败";
            line2 = "本波编成不明，当心各方向";
            backing = Color{52, 22, 22, 225};
            ink1 = Color{255, 190, 175, 255};
            ink2 = Color{235, 170, 160, 255};
            edge_rgb = Color{255, 140, 120, 0};
            break;
        case AlertStyle::Summon:
            line1 = "已提前召唤下一波";
            line2 = "敌军即刻开拔";
            backing = Color{50, 38, 14, 225};
            ink1 = Color{255, 230, 170, 255};
            ink2 = Color{235, 205, 140, 255};
            edge_rgb = Color{255, 214, 120, 0};
            break;
    }

    const float w1 = font.measure(line1, kHudSize).x;
    const float w2 = font.measure(line2, kHudSize * 0.8f).x;
    const float max_w = std::max(w1, w2);
    const float pad = 8.0f;
    const float x = (static_cast<float>(screen_w) - max_w) * 0.5f;
    // **顶中、但在 HUD 四行之下**：与 HUD 同高时会正好盖住第一行的 tick 与
    // 第二行的「主攻」方向提示——那是免费情报，被遮住就是事故（第一次目视
    // 验收截图里真的盖住了）。
    const float top = 12.0f + 4.0f * kHudLine + 10.0f;
    const Rectangle box{x - pad, top - pad, max_w + pad * 2.0f,
                        kHudLine + kHudLine * 0.8f + pad * 2.0f - 4.0f};
    DrawRectangleRec(box, backing);
    // 40 tick（2 秒）一个呼吸周期，三角波 120→240。
    const int ph = static_cast<int>(now % 40);
    const int tri = ph < 20 ? ph : 40 - ph;
    const auto glow = static_cast<unsigned char>(120 + tri * 6);
    const Color edge{edge_rgb.r, edge_rgb.g, edge_rgb.b, glow};
    DrawRectangleLinesEx(box, 1.5f, edge);
    font.draw(line1,
              rts::Vec2{x + (max_w - w1) * 0.5f, top}, kHudSize, ink1);
    font.draw(line2,
              rts::Vec2{x + (max_w - w2) * 0.5f, top + kHudLine}, kHudSize * 0.8f,
              ink2);
}

void draw_hud(const render::FontSet& font, const game::MapData& map,
              const game::DrawLists& lists) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "地图 %s (%s)   尺寸 %d×%d", map.name().c_str(),
                  map.map_id().c_str(), map.width(), map.height());
    const std::string line1 = buf;
    std::snprintf(buf, sizeof(buf), "地砖 %zu   深度序列 %zu   码点 %zu",
                  lists.tiles.size(), lists.sorted.size(), font.codepoint_count());
    const std::string line2 = buf;

    draw_hud_backing(font, {line1, line2, "方向键平移   滚轮缩放   中键拖拽   F 重新入画"});
    font.draw(line1, rts::Vec2{14.0f, 12.0f}, kHudSize, Color{235, 235, 245, 255});
    font.draw(line2, rts::Vec2{14.0f, 12.0f + kHudLine}, kHudSize,
              Color{190, 195, 210, 255});
    font.draw("方向键平移   滚轮缩放   中键拖拽   F 重新入画",
              rts::Vec2{14.0f, 12.0f + kHudLine * 2.0f}, kHudSize,
              Color{190, 195, 210, 255});
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
    register_resource_decals(atlas, opt.sprite_dir);
    const game::IsoProjection proj(atlas.px_per_tile());
    render::SceneRenderer renderer(atlas, proj);
    renderer.set_presentation(!opt.classic_visuals, 0.0f);

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

// 战况卡突出波次、堡垒血量与资源；操作提示放在底部工具栏。
void draw_battle_hud(const render::FontSet& font,
                     const game::DemoBattle& battle, bool paused, std::size_t selected_count) {
    const rts::World& w = battle.world();
    char buf[320];
    char phase[48];
    // **时间一律按秒给玩家，不给 tick**（2026-09-04 试玩反馈）。tick 是仿真的
    // 内部单位（20/秒，`rts::kTicksPerSecond`）——它对调试有用，对玩家是噪音，
    // 而「还有 260」与「还有 13 秒」是完全不同的两条信息：后者玩家能拿它跟
    // 「一座箭楼要 12 秒」比，前者要先在心里除以 20。
    //
    // 建造倒计时**向上取整**：还剩 1..19 个 tick 时显示「1 秒」而不是「0 秒」
    // ——显示 0 而画面还在建造期，读起来像卡住了。
    const int build_sec =
        (battle.build_ticks_left() + rts::kTicksPerSecond - 1) / rts::kTicksPerSecond;
    if (battle.defeated()) {
        std::snprintf(phase, sizeof(phase), "堡垒陷落·败");
    } else if (w.phase() == rts::WavePhase::Build) {
        std::snprintf(phase, sizeof(phase), "建造 %d 秒", build_sec);
    } else if (!game::combat_engaged(w)) {
        // 开打 ≠ 已接战：大军还要行军几十秒，这期间旧文案「进攻中」既不
        // 准确也不给信息（2026-09-03 试玩反馈）。判据在 `game/` 测。
        std::snprintf(phase, sizeof(phase), "敌袭迫近");
    } else {
        std::snprintf(phase, sizeof(phase), "交战");
    }
    const rts::WorldView bv = w.view(rts::Side::Defender);
    const int lead = game::strongest_spawn(bv);
    std::string dir;
    if (lead >= 0) {
        dir = "   主攻 " +
              std::string(game::display_name(game::compass_of(
                  w.keep_pos(), bv.spawns()[static_cast<std::size_t>(lead)].pos)));
    }
    // Keep combat priorities above the resource strip; controls have their own bar.
    const float panel_w = std::min(700.0f, static_cast<float>(GetScreenWidth())-28.0f);
    DrawRectangleRec(Rectangle{14,14,panel_w,158}, Color{23,28,30,238});
    DrawRectangleRec(Rectangle{14,14,4,158}, Color{221,185,114,255});
    std::snprintf(buf,sizeof(buf), "第 %d 波   %s%s", w.wave(), phase,
                  paused ? " · 已暂停" : "");
    font.draw(buf, rts::Vec2{30,24}, 30, Color{246,231,200,255});
    std::int64_t hp = 0, max_hp = 1;
    for (std::size_t k=0;k<bv.bld_pos().size();++k)
        if(bv.bld_alive()[k] && bv.bld_type()[k]==rts::BldType::Keep) {
            hp=bv.bld_hp()[k]; max_hp=std::max<std::int64_t>(1,bv.bld_max_hp()[k]);
        }
    std::snprintf(buf,sizeof(buf),"堡垒 %lld / %lld%s",static_cast<long long>(hp),
                  static_cast<long long>(max_hp),dir.c_str());
    font.draw(buf,rts::Vec2{30,64},22,Color{223,224,214,255});
    DrawRectangleRec(Rectangle{30,95,panel_w-32,5},Color{65,66,60,255});
    DrawRectangleRec(Rectangle{30,95,(panel_w-32)*static_cast<float>(hp)/static_cast<float>(max_hp),5},
                     hp*3<max_hp ? Color{230,108,88,255}:Color{131,186,146,255});
    std::snprintf(buf,sizeof(buf),"石 %d    木 %d    金 %d    人口 %d/%d    已选 %zu",
                  static_cast<int>(w.stock(rts::Resource::Stone)),
                  static_cast<int>(w.stock(rts::Resource::Wood)),
                  static_cast<int>(w.stock(rts::Resource::Gold)),bv.defender_pop(),bv.defender_pop_cap(),selected_count);
    if(w.developer()) std::snprintf(buf,sizeof(buf),"开发者模式 · 石 ∞  木 ∞  金 ∞  人口 ∞");
    font.draw(buf,rts::Vec2{30,111},22,Color{227,219,195,255});
    const auto& actual=battle.wave_plan();const auto& base=battle.baseline_plan();
    std::snprintf(buf,sizeof(buf),"%s · 本波编队：攻城锤 %+d / 不死鸟 %+d",
                  battle.wave_scouted()?"窥使侦查成功，情报影响下一波":"窥使尚未得手，沿用旧情报",
                  actual.rams-base.rams,actual.phoenixes-base.phoenixes);
    font.draw(buf,rts::Vec2{30,145},16,Color{235,190,140,255});
    if(!battle.tactical_policy_identity().empty() || !battle.defender_policy_identity().empty()) {
        if(battle.tactical_policy_identity().empty()) std::snprintf(buf,sizeof(buf),"Defender AI");
        else std::snprintf(buf,sizeof(buf),"RL: %zu squads%s",battle.learned_squads(),
                           battle.defender_policy_identity().empty()?"":" / Defender AI");
        const float width=font.measure(buf,16).x;
        const float x=14+panel_w-width-16;
        DrawRectangleRec({x-8,27,width+16,25},Color{43,52,48,245});
        font.draw(buf,{x,30},16,Color{213,193,145,255});
    }
}

std::array<Rectangle,6> battle_buttons(float width, float height) {
    const float button_w = std::min(148.0f, (width-90.0f)/6.0f);
    std::array<Rectangle,6> result{};
    for(std::size_t i=0;i<result.size();++i)
        result[i]=Rectangle{14.0f+static_cast<float>(i)*(button_w+10),height-59,button_w,42};
    return result;
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
            return {"圣城守望", "siege-rts-rl  /  方向键选择   回车确认"};
        case game::Screen::Paused:
            return {"已暂停", "方向键选择   回车确认   Esc 继续对局"};
        case game::Screen::Guide:
            return {"图鉴", "Esc 返回"};
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
            return "黄昏圣城 · 无尽围城";
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
        case game::Screen::Guide:
        case game::Screen::Help:
        case game::Screen::Battle:
            return {};
    }
    return {};
}

int run_game(const Options& opt) {
    // 地图与数值表**完全不碰图形**，所以在开窗之前读——数据坏了不该先弹一个窗。
    const bool persistent=opt.screenshot.empty();
    std::filesystem::path save_dir;
    std::optional<game::BattleArchive> archive;
    std::string storage_status;
    bool storage_ready=false;
    if(persistent) {
        try {
            save_dir=opt.save_dir.empty()?game::default_save_directory():rts::path_from_utf8(opt.save_dir);
            if(opt.save_dir.empty()) {
                const auto attacker=opt.rl_policy_path.empty()?std::string{}:game::TacticalPolicy::file_identity(opt.rl_policy_path);
                const auto defender=opt.defender_policy_path.empty()?std::string{}:
                    game::TacticalPolicy::file_identity(rts::utf8_from_path(rts::path_from_utf8(opt.defender_policy_path)/"manifest.json"));
                save_dir=game::policy_save_directory(save_dir,attacker,defender);
            }
            std::filesystem::create_directories(save_dir);
            storage_ready=true;
        } catch(const std::exception& e) {
            storage_status="存档目录读取失败，原文件已保留";std::fprintf(stderr,"save: %s\n",e.what());
        }
        if(storage_ready && !opt.battle && opt.screen.empty()) {
            for(const auto* name:{"campaign.json","campaign.json.bak"}) {
                if(!std::filesystem::exists(save_dir/name)) continue;
                try {
                    auto candidate=game::read_archive(save_dir/name);
                    (void)game::MapLoader::from_string(candidate.map_json);
                    (void)game::StatsLoader::from_string(candidate.stats_json);
                    archive=std::move(candidate);break;
                } catch(const std::exception& e) {
                    storage_status="存档读取失败，原文件已保留";std::fprintf(stderr,"save: %s\n",e.what());
                    if(std::string(e.what()).find("不兼容")!=std::string::npos) {
                        try {
                            game::preserve_incompatible_archive(save_dir/name);
                            storage_status="旧版对局已备份，请用旧版继续";
                        } catch(const std::exception& copy_error) {
                            std::fprintf(stderr,"legacy backup: %s\n",copy_error.what());
                        }
                    }
                }
            }
        }
    }
    std::string map_json=archive?archive->map_json:game::read_save_text(rts::path_from_utf8(opt.map_path));
    std::string stats_json=archive?archive->stats_json:game::read_save_text(rts::path_from_utf8(opt.stats_path));
    game::MapData map = game::MapLoader::from_string(map_json);
    const rts::StatsTable stats = game::StatsLoader::from_string(stats_json);
    std::shared_ptr<game::TacticalPolicy> tactical_policy;
    if (!opt.rl_policy_path.empty()) {
        tactical_policy=std::make_shared<game::TacticalPolicy>(opt.rl_policy_path,stats.fingerprint());
        std::fprintf(stderr,"Experimental RL policy %s; unsupported units retain scripted control\n",
                     tactical_policy->identity().c_str());
    }
    std::shared_ptr<game::MacroPolicy> defender_policy;
    if(!opt.defender_policy_path.empty()) {
        defender_policy=std::make_shared<game::MacroPolicy>(opt.defender_policy_path,opt.stats_path);
        std::fprintf(stderr,"Experimental defender AI %s; native categorical sampling\n",defender_policy->identity().c_str());
    }
    game::GameShell shell(map, stats, kBattleSeed,tactical_policy,defender_policy);
    // `--battle` = 跳过主菜单直接开局。旧命令行的行为，截图模式也靠它。
    if (opt.battle) shell.apply(game::MenuAction::StartNew);
    if(opt.developer_wave>0) {shell.battle()->enable_developer();shell.battle()->developer_wave(opt.developer_wave);}
    // `--screen` 只是**把状态机走到那一屏**，走的是与玩家一样的那几条边
    //（不是直接给 `screen_` 赋值）。所以它不会造出一个玩家到不了的状态，
    // 拍出来的也就一定是玩家看得到的画面。
    if(opt.screen=="guide") shell.apply(game::MenuAction::Guide);
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

    const std::vector<std::string_view> cover = font_coverage(map);
    const std::unique_ptr<render::FontSet> font =
        render::FontSet::open(opt.font_path, kFontBakeSize, cover);
    std::printf("字体 %s，码点 %zu 个\n", font->path().c_str(), font->codepoint_count());

    bool restore_cancelled=false;
    const auto restore_candidate=[&](const game::BattleArchive& candidate) {
        double next_restore_paint=0;
        auto restored=game::restore_battle(candidate,[&](rts::Tick current,rts::Tick total) {
            if(WindowShouldClose()) {restore_cancelled=true;return false;}
            if(current<total && GetTime()<next_restore_paint) return true;
            next_restore_paint=GetTime()+0.1;
            BeginDrawing();ClearBackground(Color{28,34,37,255});
            const auto message="正在恢复对局 "+std::to_string(total>0?current*100/total:100)+"%";
            font->draw(message,{50,80},28,Color{234,218,178,255});
            font->draw("长局恢复需要一些时间，请稍候",{50,128},22,Color{180,191,189,255});
            EndDrawing();return true;
        },tactical_policy,defender_policy);
        map_json=candidate.map_json;stats_json=candidate.stats_json;
        map=game::MapLoader::from_string(map_json);
        shell=game::GameShell(map,game::StatsLoader::from_string(stats_json),kBattleSeed,tactical_policy,defender_policy);
        shell.adopt_saved_battle(std::move(*restored),candidate.attempt,candidate.choice);
        storage_status=shell.battle()->defeated() || shell.chronicle().completed()
            ? "对局已结束，日记已保留" : "对局已恢复，选择继续对局";
    };
    if(archive) {
        try {restore_candidate(*archive);}
        catch(const std::exception& e) {
            std::fprintf(stderr,"restore: %s\n",e.what());
            if(!restore_cancelled) {
                try {restore_candidate(game::read_archive(save_dir/"campaign.json.bak"));}
                catch(const std::exception& backup_error) {
                    storage_status="存档恢复失败，原文件已保留";
                    std::fprintf(stderr,"backup: %s\n",backup_error.what());
                }
            }
        }
    }
    if(restore_cancelled) {CloseWindow();return 0;}

    render::SpriteAtlas atlas(opt.sprite_dir);
    register_resource_decals(atlas, opt.sprite_dir);
    const game::IsoProjection proj(atlas.px_per_tile());
    render::SceneRenderer renderer(atlas, proj);
    const std::vector<game::DrawItem> tiles = game::BattleScene::tiles(map);
    // 主菜单的背景画的是**地图的静态场景**（地砖 + 城墙 + 林地 + 岩壁），
    // 与地图查看器同一份装配。只画 `tiles` 的话背景是一块空荡荡的草地——
    // 墙、树、石头全是叠加物，不在地砖那一层里。
    game::DrawLists static_scene = game::SceneModel::build(map);
    if(std::none_of(static_scene.sorted.begin(),static_scene.sorted.end(),[](const auto& item){return item.sprite=="Keep";})) {
        game::DrawItem keep;
        keep.pos=map.keep(); keep.sprite="Keep";
        static_scene.sorted.push_back(keep);
        std::stable_sort(static_scene.sorted.begin(),static_scene.sorted.end(),[](const auto& lhs,const auto& rhs){return lhs.depth_f()<rhs.depth_f();});
    }

    const render::SceneOverlay overlay(*font, proj);
    const render::MenuView menu_view(*font);

    render::CameraController cam;
    cam.fit(proj, map.width(), map.height(),
            Vector2{static_cast<float>(opt.width), static_cast<float>(opt.height)});
    if(!shell.battle() && !opt.classic_visuals)
        cam.focus_keep(proj,map.keep(),Vector2{static_cast<float>(opt.width),static_cast<float>(opt.height)},0.75f);
    const Color bg{30, 30, 38, 255};

    // ——交互状态——
    // 按键绑定的**唯一说明**是 `game::help_entries()`（游戏内那一屏就是它），
    // 这里不再抄一份注释——两处各写一份必然漂移。
    //
    // #57 重开后的形状：框选取代「先按数字键选编队、再右键」；建造 / 征兵 /
    // 维修从「先进模式再点」改成「先点目标，弹出菜单」。`PopupKind` 标的是
    // **弹窗本身用哪张清单摆选项**，不是「这一格只能做一件事」——一座掉血的
    // 兵营/堡垒同时能修也能练兵，两者不互斥（试玩报出来的 bug：旧版按
    // 「维修 > 征兵」优先级二选一，受损后练兵入口直接消失）。`Train` 弹窗
    // 因此可能在练兵清单前面多插一行「维修」，见 `popup_options`。
    //
    // 「升级」按同一条道理办：它与维修、练兵**都不互斥**（一座掉血的兵营
    // 三件事全都能做），所以它也是一个可插队的行，而不是一个把别的入口顶
    // 掉的模式。`Upgrade` 作为独占弹窗只服务「点一座满血的墙/塔」——那种
    // 格子在加它之前左键点下去什么都不弹。
    enum class PopupKind : int {
        None = 0, Build, Train, Repair, Upgrade, Cancel, Demolish
    };
    struct Popup {
        PopupKind kind = PopupKind::None;
        rts::GridPos cell{};       // 建造的落点 / 兵营或堡垒 / 受损建筑
        Vector2 anchor{};          // 菜单画在哪（点开那一刻的屏幕坐标）
    };
    Popup popup;
    std::optional<rts::GridPos> inspected;
    game::SelectionCycle building_cycle;
    if(opt.inspect_x>=0) {
        if(!map.in_bounds(opt.inspect_x,opt.inspect_y)) throw std::runtime_error("inspect cell out of bounds");
        inspected=rts::GridPos{static_cast<std::int16_t>(opt.inspect_x),static_cast<std::int16_t>(opt.inspect_y)};
    }
    std::optional<rts::GridPos> order_target;
    double order_until = 0.0;
    std::string notice;
    double notice_until = 0.0;
    bool inspected_busy = false;
    std::optional<Vector2> inspected_click;
    const auto inspector_box = [&](Vector2 viewport) {
        Rectangle box{14, viewport.y-210, std::min(700.0f,viewport.x-28), 120};
        // A newly opened inspector must not cover the point used to cycle buildings.
        if (inspected_click && CheckCollisionPointRec(*inspected_click,box)) box.y=174;
        return box;
    };
    // 侦查警报的跨帧状态（`draw_alert_banner`）：斥候回报/阵亡两条闪抓的是
    // **二元判定的边沿**（#139：`DemoBattle::scout_outcome()` 三态，逐波重置
    // 为 `None`——重新武装由此是现成的，不需要再记「上一帧看见没有」）。
    // `recon_flash_style` 记当前这闪是回报还是阵亡。新一局（`shell.attempt()`
    // 变化）时在主循环里归零，与 `popup` 它们同处。
    game::DemoBattle::ScoutOutcome scout_prev = game::DemoBattle::ScoutOutcome::None;
    rts::Tick recon_flash_until = 0;
    AlertStyle recon_flash_style = AlertStyle::Report;
    // 提前召唤的确认闪：N 被 demo 层受理（`summon_accepted_now`）时点亮几秒。
    // 「建造期没了」之前是无声的——闪一下让玩家知道是自己按的 N；
    // 没闪却跳阶段，就是另有 bug，留着这个区分当自诊断。
    rts::Tick summon_flash_until = 0;
    // 不用 constexpr：MSVC 对「只在内层 lambda 里用到的 constexpr 局部变量」
    // 误报 C4189（`popup_options` 那里实测过，同一处教训）。
    const rts::Tick kReconFlashTicks = 80;   // 4 秒 @ 20 Hz

    // 侦查警报的状态推进。**从绘制里拆出来、逐 tick 调用**：截图模式把 N 个
    // tick 推完才渲一帧，状态若只在 `draw_frame` 里推进，截图会把「早已灭掉
    // 的回报闪」画出来（实测：首版在 tick 3200/4000 的截图里仍挂着横幅——
    // 那不是 bug 现场，是状态从没被推进过）。窗口模式在补 tick 的循环里调，
    // 截图模式在逐 tick 推进时同步调，两条路径看到同一个警报状态。
    const auto advance_recon_alert = [&]() {
        game::DemoBattle* b = shell.battle();
        if (b == nullptr) return;
        const game::DemoBattle::ScoutOutcome oc = b->scout_outcome();
        if (oc != scout_prev) {
            // 只闪「有结论」的两个边沿；→`None` 是换波重置，不报。
            // `Killed → Success`（再派一只成功了）会再闪一次回报——
            // 那是新情报，该闪。
            if (oc == game::DemoBattle::ScoutOutcome::Success) {
                recon_flash_until = b->world().now() + kReconFlashTicks;
                recon_flash_style = AlertStyle::Report;
            } else if (oc == game::DemoBattle::ScoutOutcome::Killed) {
                recon_flash_until = b->world().now() + kReconFlashTicks;
                recon_flash_style = AlertStyle::Failed;
            }
            scout_prev = oc;
        }
        if (game::enemy_wraith_sighted(b->world().view(rts::Side::Defender))) {
            // 窥使警报优先，并吃掉回报/阵亡闪：别在它灭掉之后又补弹一条
            // ——玩家刚才盯着看的就是它，那是噪音不是情报。
            recon_flash_until = 0;
        }
    };
    const std::vector<rts::BldType>& buildable = game::buildable_types();
    const std::vector<rts::UnitType>& trainable = game::trainable_types();
    // 征兵等级。是个不常按的持久值，用 `[`/`]` 调，clamp 到
    // `[1, unit_level_cap()]`——上限会随堡垒等级涨，所以 clamp 每帧都按当前
    // 视图重算，不是建局时定死。（编队移除后，新兵没有归属可选——出兵即自主，
    // 见 `game/defender_script.hpp`。）
    int train_level_sel = 1;
    std::vector<rts::UnitId> selected;   // 框选 / 点选出的己方单位
    bool dragging = false;
    std::optional<std::size_t> placing;
    std::optional<rts::GridPos> placement_start;
    std::vector<game::BuildPreview> placement_preview;
    std::optional<rts::UnitId> dragged_garrison;   // 有值 = 从墙头拖兵；空 = 普通框选
    Vector2 drag_screen_start{};   // 屏幕坐标：用位移量判断「点」还是「拖」
    Vector2 drag_world_start{};    // 世界像素坐标：拖动结束时拼框选矩形
    constexpr float kDragThreshold = 6.0f;   // 像素；小于它算「点」不算「拖」
    bool paused = false;
    bool atmosphere_on = !opt.classic_visuals;
    render::BattleAtmosphere atmosphere;
    render::BattleAudio audio(opt.screenshot.empty());
    if(opt.mute) audio.toggle();
    render::ChronicleView journal;
    int reached_wave = shell.battle()?shell.battle()->world().wave():1;
    if(opt.journal_page>=0) { journal.preview(opt.journal_page,opt.journal_ending,opt.journal_bottom); reached_wave=70; }
    int enemy_report_wave=0;
    bool enemy_reported=false;
    bool developer_panel=opt.screen=="developer";
    render::FieldGuide guide;guide.preview(opt.guide_entry);guide.preview_level(opt.guide_level);guide.preview_bottom(opt.guide_bottom);
    std::string developer_wave_text="1";
    int story_wave_seen=shell.battle()?shell.battle()->world().wave():0;
    int story_attempt=shell.attempt();
    bool choice_presented = false;
    std::size_t known_chapters = game::chronicle_unlocked(reached_wave);
    int preloaded_attempt = 0;
    rts::Tick last_save_tick=shell.battle()?shell.battle()->world().now():0;
    int last_save_wave=shell.battle()?shell.battle()->world().wave():0;
    bool saved_terminal=shell.battle() && (shell.battle()->defeated() || shell.chronicle().completed());
    double storage_notice_until=GetTime()+12;
    double next_auto_save=0;
    const auto save_now=[&]() -> bool {
        if(!persistent || !shell.battle() || shell.battle()->developer()) return true;
        bool saved=false;
        next_auto_save=GetTime()+10;
        try {
            if(!storage_ready) throw std::runtime_error("存档目录不可用");
            game::write_archive(save_dir/"campaign.json",game::capture_battle(shell,map_json,stats_json));
            reached_wave=shell.battle()->world().wave();
            last_save_tick=shell.battle()->world().now();last_save_wave=shell.battle()->world().wave();
            saved_terminal=shell.battle()->defeated() || shell.chronicle().completed();
            storage_status="对局已保存";saved=true;
        } catch(const std::exception& e) {storage_status="保存失败，请检查存档目录";std::fprintf(stderr,"save: %s\n",e.what());}
        storage_notice_until=GetTime()+8;
        return saved;
    };
    const auto apply_menu=[&](game::MenuAction action) {
        if(action==game::MenuAction::Save) save_now();
        else {
            if(action==game::MenuAction::Quit && !save_now()) return;
            shell.apply(action);
            if(action==game::MenuAction::StartNew || action==game::MenuAction::Restart || action==game::MenuAction::ToMain || action==game::MenuAction::Quit)
                save_now();
        }
    };

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
    if (shell.battle() != nullptr) {
        cam.focus_keep(proj, shell.battle()->world().keep_pos(),
                       Vector2{static_cast<float>(opt.width), static_cast<float>(opt.height)});
    }

    // 弹出菜单的一个选项：屏幕坐标的一个矩形 + 显示文字 + 是否合法（决定
    // 描边颜色，接管了旧版按格描红/描绿那条提示的职责）。
    struct PopupOption {
        Rectangle box;
        std::string label;
        bool legal = true;
        // 点中这一项该发哪种命令、发给清单里的第几个——分发（点击处理那段）
        // 直接读这两个字段，不再各自重新按下标猜一遍「第 i 项是不是维修」。
        // 维修行可能插在 Train 弹窗最前面，插了之后练兵清单的下标就整体
        // 错了一位；把映射关系记在选项自己身上，才不会在两处分别算一遍
        // 偏移量、算歪了却没有测试能抓（同类教训见 `can_afford_*` 那次修复）。
        PopupKind action = PopupKind::None;
        int index = 0;
    };
    // 按当前弹窗种类摆出选项列表，纵向排开、每项一行。**造价与合法性都在
    // 这里算好**——`draw_frame` 只管画，点击命中检测也只读这份表，两处
    // 用同一份数据，不会出现「画的是绿的、点了却被拒」那种错位。
    const auto popup_options = [&](const Popup& p, const rts::WorldView& v) {
        std::vector<PopupOption> out;
        // **不用 constexpr**：MSVC 对「只在内层 lambda 里用到的 constexpr
        // 局部变量」会误报 C4189（已内联成立即数，外层变量看起来没被引用），
        // 实测过——换成 const 就没有这个假警告。
        const float kW = 240.0f, kH = 30.0f, kGap = 4.0f;
        const auto& stats = shell.battle()->world().stats();
        // 人口满（全局状态，与弹窗种类无关，所以在 switch 外算一次——case 里
        // 声明变量会被 C2360 拦）。只被 `Train` 弹窗用：选项照常列出但全部
        // 灰掉，并把原因印在标签里（同升级行印「先升堡垒」那条先例）。
        const bool pop_full = game::train_pop_full(v);
        const auto push = [&](std::string label, bool legal, PopupKind action, int index) {
            const float y = p.anchor.y + static_cast<float>(out.size()) * (kH + kGap);
            out.push_back(PopupOption{Rectangle{p.anchor.x, y, kW, kH}, std::move(label),
                                      legal, action, index});
        };
        char buf[96];
        // 「维修」这一行在 `Train` 弹窗（插队）与 `Repair` 弹窗（独占）里都会
        // 出现，标签格式抽成一份——两处各写一遍格式化字符串，正是「造价没
        // 印出来」这类 bug 的同源问题（一次试玩报出来的：维修选项只有
        // 「维修」两个字，看不出要花多少木材）。
        const auto push_repair = [&]() {
            const std::int64_t wood = game::repair_wood_cost(v, p.cell);
            std::snprintf(buf, sizeof(buf), "维修 (木%d)", static_cast<int>(wood));
            push(buf, game::can_repair_hint(v, p.cell) &&
                         game::can_afford_repair(v, p.cell),
                PopupKind::Repair, 0);
        };
        // 「升级」这一行与维修同构，但**显示条件比维修宽**：维修行只在掉血
        // 时出现，而这一行对己方任何完工建筑都出现，即使当前升不了。
        //
        // 理由是两者「为什么不能做」的可见性不同：满血 → 不用修，血条上看
        // 得见；而「顶到等级上限」在画面上一个字都没有。占位系数
        // `building_level_cap_divisor = 2` 下 1 级堡垒的上限就是 1 级，于是
        // **开局每一座建筑都升不了**——那一行若干脆不出现，玩家既不知道有
        // 升级这件事，也不知道该先去升堡垒。所以它照常出现、灰着，并把
        // 原因印在标签里（`game::upgrade_block` 就是为这句文案存在的）。
        const auto push_upgrade = [&]() {
            const game::UpgradeBlock why = game::upgrade_block(v, p.cell);
            if (why == game::UpgradeBlock::NoBuilding ||
                why == game::UpgradeBlock::Unbuilt) {
                return;   // 空地与工地上不谈升级
            }
            const int lv = static_cast<int>(game::bld_level_at(v, p.cell));
            switch (why) {
                case game::UpgradeBlock::LevelCap:
                    std::snprintf(buf, sizeof(buf), "升级 Lv%d (上限%d，先升堡垒)",
                                 lv, static_cast<int>(v.building_level_cap()));
                    break;
                case game::UpgradeBlock::Busy:
                    std::snprintf(buf, sizeof(buf), "升级 Lv%d (有工程在进行)", lv);
                    break;
                default:
                    // 箭头用 ASCII `->` 而不是 `→`：后者是一个要额外登记的
                    // 码点（见 `render/src/text.cpp` 的 ui_strings()），而它
                    // 在这个字号下也不比两个 ASCII 字符清楚。
                    std::snprintf(buf, sizeof(buf), "升级 Lv%d->%d (石%d 木%d)", lv,
                                 lv + 1,
                                 static_cast<int>(game::upgrade_cost_stone(v, p.cell)),
                                 static_cast<int>(game::upgrade_cost_wood(v, p.cell)));
                    break;
            }
            push(buf, why == game::UpgradeBlock::None &&
                         game::can_afford_upgrade(v, p.cell),
                PopupKind::Upgrade, 0);
        };
        const auto push_remove = [&]() {
            if (game::can_cancel_build_hint(v, p.cell)) {
                push("取消施工 (全额返还)", true, PopupKind::Cancel, 0);
            } else if (game::can_demolish_hint(v, p.cell)) {
                push("拆除 (返还80%)", true, PopupKind::Demolish, 0);
            }
        };
        switch (p.kind) {
            case PopupKind::Build:
                for (std::size_t i = 0; i < buildable.size(); ++i) {
                    const rts::BldType bt = buildable[i];
                    const rts::BldStats& s = stats.of(bt);
                    std::snprintf(buf, sizeof(buf), "%s (石%d 木%d)",
                                 std::string(game::display_name(bt)).c_str(),
                                 static_cast<int>(s.cost_stone),
                                 static_cast<int>(s.cost_wood));
                    // 此处只选建筑类型；落点与预算在放置预览里统一检查。
                    push(buf, true,
                        PopupKind::Build, static_cast<int>(i));
                }
                break;
            case PopupKind::Train:
                // 维修与练兵不互斥：掉血的兵营/堡垒既能修也能练兵，别把两者
                // 做成二选一（试玩报出来的 bug：受损后左键只弹维修，练兵
                // 入口直接消失）。维修永远排在第 0 项，升级紧随其后——
                // 升级同样不与另两者互斥（升 `Keep` 是抬高全城上限的唯一
                // 途径，而 `Keep` 恰好也是个能练兵的建筑）。
                if (game::can_repair_hint(v, p.cell)) {
                    push_repair();
                }
                push_upgrade();
                for (std::size_t i = 0; i < trainable.size(); ++i) {
                    const rts::UnitType ut = trainable[i];
                    // 造价随 `train_level_sel` 变——`[`/`]` 调的是这一格
                    // 弹窗里全部兵种共用的同一个等级，不是逐兵种各自的。
                    std::snprintf(buf, sizeof(buf), "%s Lv%d (金%d)%s",
                                 std::string(game::display_name(ut)).c_str(),
                                 train_level_sel,
                                 static_cast<int>(v.train_cost_gold(ut, train_level_sel)),
                                 pop_full ? " (人口满)" : "");
                    push(buf, !pop_full && game::can_train_hint(v, p.cell) &&
                                 game::can_afford_train(v, ut, train_level_sel),
                        PopupKind::Train, static_cast<int>(i));
                }
                push_remove();
                break;
            case PopupKind::Repair:
                push_repair();
                push_upgrade();
                push_remove();
                break;
            case PopupKind::Upgrade:
                // 点一座满血的墙/塔落到这里。维修行照 `can_repair_hint` 判，
                // 通常不出现（满血），但写上它是为了一件事：这三种弹窗里
                // 「一座建筑能做哪些事」的清单只由 `can_*_hint` 决定，不由
                // 「玩家点出来的是哪一种弹窗」决定。少了这一行，一座刚掉血
                // 的塔会因为拾取顺序落进 Upgrade 弹窗而看不到维修入口——
                // 正是「受损后练兵入口消失」那个 bug 的同一个形状。
                if (game::can_repair_hint(v, p.cell)) {
                    push_repair();
                }
                push_upgrade();
                push_remove();
                break;
            case PopupKind::Cancel:
            case PopupKind::Demolish:
                push_remove();
                break;
            case PopupKind::None:
                break;
        }
        // Lay out once; drawing and hit testing use the same clamped rectangles.
        if (!out.empty()) {
            const float margin = 12.0f;
            float label_width = kW;
            for (const auto& option : out)
                label_width = std::max(label_width, font->measure(option.label, kHudSize*0.8f).x+20.0f);
            const float width = std::min(label_width, std::max(1.0f, GetScreenWidth()-2*margin));
            const float available = std::max(1.0f, GetScreenHeight()-2*margin-76.0f);
            const float stride = std::min(kH+kGap, available/static_cast<float>(out.size()));
            const float height = stride*static_cast<float>(out.size());
            const float x = std::clamp(p.anchor.x, margin,
                                      std::max(margin, GetScreenWidth()-margin-width));
            const float y = std::clamp(p.anchor.y, margin,
                                      std::max(margin, GetScreenHeight()-margin-height-76.0f));
            for (std::size_t i = 0; i < out.size(); ++i)
                out[i].box = Rectangle{x, y+stride*static_cast<float>(i), width,
                                       std::max(1.0f, stride-kGap)};
        }
        return out;
    };

    // 一帧的绘制。**截图模式与窗口模式共用它**——两份各画一遍的话，
    // 截图回归测试保住的就不是玩家真正看到的那个画面。
    const auto draw_frame = [&](Vector2 vp, bool has_cursor, rts::GridPos cell) {
        const game::DemoBattle* b = shell.battle();
        ClearBackground(bg);
        // 血条按屏幕尺寸画，所以每帧把当前缩放告诉渲染器（见 set_screen_scale）。
        renderer.set_screen_scale(1.0f / cam.camera().zoom);
        renderer.set_presentation(atmosphere_on,b?static_cast<float>(b->world().now())/rts::kTicksPerSecond:static_cast<float>(GetTime()));
        BeginMode2D(cam.camera());
        if (b != nullptr) {
            renderer.draw_ground(tiles);
            if(atmosphere_on) atmosphere.draw_ground(proj);
            renderer.draw_objects(game::BattleScene::sorted(
                                     map,b->world().view(rts::Side::Defender),b->world().now()));
        } else {
            // 主菜单也画地图：那张图本身就是最好的背景，而且它让「素材载入
            // 成功了没有」在第一屏就看得见。
            renderer.draw(static_scene);
        }
        if (shell.screen() == game::Screen::Battle && b != nullptr && has_cursor &&
            map.in_bounds(cell.i, cell.j) && popup.kind == PopupKind::None) {
            // 没有菜单开着时，描边只是白色悬停框——合法性提示挪进了弹窗里
            // 逐项显示（见下面 popup_options），不再需要按格上色。
            overlay.draw_cell_outline(cell, Color{235, 235, 245, 255},
                                      2.0f / cam.camera().zoom);
        }
        if (shell.screen() == game::Screen::Battle && b && placing) {
            const auto type=buildable[*placing];
            const auto view=b->world().view(rts::Side::Defender);
            std::vector<rts::GridPos> planned;
            for (const auto& item:placement_preview) if(item.legal) planned.push_back(item.cell);
            for (const auto& item : placement_preview) {
                const Color tint=!item.legal ? Color{240,85,85,135} : item.affordable ? Color{110,235,160,150} : Color{145,145,145,115};
                auto facing=game::Facing::SE;
                if(type==rts::BldType::Wall || type==rts::BldType::Gate || type==rts::BldType::Fence) {
                    facing=game::SceneModel::run_direction(view,item.cell,planned,type);
                }
                const auto& sprite=atlas.get(rts::ident_of(type),"idle",game::to_string(facing));
                const auto anchor=proj.grid_to_screen(item.cell);
                DrawTextureV(sprite.texture,{anchor.x-sprite.ground_anchor.x,anchor.y-sprite.ground_anchor.y},tint);
                if((type==rts::BldType::Wall || type==rts::BldType::Fence) &&
                    game::SceneModel::is_wall_corner(view,item.cell,planned,type)) {
                    const auto& corner=atlas.get(rts::ident_of(type),"idle","SE");
                    DrawTextureV(corner.texture,{anchor.x-corner.ground_anchor.x,anchor.y-corner.ground_anchor.y},tint);
                }
                overlay.draw_cell_outline(item.cell,tint,2.0f/cam.camera().zoom);
            }
        }
        if (shell.screen() == game::Screen::Battle && b != nullptr) {
            const rts::WorldView v = b->world().view(rts::Side::Defender);
            // 选中集：每个单位脚下画一个小圈，框选/点选的即时反馈。
            const auto ualive = v.unit_alive();
            std::vector<rts::UnitId> workers;
            b->world().enumerate_units(rts::Side::Defender, workers);
            for (const auto id : workers) {
                if (!b->forced_work_active(id)) continue;
                const auto anchor = unit_draw_anchor(v, id.index(), proj, atlas);
                const float scale = 1.0f / cam.camera().zoom;
                DrawCircleV({anchor.x, anchor.y - 28 * scale}, 6 * scale, Color{255, 160, 55, 245});
                DrawLineEx({anchor.x, anchor.y - 32 * scale}, {anchor.x, anchor.y - 27 * scale}, 2 * scale, BLACK);
            }
            for (const rts::UnitId id : selected) {
                const std::size_t s = id.index();
                if (s >= ualive.size() || !ualive[s]) continue;
                const rts::Vec2 sp = unit_draw_anchor(v, s, proj, atlas);
                const float radius = 10.0f / cam.camera().zoom;
                DrawRing(Vector2{sp.x, sp.y}, radius, radius+2.0f/cam.camera().zoom,
                         0, 360, 32, Color{255, 214, 120, 235});
            }
            if (inspected) overlay.draw_cell_outline(*inspected, Color{255,214,120,255},
                                                     3.0f/cam.camera().zoom);
            if(inspected) {
                for(std::size_t k=0;k<v.bld_pos().size();++k) {
                    if(!v.bld_alive()[k] || v.bld_pos()[k]!=*inspected || !v.bld_built()[k]) continue;
                    const auto type=v.bld_type()[k];
                    if(type!=rts::BldType::Tower && type!=rts::BldType::Flak) continue;
                    const float range=v.stats().of(type).range;
                    const auto center=rts::center_of(*inspected);
                    const Color color=type==rts::BldType::Flak?Color{110,215,245,210}:Color{246,207,116,210};
                    rts::Vec2 last=proj.world_to_screen({center.x+range,center.y});
                    for(int segment=1;segment<=96;++segment) {
                        const float angle=static_cast<float>(segment)*6.283185307f/96.0f;
                        const auto next=proj.world_to_screen({center.x+range*std::cos(angle),center.y+range*std::sin(angle)});
                        DrawLineEx({last.x,last.y},{next.x,next.y},2.0f/cam.camera().zoom,color);last=next;
                    }
                }
            }
            if (order_target && GetTime() < order_until) {
                const auto p = proj.grid_to_screen(*order_target);
                const float r = 13.0f/cam.camera().zoom;
                DrawRing(Vector2{p.x,p.y}, r, r+2.0f/cam.camera().zoom, 0,360,32,
                         Color{100,220,245,230});
                DrawLineEx(Vector2{p.x-r,p.y}, Vector2{p.x+r,p.y},
                           2.0f/cam.camera().zoom, Color{100,220,245,230});
            }
            // 空地起手是框选；墙头士兵起手则画一条调兵线，终点就是下墙后的
            // 单兵目标格。两种手势共用左键拖拽，但反馈形状明确区分。
            if (dragging && dragged_garrison) {
                const Vector2 cur = GetScreenToWorld2D(GetMousePosition(), cam.camera());
                const std::size_t s = dragged_garrison->index();
                if (s < ualive.size() && ualive[s]) {
                    const rts::Vec2 from = unit_draw_anchor(v, s, proj, atlas);
                    DrawLineEx(Vector2{from.x, from.y}, cur, 3.0f / cam.camera().zoom,
                               Color{255, 214, 120, 220});
                    DrawCircleLines(static_cast<int>(cur.x), static_cast<int>(cur.y), 10.0f,
                                    Color{255, 214, 120, 220});
                }
            } else if (dragging) {
                const Vector2 cur = GetScreenToWorld2D(GetMousePosition(), cam.camera());
                const float x0 = drag_world_start.x < cur.x ? drag_world_start.x : cur.x;
                const float y0 = drag_world_start.y < cur.y ? drag_world_start.y : cur.y;
                const float x1 = drag_world_start.x > cur.x ? drag_world_start.x : cur.x;
                const float y1 = drag_world_start.y > cur.y ? drag_world_start.y : cur.y;
                DrawRectangleLines(static_cast<int>(x0), static_cast<int>(y0),
                                   static_cast<int>(x1 - x0), static_cast<int>(y1 - y0),
                                   Color{120, 220, 255, 220});
            }
        }
        if(b && atmosphere_on) atmosphere.draw_world(proj,atlas,cam.camera().zoom);
        EndMode2D();
        if(atmosphere_on) atmosphere.draw_screen(vp,*font);
        if(journal.open) { journal.draw(*font,vp,shell.battle() && shell.battle()->developer()?70:reached_wave); return; }
        if(GetTime()<storage_notice_until && !storage_status.empty() && shell.screen()==game::Screen::Battle)
            font->draw(storage_status,{24,vp.y-104},20,Color{239,217,165,255});

        if (shell.screen() == game::Screen::Battle && b != nullptr) {
            draw_battle_hud(*font, *b, paused, selected.size());
            const float screen_w = static_cast<float>(GetScreenWidth());
            const float screen_h = static_cast<float>(GetScreenHeight());
            DrawRectangleRec(Rectangle{0,screen_h-76,screen_w,76},Color{23,28,30,245});
            const auto buttons=battle_buttons(screen_w,screen_h);
            const std::array<std::string_view,6> labels{
                "菜单",paused ? "继续" : "暂停", "回到堡垒", "查看全图", "指挥官日记", "建造 B"};
            for(std::size_t i=0;i<buttons.size();++i) {
                const bool hover=has_cursor && CheckCollisionPointRec(GetMousePosition(),buttons[i]);
                DrawRectangleRec(buttons[i], hover ? Color{77,74,56,255}:Color{42,48,46,255});
                DrawRectangleLinesEx(buttons[i],1,Color{124,116,86,255});
                font->draw(std::string(labels[i]),rts::Vec2{buttons[i].x+15,buttons[i].y+9},22,
                           Color{244,228,194,255});
            }
            char hint[160];
            if (placing) {
                const auto type=buildable[*placing];
                const auto& cost=b->world().view(rts::Side::Defender).stats().of(type);
                const auto stock=b->world().view(rts::Side::Defender).stock();
                const auto count=static_cast<std::int64_t>(placement_preview.size());
                const std::string text="建造："+std::string(game::display_name(type))+"  "+std::to_string(count)+
                    " 段/座  石 "+std::to_string(count*cost.cost_stone)+"/"+std::to_string(stock[static_cast<std::size_t>(rts::Resource::Stone)])+
                    "  木 "+std::to_string(count*cost.cost_wood)+"/"+std::to_string(stock[static_cast<std::size_t>(rts::Resource::Wood)])+
                    "   Tab 换建筑 · Shift 换轴 · 右键/Esc 退出";
                DrawRectangle(0,static_cast<int>(screen_h)-108,static_cast<int>(screen_w),32,Color{23,28,30,245});
                font->draw(text,{14,screen_h-102},18,Color{244,228,194,255});
            }
            const float hint_x=buttons.back().x+buttons.back().width+22;
            if(screen_w-hint_x>420) {
                std::snprintf(hint,sizeof(hint),"已选 %zu 名   右键移动 / 驻墙",selected.size());
                font->draw(hint,rts::Vec2{hint_x,screen_h-60},20,Color{218,221,207,255});
                std::snprintf(hint,sizeof(hint),"J 日记  V 氛围  M 声音  [/] 征兵等级 %d",train_level_sel);
                font->draw(hint,rts::Vec2{hint_x,screen_h-33},18,Color{162,174,167,255});
            }
            const auto info=b->world().view(rts::Side::Defender);
            if(inspected) {
                for(std::size_t k=0;k<info.bld_pos().size();++k) {
                    if(!info.bld_alive()[k] || info.bld_pos()[k]!=*inspected) continue;
                    const auto box=inspector_box({screen_w,screen_h});
                    const float y=box.y;
                    DrawRectangleRec(box,Color{23,28,30,238});
                    std::snprintf(hint,sizeof(hint),"%s   Lv%d   %lld / %lld",
                        std::string(game::display_name(info.bld_type()[k])).c_str(),info.bld_level()[k],
                        static_cast<long long>(info.bld_hp()[k]),static_cast<long long>(info.bld_max_hp()[k]));
                    font->draw(hint,rts::Vec2{30,y+14},24,Color{244,228,194,255});
                    std::string status="可用：点击建筑查看维修、升级或招募";
                    if(info.bld_type()[k]==rts::BldType::Tower || info.bld_type()[k]==rts::BldType::Flak) {
                        status="射程 "+std::to_string(static_cast<int>(info.stats().of(info.bld_type()[k]).range))+" 格 · "+(info.bld_type()[k]==rts::BldType::Flak?"仅对空":"仅对地");
                    }
                    int ticks=0;
                    bool engineering = true;
                    if(!info.bld_built()[k]) {status="建造中";ticks=info.bld_work_left()[k];}
                    else if(info.bld_work_left()[k]>0) {status="维修中";ticks=info.bld_work_left()[k];}
                    else if(info.bld_upgrade_left()[k]>0) {status="升级中";ticks=info.bld_upgrade_left()[k];}
                    else if(info.bld_train_type()[k]!=rts::kNoTrain) {
                        engineering = false;
                        ticks=info.bld_train_left()[k];
                        status=ticks>0 ? "招募中" : "招募完成，等待空闲出兵位置";
                    }
                    if(ticks>0) status+=(engineering ? " · 剩余单人工时 " : " · 剩余 ")+
                        std::to_string((ticks+rts::kTicksPerSecond-1)/rts::kTicksPerSecond)+" 秒";
                    font->draw(status,rts::Vec2{30,y+51},22,Color{157,204,174,255});
                    font->draw(engineering && ticks>0 ? "需工兵到场施工，多人可加速" : "金色边框标记当前建筑",rts::Vec2{30,y+84},18,Color{166,178,168,255});
                    break;
                }
            }
            if(!notice.empty() && GetTime()<notice_until) {
                const float y=screen_h-250;
                const float width=std::min(screen_w-28,font->measure(notice,20).x+32);
                DrawRectangleRec(Rectangle{14,y,width,32},Color{31,47,42,245});
                font->draw(notice,rts::Vec2{28,y+5},20,Color{240,226,192,255});
            }
            // 侦查面板（右上角）：**本波**侦查到的来袭编成 + 克制提示。
            draw_intel_panel(*font, *b, GetScreenWidth());

            const rts::WorldView dv = b->world().view(rts::Side::Defender);
            // 警报横幅（顶中）：提前召唤确认闪 > 窥使入境常驻 > 斥候回报闪。
            // **状态不在这里推进**（见 `advance_recon_alert`——截图模式一步
            // 推 N 个 tick，状态推进若绑在绘制上，截图会画出早已灭掉的闪）；
            // 这里只按当前状态画。召唤确认排最前：它是玩家自己操作的回执，
            // 只亮几秒，另两条让位给它不会漏掉真正的新信息。
            const rts::Tick alert_now = b->world().now();
            if (alert_now < summon_flash_until) {
                draw_alert_banner(*font, GetScreenWidth(), alert_now,
                                  AlertStyle::Summon);
            } else if (game::enemy_wraith_sighted(dv)) {
                draw_alert_banner(*font, GetScreenWidth(), alert_now,
                                  AlertStyle::Threat);
            } else if (alert_now < recon_flash_until) {
                draw_alert_banner(*font, GetScreenWidth(), alert_now,
                                  recon_flash_style);
            }

            // 弹出菜单：屏幕坐标，画在点开那一刻的位置。**造价写在选项里**——
            // 「点了没反应」最常见的真因是买不起，把价钱摆在眼前比事后猜便宜。
            if (popup.kind != PopupKind::None) {
                const std::vector<PopupOption> opts =
                    popup_options(popup, b->world().view(rts::Side::Defender));
                for (const PopupOption& o : opts) {
                    const bool ok = o.legal;
                    DrawRectangleRec(o.box, ok ? Color{40, 70, 40, 235}
                                              : Color{70, 40, 40, 200});
                    DrawRectangleLinesEx(o.box, 1.5f,
                                        ok ? Color{120, 220, 120, 255}
                                          : Color{160, 90, 90, 255});
                    font->draw(o.label,
                              rts::Vec2{o.box.x + 8.0f, o.box.y + 4.0f},
                              std::min(kHudSize * 0.8f, o.box.height-6.0f),
                              Color{230, 230, 235, 255});
                }
                if (!opts.empty()) font->draw("右键 / Esc 关闭菜单",
                          rts::Vec2{opts.front().box.x,
                                    std::max(2.0f, opts.front().box.y-kHudLine)},
                          kHudSize * 0.8f, Color{170, 175, 190, 255});
            }
            return;
        }

        // 菜单几屏：先压暗，再画面板。主菜单压得重一些（后面没有正在发生的事，
        // 压暗让面板成为唯一焦点）；暂停与败局压得轻，好让人还能看清战场。
        if(developer_panel) {render::draw_developer(*font,developer_wave_text);return;}
        if(shell.screen()==game::Screen::Guide) {guide.draw(*font,atlas,stats,vp,shell.battle()?&shell.battle()->world():nullptr);return;}
        menu_view.dim(vp, shell.screen() == game::Screen::Main ? 150 : 140);
        if(shell.screen()==game::Screen::Main && atmosphere_on && vp.x>=1150) {
            const Vector2 crest{vp.x*0.77f,vp.y*0.42f};
            const float radius=std::min(vp.x*0.14f,vp.y*0.25f);
            for(int ring=0;ring<3;++ring) DrawCircleLines(static_cast<int>(crest.x),static_cast<int>(crest.y),radius-static_cast<float>(ring)*12,Color{189,164,112,static_cast<unsigned char>(100-ring*24)});
            DrawLineEx({crest.x,crest.y-radius*1.20f},{crest.x,crest.y+radius*1.2f},2,Color{191,164,112,155});
            DrawLineEx({crest.x-radius*0.58f,crest.y},{crest.x+radius*0.58f,crest.y},2,Color{191,164,112,155});
            font->draw("第 47 任守城指挥官",{crest.x-radius,vp.y*0.78f},22,Color{227,211,174,255});
            font->draw("这里太安静了。",{crest.x-radius,vp.y*0.83f},20,Color{163,174,178,255});
        }
        if(shell.screen()!=game::Screen::Help) {
            DrawRectangleLinesEx({vp.x-190,20,170,42},1,Color{170,145,92,255});
            font->draw("指挥官日记",{vp.x-177,30},22,Color{228,214,181,255});
        }
        const ScreenText st = screen_text(shell.screen());
        const std::string sub = screen_subtitle(shell);
        const render::MenuView::Chrome chrome{st.title, sub, st.footer,
                                              align_of(shell.screen())};
        if (shell.screen() == game::Screen::Help) {
            menu_view.draw_help(shell.menu(), chrome, vp);
        } else {
            menu_view.draw(shell.menu(), chrome, vp);
        }
        if(GetTime()<storage_notice_until && !storage_status.empty())
            font->draw(storage_status,{24,vp.y-28},18,Color{239,217,165,255});
    };

    if (!opt.screenshot.empty()) {
        // 截图模式：先把仿真推到要看的那一刻，再渲一帧导出。**逐 tick 推**
        // （不是一步推完）：侦查警报的状态要跟每个 tick 同步走，否则截图会
        // 把早已灭掉的回报闪画出来（见 `advance_recon_alert`）。
        if (game::DemoBattle* b = shell.battle()) {
            for (int i = 0; i < opt.ticks; ++i) {
                b->update(1);
                atmosphere.observe(b->world().view(rts::Side::Defender),b->world().now(),b->world().wave());
                advance_recon_alert();
            }
        }
        shell.poll();
        if (shell.attempt() != preloaded_attempt) preload_for_battle();
        const Vector2 vp{static_cast<float>(opt.width), static_cast<float>(opt.height)};
        if (opt.placement_shot && shell.battle()) {
            if (!map.in_bounds(19,6)) return 2;
            placing=std::size_t{0}; placement_start=rts::GridPos{8,6};
            placement_preview=game::preview_build(shell.battle()->world().view(rts::Side::Defender),
                rts::BldType::Wall,game::build_line(*placement_start,{19,6},false));
            cam.focus_keep(proj,{13,6},vp);
        }
        if(shell.battle()) atmosphere.observe(shell.battle()->world().view(rts::Side::Defender),
                                              shell.battle()->world().now(),shell.battle()->world().wave());
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
            cam.focus_keep(proj, shell.battle()->world().keep_pos(), vp);
            paused = false;
            atmosphere.reset();
            audio.active(false);
            choice_presented = false;
            journal=render::ChronicleView{};
            reached_wave=shell.battle()->world().wave();
            known_chapters=game::chronicle_unlocked(reached_wave);
            enemy_report_wave=0;enemy_reported=false;
            popup = Popup{};
            selected.clear();
            dragging = false;
            dragged_garrison.reset();
            inspected.reset();
            inspected_click.reset();
            building_cycle.reset();
            order_target.reset();
            notice.clear();
            inspected_busy=false;
            scout_prev = game::DemoBattle::ScoutOutcome::None;
            recon_flash_until = 0;
            summon_flash_until = 0;
        }

        const Vector2 mouse = GetMousePosition();
        const bool in_menu = shell.screen() != game::Screen::Battle;
        if (in_menu || journal.open) {
            placing.reset(); placement_start.reset(); placement_preview.clear();
        }
        rts::GridPos cell{};
        audio.active(shell.screen()==game::Screen::Main ||
                     (shell.should_advance() && !paused && !journal.open && !developer_panel));
        if(!developer_panel && IsKeyPressed(KEY_M)) {
            audio.toggle();
            notice=!audio.ready()?"声音设备不可用":audio.muted()?"声音已关闭":"声音已开启";
            notice_until=GetTime()+3;
        }

        if(IsKeyPressed(KEY_F12) && (IsKeyDown(KEY_LEFT_ALT)||IsKeyDown(KEY_RIGHT_ALT))) {
            if(!shell.battle() || shell.battle()->defeated() || shell.chronicle().completed()) shell.apply(game::MenuAction::StartNew);
            if(shell.screen()!=game::Screen::Battle) shell.apply(game::MenuAction::Resume);
            if(!shell.battle()->developer()) {save_now();shell.battle()->enable_developer();journal.open=false;}
            developer_panel=!developer_panel;developer_wave_text.clear();
        }
        if(developer_panel) {
            audio.active(false);
            for(int ch=GetCharPressed();ch>0;ch=GetCharPressed()) if(ch>='0'&&ch<='9'&&developer_wave_text.size()<4) developer_wave_text+=static_cast<char>(ch);
            if(IsKeyPressed(KEY_BACKSPACE)&&!developer_wave_text.empty()) developer_wave_text.pop_back();
            if((IsKeyPressed(KEY_ENTER) || (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse,render::developer_apply_button(vp))))&&!developer_wave_text.empty()) {
                const int wave=std::stoi(developer_wave_text);
                if(wave>=1) {shell.battle()->developer_wave(wave);developer_panel=false;acc=0;}
            }
            if(IsKeyPressed(KEY_ESCAPE)) developer_panel=false;
            BeginDrawing();ClearBackground(Color{28,34,37,255});
            render::draw_developer(*font,developer_wave_text);
            EndDrawing();acc=0;continue;
        }
        if(shell.battle() && !shell.battle()->developer()) {
            const int wave=shell.battle()->world().wave();
            if(story_attempt!=shell.attempt()) {
                story_attempt=shell.attempt();story_wave_seen=0;
                journal=render::ChronicleView{};
                known_chapters=game::chronicle_unlocked(wave);
            }
            const int chapter=game::chronicle_to_present(story_wave_seen,wave,false);
            if(shell.screen()==game::Screen::Battle && chapter>=0) {
                journal.preview(chapter);
                popup=Popup{};dragging=false;dragged_garrison.reset();
            }
            story_wave_seen=wave;
            reached_wave=wave;
            const auto count=game::chronicle_unlocked(reached_wave);
            if(count>known_chapters) {notice="日记已更新 · 按 J 阅读";notice_until=GetTime()+6;known_chapters=count;}
        }
        if(shell.battle() && !shell.battle()->developer()) {
            const int wave=shell.battle()->world().wave();
            if(enemy_report_wave!=wave) {enemy_report_wave=wave;enemy_reported=false;}
            if(shell.battle()->wave_scouted() && !enemy_reported) {
                notice="窥使侦查成功：敌军下一波将参考当前城防";notice_until=GetTime()+10;enemy_reported=true;
            }
        }
        if(IsKeyPressed(KEY_F5)) {
            if(shell.battle() && shell.battle()->developer()) {notice="开发者对局不保存";notice_until=GetTime()+6;}
            else save_now();
        }
        journal.decision_enabled=shell.battle() && !shell.battle()->developer() && !shell.battle()->defeated() &&
            shell.chronicle().pending(shell.battle()->world().wave());
        if(journal.decision_enabled && !choice_presented) {
            journal.preview(7);choice_presented=true;
            popup=Popup{};dragging=false;dragged_garrison.reset();
        }
        if(!journal.open && IsKeyPressed(KEY_V)) atmosphere_on=!atmosphere_on;
        if(shell.screen()==game::Screen::Guide) {
            if(guide.input(vp)) shell.apply(game::MenuAction::Back);
        } else if(journal.open) {
            journal.update(vp,shell.battle() && shell.battle()->developer()?70:reached_wave);
            const auto choice=journal.take_choice();
            if(choice!=game::ChronicleChoice::None) {
                shell.choose_chronicle(choice);
                save_now();
                journal.decision_enabled=false;
            }
        } else if(IsKeyPressed(KEY_J) || (in_menu && shell.screen()!=game::Screen::Help && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                   CheckCollisionPointRec(mouse,{vp.x-190,20,170,42}))) {
            journal.open=true; popup=Popup{}; dragging=false; dragged_garrison.reset();
        } else if (in_menu) {
            if(shell.screen()==game::Screen::Help) {
                float scroll=GetMouseWheelMove()*60;
                if(IsKeyPressed(KEY_PAGE_DOWN)) scroll-=vp.y*0.6f;
                if(IsKeyPressed(KEY_PAGE_UP)) scroll+=vp.y*0.6f;
                menu_view.scroll_help(scroll);
            }
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
                apply_menu(shell.menu().action_at(hovered));
            } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
                       IsKeyPressed(KEY_SPACE)) {
                apply_menu(shell.menu().confirm());
            } else if (IsKeyPressed(KEY_ESCAPE)) {
                shell.on_escape();
            }
        } else if (game::DemoBattle* b = shell.battle()) {
            const bool on_toolbar=mouse.y>=vp.y-(placing?108:76);
            const bool on_hud=mouse.x>=14 && mouse.x<=714 && mouse.y>=14 && mouse.y<=150;
            bool has_inspector = false;
            const auto input_view = b->world().view(rts::Side::Defender);
            for (std::size_t k=0; inspected && k<input_view.bld_pos().size(); ++k)
                if (input_view.bld_alive()[k] && input_view.bld_pos()[k]==*inspected)
                    has_inspector = true;
            const bool on_inspector=has_inspector && CheckCollisionPointRec(mouse,inspector_box(vp));
            const auto show_full_map = [&] {
                cam.fit(proj, map.width(), map.height(), Vector2{vp.x, std::max(1.0f,vp.y-152.0f)});
                cam.set_viewport(vp);
            };
            if(popup.kind==PopupKind::None && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && on_toolbar) {
                const auto buttons=battle_buttons(vp.x,vp.y);
                if(CheckCollisionPointRec(mouse,buttons[0])) shell.on_escape();
                if(CheckCollisionPointRec(mouse,buttons[1])) paused=!paused;
                if(CheckCollisionPointRec(mouse,buttons[2])) cam.focus_keep(proj,b->world().keep_pos(),vp);
                if(CheckCollisionPointRec(mouse,buttons[3])) show_full_map();
                if(CheckCollisionPointRec(mouse,buttons[4])) journal.open=true;
                if(CheckCollisionPointRec(mouse,buttons[5])) { placing=std::size_t{0}; placement_start.reset(); popup=Popup{}; }
                dragging=false;
                dragged_garrison.reset();
            }
            const bool cancel_placement = placing && (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT));
            if (cancel_placement) { placing.reset(); placement_start.reset(); placement_preview.clear(); }
            else if (IsKeyPressed(KEY_ESCAPE)) shell.on_escape();
            if (IsKeyPressed(KEY_B)) { placing=std::size_t{0}; placement_start.reset(); popup=Popup{}; dragging=false; dragged_garrison.reset(); }
            if (placing && IsKeyPressed(KEY_TAB)) { *placing=(*placing+1)%buildable.size(); placement_start.reset(); }
            if (IsKeyPressed(KEY_F)) show_full_map();
            if (IsKeyPressed(KEY_SPACE)) paused = !paused;
            // 征兵等级。下限钳在这里（1，恒合法）；
            // 上限用 `b->world()` 现算，不用 `view`——这段代码跑在 `view`
            // 构造之前（见下面「拾取」那段），提前引用会是一个悬垂读取。
            if (IsKeyPressed(KEY_LEFT_BRACKET)) {
                train_level_sel = train_level_sel > 1 ? train_level_sel - 1 : 1;
            }
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) {
                const int cap = static_cast<int>(b->world().unit_level_cap());
                train_level_sel = train_level_sel < cap ? train_level_sel + 1 : cap;
            }
            if (IsKeyPressed(KEY_N)) {
                rts::Command c;
                c.kind = rts::CommandKind::Summon;
                c.side = rts::Side::Defender;
                // 反馈必须与受理用同一条判据，否则会骗玩家（闪了却没开打，
                // 或开打了却没闪）。护栏期内丢掉的 N 不闪——本来就没生效。
                if (b->summon_accepted_now()) {
                    summon_flash_until = b->world().now() + kReconFlashTicks;
                }
                b->submit_defender(&c, 1);
            }
            cam.update(GetFrameTime());

            // 拾取（与地图查看器同一条链路：screen → world → grid）。
            const Vector2 wpos = GetScreenToWorld2D(mouse, cam.camera());
            cell = proj.screen_to_grid(rts::Vec2{wpos.x, wpos.y});
            const bool in_map = map.in_bounds(cell.i, cell.j) && !on_toolbar && !on_hud && !on_inspector;
            const rts::WorldView view = b->world().view(rts::Side::Defender);

            building_cycle.observe({mouse.x,mouse.y},{wpos.x,wpos.y});
            if(!in_map || placing) building_cycle.reset();
            // Clicking outside the popup is also a new map click, not a swallowed dismissal.
            if(popup.kind!=PopupKind::None && in_map && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                const auto options=popup_options(popup,view);
                const bool on_option=std::any_of(options.begin(),options.end(),[&](const PopupOption& o) {
                    return CheckCollisionPointRec(mouse,o.box);
                });
                if(!on_option) popup=Popup{};
            }

            placement_preview.clear();
            if (placing) {
                const auto type = buildable[*placing];
                const bool line = type == rts::BldType::Wall || type == rts::BldType::Fence;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && in_map) placement_start=cell;
                if (in_map || placement_start) {
                    const rts::GridPos end{static_cast<std::int16_t>(std::clamp(int(cell.i),0,map.width()-1)),
                                           static_cast<std::int16_t>(std::clamp(int(cell.j),0,map.height()-1))};
                    const auto cells = line && placement_start ? game::build_line(*placement_start,end,
                        IsKeyDown(KEY_LEFT_SHIFT)||IsKeyDown(KEY_RIGHT_SHIFT)) : std::vector<rts::GridPos>{end};
                    placement_preview=game::preview_build(view,type,cells);
                }
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && placement_start) {
                    if (in_map) {
                        std::vector<rts::Command> commands;
                        for (const auto& item : placement_preview) if (item.legal && item.affordable)
                            commands.push_back(game::build_command(type,item.cell,map.width()));
                        if (!commands.empty()) b->submit_defender(commands.data(),commands.size());
                        notice="建造指令已提交："+std::to_string(commands.size())+" 段 / 座";
                        notice_until=GetTime()+3;
                    }
                    placement_start.reset();
                }
            } else if (cancel_placement) {
                // Consume cancellation so the same right click cannot issue unit orders.
            } else if (popup.kind != PopupKind::None) {
                // 菜单开着时，鼠标只做两件事：点选项生效 / 右键关闭。
                // 不碰框选与下令——两套输入不该在同一帧里叠着解释。
                if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                    popup = Popup{};
                } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    const std::vector<PopupOption> opts = popup_options(popup, view);
                    for (std::size_t i = 0; i < opts.size(); ++i) {
                        if (!opts[i].legal || !CheckCollisionPointRec(mouse, opts[i].box)) {
                            continue;
                        }
                        // 深层裁决（资源够不够、点位规则）在 World 的解算里；
                        // 这里只把命令发出去，`legal` 只是同一份判据算出来的
                        // 展示层提示，不是第二套规则。
                        //
                        // 按 `opts[i].action`/`.index` 分发，不按 `popup.kind`
                        // 与「第 i 项」重新配对——Train 弹窗里维修行可能插在
                        // 最前面，此时第 0 项是 Repair、第 1 项才是
                        // `trainable[0]`，这份对应关系只在 `popup_options`
                        // 算过一遍，两处各算一遍必然有一天算歪。
                        rts::Command c;
                        bool have = true;
                        const std::size_t opt_idx = static_cast<std::size_t>(opts[i].index);
                        switch (opts[i].action) {
                            case PopupKind::Build:
                                placing=opt_idx;
                                placement_start.reset();
                                dragging=false;
                                dragged_garrison.reset();
                                have=false;
                                break;
                            case PopupKind::Train:
                                c = game::train_command(
                                    trainable[opt_idx],
                                    train_level_sel, popup.cell, map.width());
                                break;
                            case PopupKind::Repair:
                                c = game::repair_command(popup.cell, map.width());
                                break;
                            case PopupKind::Upgrade:
                                c = game::upgrade_command(popup.cell, map.width());
                                break;
                            case PopupKind::Cancel:
                                c = game::cancel_build_command(popup.cell, map.width());
                                break;
                            case PopupKind::Demolish:
                                c = game::demolish_command(popup.cell, map.width());
                                break;
                            case PopupKind::None:
                                have = false;
                                break;
                        }
                        if (have) {
                            b->submit_defender(&c, 1);
                            inspected = popup.cell;
                            inspected_busy=false;
                            notice = "指令已提交：" + opts[i].label;
                            notice_until = GetTime()+3.0;
                        }
                        break;
                    }
                    popup = Popup{};   // 点中选项或点在菜单外：都关掉
                }
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                // 墙头士兵按其**画出来的位置**拾取；命中它就是单兵拖动，
                // 否则地图内照旧进入框选/点建筑流程。
                std::vector<rts::UnitId> defenders;
                b->world().enumerate_units(rts::Side::Defender, defenders);
                dragged_garrison = in_map ? pick_garrisoned_unit(
                    view, defenders, proj, atlas, wpos) : std::nullopt;
                if (dragged_garrison || in_map) {
                    dragging = true;
                    drag_screen_start = mouse;
                    drag_world_start = wpos;
                    if (dragged_garrison) selected = {*dragged_garrison};
                }
            } else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && dragging) {
                dragging = false;
                const std::optional<rts::UnitId> dragged = dragged_garrison;
                dragged_garrison.reset();
                const float ddx = mouse.x - drag_screen_start.x;
                const float ddy = mouse.y - drag_screen_start.y;
                if (ddx * ddx + ddy * ddy > kDragThreshold * kDragThreshold) {
                    if (dragged && in_map) {
                        const rts::UnitId one[] = {*dragged};
                        b->issue_move_order(one, cell);
                        selected.assign(one, one + 1);
                    } else if (!dragged) {
                        // 普通拖拽：框选。矩形用世界像素坐标拼。
                        const float x0 = drag_world_start.x < wpos.x ? drag_world_start.x
                                                                     : wpos.x;
                        const float y0 = drag_world_start.y < wpos.y ? drag_world_start.y
                                                                     : wpos.y;
                        const float x1 = drag_world_start.x > wpos.x ? drag_world_start.x
                                                                     : wpos.x;
                        const float y1 = drag_world_start.y > wpos.y ? drag_world_start.y
                                                                     : wpos.y;
                        std::vector<rts::UnitId> ids;
                        b->world().enumerate_units(rts::Side::Defender, ids);
                        selected = game::units_in_rect(
                            view, ids, proj, game::Rect{x0, y0, x1 - x0, y1 - y0});
                    }
                } else if (in_map && !dragged) {
                    const auto hits=building_sprite_hits(map,view,b->world().now(),proj,atlas,wpos,atmosphere_on);
                    if(const auto hit=building_cycle.select(hits,{mouse.x,mouse.y},{wpos.x,wpos.y})) cell=*hit;
                    // Keep the original click location free for overlap cycling.
                    const Vector2 menu_anchor{
                        mouse.x+260.0f<vp.x?mouse.x+20.0f:std::max(0.0f,mouse.x-260.0f),
                        std::clamp(mouse.y,154.0f,std::max(154.0f,vp.y-430.0f))};
                    inspected = cell;
                    inspected_click = mouse;
                    inspected_busy=false;
                    // 点（没拖开）：这一格能不能弹出菜单。**练兵优先于维修**——
                    // 能练兵的格子（完工的兵营/堡垒、没在练）一律走 Train 弹窗，
                    // 维修选项由 `popup_options` 按需插进那份清单最前面，两者
                    // 同时出现，不是二选一（试玩报的 bug：旧版反过来判，
                    // 兵营/堡垒掉血后练兵入口直接消失，只剩维修）。
                    // 不能练兵、但掉了血的建筑（墙、塔……）才落到纯 Repair 弹窗。
                    const auto tile_is_open_for_building =
                        [](const rts::WorldView& v, rts::GridPos c) {
                            if (!v.terrain().buildable(c.i, c.j)) return false;
                            const auto bp = v.bld_pos();
                            const auto ba = v.bld_alive();
                            for (std::size_t k = 0; k < bp.size(); ++k) {
                                if (ba[k] && bp[k] == c) return false;
                            }
                            const auto op = v.obstacle_pos();
                            const auto oa = v.obstacle_alive();
                            for (std::size_t k = 0; k < op.size(); ++k) {
                                if (oa[k] && op[k] == c) return false;
                            }
                            return true;
                        };
                    if (game::can_train_hint(view, cell)) {
                        popup = Popup{PopupKind::Train, cell, menu_anchor};
                    } else if (game::can_repair_hint(view, cell)) {
                        popup = Popup{PopupKind::Repair, cell, menu_anchor};
                    } else if (const game::UpgradeBlock why =
                                   game::upgrade_block(view, cell);
                               why != game::UpgradeBlock::NoBuilding &&
                               why != game::UpgradeBlock::Unbuilt) {
                        // 满血的墙 / 塔 / 采集建筑落到这里——加它之前这种格子
                        // 左键点下去只会清空选中集，于是升级这件事对玩家不存在。
                        //
                        // **条件刻意不是 `can_upgrade_hint`**：那个在顶到等级
                        // 上限时是 false，而 `divisor = 2` 下开局每座建筑都顶
                        // 着上限，用它做拾取判据等于让菜单在最需要解释的时候
                        // 恰好不出现。这里只问「这一格有没有一座己方完工建筑」，
                        // 能不能升由那一行自己灰着说明（见 `push_upgrade`）。
                        popup = Popup{PopupKind::Upgrade, cell, menu_anchor};
                    } else if (game::can_cancel_build_hint(view, cell)) {
                        popup = Popup{PopupKind::Cancel, cell, menu_anchor};
                    } else if (tile_is_open_for_building(view, cell)) {
                        popup = Popup{PopupKind::Build, cell, menu_anchor};
                    } else {
                        selected.clear();   // 点在别处：取消选中（标准 RTS 习惯）
                    }
                }
            } else if (in_map && !selected.empty() &&
                       IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                // 右键对选中集下令。**辅助性覆盖**：两种单兵指令（驻墙 / 开拔）
                // 都落成 `DefenderScript` 里的临时 ManualOrder，到达即失效、
                // 回归自主——它们不是 `rts::Command`，不进世界、不进哈希
                // （`game/player_input.hpp` 文件头）。唯一还走命令通道的是
                // 清野（标记是全局的，与选中集无关）。
                if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                    if (const auto hit = pick_building_sprite(map, view, b->world().now(), proj, atlas, wpos, atmosphere_on)) cell = *hit;
                    const bool issued = b->issue_forced_work(selected, cell);
                    notice = issued ? "强制抢修：工匠将忽略危险，工程结束后恢复自主" : "请选择工匠，并指向在建、在修或升级中的建筑";
                    notice_until = GetTime() + 3.0;
                } else switch (game::classify_click(view, cell)) {
                    case game::ClickTarget::Wall:
                        b->issue_garrison_order(selected, cell);
                        order_target = cell;
                        order_until = GetTime()+2.5;
                        notice = "已下达驻墙指令";
                        notice_until = GetTime()+3.0;
                        break;
                    case game::ClickTarget::Obstacle: {
                        const rts::Command c = game::clear_command(cell, map.width());
                        b->submit_defender(&c, 1);
                        break;
                    }
                    case game::ClickTarget::Ground:
                        b->issue_move_order(selected, cell);
                        order_target = cell;
                        order_until = GetTime()+2.5;
                        notice = "已下达移动指令，到达后恢复自主作战";
                        notice_until = GetTime()+3.0;
                        break;
                    case game::ClickTarget::None:
                        break;
                }
            }
        }

        if (shell.should_advance() && !paused && !journal.open) {
            acc += static_cast<double>(GetFrameTime());
            int steps = 0;
            // 单帧最多补 5 个 tick：掉帧时宁可仿真慢下来，也不追出一大步。
            while (acc >= kTickDt && steps < 5 && shell.should_advance()) {
                shell.battle()->update(1);
                atmosphere.observe(shell.battle()->world().view(rts::Side::Defender),
                                   shell.battle()->world().now(),shell.battle()->world().wave());
                audio.play(atmosphere.signals().events(),proj,cam.camera(),vp);
                advance_recon_alert();
                acc -= kTickDt;
                ++steps;
            }
        } else {
            // **暂停或在菜单里时把余量清掉。** 不清的话回到对局的那一帧会
            // 一次补满 5 个 tick，画面上是「一松手世界猛地跳一下」。
            acc = 0.0;
        }

        if (inspected && shell.battle()) {
            const auto view = shell.battle()->world().view(rts::Side::Defender);
            bool busy = false;
            bool found = false;
            for (std::size_t k=0;k<view.bld_pos().size();++k) {
                if (!view.bld_alive()[k] || view.bld_pos()[k]!=*inspected) continue;
                found = true;
                busy = view.bld_work_left()[k]>0 || view.bld_upgrade_left()[k]>0 ||
                       view.bld_train_type()[k]!=rts::kNoTrain;
                break;
            }
            if (found && inspected_busy && !busy) {
                notice = "生产已完成";
                notice_until = GetTime()+3.0;
            }
            inspected_busy = busy;
        }
        BeginDrawing();
        draw_frame(vp, !in_menu, cell);
        EndDrawing();
        if(GetTime()>=next_auto_save && shell.battle() && (shell.battle()->world().wave()!=last_save_wave ||
            shell.battle()->world().now()-last_save_tick>=1200 ||
            (!saved_terminal && (shell.battle()->defeated() || shell.chronicle().completed())))) save_now();
    }
    const bool saved_on_exit=save_now();
    CloseWindow();
    return saved_on_exit?0:3;
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
    const bool game_mode = opt.battle || opt.menu || !opt.rl_policy_path.empty() || !opt.defender_policy_path.empty() ||
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
