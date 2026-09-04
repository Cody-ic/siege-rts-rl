#include "game/defender_macro.hpp"

#include <algorithm>
#include <cmath>

#include "game/player_input.hpp"
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

}   // namespace

DefenderMacro::DefenderMacro(const MapData& map, const MacroParams& params)
    : p_(params) {
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
    }
    std::int64_t tower_left = std::max<std::int64_t>(0, stone - keep_need);

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

    // —— 5) 征兵 ——
    //
    // 弓手是门前唯一可移动的火力，而它每波近乎全灭（实测），所以补员是流量开销、
    // 每轮都要发。等级取上限（`train_at_cap_level`，理由见头文件）。
    {
        const std::int32_t lv =
            p_.train_at_cap_level ? std::max<std::int32_t>(1, v.unit_level_cap()) : 1;
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
            const std::int64_t cost = v.train_cost_gold(rts::UnitType::Archer, lv);
            if (cost <= 0 || gold < cost) break;
            if (!can_train_hint(v, bpos[k])) continue;
            gold -= cost;
            ++pop;
            cmds.push_back(train_command(rts::UnitType::Archer, lv, bpos[k], mw));
            ++stats_.units_trained;
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
