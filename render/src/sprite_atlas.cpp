#include "render/sprite_atlas.hpp"

#include <cstddef>
#include <fstream>
#include <ios>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

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

    const json& sprites = doc.contains("sprites") ? doc["sprites"] : json::object();
    if (!sprites.is_object() || sprites.empty()) {
        throw AssetError(meta_path + " 的 `sprites` 为空或不是对象");
    }

    for (auto it = sprites.begin(); it != sprites.end(); ++it) {
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
    return cache_.emplace(name, Sprite{tex, sm.ground_anchor}).first->second;
}

std::size_t SpriteAtlas::verify_all_declared() {
    static const char* const kFacings[] = {"SE", "SW", "NE", "NW"};
    std::vector<std::string> missing;
    std::size_t loaded = 0;

    // `meta_` 是 std::map，所以遍历顺序稳定——报错清单每次跑都一样，
    // 便于 diff。（渲染顺序不进仿真，这里不是确定性要求，但廉价。）
    for (const auto& [ident, states] : meta_) {
        for (const auto& [state, sm] : states) {
            for (const char* facing : kFacings) {
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

void SpriteAtlas::preload_idle(const std::vector<std::string>& idents) {
    static const char* const kFacings[] = {"SE", "SW", "NE", "NW"};
    std::vector<std::string> missing;
    for (const std::string& id : idents) {
        for (const char* f : kFacings) {
            try {
                get(id, "idle", f);
            } catch (const AssetError& e) {
                missing.push_back(std::string(e.what()));
            }
        }
    }
    if (!missing.empty()) {
        // 一次把**全部**缺失报出来，而不是抛第一个就走。
        // 缺素材通常是一批（漏渲一个实体 = 缺四个朝向），一个一个修四遍很蠢。
        std::string all = "预载失败，共 " + std::to_string(missing.size()) + " 处：";
        for (const std::string& m : missing) all += "\n  - " + m;
        throw AssetError(all);
    }
}

}  // namespace render
