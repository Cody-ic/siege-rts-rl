// 精灵图集：把 `_sprite_meta.json` 与 300 张 PNG 变成「标识符 → (纹理, 地面锚点)」。
//
// **`地图与场景设计.md` 第 7 节：像素几何的唯一来源是 `_sprite_meta.json`。**
// 地图文件、地图规范、`rts_core`、`game/` 都不复述任何像素数字——复述就会漂移。
// 本类是那个来源在 C++ 侧的唯一读取点。
//
// 文件名规律（实测，与 7.2 的 3D 路线一致）：
//     <标识符>_<状态>_<朝向>[_<帧号>].png
// `idle` 一律**不带**帧号（`frames` 只有一项）；`move` 带帧号，取值来自 `frames`。

#ifndef RENDER_SPRITE_ATLAS_HPP
#define RENDER_SPRITE_ATLAS_HPP

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "raylib.h"

namespace render {

// 素材缺失、元数据读不动、或元数据与磁盘对不上。
//
// 单独一个类型而不是复用 game::MapFormatError：这两种失败的处置完全不同——
// 地图坏了要改地图，素材缺了要重渲精灵。
class AssetError : public std::runtime_error {
public:
    explicit AssetError(const std::string& what) : std::runtime_error(what) {}
};

// 一张已载入的精灵。
struct Sprite {
    Texture2D texture{};
    // 画布内代表「脚底 / 格心」的像素坐标。**是浮点**——元数据里确实有 .5
    // （例如 Archer 的 227.5），四舍五入会让高个子单位上下抖半个像素。
    Vector2 ground_anchor{};
};

class SpriteAtlas {
public:
    // `sprite_dir` 是 `tools/sprite_gen/out_3d`。构造时只读元数据，**不载入任何纹理**
    // ——纹理要 GL 上下文，而构造这个对象的时机不该被这一条绑住。
    explicit SpriteAtlas(const std::string& sprite_dir);
    ~SpriteAtlas();

    SpriteAtlas(const SpriteAtlas&) = delete;
    SpriteAtlas& operator=(const SpriteAtlas&) = delete;

    // 一格菱形的像素宽。**唯一的来源**，别在别处写死 256。
    int px_per_tile() const noexcept { return px_per_tile_; }

    // 取精灵。首次取时载入纹理并缓存。
    // 标识符 / 状态 / 朝向 / 帧号任一对不上都抛 AssetError，**不返回一张占位图**
    // ——占位图会让「素材没渲」变成一个要盯着画面才发现的问题。
    const Sprite& get(std::string_view ident, std::string_view state,
                      std::string_view facing, int frame = 0);

    // 预先把一批标识符的 idle 四朝向全部载入，缺一个就抛。
    //
    // 存在的理由是 `preview_map.py` 的 `build()` 已经踩过：**渲到一半才报错**
    // 比一开始就报错难查得多——你会先怀疑摆位、再怀疑深度，最后才想到是素材缺了。
    void preload_idle(const std::vector<std::string>& idents);

    // 某个标识符的某个状态有几帧。渲染动画时要用。
    const std::vector<int>& frames_of(std::string_view ident,
                                      std::string_view state) const;

    // 「打出去」的那一帧（弓弦松开 / 刀锋落下）。没有这个概念时返回 0。
    //
    // **正确用法不是「按固定节奏播完这些帧」**，而是：
    //
    //     保持命中前的帧，直到仿真说前摇结束，再播其余帧。
    //
    // 理由（元数据的 `note` 里也写着）：攻击前摇是**待标定的数值**，
    // 若前端按帧数决定时长，改一次前摇就得重渲精灵——**一个待定数值被烤进了资产**。
    // 帧数与 tick 因此必须解耦：精灵只提供关键姿态、不提供时长。
    // 这与 6.3「回放格式不得依赖待定数值」是同一条原则。
    //
    // 本类是这个字段在 C++ 侧的**唯一**读取点。放在这里而不是等用到时再另读一遍
    // JSON，是为了不制造第二个读取点——那正是 `_sprite_meta.json` 作为「像素几何
    // 唯一来源」要防的事（§7）。
    int impact_frame_of(std::string_view ident, std::string_view state) const;

    // 某个标识符有没有某个状态。**状态名不是固定集合**（元数据的 `note` 明写：
    // 单位有 idle/move，能攻击的还有 attack，工匠是 work），所以调用方不能假定，
    // 要问。
    bool has_state(std::string_view ident, std::string_view state) const noexcept;

private:
    struct StateMeta {
        Vector2 canvas{};
        Vector2 ground_anchor{};
        std::vector<int> frames;
        int impact_frame = 0;   // 0 = 该状态没有「命中帧」这个概念
        bool is_tile = false;   // 元数据里的 kind == "tile"
    };

    // 有序容器。渲染顺序不进仿真，所以这里不是确定性要求；
    // 但按 CLAUDE.md 的口径「不要让无序容器的迭代顺序影响结果」是廉价的好习惯，
    // 而且报错信息里列出「有哪些可用标识符」时有序的输出好读得多。
    std::map<std::string, std::map<std::string, StateMeta>> meta_;
    std::map<std::string, Sprite> cache_;

    std::string dir_;
    int px_per_tile_ = 0;

    const StateMeta& state_meta(std::string_view ident, std::string_view state) const;
    std::string file_name(std::string_view ident, std::string_view state,
                          std::string_view facing, int frame) const;
};

}  // namespace render

#endif  // RENDER_SPRITE_ATLAS_HPP
