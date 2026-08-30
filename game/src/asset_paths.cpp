#include "game/asset_paths.hpp"

#include <filesystem>
#include <system_error>

#include "rts/utf8_path.hpp"

namespace game {
namespace {

namespace fs = std::filesystem;

// 存在性判断一律用**不抛版本**的 `fs::exists(p, ec)`。抛版本在权限受限的目录上
// 会抛 `filesystem_error`，而这一层的调用点是「逐级向上试」——一个不可读的中间
// 目录应当让这一级判否、继续往上，不该让整个发现过程失败。
bool exists_ok(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && !ec;
}

}  // namespace

std::optional<AssetPaths> assets_under(std::string_view dir_utf8) {
    if (dir_utf8.empty()) return std::nullopt;
    const fs::path root = rts::path_from_utf8(dir_utf8);
    const fs::path map = root / rts::path_from_utf8(kDefaultMapRel);
    const fs::path stats = root / rts::path_from_utf8(kDefaultStatsRel);
    const fs::path sprites = root / rts::path_from_utf8(kDefaultSpritesRel);
    if (!exists_ok(map) || !exists_ok(stats) ||
        !exists_ok(sprites / rts::path_from_utf8(kSpriteMetaName))) {
        return std::nullopt;
    }
    AssetPaths out;
    out.root = rts::utf8_from_path(root);
    out.map = rts::utf8_from_path(map);
    out.stats = rts::utf8_from_path(stats);
    out.sprites = rts::utf8_from_path(sprites);
    return out;
}

std::optional<AssetPaths> discover_assets(const std::vector<std::string>& start_dirs,
                                         int max_up) {
    // **起点的顺序就是优先级**，逐个起点各自向上走完再换下一个。
    // 不做「所有起点交替向上」那种走法：那会让结果取决于两个起点的相对深度，
    // 而「我在仓库 A 里跑，读到的却是仓库 B 的图」是这一层最坏的失败。
    for (const std::string& start : start_dirs) {
        if (start.empty()) continue;
        std::error_code ec;
        // 规范化一遍。`weakly_canonical` 不要求路径全段存在（`args[0]` 的父目录
        // 一定存在，但工作目录在极端情况下可能已被删除），且它会把
        // `build/render/Release/..` 这类相对段折叠掉——不折叠的话
        // `parent_path()` 只会把 `..` 一段一段脱掉，永远走不上去。
        fs::path dir = fs::weakly_canonical(rts::path_from_utf8(start), ec);
        if (ec) dir = rts::path_from_utf8(start);
        for (int up = 0; up <= max_up; ++up) {
            if (std::optional<AssetPaths> hit = assets_under(rts::utf8_from_path(dir))) {
                return hit;
            }
            const fs::path parent = dir.parent_path();
            if (parent.empty() || parent == dir) break;   // 到根了
            dir = parent;
        }
    }
    return std::nullopt;
}

std::string parent_dir_of(std::string_view path_utf8) {
    if (path_utf8.empty()) return {};
    const fs::path p = rts::path_from_utf8(path_utf8);
    const fs::path parent = p.parent_path();
    if (parent.empty()) return {};
    return rts::utf8_from_path(parent);
}

std::string current_dir() {
    std::error_code ec;
    const fs::path p = fs::current_path(ec);
    if (ec) return {};
    return rts::utf8_from_path(p);
}

}  // namespace game
