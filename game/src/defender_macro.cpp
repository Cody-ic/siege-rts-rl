#include "game/defender_macro.hpp"

#include <algorithm>
#include <cmath>

#include "game/player_input.hpp"
#include "rts/fog.hpp"
#include "rts/world_view.hpp"

namespace game {
namespace {

int cheb(rts::GridPos a, rts::GridPos b) {
    const int dx = static_cast<int>(a.i) - static_cast<int>(b.i);
    const int dy = static_cast<int>(a.j) - static_cast<int>(b.j);
    return std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
}

// 建筑槽位 → 这一格有没有己方建筑。`player_input` 里那个是内部符号，
// 这里按同一条判据自己扫一遍（只读，不改判据）。
int bld_slot_at(const rts::WorldView& v, rts::GridPos cell) {
    const auto pos = v.bld_pos();
    const auto alive = v.bld_alive();
    for (std::size_t k = 0; k < pos.size(); ++k) {
        if (alive[k] && pos[k] == cell) return static_cast<int>(k);
    }
    return -1;
}

// 有地面单位站着的格建不了——**这条是 `World` 的 `Build` 解算里的规则**
// （「地面单位站着的格不落地基，会把人封进墙里」，`rts_core/src/world.cpp`），
// 而 `can_place_hint` 不查它（它查的是地形/占位/资源点归属那三条）。
//
// 不镜像这一条的后果实测过：塔位候选是**环内侧一格**那一圈，正是自家单位
// 聚集的地方（弓手去登墙、闲人回驻防环都路过），于是宏观层每个周期都往
// 站着人的格下一条 `Build`、被静默拒绝、下个周期再来一次——一局刷出
// 两千七百多条无效命令。`World` 不会报错，只是不执行。
bool ground_unit_on(const rts::WorldView& v, rts::GridPos cell) {
    const auto ut = v.unit_type();
    const auto up = v.unit_pos();
    const auto ua = v.unit_alive();
    for (std::size_t k = 0; k < ut.size(); ++k) {
        if (!ua[k]) continue;
        if (rts::is_aerial(ut[k])) continue;   // 空军飞在上面无妨（同 World）
        if (rts::grid_of(up[k]) == cell) return true;
    }
    return false;
}

// **过迷雾**数一数看得见的敌方单位。
//
// 这是本层唯一的敌情来源（除了免费的方向提示与总兵力），刻意如此：
// `WorldView` 是 god 视角（那是不变量 2），所以「不作弊」必须靠**主动只读
// 迷雾**来保证，而不是靠没人去读。判据与 `rts_core/src/obs_pack.cpp` 的
// 硬要求 1 逐字同款——越界回落 `Unseen`，只有 `Visible` 算看得见
// （`!= Unseen` 会把「记忆里的」也算进来，那对**单位**是错的：迷雾里只记
// 建筑，单位不进记忆）。
struct VisibleFoe {
    int knight = 0;
    int ram = 0;
    // 空军。**`Phoenix` 与 `Wraith` 分开数**，因为它们对守方的意义不同：
    // 前者是打击（要靠 `Flak` 打下来），后者是侦查（要靠 `Flak` 的**视野
    // 否定半径**赶走）。合成一个数会让「每波恒 1 只的窥使」把防空目标顶起
    // 一格，那不是空袭威胁。而玩家侧本来就分得清——`CLAUDE.md` 要求克制
    // 关系在 UI 里完全透明，剪影也是刻意区分的。
    int phoenix = 0;
    int wraith = 0;
    int total = 0;
};

VisibleFoe census_visible_foes(const rts::WorldView& v) {
    VisibleFoe out;
    const rts::FogLayer& fog = v.fog();
    const auto ut = v.unit_type();
    const auto up = v.unit_pos();
    const auto ua = v.unit_alive();
    for (std::size_t k = 0; k < ut.size(); ++k) {
        if (!ua[k]) continue;
        if (rts::side_of(ut[k]) != rts::Side::Attacker) continue;
        const rts::GridPos g = rts::grid_of(up[k]);
        if (!fog.in_bounds(g.i, g.j)) continue;
        if (fog.at(g.i, g.j) != rts::Vis::Visible) continue;
        ++out.total;
        if (ut[k] == rts::UnitType::Knight) ++out.knight;
        if (ut[k] == rts::UnitType::Ram) ++out.ram;
        if (ut[k] == rts::UnitType::Phoenix) ++out.phoenix;
        if (ut[k] == rts::UnitType::Wraith) ++out.wraith;
    }
    return out;
}

}   // namespace

DefenderMacro::DefenderMacro(const MapData& map, const MacroParams& params)
    : p_(params) {
    // 空军见闻的滑窗。**至少 1 格**：0 长度会让下面那个取余除以零。
    air_seen_.assign(static_cast<std::size_t>(std::max(1, p_.air_memory_waves)), 0);
    keep_ = map.keep();
    map_w_ = map.width();
    map_h_ = map.height();

    // 环半径 = max(|墙格 − keep|)（与 `tools/calibration_runner` 的
    // `collect_map_info` 同一条判据——同一件事只能有一个定义）。
    for (const WallSegment& s : map.walls()) {
        ring_r_ = std::max(ring_r_, cheb(s.pos, keep_));
    }

    // 环上逐格分类：有墙段的进 `wall_cells_`，没有的就是设计缺口。
    // **按 (x, y) 行主序扫，顺序即规范**（纪律 3）。
    for (int x = 0; x < map_w_; ++x) {
        for (int y = 0; y < map_h_; ++y) {
            const rts::GridPos c{static_cast<std::int16_t>(x),
                                 static_cast<std::int16_t>(y)};
            if (cheb(c, keep_) != ring_r_) continue;
            if (map.wall_at(x, y) != nullptr) {
                wall_cells_.push_back(c);
            } else {
                gaps_.push_back(c);
            }
        }
    }

    // 护堡垒的塔位：离堡垒**切比雪夫 4–6 格**那几圈。
    //
    // 下界 4 让开堡垒的 3 格净空（开局那 7 个单位要站得下，`地图与场景设计.md`
    // 4.4.0），上界 6 保证它的射程 7 真的罩住堡垒本身与它四周。这一组**排在
    // 环内侧那一圈之前**建，理由见头文件 `keep_guard_towers`。
    for (int rad = 4; rad <= 6; ++rad) {
        for (int x = 0; x < map_w_; ++x) {
            for (int y = 0; y < map_h_; ++y) {
                const rts::GridPos c{static_cast<std::int16_t>(x),
                                     static_cast<std::int16_t>(y)};
                if (cheb(c, keep_) != rad) continue;
                if (map.no_build_at(x, y)) continue;
                keep_guard_spots_.push_back(c);
            }
        }
    }

    // 塔位候选：环**内侧一格**的那一圈。塔射程 7 > 1，所以贴着墙摆就够得着
    // 墙外——而摆在墙上不行（那一格是墙实体）。
    const int inner = ring_r_ - 1;
    if (inner > 0) {
        for (int x = 0; x < map_w_; ++x) {
            for (int y = 0; y < map_h_; ++y) {
                const rts::GridPos c{static_cast<std::int16_t>(x),
                                     static_cast<std::int16_t>(y)};
                if (cheb(c, keep_) != inner) continue;
                if (map.no_build_at(x, y)) continue;
                tower_spots_.push_back(c);
            }
        }
    }
}

void DefenderMacro::decide(const rts::World& w, std::vector<rts::Command>& cmds,
                           std::vector<UnitOrder>& orders) {
    const rts::WorldView v = w.view(rts::Side::Defender);
    const int mw = v.width();
    const std::size_t budget0 =
        cmds.size() + static_cast<std::size_t>(
                          p_.max_commands_per_decision > 0
                              ? p_.max_commands_per_decision
                              : 0);
    const auto room = [&] { return cmds.size() < budget0; };

    // **本轮的本地账本**（纪律 2）：命令要下一次 `advance()` 才结算。
    const auto stock = v.stock();
    std::int64_t stone = stock[static_cast<std::size_t>(rts::Resource::Stone)];
    std::int64_t wood = stock[static_cast<std::size_t>(rts::Resource::Wood)];
    std::int64_t gold = stock[static_cast<std::size_t>(rts::Resource::Gold)];

    // 石材的分配。**堡垒那一份走「预留」而不是「分成」**，这是一次实测逼出来的
    // 订正：按「当前库存的 30%」给堡垒，而塔在每一轮都把库存抽干，于是那 30%
    // 永远到不了 200（一次升级的价钱），实测 `upg=0`——堡垒一次都没升过，
    // 「人口上限 / 两条等级上限」三个输出全程是装饰品。
    //
    // 改法：**只要有一样正咬着，就先把升级钱从塔的预算里扣出来锁住**，攒够为止。
    // 采集建筑仍按千分比分成（它单笔便宜、且早铺早复利，不需要锁）。
    const bool pop_bound = v.defender_pop() >= v.defender_pop_cap();
    bool level_bound = false;
    {
        const auto bt2 = v.bld_type();
        const auto blv2 = v.bld_level();
        const auto ba2 = v.bld_alive();
        const auto bb2 = v.bld_built();
        const std::int32_t cap = v.building_level_cap();
        for (std::size_t k = 0; k < bt2.size(); ++k) {
            if (!ba2[k] || !bb2[k]) continue;
            if (bt2[k] == rts::BldType::Keep) continue;   // 堡垒不受这条上限约束
            if (blv2[k] >= cap) {
                level_bound = true;
                break;
            }
        }
    }
    const rts::GridPos keep_cell = v.keep_pos();
    const std::int64_t keep_need =
        (pop_bound || level_bound) && can_upgrade_hint(v, keep_cell)
            ? upgrade_cost_stone(v, keep_cell)
            : 0;
    // 采集建筑按「每波几座」限速（见头文件），所以它不预扣钱包——
    // 塔的预算只让开堡垒那一份。
    if (v.wave() != last_wave_) {
        last_wave_ = v.wave();
        gath_this_wave_ = 0;
        // 空军见闻滑到下一格。**上一波见过几只，从此只在窗口里活
        // `air_memory_waves` 波**，见 `air_seen_` 的声明处。
        air_seen_[static_cast<std::size_t>(air_slot_)] = air_this_wave_;
        air_slot_ = (air_slot_ + 1) % static_cast<int>(air_seen_.size());
        air_this_wave_ = 0;
    }
    std::int64_t tower_left = std::max<std::int64_t>(0, stone - keep_need);

    // **过迷雾数一次敌情，两处消费**（编成偏置在 6a、防空座数在 6a2）。
    // 提到这里是因为它必须先于防空那一段，而它本来在编成那一段里面。
    // 数两次不会错，但那是同一件事写在两处——本仓库通篇在防的东西。
    const VisibleFoe foe = census_visible_foes(v);
    // **本波见过的最多空军**（取 max 而不是累加：同一只不死鸟每个决策拍
    // 都会被数一次，累加出来是「决策拍数」而不是「几只」）。
    air_this_wave_ = std::max(air_this_wave_, foe.phoenix);

    // 受威胁的方向 = 兵力最多的那个集结点（免费情报，会被佯攻欺骗——设计如此）。
    rts::GridPos threat = keep_;
    {
        const int idx = strongest_spawn(v);
        const auto& sp = v.spawns();
        if (idx >= 0 && static_cast<std::size_t>(idx) < sp.size()) {
            threat = sp[static_cast<std::size_t>(idx)].pos;
        }
    }

    // —— 1) 封缺口 ——
    //
    // 排在最前面且**不受千分比约束**：它是前提不是投资（见头文件）。
    // 离受威胁方向近的先封。
    if (p_.seal_gaps && !gaps_.empty()) {
        const rts::BldStats& ws = v.stats().of(rts::BldType::Wall);
        std::vector<int> order(gaps_.size());
        for (std::size_t k = 0; k < order.size(); ++k) order[k] = static_cast<int>(k);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return cheb(gaps_[static_cast<std::size_t>(a)], threat) <
                   cheb(gaps_[static_cast<std::size_t>(b)], threat);
        });
        for (const int gi : order) {
            if (!room()) break;
            const rts::GridPos c = gaps_[static_cast<std::size_t>(gi)];
            if (bld_slot_at(v, c) >= 0) continue;   // 已经砌上了（或正在施工）
            if (!can_place_hint(v, rts::BldType::Wall, c)) continue;
            if (ground_unit_on(v, c)) continue;
            if (stone < ws.cost_stone || wood < ws.cost_wood) continue;
            stone -= ws.cost_stone;
            wood -= ws.cost_wood;
            tower_left = std::max<std::int64_t>(0, tower_left - ws.cost_stone);
            cmds.push_back(build_command(rts::BldType::Wall, c, mw));
            ++stats_.walls_built;
        }
    }

    // —— 2) 维修 ——
    //
    // 只花木材，且「维修显著便宜于重建」是既定设计，所以它排在建造之前：
    // 抢救残血结构永远比等它塌了重建划算。
    {
        const auto bt = v.bld_type();
        const auto bpos = v.bld_pos();
        const auto bhp = v.bld_hp();
        const auto bmax = v.bld_max_hp();
        const auto balive = v.bld_alive();
        for (std::size_t k = 0; k < bpos.size(); ++k) {
            if (!room()) break;
            if (!balive[k]) continue;
            // **不查阵营**：花名册里 11 种建筑全是守方的，攻方无经济、
            // 也没有任何建筑，所以 `side_of` 只有单位那一个重载。
            if (bmax[k] <= 0) continue;
            if (bhp[k] * 1000 >= bmax[k] * p_.repair_hp_permille) continue;
            if (!can_repair_hint(v, bpos[k])) continue;
            const std::int64_t cost = repair_wood_cost(v, bpos[k]);
            if (cost <= 0 || wood < cost) continue;
            wood -= cost;
            cmds.push_back(repair_command(bpos[k], mw));
            ++stats_.repairs;
        }
    }

    // —— 2b) 兵营：补员速率的分母 ——
    //
    // `World` 的 `Train` 一座建筑一次只练一名，所以出兵口的**数量**直接就是
    // 每 `train_ticks` 能补几名。此前脚本一座兵营都不建 ⇒ 全场只有 `Keep`
    // 一个口 ⇒ 打光一波补不回来（实测波末存活中位 2，而人口上限抬到 25
    // 也照旧是 2——瓶颈在这儿，不在上限）。
    //
    // 摆位取「堡垒与受威胁那面之间」：CLAUDE.md 说「离前线越近补员越快、
    // 也越容易被点掉，位置即决策」，这里选偏前但仍在环内的一圈。
    if (p_.barracks_target > 0) {
        int have = 0;
        {
            const auto bt2 = v.bld_type();
            const auto ba2 = v.bld_alive();
            for (std::size_t k = 0; k < bt2.size(); ++k) {
                if (ba2[k] && bt2[k] == rts::BldType::Barrack) ++have;
            }
        }
        if (have < p_.barracks_target) {
            const rts::BldStats& bs = v.stats().of(rts::BldType::Barrack);
            // 候选：离堡垒 3..ring−3 且朝受威胁那一面的格。按「到受威胁方向
            // 的距离」升序取第一个能落地的。固定序：先按半径、再按格序扫。
            rts::GridPos best{};
            bool found = false;
            int best_d = 0;
            for (int rad = 3; rad <= std::max(3, ring_r_ - 3); ++rad) {
                for (int x = 0; x < map_w_; ++x) {
                    for (int y = 0; y < map_h_; ++y) {
                        const rts::GridPos c{static_cast<std::int16_t>(x),
                                             static_cast<std::int16_t>(y)};
                        if (cheb(c, keep_) != rad) continue;
                        if (bld_slot_at(v, c) >= 0) continue;
                        if (!can_place_hint(v, rts::BldType::Barrack, c)) continue;
                        if (ground_unit_on(v, c)) continue;
                        const int d = cheb(c, threat);
                        if (!found || d < best_d) {
                            found = true;
                            best_d = d;
                            best = c;
                        }
                    }
                }
                if (found) break;   // 就近的一圈里找到了就别再往外找
            }
            if (found && room() && stone >= bs.cost_stone && wood >= bs.cost_wood) {
                stone -= bs.cost_stone;
                wood -= bs.cost_wood;
                tower_left = std::max<std::int64_t>(0, tower_left - bs.cost_stone);
                cmds.push_back(build_command(rts::BldType::Barrack, best, mw));
                ++stats_.barracks_built;
            }
        }
    }

    // —— 3) 城外采集建筑 ——
    //
    // 只在已解禁、且工匠往返得了的距离内建。`can_place_hint` 已经把「采集建筑
    // 必须踩在对应种类的资源点上」那条查掉了，这里不重复推。
    {
        const auto& sites = v.resources();
        // 铺设顺序。**它不是无关紧要的**：三种资源里只有石材是紧的
        // （§4.5 实测），所以「先铺哪一种」决定新增收入落在活的那条轴上
        // 还是死的那两条上。固定序（`stable_sort` + 事先算好的键），
        // 不用任何未定序容器——纪律 3。
        std::vector<int> order(sites.size());
        for (std::size_t k = 0; k < order.size(); ++k) order[k] = static_cast<int>(k);
        if (p_.gather_order != MacroParams::GatherOrder::MapOrder) {
            const bool by_kind =
                p_.gather_order == MacroParams::GatherOrder::StoneFirst ||
                p_.gather_order == MacroParams::GatherOrder::GoldFirst;
            const rts::Resource want =
                p_.gather_order == MacroParams::GatherOrder::GoldFirst
                    ? rts::Resource::Gold
                    : rts::Resource::Stone;
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const rts::ResourceSite& sa = sites[static_cast<std::size_t>(a)];
                const rts::ResourceSite& sb = sites[static_cast<std::size_t>(b)];
                if (by_kind) {
                    const int ka = sa.kind == want ? 0 : 1;
                    const int kb = sb.kind == want ? 0 : 1;
                    if (ka != kb) return ka < kb;
                }
                return cheb(sa.pos, keep_) < cheb(sb.pos, keep_);
            });
        }
        for (const int oi : order) {
            if (!room()) break;
            const std::size_t k = static_cast<std::size_t>(oi);
            const rts::ResourceSite& s = sites[k];
            if (v.wave() < s.unlock_wave) continue;
            if (cheb(s.pos, keep_) > p_.gatherer_max_dist) continue;
            if (bld_slot_at(v, s.pos) >= 0) continue;
            const rts::BldType bt = rts::gatherer_of(s.kind);
            if (!can_place_hint(v, bt, s.pos)) continue;
            if (ground_unit_on(v, s.pos)) continue;
            if (gath_this_wave_ >= p_.gatherers_per_wave) break;
            const rts::BldStats& bs = v.stats().of(bt);
            if (stone < bs.cost_stone || wood < bs.cost_wood) continue;
            ++gath_this_wave_;
            stone -= bs.cost_stone;
            tower_left = std::max<std::int64_t>(0, tower_left - bs.cost_stone);
            wood -= bs.cost_wood;
            cmds.push_back(build_command(bt, s.pos, mw));
            ++stats_.gatherers_built;
        }
    }

    // —— 4) 升堡垒 ——
    //
    // 它一次买三样东西（人口上限 + 建筑等级上限 + 兵种等级上限），所以判据不是
    // 「有钱就升」，而是**其中至少有一样正咬着**：人口顶满，或者已有建筑顶到了
    // 建筑等级上限。否则那笔石头该先去建塔。
    // 判据已在上面算过（`keep_need > 0` 就表示「有一样正咬着且升得了」），
    // 这里只负责「钱够了就下命令」——攒钱那一半由 `tower_left` 的扣减实现。
    if (room() && keep_need > 0) {
        const std::int64_t cw = upgrade_cost_wood(v, keep_cell);
        if (stone >= keep_need && wood >= cw) {
            stone -= keep_need;
            wood -= cw;
            cmds.push_back(upgrade_command(keep_cell, mw));
            ++stats_.upgrades;
        }
    }

    // —— 5) 征兵：按目标编成，不再只招弓手 ——
    //
    // 顺序是**工匠 → 斥候 → 战斗兵**，而工匠排第一不是笔误：建造、维修、升级
    // 三件工程全靠它推进，所以它是这一层其余每一条的**产能前提**。开局只有
    // 1 名、此前从不补，实测后果是堡垒升级永远排不上工时（头文件 `mason_target`）。
    //
    // 战斗兵按 `mix_*` 的目标比例挑「当前最欠的那一种」，再叠一层**过迷雾**的
    // 反应（看见骑士加枪卫、看见攻城锤加游骑，两条直接抄克制二部图）。
    // 等级取上限（`train_at_cap_level`，理由见头文件）。
    {
        const std::int32_t lv =
            p_.train_at_cap_level ? std::max<std::int32_t>(1, v.unit_level_cap()) : 1;

        // 现有编成普查（只数活着的己方单位；在训的那一名由 `defender_pop()`
        // 占人口，但它是什么兵种要读 `b_train_type_`，这里不细分——一名的
        // 误差比多一条状态便宜）。
        int have_archer = 0, have_spear = 0, have_ranger = 0;
        int have_mason = 0, have_scout = 0;
        {
            std::vector<rts::UnitId> ids;
            w.enumerate_units(rts::Side::Defender, ids);
            for (const rts::UnitId id : ids) {
                switch (w.unit_type(id)) {
                    case rts::UnitType::Archer: ++have_archer; break;
                    case rts::UnitType::Spear: ++have_spear; break;
                    case rts::UnitType::Ranger: ++have_ranger; break;
                    case rts::UnitType::Mason: ++have_mason; break;
                    case rts::UnitType::Scout: ++have_scout; break;
                    default: break;
                }
            }
        }

        // 过迷雾的反应：看得见的骑士/攻城锤把对应兵种的**权重**抬一档，
        // 然后**归一化**回 1000。
        //
        // **相加 + 弓手拿余数是错的，第一版就是那么写的**：骑士与攻城锤几乎
        // 每波都同时可见 ⇒ 两档反应叠起来是 +600 ⇒ 目标份额变成
        // A50/S500/R450，弓手被压到零（实测第 20 波场上 Ar3 Sp5 Ra2）。
        // 而弓手是**墙上唯一吃高度优势的输出**，把它压没等于放弃三阶段攻防的
        // 第一段。归一化之后同样的局面给 A406/S312/R281——反应仍然明显，
        // 但它抢的是份额、不是把基础编成清零。
        int want_archer = std::max(
            0, 1000 - p_.mix_spear_permille - p_.mix_ranger_permille);
        int want_spear = p_.mix_spear_permille;
        int want_ranger = p_.mix_ranger_permille;
        if (foe.knight > 0) want_spear += p_.react_permille;
        if (foe.ram > 0) want_ranger += p_.react_permille;
        {
            const int sum = want_archer + want_spear + want_ranger;
            if (sum > 0) {
                want_archer = want_archer * 1000 / sum;
                want_spear = want_spear * 1000 / sum;
                want_ranger = want_ranger * 1000 / sum;
            }
        }

        // 「当前最欠的那一种」= 实际占比与目标占比的差最大的那个。
        // **固定序的三元比较**，不排序也不用容器——纪律 3。
        const auto pick_combat = [&](int a, int sp, int rg) {
            const int n = a + sp + rg;
            if (n == 0) return rts::UnitType::Archer;   // 一个战斗兵都没有：先要输出
            const int da = want_archer - a * 1000 / n;
            const int ds = want_spear - sp * 1000 / n;
            const int dr = want_ranger - rg * 1000 / n;
            if (ds >= da && ds >= dr) return rts::UnitType::Spear;
            if (dr >= da && dr >= ds) return rts::UnitType::Ranger;
            return rts::UnitType::Archer;
        };

        const auto bt = v.bld_type();
        const auto bpos = v.bld_pos();
        const auto balive = v.bld_alive();
        const auto bbuilt = v.bld_built();
        const auto btrain = v.bld_train_left();
        int pop = v.defender_pop();
        const int cap = v.defender_pop_cap();
        for (std::size_t k = 0; k < bpos.size(); ++k) {
            if (!room()) break;
            if (pop >= cap) break;
            if (!balive[k] || !bbuilt[k]) continue;
            if (bt[k] != rts::BldType::Barrack && bt[k] != rts::BldType::Keep) continue;
            if (btrain[k] > 0) continue;   // 这一座正在练兵，排不进第二个
            if (!can_train_hint(v, bpos[k])) continue;

            // 挑兵种：产能与情报的绝对缺口优先，**但受非战斗人口上界约束**。
            // 没有那条上界时，开局 3 个空位会被 2 工匠 + 1 斥候吃光，
            // 一个战斗兵都招不上（头文件 `noncombat_max_permille`）。
            const int noncombat_cap = cap * p_.noncombat_max_permille / 1000;
            const int noncombat_now = have_mason + have_scout;
            rts::UnitType ut;
            // **斥候排在工匠之前**，这一条是被测试逼出来的：工匠的目标是 3、
            // 而非战斗预算在人口上限 10 时也是 3 ⇒ 工匠把预算吃光、
            // **斥候永远招不出来**（实测 6000 拍后场上斥候 0 名，于是侦查
            // 那条机制整个不发生）。
            //
            // 谁该让位是清楚的：斥候的目标是 **1**、只要 25 金，而且它是
            // **唯一的情报来源**——少一名工匠只是建得慢一点（产能是连续的），
            // 少了斥候是整条决策轴消失（情报是离散的）。
            if (have_scout < p_.scout_target && noncombat_now < noncombat_cap) {
                ut = rts::UnitType::Scout;
            } else if (have_mason < p_.mason_target &&
                       noncombat_now < noncombat_cap) {
                ut = rts::UnitType::Mason;
            } else {
                ut = pick_combat(have_archer, have_spear, have_ranger);
            }

            const std::int64_t cost = v.train_cost_gold(ut, lv);
            if (cost <= 0 || gold < cost) break;
            gold -= cost;
            ++pop;
            cmds.push_back(train_command(ut, lv, bpos[k], mw));
            ++stats_.units_trained;
            switch (ut) {
                case rts::UnitType::Archer: ++have_archer; ++stats_.trained_archer; break;
                case rts::UnitType::Spear: ++have_spear; ++stats_.trained_spear; break;
                case rts::UnitType::Ranger: ++have_ranger; ++stats_.trained_ranger; break;
                case rts::UnitType::Mason: ++have_mason; ++stats_.trained_mason; break;
                case rts::UnitType::Scout: ++have_scout; ++stats_.trained_scout; break;
                default: break;
            }
        }
    }

    // —— 6) 建塔 ——
    //
    // 摆在受威胁那一面：塔位按「到受威胁方向的距离」排序，近的先建。
    // **这是「防御建筑定义防御的形状」那条设计的执行手段**——形状由塔给出，
    // 洞由机动部队补。
    // 6a) 先把护堡垒的近卫塔配满（几何理由见头文件）。
    if (p_.keep_guard_towers > 0 && !keep_guard_spots_.empty()) {
        const rts::BldStats& ts = v.stats().of(rts::BldType::Tower);
        int have = 0;
        {
            const auto bt = v.bld_type();
            const auto bp = v.bld_pos();
            const auto ba = v.bld_alive();
            for (std::size_t k = 0; k < bt.size(); ++k) {
                if (!ba[k] || bt[k] != rts::BldType::Tower) continue;
                if (cheb(bp[k], keep_) <= 6) ++have;
            }
        }
        for (const rts::GridPos c : keep_guard_spots_) {
            if (!room() || have >= p_.keep_guard_towers) break;
            if (bld_slot_at(v, c) >= 0) continue;
            if (!can_place_hint(v, rts::BldType::Tower, c)) continue;
            if (ground_unit_on(v, c)) continue;
            if (tower_left < ts.cost_stone || wood < ts.cost_wood) break;
            tower_left -= ts.cost_stone;
            stone -= ts.cost_stone;
            wood -= ts.cost_wood;
            cmds.push_back(build_command(rts::BldType::Tower, c, mw));
            ++stats_.towers_built;
            ++have;
        }
    }

    // —— 6a2) 防空 ——
    //
    // **此前一座都不建 ⇒ `Phoenix` 全程无人可挡**，而「每座 AA 意味着该位置
    // 少一座对地火力」这组两难被 CLAUDE.md 称作「本作智斗最可读的载体」——
    // 在训练里它一次都没发生过。它也是「Flak 视野否定覆盖率」那项核验的前提。
    //
    // **排在环塔之前**是有意的：两者抢同一份石材，而那正是那个机会成本本身。
    // 若排在后面，钱永远先被环塔花完，取舍就退化成「有余钱才防空」。
    // 摆位与近卫塔同一组候选（离堡垒 4–6）——`Flak` 射程 5、视野 9，
    // 摆在城心一带能罩住堡垒与内城，而空军的目标正是塔与工人。
    //
    // **座数随「记忆里的空军」走，不是常数**（2026-09-06，队友在 #141 投的
    // 那一票：「随已观测空军数走，记忆口径」）。此前是常数 2，而同一批把攻方
    // 的 `Phoenix` 上限从恒 1 放开到 5 —— 只放开一头就是镜像版的坏陪练：
    // 攻方最多来 5 只，守方永远只有 2 座防空。
    //
    // 三条口径上的定法：
    //
    //   1. **只数 `Phoenix`，不数 `Wraith`**。窥使每波恒 1 只、无战力，
    //      把它算进来等于给防空目标垫了个恒定的 +1，那不是空袭威胁。
    //      （窥使的对策是 `Flak` 的**视野否定半径**，那是既有座数的副作用，
    //      不需要为它多建一座。）
    //   2. **滑窗控制未来补建目标**。已有 Flak 保留：目标下降不拆建筑，
    //      只有战损后才少补。释放地块需要独立的拆除策略，本层尚未实现。
    //   3. **不看真实数量、只看看得见过的**（`census_visible_foes` 过迷雾）。
    //      读真实数量 = 守方凭空知道该建几座，玩家做不到 ⇒ 攻方 RL 会学到
    //      一个「先知守方」，与「情报要花资源买」整条设计轴相反。
    //
    // 反馈回路是设计要的：攻方那边 `flak_per_phoenix_cut` 让记忆里每 2 座
    // 防空少来一只不死鸟，两条合起来是**负反馈**、有不动点——那正是
    // `CLAUDE.md` 说的「这组双向预判是本作智斗最可读的载体」。
    int flak_want = p_.flak_target;
    if (p_.flak_per_phoenix > 0) {
        int seen = air_this_wave_;
        for (const int n : air_seen_) seen = std::max(seen, n);
        // 向上取整：见过 1 只就该有 1 座，而不是 0 座。
        flak_want += (seen + p_.flak_per_phoenix - 1) / p_.flak_per_phoenix;
    }
    if (p_.flak_max > 0) flak_want = std::min(flak_want, p_.flak_max);
    stats_.flak_target_now = flak_want;

    if (flak_want > 0 && !keep_guard_spots_.empty()) {
        const rts::BldStats& fs = v.stats().of(rts::BldType::Flak);
        int have = 0;
        {
            const auto bt2 = v.bld_type();
            const auto ba2 = v.bld_alive();
            for (std::size_t k = 0; k < bt2.size(); ++k) {
                if (ba2[k] && bt2[k] == rts::BldType::Flak) ++have;
            }
        }
        for (const rts::GridPos c : keep_guard_spots_) {
            if (!room() || have >= flak_want) break;
            if (bld_slot_at(v, c) >= 0) continue;
            if (!can_place_hint(v, rts::BldType::Flak, c)) continue;
            if (ground_unit_on(v, c)) continue;
            if (tower_left < fs.cost_stone || wood < fs.cost_wood) break;
            tower_left -= fs.cost_stone;
            stone -= fs.cost_stone;
            wood -= fs.cost_wood;
            cmds.push_back(build_command(rts::BldType::Flak, c, mw));
            ++stats_.flaks_built;
            ++have;
        }
    }

    // 6b) 环内侧那一圈。**同时数一下受威胁那一段还有没有摆得下的空位**——
    //     那个数决定下面 6c 要不要改升级（「先铺开、再升高」，见头文件那条）。
    //
    // **「饱和」只算受威胁那一段，不算整环。** 整环有 8(R−1) ≈ 88 个塔位、
    // 一万石都填不满，拿它当判据的话 6c 是死代码。而一座塔离受威胁的那段墙
    // 超过自己的射程就打不到那里的攻方——所以真正会被填满、也真正相关的，
    // 是**离受威胁墙段一个射程以内**那一段弧（十几格）。判据从 `Tower.range`
    // 推、不写死一个半径。
    rts::GridPos threat_wall = keep_;
    if (!wall_cells_.empty()) {
        int best_d = 0;
        for (std::size_t kk = 0; kk < wall_cells_.size(); ++kk) {
            const int dd = cheb(wall_cells_[kk], threat);
            if (kk == 0 || dd < best_d) {
                best_d = dd;
                threat_wall = wall_cells_[kk];
            }
        }
    }
    const int near_r =
        static_cast<int>(v.stats().of(rts::BldType::Tower).range);
    bool ring_saturated = false;
    int free_spots_seen = 0;
    if (!tower_spots_.empty()) {
        const rts::BldStats& ts = v.stats().of(rts::BldType::Tower);
        std::vector<int> order(tower_spots_.size());
        for (std::size_t k = 0; k < order.size(); ++k) order[k] = static_cast<int>(k);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return cheb(tower_spots_[static_cast<std::size_t>(a)], threat) <
                   cheb(tower_spots_[static_cast<std::size_t>(b)], threat);
        });
        int placed_here = 0;
        int free_spots = 0;
        for (const int si : order) {
            const rts::GridPos c = tower_spots_[static_cast<std::size_t>(si)];
            if (bld_slot_at(v, c) >= 0) continue;
            if (!can_place_hint(v, rts::BldType::Tower, c)) continue;
            // **`ground_unit_on` 不算进「饱和」**：那是本拍恰好有人站着，
            // 下一拍就没了。把它当饱和会让「有人路过」变成「该升级了」，
            // 而升级是不可逆的花钱——判据要用地形与占位，不用一瞬间的站位。
            if (cheb(c, threat_wall) <= near_r) ++free_spots;
            if (!room()) continue;
            if (placed_here >= p_.max_towers_per_entry) continue;
            if (ground_unit_on(v, c)) continue;
            if (tower_left < ts.cost_stone || wood < ts.cost_wood) continue;
            tower_left -= ts.cost_stone;
            stone -= ts.cost_stone;
            wood -= ts.cost_wood;
            cmds.push_back(build_command(rts::BldType::Tower, c, mw));
            ++stats_.towers_built;
            ++placed_here;
        }
        free_spots_seen = free_spots;
        ring_saturated = (free_spots == 0);
    } else {
        ring_saturated = true;   // 一个候选位都没有（内环退化）也算饱和
    }
    stats_.near_free_spots = free_spots_seen;

    // —— 6c) 环饱和了就升级已有建筑 ——
    //
    // 塔优先（升级买火力），其次是受威胁那一面的墙（升级买血量，而入口攻防
    // 就是一场「墙先破还是攻方先死」的赛跑）。等级上限由 `Keep` 给出，顶到了
    // 就不发——`World` 会静默拒绝，而静默拒绝的命令是查不出来的浪费。
    if (p_.upgrade_when_saturated && ring_saturated) {
        const std::int32_t cap = v.building_level_cap();
        const auto bt = v.bld_type();
        const auto bp = v.bld_pos();
        const auto blv = v.bld_level();
        const auto ba = v.bld_alive();
        const auto bb = v.bld_built();
        const auto bwork = v.bld_work_left();
        const auto bup = v.bld_upgrade_left();
        // 两轮：先塔后墙。**顺序即规范**（固定序，不排序——`bld_pos` 的槽位序
        // 本身是确定的，而按距离排序时距离相等的那些顺序不稳）。
        for (int pass = 0; pass < 2 && room(); ++pass) {
            const rts::BldType want =
                pass == 0 ? rts::BldType::Tower : rts::BldType::Wall;
            for (std::size_t k = 0; k < bt.size(); ++k) {
                if (!room()) break;
                if (!ba[k] || !bb[k]) continue;
                if (bt[k] != want) continue;
                if (blv[k] >= cap) continue;          // 顶到上限，别发
                if (bwork[k] > 0 || bup[k] > 0) continue;   // 有工程在推进
                // 墙只升受威胁那一面的（全环 8R 格，全升是把钱摊薄成无效）。
                if (want == rts::BldType::Wall && cheb(bp[k], threat) > ring_r_) {
                    continue;
                }
                const std::int64_t us = v.bld_upgrade_cost_stone(bt[k], blv[k]);
                const std::int64_t uw = v.bld_upgrade_cost_wood(bt[k], blv[k]);
                if (tower_left < us || wood < uw) break;
                tower_left -= us;
                stone -= us;
                wood -= uw;
                cmds.push_back(upgrade_command(bp[k], mw));
                ++stats_.bld_upgrades;
            }
        }
    }

    // —— 6d) 破口处放木栅 ——
    //
    // CLAUDE.md「保留一个廉价应急防御层（木栅栏、临时箭塔），可在波次进行中
    // 即时放置」的落点，此前一座都没造过。它同时是**木材唯一的大宗出口**
    // （15 木/座，而木材实测堆到 3000+，`资源点分布与经济平衡.md` §4.5——
    // 「木材是死轴」那条结论有一半可能就是「脚本不用它」）。
    //
    // 只在**交战期**、只在**环上缺墙的格**放：木栅补的是形状里的洞，
    // 不该拿它去铺满城内。它挡不住多久，但「立刻生效」正是它与石墙的分工
    // （石材买永久结构、代价是时间；木材买立刻生效的临时结构）。
    if (p_.build_fence_at_breach && v.phase() == rts::WavePhase::Assault) {
        if (v.wave() != fence_wave_) {
            fence_wave_ = v.wave();
            fence_this_wave_ = 0;
        }
        const rts::BldStats& fs = v.stats().of(rts::BldType::Fence);
        // 环上现在缺墙的格 = 本波被打穿的 + 地图自带的设计缺口。逐格现算，
        // 不缓存——墙况每拍都在变。
        for (const rts::GridPos c : wall_cells_) {
            if (!room()) break;
            if (fence_this_wave_ >= p_.fence_per_wave) break;
            if (bld_slot_at(v, c) >= 0) continue;   // 墙还在（或已经补上了）
            if (!can_place_hint(v, rts::BldType::Fence, c)) continue;
            if (ground_unit_on(v, c)) continue;
            if (stone < fs.cost_stone || wood < fs.cost_wood) break;
            stone -= fs.cost_stone;
            wood -= fs.cost_wood;
            ++fence_this_wave_;
            cmds.push_back(build_command(rts::BldType::Fence, c, mw));
            ++stats_.fences_built;
        }
    }

    // —— 6e) 清野 ——
    //
    // 破坏可破坏障碍产一次性资源（`Stump`/`Sapling` 出木、`Rubble` 出石），
    // 此前一次都没下过。`Clear` 是**全局标记**、不按编队——任何闲着的、能
    // 破坏结构的单位都会响应，所以这里只负责「标出来」，不管派谁去。
    // 只标城外一圈以内的：远的走过去的时间不值那点资源，而且要穿集结区。
    if (p_.clear_obstacles) {
        const auto op = v.obstacle_pos();
        const auto oa = v.obstacle_alive();
        // **「已经标过」读 `World` 的那一份，不自己记一份。**
        // `o_clear_ordered_` 是个只进不退的标记，而 `WorldView` 正好暴露了它
        // （`obstacle_clear_ordered()`）。第一版我在这里存了个本地 vector——
        // 那是「同一件事写在两处然后漂移」的又一例：障碍被清掉、槽位回收给
        // 新障碍时，本地那份就开始说谎。不查它的后果是每个决策周期把同一批
        // 障碍重标一遍（同「塔位每周期重发、一局刷 2776 条」那个坑）。
        const auto ordered = v.obstacle_clear_ordered();
        for (std::size_t k = 0; k < op.size(); ++k) {
            if (!room()) break;
            if (!oa[k]) continue;
            if (k < ordered.size() && ordered[k] != 0) continue;
            if (cheb(op[k], keep_) > p_.clear_max_dist) continue;
            cmds.push_back(clear_command(op[k], mw));
            ++stats_.clears_ordered;
        }
    }

    // —— 7) 弓手往哪儿站 ——
    //
    // 两种形态，**互斥**，按「城里有没有敌人」二选一。都不进 `Command`
    // （辅助性临时指令通道），所以不占命令预算。
    //
    //   城里有敌人 ⇒ **叫下墙去堵**（开拔指令。对已上墙的单位就是「下来」）
    //   城里没敌人 ⇒ 集中到受威胁那一面（驻守意愿，吃高度优势）
    //
    // 二选一而不是同时给：同一批单位下两条互相矛盾的临时指令，后一条会覆盖
    // 前一条，那样「哪一条生效」就取决于调用顺序而不是判断——正是要避免的形状。
    std::vector<rts::UnitId> archers;
    if (p_.concentrate_archers || p_.defend_breach) {
        std::vector<rts::UnitId> ids;
        w.enumerate_units(rts::Side::Defender, ids);
        for (const rts::UnitId id : ids) {
            if (w.unit_type(id) == rts::UnitType::Archer) archers.push_back(id);
        }
    }

    // 城里的敌人：离堡垒最近的那个最危险，弓手朝它去。
    int intruders = 0;
    rts::GridPos rally{};
    bool has_rally = false;
    if (p_.defend_breach && !archers.empty()) {
        const auto ut = v.unit_type();
        const auto up = v.unit_pos();
        const auto ua = v.unit_alive();
        int best_d = 0;
        for (std::size_t k = 0; k < ut.size(); ++k) {
            if (!ua[k]) continue;
            if (rts::side_of(ut[k]) != rts::Side::Attacker) continue;
            if (!rts::is_combat(ut[k])) continue;   // 侦查单位不值得放空一条墙
            const rts::GridPos gp = rts::grid_of(up[k]);
            if (cheb(gp, keep_) >= ring_r_) continue;   // 还在墙外
            ++intruders;
            const int d = cheb(gp, keep_);
            if (!has_rally || d < best_d) {
                has_rally = true;
                best_d = d;
                rally = gp;
            }
        }
    }

    if (has_rally && intruders >= p_.breach_intruders_min) {
        UnitOrder mo;
        mo.ids = archers;
        mo.target = rally;
        mo.garrison = false;   // 开拔 ⇒ 已上墙的弓手会被放下来
        orders.push_back(std::move(mo));
        ++stats_.breach_recalls;
    } else if (p_.concentrate_archers && !wall_cells_.empty() && !archers.empty()) {
        int best = -1;
        int best_d = 0;
        for (std::size_t k = 0; k < wall_cells_.size(); ++k) {
            const int d = cheb(wall_cells_[k], threat);
            if (best < 0 || d < best_d) {
                best = static_cast<int>(k);
                best_d = d;
            }
        }
        if (best >= 0) {
            UnitOrder go;
            go.ids = std::move(archers);
            go.target = wall_cells_[static_cast<std::size_t>(best)];
            go.garrison = true;
            orders.push_back(std::move(go));
            ++stats_.garrison_wishes;
        }
    }
}

}   // namespace game
