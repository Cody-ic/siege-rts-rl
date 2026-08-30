#include "game/stats_loader.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "rts/roster.hpp"
#include "rts/utf8_path.hpp"

namespace game {
namespace {

using json = nlohmann::json;

[[noreturn]] void fail(const std::string& origin, const std::string& what) {
    throw StatsFormatError(origin + "：" + what);
}

// `where` 取 `string_view`，理由与 `map_loader.cpp` 的同名函数逐字相同
// （GCC 13 `-Wdangling-reference` 对「返回引用 + 引用参数绑定临时量」误报，
// 值类型参数不触发——`训练服务器环境.md` 第 5 节的四格分离实验）。
const json& need(const json& obj, const char* key, const std::string& origin,
                 std::string_view where) {
    if (!obj.is_object() || !obj.contains(key)) {
        fail(origin, std::string(where) + " 缺少字段 `" + key + "`");
    }
    return obj.at(key);
}

std::int64_t need_i64(const json& v, const std::string& origin,
                      const std::string& where) {
    if (!v.is_number_integer()) fail(origin, where + " 必须是整数");
    return v.get<std::int64_t>();
}

std::int32_t need_i32(const json& v, const std::string& origin,
                      const std::string& where) {
    const std::int64_t x = need_i64(v, origin, where);
    if (x < INT32_MIN || x > INT32_MAX) fail(origin, where + " 超出 int32 范围");
    return static_cast<std::int32_t>(x);
}

float need_f32(const json& v, const std::string& origin, const std::string& where) {
    if (!v.is_number()) fail(origin, where + " 必须是数");
    const double x = v.get<double>();
    if (!std::isfinite(x)) fail(origin, where + " 必须是有限数");
    return static_cast<float>(x);
}

// 认不出的键（不以 `_` 开头）一律报错。数值表最典型的手误是**键名拼错**——
// 拼错的键若被静默忽略，那一项就悄悄落回默认值，症状与「漏写」相同、离病因更远。
void reject_unknown_keys(const json& obj, const std::string& origin,
                         const std::string& where,
                         const std::vector<std::string_view>& known) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& key = it.key();
        if (!key.empty() && key[0] == '_') continue;
        bool ok = false;
        for (const std::string_view k : known) {
            if (key == k) {
                ok = true;
                break;
            }
        }
        if (!ok) fail(origin, where + " 有认不出的键 `" + key + "`（拼错了？）");
    }
}

// 三张子表的合法键就是花名册本身。**从 `ident_of` 现生成，不手抄一份**——
// 手抄的清单是与 `rts/roster.hpp` 漂移的起点（新增兵种时这里会静默落后，
// 而落后的表现恰好是「合法的新键被判成拼写错误」，一条误导性的报错）。
template <class EnumAtFn>
std::vector<std::string_view> idents(int count, EnumAtFn at) {
    std::vector<std::string_view> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int k = 0; k < count; ++k) out.push_back(rts::ident_of(at(k)));
    return out;
}

rts::UnitStats read_unit(const json& v, const std::string& origin,
                         const std::string& where) {
    reject_unknown_keys(v, origin, where,
                        {"max_hp", "damage", "range", "speed", "vision",
                         "windup_ticks", "cooldown_ticks", "vs_structure_permille",
                         "aoe_radius", "cost_gold", "train_ticks"});
    rts::UnitStats s;
    s.max_hp = need_i64(need(v, "max_hp", origin, where), origin, where + ".max_hp");
    s.damage = need_i64(need(v, "damage", origin, where), origin, where + ".damage");
    s.range = need_f32(need(v, "range", origin, where), origin, where + ".range");
    s.speed = need_f32(need(v, "speed", origin, where), origin, where + ".speed");
    s.vision = need_f32(need(v, "vision", origin, where), origin, where + ".vision");
    s.windup_ticks = need_i32(need(v, "windup_ticks", origin, where), origin,
                              where + ".windup_ticks");
    s.cooldown_ticks = need_i32(need(v, "cooldown_ticks", origin, where), origin,
                                where + ".cooldown_ticks");
    s.vs_structure_permille =
        need_i32(need(v, "vs_structure_permille", origin, where), origin,
                 where + ".vs_structure_permille");
    s.aoe_radius = need_f32(need(v, "aoe_radius", origin, where), origin,
                            where + ".aoe_radius");
    s.cost_gold =
        need_i64(need(v, "cost_gold", origin, where), origin, where + ".cost_gold");
    s.train_ticks = need_i32(need(v, "train_ticks", origin, where), origin,
                             where + ".train_ticks");

    if (s.max_hp < 1) fail(origin, where + ".max_hp 必须 >= 1");
    if (s.damage < 0) fail(origin, where + ".damage 不得为负");
    if (s.range < 0 || s.speed < 0 || s.vision < 0 || s.aoe_radius < 0) {
        fail(origin, where + " 的 range / speed / vision / aoe_radius 不得为负");
    }
    if (s.windup_ticks < 0) fail(origin, where + ".windup_ticks 不得为负");
    if (s.cooldown_ticks < 1) fail(origin, where + ".cooldown_ticks 必须 >= 1");
    if (s.vs_structure_permille < 0) {
        fail(origin, where + ".vs_structure_permille 不得为负");
    }
    if (s.cost_gold < 0) fail(origin, where + ".cost_gold 不得为负");
    if (s.train_ticks < 0) fail(origin, where + ".train_ticks 不得为负");
    return s;
}

rts::BldStats read_bld(const json& v, const std::string& origin,
                       const std::string& where) {
    reject_unknown_keys(v, origin, where,
                        {"max_hp", "damage", "range", "vision", "windup_ticks",
                         "cooldown_ticks", "cost_stone", "cost_wood", "build_ticks",
                         "income_amount", "aoe_radius"});
    rts::BldStats s;
    s.max_hp = need_i64(need(v, "max_hp", origin, where), origin, where + ".max_hp");
    s.damage = need_i64(need(v, "damage", origin, where), origin, where + ".damage");
    s.range = need_f32(need(v, "range", origin, where), origin, where + ".range");
    s.vision = need_f32(need(v, "vision", origin, where), origin, where + ".vision");
    s.windup_ticks = need_i32(need(v, "windup_ticks", origin, where), origin,
                              where + ".windup_ticks");
    s.cooldown_ticks = need_i32(need(v, "cooldown_ticks", origin, where), origin,
                                where + ".cooldown_ticks");
    s.cost_stone = need_i64(need(v, "cost_stone", origin, where), origin,
                            where + ".cost_stone");
    s.cost_wood =
        need_i64(need(v, "cost_wood", origin, where), origin, where + ".cost_wood");
    s.build_ticks = need_i32(need(v, "build_ticks", origin, where), origin,
                             where + ".build_ticks");
    s.income_amount = need_i64(need(v, "income_amount", origin, where), origin,
                               where + ".income_amount");
    s.aoe_radius = need_f32(need(v, "aoe_radius", origin, where), origin,
                            where + ".aoe_radius");

    if (s.max_hp < 1) fail(origin, where + ".max_hp 必须 >= 1");
    if (s.damage < 0) fail(origin, where + ".damage 不得为负");
    if (s.range < 0 || s.vision < 0) fail(origin, where + " 的 range / vision 不得为负");
    if (s.windup_ticks < 0) fail(origin, where + ".windup_ticks 不得为负");
    if (s.cooldown_ticks < 1) fail(origin, where + ".cooldown_ticks 必须 >= 1");
    if (s.cost_stone < 0 || s.cost_wood < 0) {
        fail(origin, where + " 的 cost_stone / cost_wood 不得为负");
    }
    if (s.build_ticks < 0) fail(origin, where + ".build_ticks 不得为负");
    if (s.income_amount < 0) fail(origin, where + ".income_amount 不得为负");
    if (s.aoe_radius < 0) fail(origin, where + ".aoe_radius 不得为负");
    return s;
}

rts::ObstacleStats read_obstacle(const json& v, const std::string& origin,
                                 const std::string& where) {
    reject_unknown_keys(v, origin, where, {"max_hp", "yield_amount"});
    rts::ObstacleStats s;
    s.max_hp = need_i64(need(v, "max_hp", origin, where), origin, where + ".max_hp");
    s.yield_amount = need_i64(need(v, "yield_amount", origin, where), origin,
                              where + ".yield_amount");
    if (s.max_hp < 1) fail(origin, where + ".max_hp 必须 >= 1");
    if (s.yield_amount < 0) fail(origin, where + ".yield_amount 不得为负");
    return s;
}

}  // namespace

rts::StatsTable StatsLoader::from_file(const std::string& path) {
    // 必须经 `rts::path_from_utf8`，理由同 `MapLoader::from_file`（中文路径）。
    std::ifstream in(rts::path_from_utf8(path), std::ios::binary);
    if (!in) {
        throw StatsFormatError(path +
                               "：打不开这个文件"
                               "\n    （路径含非 ASCII 字符时尤其要注意——见 rts/utf8_path.hpp）");
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return from_string(buf.str(), path);
}

rts::StatsTable StatsLoader::from_string(std::string_view json_text,
                                         const std::string& origin) {
    json doc;
    try {
        doc = json::parse(json_text);
    } catch (const json::parse_error& e) {
        throw StatsFormatError(origin + "：JSON 解析失败——" + e.what());
    }
    if (!doc.is_object()) fail(origin, "顶层必须是一个对象");
    reject_unknown_keys(doc, origin, "顶层",
                        {"schema", "units", "buildings", "obstacles", "global"});

    const json& schema_v = need(doc, "schema", origin, "顶层");
    if (!schema_v.is_string()) fail(origin, "`schema` 必须是字符串");
    const std::string schema = schema_v.get<std::string>();
    // 与 `rts::kStatsShapeTag` 同步进格（stats/3 → stats/4：机制第四批加了
    // 建筑 AOE 半径与冲锋三参数）。刻意不做向后兼容——旧 schema 的表
    // 缺新字段，静默补默认值正是「能跑但打不动」那种坑。
    if (schema != "stats/4") {
        fail(origin, "`schema` = \"" + schema + "\"，本程序只认 \"stats/4\"");
    }

    rts::StatsTable t;

    // 三张子表都要求**恰好铺满**各自的枚举：每个成员必有一行（缺了会「能跑但
    // 打不动」），认不出的键报错（拼错会静默落回默认值）。铺满检查靠
    // `reject_unknown_keys` + 逐成员 `need` 的组合，两个方向各管一半。
    const json& units = need(doc, "units", origin, "顶层");
    if (!units.is_object()) fail(origin, "`units` 必须是对象");
    reject_unknown_keys(units, origin, "`units`",
                        idents(rts::kUnitTypeCount, rts::unit_at));
    for (int k = 0; k < rts::kUnitTypeCount; ++k) {
        const rts::UnitType u = rts::unit_at(k);
        const std::string key{rts::ident_of(u)};
        const std::string where = "`units." + key + "`";
        t.unit[static_cast<std::size_t>(k)] =
            read_unit(need(units, key.c_str(), origin, "`units`"), origin, where);
    }

    const json& blds = need(doc, "buildings", origin, "顶层");
    if (!blds.is_object()) fail(origin, "`buildings` 必须是对象");
    reject_unknown_keys(blds, origin, "`buildings`",
                        idents(rts::kBldTypeCount, rts::bld_at));
    for (int k = 0; k < rts::kBldTypeCount; ++k) {
        const rts::BldType b = rts::bld_at(k);
        const std::string key{rts::ident_of(b)};
        const std::string where = "`buildings." + key + "`";
        t.bld[static_cast<std::size_t>(k)] =
            read_bld(need(blds, key.c_str(), origin, "`buildings`"), origin, where);
    }

    const json& obst = need(doc, "obstacles", origin, "顶层");
    if (!obst.is_object()) fail(origin, "`obstacles` 必须是对象");
    reject_unknown_keys(obst, origin, "`obstacles`",
                        idents(rts::kObstacleTypeCount, rts::obstacle_at));
    for (int k = 0; k < rts::kObstacleTypeCount; ++k) {
        const rts::ObstacleType o = rts::obstacle_at(k);
        const std::string key{rts::ident_of(o)};
        const std::string where = "`obstacles." + key + "`";
        t.obstacle[static_cast<std::size_t>(k)] =
            read_obstacle(need(obst, key.c_str(), origin, "`obstacles`"), origin, where);
    }

    const json& global = need(doc, "global", origin, "顶层");
    if (!global.is_object()) fail(origin, "`global` 必须是对象");
    reject_unknown_keys(
        global, origin, "`global`",
        std::vector<std::string_view>{
            "hp_permille_per_level", "dmg_permille_per_level", "income_period_ticks",
            "mason_work_radius", "repair_hp_per_work_tick", "repair_wood_per_1000hp",
            "cancel_refund_permille", "garrison_mount_ticks",
            "high_ground_miss_permille", "high_ground_dmg_permille",
            "high_ground_range_bonus", "charge_bonus_permille_per_cell",
            "charge_max_cells", "anti_charge_permille"});
    t.global.hp_permille_per_level =
        need_i32(need(global, "hp_permille_per_level", origin, "`global`"), origin,
                 "`global.hp_permille_per_level`");
    t.global.dmg_permille_per_level =
        need_i32(need(global, "dmg_permille_per_level", origin, "`global`"), origin,
                 "`global.dmg_permille_per_level`");
    t.global.income_period_ticks =
        need_i32(need(global, "income_period_ticks", origin, "`global`"), origin,
                 "`global.income_period_ticks`");
    t.global.mason_work_radius =
        need_f32(need(global, "mason_work_radius", origin, "`global`"), origin,
                 "`global.mason_work_radius`");
    t.global.repair_hp_per_work_tick =
        need_i64(need(global, "repair_hp_per_work_tick", origin, "`global`"), origin,
                 "`global.repair_hp_per_work_tick`");
    t.global.repair_wood_per_1000hp =
        need_i64(need(global, "repair_wood_per_1000hp", origin, "`global`"), origin,
                 "`global.repair_wood_per_1000hp`");
    t.global.cancel_refund_permille =
        need_i32(need(global, "cancel_refund_permille", origin, "`global`"), origin,
                 "`global.cancel_refund_permille`");
    t.global.garrison_mount_ticks =
        need_i32(need(global, "garrison_mount_ticks", origin, "`global`"), origin,
                 "`global.garrison_mount_ticks`");
    t.global.high_ground_miss_permille =
        need_i32(need(global, "high_ground_miss_permille", origin, "`global`"), origin,
                 "`global.high_ground_miss_permille`");
    t.global.high_ground_dmg_permille =
        need_i32(need(global, "high_ground_dmg_permille", origin, "`global`"), origin,
                 "`global.high_ground_dmg_permille`");
    t.global.high_ground_range_bonus =
        need_f32(need(global, "high_ground_range_bonus", origin, "`global`"), origin,
                 "`global.high_ground_range_bonus`");
    t.global.charge_bonus_permille_per_cell = need_i32(
        need(global, "charge_bonus_permille_per_cell", origin, "`global`"), origin,
        "`global.charge_bonus_permille_per_cell`");
    t.global.charge_max_cells =
        need_f32(need(global, "charge_max_cells", origin, "`global`"), origin,
                 "`global.charge_max_cells`");
    t.global.anti_charge_permille =
        need_i32(need(global, "anti_charge_permille", origin, "`global`"), origin,
                 "`global.anti_charge_permille`");
    if (t.global.hp_permille_per_level < 0 || t.global.dmg_permille_per_level < 0) {
        fail(origin, "`global` 的等级缩放系数不得为负");
    }
    if (t.global.income_period_ticks < 1) {
        fail(origin, "`global.income_period_ticks` 必须 >= 1");
    }
    if (t.global.mason_work_radius < 0) {
        fail(origin, "`global.mason_work_radius` 不得为负");
    }
    if (t.global.repair_hp_per_work_tick < 1) {
        fail(origin, "`global.repair_hp_per_work_tick` 必须 >= 1");
    }
    if (t.global.repair_wood_per_1000hp < 0) {
        fail(origin, "`global.repair_wood_per_1000hp` 不得为负");
    }
    // 上界 1000 是**结构性**的，不是风格检查：退款超过造价意味着
    // 「下单再撤单」净赚资源——一条不用打仗的印钞回路（「结构封死」准则）。
    if (t.global.cancel_refund_permille < 0 ||
        t.global.cancel_refund_permille > 1000) {
        fail(origin, "`global.cancel_refund_permille` 必须在 [0, 1000] 内");
    }
    if (t.global.garrison_mount_ticks < 0) {
        fail(origin, "`global.garrison_mount_ticks` 不得为负");
    }
    // 两个上界同样是结构性的：miss 是概率（>1000 无意义），伤害倍率 > 1000
    // 会把「高度优势」写成高度劣势——那不是标定出一个大数，是把不等号写反。
    if (t.global.high_ground_miss_permille < 0 ||
        t.global.high_ground_miss_permille > 1000) {
        fail(origin, "`global.high_ground_miss_permille` 必须在 [0, 1000] 内");
    }
    if (t.global.high_ground_dmg_permille < 0 ||
        t.global.high_ground_dmg_permille > 1000) {
        fail(origin, "`global.high_ground_dmg_permille` 必须在 [0, 1000] 内");
    }
    if (t.global.high_ground_range_bonus < 0) {
        fail(origin, "`global.high_ground_range_bonus` 不得为负");
    }
    if (t.global.charge_bonus_permille_per_cell < 0) {
        fail(origin, "`global.charge_bonus_permille_per_cell` 不得为负");
    }
    if (t.global.charge_max_cells < 0) {
        fail(origin, "`global.charge_max_cells` 不得为负");
    }
    // 下界 1000 是结构性的：低于恒等就把「枪阵克骑」写成了「骑克枪阵」——
    // 那不是标定出一个小数，是把克制方向写反（同 high_ground 两条的上界）。
    if (t.global.anti_charge_permille < 1000) {
        fail(origin, "`global.anti_charge_permille` 必须 >= 1000（恒等即无克制）");
    }

    return t;
}

}  // namespace game
