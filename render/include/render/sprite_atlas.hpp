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

#include <cstddef>
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

    // 把元数据声明的**每一个** (标识符, 状态, 朝向, 帧) 都载入一遍，
    // 缺的一次全报出来。返回实际载入的张数。
    //
    // **为什么需要它，而 `check_assets.py` 不够。**
    // 那个脚本查的是 `assets.json`——模型路径存在、主体模型没被别人共用；
    // 它不看渲染产物。而 `preload_idle()` 只覆盖 `idle`。于是「元数据声明了某个状态
    // 但 PNG 没渲出来」这类错，最早暴露的时机是**动画第一次播到那个状态**。
    // #41 把素材从 300 张扩到 476 张、状态从 idle/move 扩到含 attack/work 之后，
    // 这块无人覆盖的地带值不值得补就不再是个问题了。
    //
    // **它的价值在于跑的是读取器这一侧的文件名规则**（`file_name()`），
    // 与生成器那一侧独立。生成器侧或读取器侧各自的检查都抓不到「两侧约定漂移」，
    // 而那恰好是换素材包时最容易发生的事——两边都自认为对。
    //
    // 代价是它真的把 476 张纹理全传上 GPU（约 1 秒）。所以它是一个显式的模式
    // （`--verify-assets`），不是每次启动都跑。
    std::size_t verify_all_declared();

    // **花名册里的每一个实体都要有一张精灵。** 缺的一次全报出来。
    //
    // 上面那条查「元数据声明了的都渲出来了」，这条查**反方向**：
    // 元数据自己没声明的，它当然不会报。而漏声明恰恰是更常见的那一半——
    // 加一个单位只要改一个枚举，出图要跑 `tools/sprite_gen/`，两件事天然会脱节。
    //
    // 两组标识符现在正好 1:1（元数据 35 项 = 11 单位 + 11 建筑 + 3 障碍
    // + 10 地形贴图变体）。**现在钉住是因为现在它是对的**——
    // 等到某次真的对不上再补检查，就得先花时间弄清哪一边才是对的。
    //
    // 只查 `idle`：新增实体最起码要有 idle（`_sprite_meta.json` 的 note：
    // 「未声明状态的实体只有 idle」）。逐状态的完整性由上面那条负责。
    std::size_t verify_roster_covered();

    // 某个标识符的某个状态有几帧。渲染动画时要用。
    const std::vector<int>& frames_of(std::string_view ident,
                                      std::string_view state) const;

    // 「打出去」的那一帧（弓弦松开 / 刀锋落下）。没有这个概念时返回 0。
    //
    // **字面与含义有半格错位，别照字面用。** 元数据的 `note` 说的是「打出去」，
    // 而「弓弦松开」与「刀锋落下」是两件不同的事——字段名只说了后一件：
    //
    //   * 近战（`Ghoul` / `Spear` / `Knight`）：这一帧伤害**就落地**
    //   * 远程（`Archer` / `Shade` / `Flak`）：这一帧是**释放**，
    //     伤害要等飞行时间之后才落地
    //   * **`Ram`：两类都不属于。** 它贴身（所以没有飞行物），但这一帧只是
    //     **撞木推出**，伤害要等挥击走完才落地。它是贴身撞锤、不是抛射器械
    //     （2026-08-29 定案；CLAUDE.md 原写「弹道有飞行时间」是误导性措辞，已改）
    //
    // 把远程或 `Ram` 的伤害排在这一帧，就与 CLAUDE.md「投石车对静止密集目标高效，
    // 对散开移动目标大幅 miss」这条机制性克制直接矛盾——**它只有在「承诺」与
    // 「落地」之间真有一段时间差时才成立**，而三类的成因各不相同（近战没有时间差、
    // 远程是飞行时间、`Ram` 是前摇）。
    //
    // **上一版这里把成因窄化成了「飞行时间」，那是错的**：它引 `Ram` 当论据，
    // 而 `Ram` 恰恰没有弹丸——那句「只有在弹丸真有飞行时间时才成立」把一条更一般的
    // 要求（承诺与落地之间有时间差）说成了一个特例。**结论一直是对的，论据错了。**
    //
    // 所以这个字段的准确读法是「**攻击被承诺的那一帧**」，
    // 而那一刻发生什么由仿真定，不由精灵定。（已提给 #28。）
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

    // 某个标识符的朝向集合。**不是固定的四个方位**——弹丸只有一个 `FREE`。
    //
    // 元数据顶层的 `dirs` 是默认值，某个实体自带 `dirs` 时以它为准。
    // 本类**不再有任何硬编码的朝向名单**：曾经 `verify_all_declared()` 与
    // `preload_idle()` 各写了一份 `{"SE","SW","NE","NW"}`，于是加进一个只有
    // `FREE` 的条目会让它们报「缺 8 张素材」——而缺的那 8 张本来就不该存在。
    const std::vector<std::string>& dirs_of(std::string_view ident) const;

    // 绕它旋转的那个像素点。**只对 `kind == "projectile"` 有意义**，
    // 其余实体返回 `ground_anchor`（它们不旋转，所以这个退化值不会被用错）。
    //
    // **不要拿 `ground_anchor` 当弹丸的旋转中心**：那是世界原点的投影、代表
    // 「实体脚底」，而弹丸不站在地上。两者实测差 19–26 px，而症状只在
    // **转起来之后**才看得见（箭绕一个看不见的点公转），静态图上完全正常。
    Vector2 pivot_of(std::string_view ident, std::string_view state) const;

    // 这个标识符是不是弹丸。决定要不要走「按飞行角旋转」那条绘制路径，
    // 而不是「按朝向选图」。
    bool is_projectile(std::string_view ident) const noexcept;

private:
    struct StateMeta {
        Vector2 canvas{};
        Vector2 ground_anchor{};
        Vector2 pivot{};        // kind == "projectile" 才有；否则复制 ground_anchor
        std::vector<int> frames;
        int impact_frame = 0;   // 0 = 该状态没有「命中帧」这个概念
        bool is_tile = false;   // 元数据里的 kind == "tile"
        bool is_projectile = false;   // 元数据里的 kind == "projectile"
    };

    // 有序容器。渲染顺序不进仿真，所以这里不是确定性要求；
    // 但按 CLAUDE.md 的口径「不要让无序容器的迭代顺序影响结果」是廉价的好习惯，
    // 而且报错信息里列出「有哪些可用标识符」时有序的输出好读得多。
    std::map<std::string, std::map<std::string, StateMeta>> meta_;
    std::map<std::string, Sprite> cache_;

    // 元数据顶层的 `dirs`，以及自带 `dirs` 的那些实体（当前只有弹丸）。
    // 分开存而不是给每个 ident 都复制一份：默认值改动时只有一个地方要改，
    // 而「这个实体是不是特殊的」也就能被直接读出来。
    std::vector<std::string> default_dirs_;
    std::map<std::string, std::vector<std::string>> dirs_;

    std::string dir_;
    int px_per_tile_ = 0;

    const StateMeta& state_meta(std::string_view ident, std::string_view state) const;
    std::string file_name(std::string_view ident, std::string_view state,
                          std::string_view facing, int frame) const;
};

}  // namespace render

#endif  // RENDER_SPRITE_ATLAS_HPP
