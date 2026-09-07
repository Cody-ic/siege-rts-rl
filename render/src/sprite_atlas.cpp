#include "render/sprite_atlas.hpp"

#include <cstddef>
#include <fstream>
#include <ios>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "rts/roster.hpp"
#include "rts/utf8_path.hpp"

namespace render {
namespace {

using json = nlohmann::json;

Vector2 need_vec2(const json& v, const std::string& where) {
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) {
        throw AssetError(where + " 必须是两个数字");
    }
    return Vector2{v[0].get<float>(), v[1].get<float>()};
}

}  // namespace

SpriteAtlas::SpriteAtlas(const std::string& sprite_dir) : dir_(sprite_dir) {
    const std::string meta_path = dir_ + "/_sprite_meta.json";
    std::ifstream in(rts::path_from_utf8(meta_path), std::ios::binary);
    if (!in) {
        throw AssetError("打不开 " + meta_path +
                         "\n    精灵成品在 tools/sprite_gen/out_3d/，用 --sprites 指定目录");
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    json doc;
    try {
        doc = json::parse(buf.str());
    } catch (const json::parse_error& e) {
        throw AssetError(meta_path + " 解析失败——" + e.what());
    }

    if (!doc.contains("px_per_tile") || !doc["px_per_tile"].is_number_integer()) {
        throw AssetError(meta_path + " 缺少整数字段 `px_per_tile`");
    }
    px_per_tile_ = doc["px_per_tile"].get<int>();
    if (px_per_tile_ <= 0) {
        throw AssetError(meta_path + " 的 `px_per_tile` 必须为正");
    }

    // 7.1 留下的规矩：`tile` 必须从 `px_per_tile` 推导，不得手写。
    // 这里顺手核一遍元数据自己有没有守住——它曾经写成手抄的 [128, 64]，
    // 而实际投影是 362×181，比例对、尺度差 2√2 倍，**肉眼看不出来**，
    // 结果是前端把所有单位画大 2.83 倍。
    if (doc.contains("tile")) {
        const Vector2 t = need_vec2(doc["tile"], "`tile`");
        const float want_w = static_cast<float>(px_per_tile_);
        const float want_h = static_cast<float>(px_per_tile_) / 2.0f;
        if (t.x != want_w || t.y != want_h) {
            throw AssetError(
                meta_path + " 的 `tile` 与 `px_per_tile` 对不上：tile = [" +
                std::to_string(t.x) + ", " + std::to_string(t.y) + "]，而 px_per_tile = " +
                std::to_string(px_per_tile_) + " 要求 [" + std::to_string(want_w) + ", " +
                std::to_string(want_h) +
                "]。\n    见 地图与场景设计.md 7.1：tile 必须从 px_per_tile 推导，不得手写。");
        }
    }

    // 顶层 `dirs`：默认朝向集合。缺了就退回四个方位——旧元数据（本字段之前录的）
    // 没有它，而那时朝向集合确实就是这四个。
    if (doc.contains("dirs") && doc["dirs"].is_array()) {
        for (const json& d : doc["dirs"]) {
            if (d.is_string()) default_dirs_.push_back(d.get<std::string>());
        }
    }
    if (default_dirs_.empty()) {
        default_dirs_ = {"SE", "SW", "NE", "NW"};
    }

    const json& sprites = doc.contains("sprites") ? doc["sprites"] : json::object();
    if (!sprites.is_object() || sprites.empty()) {
        throw AssetError(meta_path + " 的 `sprites` 为空或不是对象");
    }

    for (auto it = sprites.begin(); it != sprites.end(); ++it) {
        // 每实体可覆盖 `dirs`。只在与默认不同时元数据里才有这个字段，
        // 所以「没有它」不是缺陷，是「用默认的」。
        if (it.value().contains("dirs") && it.value()["dirs"].is_array()) {
            std::vector<std::string> ds;
            for (const json& d : it.value()["dirs"]) {
                if (d.is_string()) ds.push_back(d.get<std::string>());
            }
            if (ds.empty()) {
                throw AssetError(meta_path + "：`sprites." + it.key() +
                                 ".dirs` 是空数组。要么给出朝向名，要么整个字段别写"
                                 "（那表示用顶层的默认值）");
            }
            dirs_[it.key()] = std::move(ds);
        }
        const json& states = it.value().contains("states") ? it.value()["states"]
                                                           : json::object();
        if (!states.is_object()) continue;
        for (auto st = states.begin(); st != states.end(); ++st) {
            const std::string where =
                "`sprites." + it.key() + ".states." + st.key() + "`";
            StateMeta sm;
            if (!st.value().contains("ground_anchor")) {
                throw AssetError(meta_path + "：" + where + " 缺少 `ground_anchor`");
            }
            sm.ground_anchor = need_vec2(st.value()["ground_anchor"],
                                         where + ".ground_anchor");
            if(st.value().contains("muzzle_by_facing")) {
                for(auto a=st.value()["muzzle_by_facing"].begin();a!=st.value()["muzzle_by_facing"].end();++a)
                    sm.muzzle_by_facing[a.key()]=need_vec2(a.value(),where+".muzzle_by_facing");
            }
            if(st.value().contains("ground_anchor_by_facing")) {
                for(auto a=st.value()["ground_anchor_by_facing"].begin();a!=st.value()["ground_anchor_by_facing"].end();++a)
                    sm.ground_by_facing[a.key()]=need_vec2(a.value(),where+".ground_anchor_by_facing");
            }
            if (st.value().contains("canvas")) {
                sm.canvas = need_vec2(st.value()["canvas"], where + ".canvas");
            }
            if (st.value().contains("frames") && st.value()["frames"].is_array()) {
                for (const json& f : st.value()["frames"]) {
                    if (f.is_number_integer()) sm.frames.push_back(f.get<int>());
                }
            }
            if (sm.frames.empty()) sm.frames.push_back(1);
            if (st.value().contains("impact_frame") &&
                st.value()["impact_frame"].is_number_integer()) {
                sm.impact_frame = st.value()["impact_frame"].get<int>();
                // 命中帧必须真的在 frames 里。**不在就是元数据坏了**，而它坏掉的
                // 症状是「动画在某个不存在的帧上等前摇结束」——那会表现成单位卡住，
                // 一个看着像仿真 bug 而根因在资产里的问题。所以在这里就红。
                bool found = false;
                for (int f : sm.frames) {
                    if (f == sm.impact_frame) { found = true; break; }
                }
                if (!found) {
                    std::string list;
                    for (int f : sm.frames) {
                        if (!list.empty()) list += ", ";
                        list += std::to_string(f);
                    }
                    throw AssetError(meta_path + "：" + where + " 的 impact_frame = " +
                                     std::to_string(sm.impact_frame) +
                                     " 不在 frames [" + list + "] 里");
                }
            }
            sm.is_tile = st.value().contains("kind") &&
                         st.value()["kind"].is_string() &&
                         st.value()["kind"].get<std::string>() == "tile";
            sm.is_projectile = st.value().contains("kind") &&
                               st.value()["kind"].is_string() &&
                               st.value()["kind"].get<std::string>() == "projectile";
            // 旋转中心。**弹丸必须有**——缺了就是元数据坏了，而拿
            // `ground_anchor` 顶上去的症状只在转起来之后才看得见，
            // 那时根因已经很难追回到资产上。所以在这里就红。
            if (sm.is_projectile) {
                if (!st.value().contains("pivot")) {
                    throw AssetError(meta_path + "：" + where +
                                     " 的 kind 是 projectile 但缺少 `pivot`。"
                                     "\n    出图跑 tools/sprite_gen/ 重渲——"
                                     "不要拿 ground_anchor 代替它，那是脚底、不是旋转中心");
                }
                sm.pivot = need_vec2(st.value()["pivot"], where + ".pivot");
            } else {
                // 非弹丸不旋转，给一个不会被用错的退化值。
                sm.pivot = sm.ground_anchor;
            }
            meta_[it.key()][st.key()] = sm;
        }
    }
}

SpriteAtlas::~SpriteAtlas() {
    // 纹理是 GL 资源。析构时 GL 上下文可能已经没了（CloseWindow 之后），
    // 那时 UnloadTexture 是无操作，raylib 自己也会在 CloseWindow 里清掉。
    // 显式写出来是为了让「谁负责释放」这个问题有答案，而不是靠巧合。
    for (auto& kv : cache_) {
        if (kv.second.texture.id != 0) UnloadTexture(kv.second.texture);
    }
}

const SpriteAtlas::StateMeta& SpriteAtlas::state_meta(std::string_view ident,
                                                      std::string_view state) const {
    const auto i = meta_.find(std::string(ident));
    if (i == meta_.end()) {
        std::string known;
        int n = 0;
        for (const auto& kv : meta_) {
            if (n++ >= 8) { known += " …"; break; }
            if (!known.empty()) known += " / ";
            known += kv.first;
        }
        throw AssetError("元数据里没有标识符 `" + std::string(ident) +
                         "`\n    已登记的有：" + known);
    }
    const auto s = i->second.find(std::string(state));
    if (s == i->second.end()) {
        throw AssetError("`" + std::string(ident) + "` 没有状态 `" +
                         std::string(state) + "`");
    }
    return s->second;
}

const std::vector<int>& SpriteAtlas::frames_of(std::string_view ident,
                                               std::string_view state) const {
    return state_meta(ident, state).frames;
}

Vector2 SpriteAtlas::muzzle_offset(std::string_view ident,std::string_view facing) const {
    const auto& sm=state_meta(ident,"attack");
    const auto it=sm.muzzle_by_facing.find(std::string(facing));
    if(it==sm.muzzle_by_facing.end()) return {0,-stand_lift_px(ident)};
    const auto a=sm.ground_by_facing.find(std::string(facing));
    const auto anchor=a==sm.ground_by_facing.end()?sm.ground_anchor:a->second;
    return {it->second.x-anchor.x,it->second.y-anchor.y};
}

int SpriteAtlas::impact_frame_of(std::string_view ident,
                                 std::string_view state) const {
    return state_meta(ident, state).impact_frame;
}

bool SpriteAtlas::has_state(std::string_view ident,
                           std::string_view state) const noexcept {
    const auto i = meta_.find(std::string(ident));
    if (i == meta_.end()) return false;
    return i->second.find(std::string(state)) != i->second.end();
}

const std::vector<std::string>& SpriteAtlas::dirs_of(std::string_view ident) const {
    const auto i = dirs_.find(std::string(ident));
    return (i != dirs_.end()) ? i->second : default_dirs_;
}

Vector2 SpriteAtlas::pivot_of(std::string_view ident,
                              std::string_view state) const {
    return state_meta(ident, state).pivot;
}

bool SpriteAtlas::is_projectile(std::string_view ident) const noexcept {
    const auto i = meta_.find(std::string(ident));
    if (i == meta_.end() || i->second.empty()) return false;
    // 逐状态存的，但 `kind` 是实体级的属性——取任一状态即可。
    return i->second.begin()->second.is_projectile;
}

std::size_t SpriteAtlas::verify_stand_geometry() const {
    // 能站人的建筑就是「墙段」那两种（`World::tick_garrison` 的判据是 Wall‖Gate）。
    // 写在这里的是**标识符**、不是枚举——`render/` 不依赖 `rts::BldType`，
    // 而 `rts_core` 的花名册与元数据之间的一致性已由 `verify_roster_covered()` 管。
    static constexpr std::string_view kGarrisonable[] = {"Wall", "Gate"};
    std::vector<std::string> bad;
    std::size_t checked = 0;
    for (std::string_view ident : kGarrisonable) {
        const StateMeta& sm = state_meta(ident, "idle");
        const float lift = stand_lift_px(ident, "idle");
        const float top = sm.ground_anchor.y;
        ++checked;
        if (lift <= top * 0.5f || lift >= top) {
            bad.push_back(std::string(ident) + "：抬升 " + std::to_string(lift) +
                          " px 不在 (" + std::to_string(top * 0.5f) + ", " +
                          std::to_string(top) + ") 内");
        }
    }
    if (!bad.empty()) {
        std::string msg = "驻守抬升不在合理区间内（见 SpriteAtlas::kStandFrac）：";
        for (const std::string& b : bad) msg += "\n    " + b;
        msg += "\n    抬太少人会陷进墙体、抬太多会飘在垛口上方，两种都要目视重标。";
        throw AssetError(msg);
    }
    return checked;
}

std::size_t SpriteAtlas::verify_linear_anchor() {
    // 线性结构（走向决定朝向的那两种）× `game::SceneModel::run_direction`
    // 真正会返回的那两个朝向。理由见头文件那段。
    static constexpr std::string_view kLinear[] = {"Wall", "Gate"};
    static constexpr std::string_view kUsedFacings[] = {"SE", "NE"};

    std::vector<std::string> bad;
    std::size_t checked = 0;
    for (std::string_view ident : kLinear) {
        const StateMeta& sm = state_meta(ident, "idle");
        for (std::string_view facing : kUsedFacings) {
            const std::string name = file_name(ident, "idle", facing, 0);
            const std::string path = dir_ + "/" + name;
            bool ok = false;
            const std::vector<unsigned char> bytes = rts::read_file_bytes(path, &ok);
            if (!ok) {
                throw AssetError("载入不了 " + path + "（verify_linear_anchor）");
            }
            Image img = LoadImageFromMemory(".png", bytes.data(),
                                            static_cast<int>(bytes.size()));
            if (img.data == nullptr) {
                throw AssetError("解不开 " + path + "，文件在但不是有效的 PNG");
            }
            // alpha 包围盒。逐像素扫一遍——只有 4 张图，且这是显式的校验模式。
            int lo_x = img.width, hi_x = -1;
            for (int y = 0; y < img.height; ++y) {
                for (int x = 0; x < img.width; ++x) {
                    if (GetImageColor(img, x, y).a == 0) continue;
                    if (x < lo_x) lo_x = x;
                    if (x > hi_x) hi_x = x;
                }
            }
            const int w = img.width;
            UnloadImage(img);
            ++checked;
            if (hi_x < 0) {
                bad.push_back(std::string(ident) + "_" + std::string(facing) +
                              "：整张全透明");
                continue;
            }
            const float center = (static_cast<float>(lo_x) + static_cast<float>(hi_x)) * 0.5f;
            const float off = center - sm.ground_anchor.x;
            if (off > kAnchorTolPx || off < -kAnchorTolPx) {
                bad.push_back(std::string(ident) + "_" + std::string(facing) +
                              "：内容中心偏离锚点 " + std::to_string(off) +
                              " px（容差 ±" + std::to_string(kAnchorTolPx) + "）");
            }
            if (lo_x == 0 || hi_x == w - 1) {
                bad.push_back(std::string(ident) + "_" + std::string(facing) +
                              "：内容压在画布边缘（left=" + std::to_string(lo_x) +
                              " right=" + std::to_string(hi_x) + " 宽=" +
                              std::to_string(w) + "），说明被切掉了一截");
            }
        }
    }
    if (!bad.empty()) {
        std::string msg =
            "线性结构用到的朝向，内容没有落在锚点上（门与墙会错开、墙会被切）：";
        for (const std::string& b : bad) msg += "\n    " + b;
        msg += "\n    这套素材四个朝向共用一个 ground_anchor，而 SW/NW 两张是偏的；"
               "\n    `game::SceneModel::run_direction` 因此只许取 SE 与 NE。"
               "\n    改了那张走向表、或换了素材包，就要回来一起看。";
        throw AssetError(msg);
    }
    return checked;
}

float SpriteAtlas::stand_lift_px(std::string_view ident,
                                 std::string_view state,std::string_view facing) const {
    // `state_meta` 找不到就抛（同本类其余读取点）：驻守单位脚下那座建筑的精灵
    // 一定已经画在同一帧里，取不到说明标识符错了，不该退化成「贴地画」——
    // 那会把「建筑标识符写错」变成一个要盯着画面才看得出的问题。
    const auto& sm=state_meta(ident,state);
    const auto found=sm.ground_by_facing.find(std::string(facing));
    const float adjustment=found==sm.ground_by_facing.end()?0:found->second.y-sm.ground_anchor.y;
    return sm.ground_anchor.y*kStandFrac+adjustment;
}

std::string SpriteAtlas::file_name(std::string_view ident, std::string_view state,
                                   std::string_view facing, int frame) const {
    const StateMeta& sm = state_meta(ident, state);
    std::string name = std::string(ident) + "_" + std::string(state) + "_" +
                       std::string(facing);
    // 帧号只在这个状态**真有多帧**时出现。单帧状态（全部 idle）的文件名不带它。
    if (sm.frames.size() > 1) {
        const int f = (frame != 0) ? frame : sm.frames.front();
        bool ok = false;
        for (int cand : sm.frames) {
            if (cand == f) { ok = true; break; }
        }
        if (!ok) {
            std::string list;
            for (int cand : sm.frames) {
                if (!list.empty()) list += ", ";
                list += std::to_string(cand);
            }
            throw AssetError("`" + std::string(ident) + "` 的 `" + std::string(state) +
                             "` 没有第 " + std::to_string(f) + " 帧（有 " + list + "）");
        }
        name += "_" + std::to_string(f);
    }
    return name + ".png";
}

const Sprite& SpriteAtlas::get(std::string_view ident, std::string_view state,
                               std::string_view facing, int frame) {
    const std::string name = file_name(ident, state, facing, frame);
    const auto hit = cache_.find(name);
    if (hit != cache_.end()) return hit->second;

    const std::string path = dir_ + "/" + name;

    // **不用 `LoadTexture(path)`。** raylib 的文件读取走窄 `fopen`，路径含非 ASCII
    // 字符时会失败。自己读字节 + 内存版 API，理由见 rts/utf8_path.hpp。
    bool ok = false;
    const std::vector<unsigned char> bytes = rts::read_file_bytes(path, &ok);
    if (!ok) {
        // **不退化成占位图。** 占位图会把「素材没渲」变成一个要盯着画面才发现的问题，
        // 而这个仓库已经两次靠「渲完看图」才发现素材撞车（#19、#27）。
        throw AssetError("载入不了 " + path +
                         "\n    元数据登记了它，磁盘上却没有——先跑一遍精灵流水线，"
                         "或核对 tools/sprite_gen/check_assets.py");
    }
    Image img = LoadImageFromMemory(".png", bytes.data(), static_cast<int>(bytes.size()));
    if (img.data == nullptr) {
        throw AssetError("解不开 " + path + "，文件在但不是有效的 PNG");
    }
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    if (tex.id == 0) {
        throw AssetError("传不上 GPU：" + path);
    }
    const StateMeta& sm = state_meta(ident, state);
    const auto anchor=sm.ground_by_facing.find(std::string(facing));
    return cache_.emplace(name, Sprite{tex,anchor==sm.ground_by_facing.end()?sm.ground_anchor:anchor->second}).first->second;
}

bool SpriteAtlas::opaque_at(const Sprite& sprite, int x, int y) {
    const int w = sprite.texture.width, h = sprite.texture.height;
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    auto it = pick_alpha_.find(sprite.texture.id);
    if (it == pick_alpha_.end()) {
        Image image = LoadImageFromTexture(sprite.texture);
        Color* pixels = LoadImageColors(image);
        if (pixels == nullptr) {
            UnloadImage(image);
            throw AssetError("无法读取精灵拾取遮罩");
        }
        std::vector<unsigned char> alpha(static_cast<std::size_t>(w)*h);
        for (std::size_t i = 0; i < alpha.size(); ++i) alpha[i] = pixels[i].a;
        UnloadImageColors(pixels);
        UnloadImage(image);
        it = pick_alpha_.emplace(sprite.texture.id, std::move(alpha)).first;
    }
    return it->second[static_cast<std::size_t>(y)*w+x] >= 64;
}

std::size_t SpriteAtlas::verify_all_declared() {
    std::vector<std::string> missing;
    std::size_t loaded = 0;

    // `meta_` 是 std::map，所以遍历顺序稳定——报错清单每次跑都一样，
    // 便于 diff。（渲染顺序不进仿真，这里不是确定性要求，但廉价。）
    for (const auto& [ident, states] : meta_) {
        for (const auto& [state, sm] : states) {
            // **朝向按实体取。** 这里曾硬编码 `{"SE","SW","NE","NW"}`，于是
            // 只有 `FREE` 一个朝向的弹丸会被报成「缺 4 张素材」——而那 4 张
            // 本来就不该存在。「本函数查的是元数据声明了的都渲出来了」，
            // 那就必须按元数据说的朝向查，不能按一份写死的名单查。
            for (const std::string& facing : dirs_of(ident)) {
                // 单帧状态传 0（`file_name()` 会取 frames 首项且不加后缀）；
                // 多帧状态**逐帧**要，因为「声明了 4 帧只渲出 3 帧」正是要抓的错。
                const std::vector<int> frames =
                    (sm.frames.size() > 1) ? sm.frames : std::vector<int>{0};
                for (int f : frames) {
                    try {
                        get(ident, state, facing, f);
                        ++loaded;
                    } catch (const AssetError& e) {
                        missing.push_back(std::string(e.what()));
                    }
                }
            }
        }
    }
    if (!missing.empty()) {
        // 同 preload_idle：一次把全部缺失报出来。缺素材通常是一批
        // （漏渲一个状态 = 缺四个朝向 × 帧数），一个一个修很蠢。
        std::string all = "元数据声明的素材有 " + std::to_string(missing.size()) +
                          " 处载入不了（已成功 " + std::to_string(loaded) + " 张）：";
        int shown = 0;
        for (const std::string& m : missing) {
            if (++shown > 20) {
                all += "\n  … 另有 " + std::to_string(missing.size() - 20) + " 处";
                break;
            }
            all += "\n  - " + m;
        }
        throw AssetError(all);
    }
    return loaded;
}

std::size_t SpriteAtlas::verify_roster_covered() {
    std::vector<std::string> missing;
    std::size_t checked = 0;

    // **机械地按 count 遍历三个枚举**，不手抄名单。抄一份名单意味着
    // 「加了枚举值忘了加进名单」这条路径没人管，而那正是本函数要防的那类脱节。
    const auto want = [&](std::string_view ident, std::string_view what) {
        ++checked;
        if (meta_.find(std::string(ident)) == meta_.end()) {
            missing.emplace_back(std::string(what) + " " + std::string(ident));
        }
    };
    for (int i = 0; i < rts::kUnitTypeCount; ++i) {
        want(rts::ident_of(rts::unit_at(i)), "单位");
    }
    for (int i = 0; i < rts::kBldTypeCount; ++i) {
        want(rts::ident_of(rts::bld_at(i)), "建筑");
    }
    for (int i = 0; i < rts::kObstacleTypeCount; ++i) {
        want(rts::ident_of(rts::obstacle_at(i)), "障碍");
    }

    if (!missing.empty()) {
        std::string all = "花名册里有 " + std::to_string(missing.size()) +
                          " 个实体在精灵元数据里没有条目（共查 " +
                          std::to_string(checked) + " 个）：";
        for (const std::string& m : missing) all += "\n  - " + m;
        all += "\n  出图跑 tools/sprite_gen/，别只改枚举。";
        throw AssetError(all);
    }
    return checked;
}

void SpriteAtlas::preload_idle(const std::vector<std::string>& idents) {
    std::vector<std::string> missing;
    for (const std::string& id : idents) {
        // 朝向按实体取，同 `verify_all_declared()`——这里曾是第二份写死的
        // `{"SE","SW","NE","NW"}`。**同一个常量写在两处**，所以加 `FREE` 时
        // 两处都会坏；一处修了另一处没修的话，症状是「校验通过但预载失败」。
        for (const std::string& f : dirs_of(id)) {
            try {
                get(id, "idle", f);
            } catch (const AssetError& e) {
                missing.push_back(std::string(e.what()));
            }
        }
    }
    if (!missing.empty()) {
        // 一次把**全部**缺失报出来，而不是抛第一个就走。
        // 缺素材通常是一批（漏渲一个实体 = 缺它的每个朝向），一个一个修很蠢。
        std::string all = "预载失败，共 " + std::to_string(missing.size()) + " 处：";
        for (const std::string& m : missing) all += "\n  - " + m;
        throw AssetError(all);
    }
}

void SpriteAtlas::register_decal_image(std::string_view ident, const std::string& path) {
    const std::string id(ident);
    if (meta_.find(id) != meta_.end()) {
        // 与素材条目撞名：decal 与流水线出的实体精灵共用同一个命名空间，
        // 静默覆盖会让「谁的图」变成一个要逐像素看才能回答的问题。
        throw AssetError("register_decal_image：`" + id +
                         "` 已经在元数据或已注册的 decal 里——换个名字");
    }

    // 同 `get()`：不用 `LoadImage(path)`。raylib 的文件读取走窄 `fopen`，
    // 路径含非 ASCII 字符时会失败，自己读字节 + 内存版 API（理由见
    // rts/utf8_path.hpp）。这条 decal 路径与花名册那条共用同一段素材加载
    // 代码所在文件，没有理由绕开这条已经踩过的坑。
    bool ok = false;
    const std::vector<unsigned char> bytes = rts::read_file_bytes(path, &ok);
    if (!ok) {
        throw AssetError("register_decal_image：读不动 `" + path + "`");
    }
    Image img = LoadImageFromMemory(".png", bytes.data(), static_cast<int>(bytes.size()));
    if (img.data == nullptr) {
        throw AssetError("register_decal_image：解不开 `" + path + "`，文件在但不是有效的 PNG");
    }

    const float w = static_cast<float>(img.width);
    const float h = static_cast<float>(img.height);

    StateMeta sm;
    sm.canvas = Vector2{w, h};
    // 地面锚点取画布底边中点：这批素材已经裁到内容边界，物体的视觉底部
    // 贴着画布下沿。
    sm.ground_anchor = Vector2{w / 2.0f, h};
    sm.pivot = sm.ground_anchor;
    sm.frames = {1};
    meta_[id]["idle"] = sm;

    // 四朝向各传一份纹理（共用一张 Image 各自上传）：`cache_` 的析构会逐个
    // UnloadTexture，多份共享一个 texture id 会变成重复释放。
    for (const std::string& f : dirs_of(id)) {
        Texture2D tex = LoadTextureFromImage(img);
        if (tex.id == 0) {
            UnloadImage(img);
            throw AssetError("register_decal_image：传不上 GPU：`" + id + "`");
        }
        cache_.emplace(id + "_idle_" + f + ".png", Sprite{tex, sm.ground_anchor});
    }
    UnloadImage(img);
}

}  // namespace render
