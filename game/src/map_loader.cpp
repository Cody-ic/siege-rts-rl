#include "game/map_loader.hpp"

#include <cstdint>
#include <fstream>
#include <ios>
#include <map>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace game {
namespace {

using json = nlohmann::json;

// 报错要带上「哪一份文件、哪一个字段」。载入失败时人手上只有这条消息，
// 而地图文件动辄上百行，只说「格式错误」等于什么都没说。
[[noreturn]] void fail(const std::string& origin, const std::string& what) {
    throw MapFormatError(origin + "：" + what);
}

const json& need(const json& obj, const char* key, const std::string& origin,
                 const std::string& where) {
    if (!obj.is_object() || !obj.contains(key)) {
        fail(origin, where + " 缺少字段 `" + key + "`");
    }
    return obj.at(key);
}

int need_int(const json& v, const std::string& origin, const std::string& where) {
    if (!v.is_number_integer()) fail(origin, where + " 必须是整数");
    return v.get<int>();
}

std::string need_string(const json& v, const std::string& origin,
                        const std::string& where) {
    if (!v.is_string()) fail(origin, where + " 必须是字符串");
    return v.get<std::string>();
}

// 坐标一律是 `[x, y]`。**这条与 `rows[y][x]` 配套，写反了在方形地图上不报错、
// 只是整张图转置**，所以由非方形夹具钉住（见 tests/map_loader_test.cpp）。
rts::GridPos need_pos(const json& v, const std::string& origin,
                      const std::string& where, int w, int h) {
    if (!v.is_array() || v.size() != 2) {
        fail(origin, where + " 必须是 [x, y] 两个整数");
    }
    const int x = need_int(v[0], origin, where + "[0]");
    const int y = need_int(v[1], origin, where + "[1]");
    if (x < 0 || y < 0 || x >= w || y >= h) {
        fail(origin, where + " = [" + std::to_string(x) + ", " + std::to_string(y) +
                         "] 越出 size [" + std::to_string(w) + ", " +
                         std::to_string(h) + "]");
    }
    return rts::GridPos{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
}

// palette 必须**正好**是这五项且同序：`rows` 里的字符就是它的下标。
// 顺序一变，同一份 rows 就解出另一张图，而文件看起来完全正常。
const char* const kPalette[] = {"Plain", "Rock", "Forest", "Water", "Bridge"};

void check_palette(const json& layer, const std::string& origin) {
    const json& pal = need(layer, "palette", origin, "`layers.terrain`");
    // 显式转成 size_t：`pal.size()` 是无符号而 kTerrainCount 是 int。
    // MSVC 对已知为正的 constexpr 不报 C4018，GCC 的 -Wsign-compare 会报，
    // 而两边都开着 -Werror / /WX ——这类不对称正是本项目反复吃亏的地方。
    if (!pal.is_array() || pal.size() != static_cast<std::size_t>(kTerrainCount)) {
        fail(origin, "`layers.terrain.palette` 必须正好 " +
                         std::to_string(kTerrainCount) + " 项");
    }
    for (std::size_t k = 0; k < pal.size(); ++k) {
        if (!pal[k].is_string() || pal[k].get<std::string>() != kPalette[k]) {
            fail(origin, "`layers.terrain.palette` 第 " + std::to_string(k) +
                             " 项应为 `" + kPalette[k] +
                             "`（顺序也要一致，因为 rows 里的字符是它的下标）");
        }
    }
}

// 读一层的 `rows`：行数 = h、每行长度 = w、字符落在允许集合内。
// 三条都查，因为它们的失败症状不同：行数不对是文件截断，行长不对是手写时漏了一格，
// 字符不对是调色板与 rows 对不上。
std::vector<std::string> read_rows(const json& layer, const std::string& origin,
                                   const std::string& where, int w, int h,
                                   char max_char) {
    const json& rows = need(layer, "rows", origin, where);
    if (!rows.is_array() || rows.size() != static_cast<std::size_t>(h)) {
        // 三元的两支要同型：`rows.size()` 是 size_t，所以 0 也写成 size_t，
        // 否则由整型提升去决定共同类型，又是一处两套工具链可能给不同警告的地方。
        const std::size_t got = rows.is_array() ? rows.size() : std::size_t{0};
        fail(origin, where + ".rows 应有 " + std::to_string(h) + " 行，实际 " +
                         std::to_string(got) + " 行");
    }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        const std::string row =
            need_string(rows[static_cast<std::size_t>(y)], origin,
                        where + ".rows[" + std::to_string(y) + "]");
        if (row.size() != static_cast<std::size_t>(w)) {
            fail(origin, where + ".rows[" + std::to_string(y) + "] 长度应为 " +
                             std::to_string(w) + "，实际 " +
                             std::to_string(row.size()));
        }
        for (int x = 0; x < w; ++x) {
            const char c = row[static_cast<std::size_t>(x)];
            if (c < '0' || c > max_char) {
                fail(origin, where + ".rows[" + std::to_string(y) + "][" +
                                 std::to_string(x) + "] = '" + std::string(1, c) +
                                 "' 不在 '0'..'" + std::string(1, max_char) + "' 之内");
            }
        }
        out.push_back(row);
    }
    return out;
}

// 枚举一律用白名单查表，不接受未登记的取值。
// 静默忽略拼错的枚举（例如 `no_bulid`）会让那一层相当于全 0，而地图看起来完全正常
// ——又一处「该红却绿」。这条抄自 Python 侧对 `layers` 的处理（#31）。
template <class E>
E lookup(const std::map<std::string, E>& table, const std::string& key,
         const std::string& origin, const std::string& where) {
    const auto it = table.find(key);
    if (it == table.end()) {
        std::string allowed;
        for (const auto& kv : table) {
            if (!allowed.empty()) allowed += " / ";
            allowed += kv.first;
        }
        fail(origin, where + " = `" + key + "` 不是合法取值（应为 " + allowed + "）");
    }
    return it->second;
}

const std::map<std::string, ResourceType> kResourceTypes{
    {"stone", ResourceType::Stone},
    {"wood", ResourceType::Wood},
    {"gold", ResourceType::Gold},
};

const std::map<std::string, ResourceTier> kResourceTiers{
    {"inner", ResourceTier::Inner},
    {"outer", ResourceTier::Outer},
};

const std::map<std::string, CorridorKind> kCorridorKinds{
    {"defile", CorridorKind::Defile},
    {"economy", CorridorKind::Economy},
    {"forest", CorridorKind::Forest},
    {"open", CorridorKind::Open},
};

const std::map<std::string, WallKind> kWallKinds{
    {"Gate", WallKind::Gate},
    {"Wall", WallKind::Wall},
};

}  // namespace

MapData MapLoader::from_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw MapFormatError(path + "：打不开这个文件");
    std::ostringstream buf;
    buf << in.rdbuf();
    return from_string(buf.str(), path);
}

MapData MapLoader::from_string(std::string_view json_text, const std::string& origin) {
    json doc;
    try {
        doc = json::parse(json_text);
    } catch (const json::parse_error& e) {
        throw MapFormatError(origin + "：JSON 解析失败——" + e.what());
    }
    if (!doc.is_object()) fail(origin, "顶层必须是一个对象");

    MapData m;

    const int format = need_int(need(doc, "format", origin, "顶层"), origin, "`format`");
    if (format != 1) {
        fail(origin, "`format` = " + std::to_string(format) + "，本程序只认 1");
    }

    m.map_id_ = need_string(need(doc, "map_id", origin, "顶层"), origin, "`map_id`");
    m.name_ = need_string(need(doc, "name", origin, "顶层"), origin, "`name`");

    const json& size = need(doc, "size", origin, "顶层");
    if (!size.is_array() || size.size() != 2) fail(origin, "`size` 必须是 [宽, 高]");
    m.width_ = need_int(size[0], origin, "`size[0]`");
    m.height_ = need_int(size[1], origin, "`size[1]`");
    if (m.width_ <= 0 || m.height_ <= 0) {
        fail(origin, "`size` 必须为正，实际 [" + std::to_string(m.width_) + ", " +
                         std::to_string(m.height_) + "]");
    }

    const json& layers = need(doc, "layers", origin, "顶层");
    // `layers` 用白名单：出现未登记的层直接报错而不是忽略（理由见 lookup()）。
    for (auto it = layers.begin(); it != layers.end(); ++it) {
        if (it.key() != "terrain" && it.key() != "no_build") {
            fail(origin, "`layers` 里有未登记的层 `" + it.key() +
                             "`（只认 terrain / no_build）");
        }
    }

    const json& terrain_layer = need(layers, "terrain", origin, "`layers`");
    check_palette(terrain_layer, origin);
    const std::vector<std::string> trows =
        read_rows(terrain_layer, origin, "`layers.terrain`", m.width_, m.height_,
                  static_cast<char>('0' + kTerrainCount - 1));

    const json& nb_layer = need(layers, "no_build", origin, "`layers`");
    const std::vector<std::string> nrows =
        read_rows(nb_layer, origin, "`layers.no_build`", m.width_, m.height_, '1');

    const auto cells = static_cast<std::size_t>(m.width_) *
                       static_cast<std::size_t>(m.height_);
    m.terrain_.reserve(cells);
    m.no_build_.reserve(cells);
    // 这里就是 `rows[y][x]` 那条约定的唯一落点：外层 y、内层 x。
    for (int y = 0; y < m.height_; ++y) {
        for (int x = 0; x < m.width_; ++x) {
            const char c = trows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            m.terrain_.push_back(static_cast<Terrain>(c - '0'));
            m.no_build_.push_back(
                nrows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == '1');
        }
    }

    m.keep_ = need_pos(need(doc, "keep", origin, "顶层"), origin, "`keep`", m.width_,
                       m.height_);

    const json& spawns = need(doc, "spawns", origin, "顶层");
    if (!spawns.is_array() || spawns.empty()) {
        fail(origin, "`spawns` 不能为空——没有集结点就没有进攻方");
    }
    for (std::size_t k = 0; k < spawns.size(); ++k) {
        const std::string where = "`spawns[" + std::to_string(k) + "]`";
        const json& s = spawns[k];
        SpawnPoint sp;
        sp.id = need_int(need(s, "id", origin, where), origin, where + ".id");
        sp.pos = need_pos(need(s, "pos", origin, where), origin, where + ".pos",
                          m.width_, m.height_);
        sp.corridor = lookup(kCorridorKinds,
                             need_string(need(s, "corridor", origin, where), origin,
                                         where + ".corridor"),
                             origin, where + ".corridor");
        m.spawns_.push_back(sp);
    }

    const json& resources = need(doc, "resources", origin, "顶层");
    if (!resources.is_array()) fail(origin, "`resources` 必须是数组");
    for (std::size_t k = 0; k < resources.size(); ++k) {
        const std::string where = "`resources[" + std::to_string(k) + "]`";
        const json& r = resources[k];
        ResourceNode node;
        node.type = lookup(kResourceTypes,
                           need_string(need(r, "type", origin, where), origin,
                                       where + ".type"),
                           origin, where + ".type");
        node.pos = need_pos(need(r, "pos", origin, where), origin, where + ".pos",
                            m.width_, m.height_);
        node.tier = lookup(kResourceTiers,
                           need_string(need(r, "tier", origin, where), origin,
                                       where + ".tier"),
                           origin, where + ".tier");
        m.resources_.push_back(node);
    }

    const json& walls = need(doc, "initial_walls", origin, "顶层");
    if (!walls.is_array()) fail(origin, "`initial_walls` 必须是数组");
    for (std::size_t k = 0; k < walls.size(); ++k) {
        const std::string where = "`initial_walls[" + std::to_string(k) + "]`";
        const json& w = walls[k];
        WallSegment seg;
        seg.kind = lookup(kWallKinds,
                          need_string(need(w, "kind", origin, where), origin,
                                      where + ".kind"),
                          origin, where + ".kind");
        seg.pos = need_pos(need(w, "pos", origin, where), origin, where + ".pos",
                           m.width_, m.height_);
        const json& hp = need(w, "hp_frac", origin, where);
        if (!hp.is_number()) fail(origin, where + ".hp_frac 必须是数字");
        seg.hp_frac = hp.get<float>();
        if (!(seg.hp_frac > 0.0f) || seg.hp_frac > 1.0f) {
            // hp_frac == 0 是「已经没了」，那应当表现为这一格根本没有墙段，
            // 而不是一段血量为 0 的墙——后者会让渲染层画出一段看不见摸不着的墙。
            fail(origin, where + ".hp_frac 应落在 (0, 1] 内，实际 " +
                             std::to_string(seg.hp_frac));
        }
        m.walls_.push_back(seg);
    }

    return m;
}

}  // namespace game
