#include "rts/world.hpp"

#include <string>
#include <utility>

#include "rts/combat_math.hpp"
#include "rts/world_view.hpp"

namespace rts {
namespace {

std::string with_int(std::string_view head, long long v) {
    return std::string(head) + std::to_string(v);
}

std::uint16_t bit_of(UnitAction a) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(a));
}

std::uint16_t bit_of(CommandKind k) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(k));
}

// 地形展开 **+ 建局前必须先过的那几条校验**。
//
// 为什么是一个函数而不是构造函数体里的几个 `if`：成员初始化在函数体**之前**跑，
// 而 `TerrainMasks` 的构造要解引用 `init.terrain.data()`。空的 terrain 会先撞上
// 那边的断言（Debug）或直接是未定义行为（Release），根本走不到函数体里的检查。
//
// 这一处顺序错了的症状恰好是最坏的一种：Debug 下报一条指向 terrain.cpp 的断言
// （看起来像 `TerrainMasks` 有 bug），Release 下不报。
TerrainMasks checked_masks(const WorldInit& init) {
    if (init.width <= 0 || init.height <= 0) {
        throw ContractError("地图尺寸必须为正");
    }
    const std::size_t cells =
        static_cast<std::size_t>(init.width) * static_cast<std::size_t>(init.height);
    if (init.terrain.size() != cells) {
        throw ContractError(with_int("terrain 长度必须等于 w×h，实际 ",
                                     static_cast<long long>(init.terrain.size())));
    }
    if (!init.no_build.empty() && init.no_build.size() != cells) {
        throw ContractError("no_build 要么为空，要么长度等于 w×h");
    }
    // `Command::slot` 的编码上界，见 `slot_of` 上面那段。**这条必须是运行期的**：
    // 地图边长待标定，而 256×256 正好越界。
    if (cells >= static_cast<std::size_t>(kNoSlot)) {
        throw ContractError(with_int(
            "地图格数必须小于 kNoSlot(65535)，否则 Command::slot 编码会与「无槽位」"
            "哨兵撞上。实际格数 ",
            static_cast<long long>(cells)));
    }
    return TerrainMasks(init.width, init.height, init.terrain.data(),
                        init.no_build.empty() ? nullptr : init.no_build.data());
}

}  // namespace

World::World(WorldInit init)
    : terrain_(checked_masks(init)),
      keep_(init.keep),
      spawns_(std::move(init.spawns)),
      resources_(std::move(init.resources)),
      map_id_(std::move(init.map_id)),
      map_content_hash_(init.map_content_hash),
      seed_(init.seed),
      stats_(init.stats),
      stats_fp_(init.stats.fingerprint()),
      tier_income_permille_(init.tier_income_permille),
      pop_cap_base_(init.pop_cap_base),
      pop_cap_per_keep_level_(init.pop_cap_per_keep_level),
      nominal_level_(init.nominal_level),
      rng_(init.seed),
      fog_{FogLayer(init.width, init.height), FogLayer(init.width, init.height)} {
    // 尺寸、数组长度与格数上界已经在 `checked_masks` 里查过（必须在成员初始化
    // 之前查，见那边的注释）。下面这些要用 `terrain_.in_bounds`，所以在这里。
    if (!terrain_.in_bounds(keep_.i, keep_.j)) {
        throw ContractError("keep 坐标越界");
    }
    if (init.nominal_level < kMinUnitLevel) {
        throw ContractError("nominal_level 不得小于 kMinUnitLevel");
    }
    if (init.tier_income_permille.inner < 0 || init.tier_income_permille.outer < 0) {
        throw ContractError("tier_income_permille 不得为负（负产出不是设计里的东西）");
    }
    if (init.pop_cap_base < 0 || init.pop_cap_per_keep_level < 0) {
        throw ContractError("pop_cap_base / pop_cap_per_keep_level 不得为负"
                            "（负的人口上限不是设计里的东西；要关掉上限就调大它们）");
    }
    for (const SpawnSite& s : spawns_) {
        if (!terrain_.in_bounds(s.pos.i, s.pos.j)) {
            throw ContractError("集结点坐标越界");
        }
    }
    for (const ResourceSite& r : resources_) {
        if (!terrain_.in_bounds(r.pos.i, r.pos.j)) {
            throw ContractError("资源点坐标越界");
        }
    }
    spawn_chosen_.assign(spawns_.size(), 0);

    // 占位格（派生缓存，不进哈希——见头文件那段）。必须在放建筑与障碍**之前**
    // 就位，`place_bld` / `place_obstacle` 要往里写。
    bld_at_.assign(terrain_.cell_count(), 0);
    obstacle_at_.assign(terrain_.cell_count(), 0);

    // 初始建筑。
    int keep_count = 0;
    for (const BldInit& b : init.buildings) {
        if (!terrain_.in_bounds(b.pos.i, b.pos.j)) {
            throw ContractError("初始建筑坐标越界");
        }
        if (b.hp <= 0 || b.hp > b.max_hp) {
            throw ContractError("初始建筑血量必须满足 0 < hp <= max_hp");
        }
        if (b.type == BldType::Keep) {
            ++keep_count;
            if (!(b.pos == keep_)) {
                throw ContractError("Keep 的位置必须与 WorldInit::keep 一致");
            }
        }
        place_bld(b.type, b.pos, b.hp, b.max_hp);
    }
    // **恰好一座 `Keep`。** 零座会让「丢失即败」这个败北条件无从表达，
    // 两座会让它变成「两座都丢才败」——两种都不是设计里的东西，
    // 而两种都不会有任何别的地方报错。
    if (keep_count != 1) {
        throw ContractError(with_int("必须恰好有一座 Keep（丢失即败的那一座），实际 ",
                                     keep_count));
    }

    // 初始单位。校验全部复用 `spawn_unit`（类型越界、等级下限、血量），
    // 不在这里抄一份——抄一份的后果是两处迟早不一致，而不一致的那一侧
    // 会让某种非法初始状态从回放里悄悄进来。
    //
    // **位置刻意不做界内检查**，与 `spawn_unit` 保持一致：单位位置是连续的浮点，
    // 边界外该怎么处理（夹紧、判死、还是不可能发生）属寻路与移动，是 1c 的事。
    // 在这里单独加一道检查会造出「建局时管、跑起来不管」的不对称。
    for (const UnitInit& u : init.units) {
        spawn_unit(u.type, u.pos, u.level, u.hp, u.max_hp);
    }

    // 初始障碍。校验同样全部复用 `place_obstacle`（类型越界、界内、血量），
    // 理由与上面单位那段逐字相同。
    //
    // **这是障碍进世界的唯一入口。** `World` 刻意不提供「跑动中生成一个障碍」的
    // 方法，于是 `无尽模式与地形分层.md` 6.6 那条「不可再生」在结构上成立，
    // 而不是靠一句文档约定——可再生 + 破坏后产出资源 = 一条无限资源循环。
    for (const ObstacleInit& o : init.obstacles) {
        place_obstacle(o.type, o.pos, o.hp, o.max_hp);
    }
}

// ——波次——

void World::begin_assault() noexcept { phase_ = WavePhase::Assault; }

void World::begin_next_wave(std::int32_t next_nominal_level) {
    if (next_nominal_level < kMinUnitLevel) {
        throw ContractError("nominal_level 不得小于 kMinUnitLevel");
    }
    ++wave_;
    phase_ = WavePhase::Build;
    nominal_level_ = next_nominal_level;
    // 集结点的选择是**逐波**的（分兵佯攻每波重新决定），所以跨波清零。
    // 编成位分配同理由命令重设，但**不在这里清**：`Composition` 在建造阶段下达，
    // 而这个函数正是进入建造阶段的那一步，清了就把同一 tick 里先到的命令抹掉。
    for (std::uint8_t& c : spawn_chosen_) c = std::uint8_t{0};
}

// ——输入——

void World::validate(Side side, const Command& c) const {
    if (!is_legal_for(c.kind, side)) {
        throw ContractError(std::string("命令 ") + std::string(ident_of(c.kind)) +
                            " 不属于 " + std::string(ident_of(side)) + " 的动作空间");
    }
    // `c.side` 与参数不一致是**格式合法、语义非法**的那一类（见 `rts/command.hpp`
    // 里 `owner_of` 的注释）。回放读进来的字节可以是任何东西，所以要查。
    if (c.side != side) {
        throw ContractError("Command::side 与 submit 的 side 参数不一致");
    }

    const std::size_t cells = terrain_.cell_count();
    switch (c.kind) {
        case CommandKind::None:
        case CommandKind::Summon:
            break;
        case CommandKind::Build:
            if (static_cast<int>(c.what) >= kBldTypeCount) {
                throw ContractError("Build 的建筑类型越界");
            }
            [[fallthrough]];
        case CommandKind::Repair:
        case CommandKind::Cancel:
        case CommandKind::Demolish:
        // `Clear`/`Upgrade` 与它们同一条校验：都只要一个合法的格下标。
        // **这里刻意不查「那一格上真的有障碍/建筑/顶没顶到等级上限」**——
        // 那要遍历建筑数组、算 `building_level_cap()`，属机制（1c），
        // 而本层只做字节校验。同 `Build` 不查「买不买得起」。
        case CommandKind::Clear:
        case CommandKind::Upgrade:
            if (c.slot == kNoSlot || static_cast<std::size_t>(c.slot) >= cells) {
                throw ContractError("槽位越界（编码是格线性下标 y*w+x）");
            }
            break;
        case CommandKind::Train:
            if (static_cast<int>(c.what) >= kUnitTypeCount ||
                side_of(unit_at(c.what)) != Side::Defender) {
                throw ContractError("Train 只能产出守方单位");
            }
            if (c.slot == kNoSlot || static_cast<std::size_t>(c.slot) >= cells) {
                throw ContractError("Train 的兵营槽位越界");
            }
            // **只查格式，不查 `unit_level_cap()`**——同 `Upgrade` 那条纪律：
            // 顶没顶到上限是状态依赖的检查，属机制（`apply_one`），这里只挡
            // 「等级字段本身就没编码出一个合法值」（0 不是任何单位会有的等级，
            // `kMinUnitLevel == 1`）。
            if (c.level < kMinUnitLevel) {
                throw ContractError("Train 的 level 字段不得小于 kMinUnitLevel");
            }
            break;
        case CommandKind::Composition:
            if (static_cast<int>(c.what) >= kUnitTypeCount ||
                side_of(unit_at(c.what)) != Side::Attacker) {
                throw ContractError("Composition 只能配比攻方单位");
            }
            // **刻意不校验编成位权重的上界。** 「硬性封顶约 40」里的 40 是个
            // 待标定的数值（CLAUDE.md「关于数值」把编成位占用列在待定清单里），
            // 而契约规则要求「接口的承诺不得依赖任何待定数值」。
            // 封顶该在应用预算的那一层查，不在字节校验这一层。
            break;
        case CommandKind::PickSpawn:
            if (c.slot == kNoSlot ||
                static_cast<std::size_t>(c.slot) >= spawns_.size()) {
                throw ContractError("集结点下标越界");
            }
            break;
    }
}

void World::submit(Side side, const Command* cmds, std::size_t count) {
    if (count > 0 && cmds == nullptr) throw ContractError("cmds 为空指针");
    // **先全部校验，再全部入队。** 半批入队会让「这次提交失败了」与
    // 「前三条生效了、第四条没有」不可区分，而回放里两者的后果完全不同。
    for (std::size_t k = 0; k < count; ++k) validate(side, cmds[k]);
    std::vector<Command>& q = cmd_queue_[static_cast<std::size_t>(index_of(side))];
    q.insert(q.end(), cmds, cmds + count);
}

void World::submit_actions(Side side, const UnitAction* actions, std::size_t count) {
    const int live = live_unit_count(side);
    if (count != static_cast<std::size_t>(live)) {
        throw ContractError(
            with_int("动作数组长度必须恰好等于该侧活着的单位数 ", live) +
            with_int("，实际 ", static_cast<long long>(count)));
    }
    if (count > 0 && actions == nullptr) throw ContractError("actions 为空指针");

    // 先全部校验再全部写入，理由同 `submit`：半批生效不可区分于整批失败。
    for (std::size_t k = 0; k < count; ++k) {
        if (static_cast<int>(actions[k]) >= kUnitActionCount) {
            throw ContractError(with_int("动作枚举值越界，下标 ",
                                         static_cast<long long>(k)));
        }
    }

    // 按 `enumerate_units` 的顺序（槽位下标升序）逐个写入。
    std::size_t k = 0;
    for (std::size_t s = 0; s < unit_pool_.slot_count(); ++s) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(s))) continue;
        if (side_of(u_type_[s]) != side) continue;
        u_action_[s] = actions[k++];
    }
}

void World::submit_garrison_wishes(Side side, const std::uint16_t* targets,
                                   std::size_t count) {
    // 登墙是守方专属（「攻方没有登墙手段」是结构，CLAUDE.md）。给攻方开这个
    // 通道会让那条从结构退化成约定，所以在这里直接拒。
    if (side != Side::Defender) {
        throw ContractError("登墙意愿是守方专属通道（攻方没有登墙手段）");
    }
    const int live = live_unit_count(side);
    if (count != static_cast<std::size_t>(live)) {
        throw ContractError(
            with_int("登墙意愿数组长度必须恰好等于守方活着的单位数 ", live) +
            with_int("，实际 ", static_cast<long long>(count)));
    }
    if (count > 0 && targets == nullptr) throw ContractError("targets 为空指针");

    // 先全部校验再全部写入，理由同 `submit`：半批生效不可区分于整批失败。
    const std::size_t cells = terrain_.cell_count();
    for (std::size_t k = 0; k < count; ++k) {
        if (targets[k] != kNoSlot && static_cast<std::size_t>(targets[k]) >= cells) {
            throw ContractError(with_int("登墙意愿的槽位越界，下标 ",
                                         static_cast<long long>(k)));
        }
    }

    // 按 `enumerate_units` 的顺序（槽位下标升序）逐个写入，与 `submit_actions` 同。
    std::size_t k = 0;
    for (std::size_t s = 0; s < unit_pool_.slot_count(); ++s) {
        if (!unit_pool_.alive_at(static_cast<std::uint16_t>(s))) continue;
        if (side_of(u_type_[s]) != side) continue;
        u_garrison_target_[s] = targets[k++];
    }
}

void World::apply_one(const Command& c) {
    switch (c.kind) {
        case CommandKind::None:
            // **「跳过」是一个合法动作，不是「这一格没填」**（`rts/command.hpp`）。
            // 所以它什么都不做，而且这不算「本版没解算」。
            break;
        case CommandKind::Summon:
            // CLAUDE.md 明写「必须提供『提前召唤下一波』（附奖励）」。
            // 奖励在 `train/`；这里只做阶段转换，不需要任何数值。
            if (phase_ == WavePhase::Build) begin_assault();
            break;
        case CommandKind::Composition:
            composition_[static_cast<std::size_t>(c.what)] = composition_slots(c);
            break;
        case CommandKind::PickSpawn:
            // 可对多个集结点各下一条 = 分兵佯攻，所以是置位而不是赋值。
            spawn_chosen_[static_cast<std::size_t>(c.slot)] = std::uint8_t{1};
            break;
        // ——第二批解算的五种——
        //
        // **世界状态层面的不合法一律无操作**（买不起、格被占、点位不匹配、
        // 目标不存在）。结构合法性已在 `validate` 拦过（字节层：越界、错侧），
        // 剩下的是「掩码本可以屏蔽、但掩码刻意只做每侧一份」那一类——
        // 丢弃是确定性的，且与 RL 约定一致（错误动作只浪费样本）。
        case CommandKind::Build: {
            const BldType bt = static_cast<BldType>(c.what);
            // `Keep` 不可再建：它的唯一性是「丢失即败」与「兵力地板天然不可
            // 摧毁」两条论证共同的前提。结构，不是数值。
            if (bt == BldType::Keep) break;
            const GridPos p = pos_of_slot(c.slot, width());
            if (!terrain_.buildable(p.i, p.j)) break;
            const std::size_t cell = static_cast<std::size_t>(c.slot);
            if (bld_at_[cell] != 0 || obstacle_at_[cell] != 0) break;
            // 地面单位站着的格不落地基（会把人封进墙里）；空军飞在上面无妨。
            {
                bool occupied = false;
                for (std::size_t s = 0; s < unit_pool_.slot_count(); ++s) {
                    if (!unit_pool_.alive_at(static_cast<std::uint16_t>(s))) continue;
                    if (is_aerial(u_type_[s])) continue;
                    if (grid_of(u_pos_[s]) == p) {
                        occupied = true;
                        break;
                    }
                }
                if (occupied) break;
            }
            // 资源点规则：采集建筑必须建在**对应种类**的资源点上（收入的
            // 唯一来源，CLAUDE.md「守方多资源」），其余建筑不得占资源点——
            // 把资源点糊死是不可逆的浪费，按「结构封死」处理，不留给数值劝退。
            {
                const ResourceSite* site = nullptr;
                for (const ResourceSite& r : resources_) {
                    if (r.pos == p) {
                        site = &r;
                        break;
                    }
                }
                if (is_gatherer(bt)) {
                    if (site == nullptr || site->kind != resource_of(bt)) break;
                } else if (site != nullptr) {
                    break;
                }
            }
            const BldStats& s = stats_.of(bt);
            if (stock_[static_cast<std::size_t>(Resource::Stone)] < s.cost_stone ||
                stock_[static_cast<std::size_t>(Resource::Wood)] < s.cost_wood) {
                break;
            }
            stock_[static_cast<std::size_t>(Resource::Stone)] -= s.cost_stone;
            stock_[static_cast<std::size_t>(Resource::Wood)] -= s.cost_wood;
            // 工地从 1 血起步、由工匠随工时把血盖上去（tick_economy）。
            // 工期为 0（未标定表的诚实默认）当场完工，那就该是满血。
            if (s.build_ticks > 0) {
                place_bld(bt, p, 1, s.max_hp, s.build_ticks);
            } else {
                place_bld(bt, p, s.max_hp, s.max_hp, 0);
            }
            break;
        }
        case CommandKind::Repair: {
            const std::size_t cell = static_cast<std::size_t>(c.slot);
            if (bld_at_[cell] == 0) break;
            const std::size_t k = static_cast<std::size_t>(bld_at_[cell] - 1);
            if (!b_built_[k]) break;              // 工地不「修」，工地是往前盖
            if (b_work_[k] > 0) break;             // 已经在修了
            if (b_upgrade_left_[k] > 0) break;     // 在升级：两件工程不能同时推进
            const std::int64_t missing = b_max_hp_[k] - b_hp_[k];
            if (missing <= 0) break;
            const GlobalStats& g = stats_.global;
            // 维修只花木材（木材那条决策轴：恢复速度与可维持性）。
            // 费用与工时都对缺口取整向上；除法是普通整数除法，**不走
            // `apply_permille`**——那个「钳到 >= 1」是伤害规则，不是记账规则。
            const std::int64_t wood = (missing * g.repair_wood_per_1000hp + 999) / 1000;
            if (stock_[static_cast<std::size_t>(Resource::Wood)] < wood) break;
            const std::int64_t rate =
                g.repair_hp_per_work_tick < 1 ? 1 : g.repair_hp_per_work_tick;
            std::int64_t ticks = (missing + rate - 1) / rate;
            if (ticks > INT32_MAX) ticks = INT32_MAX;
            stock_[static_cast<std::size_t>(Resource::Wood)] -= wood;
            b_work_[k] = static_cast<std::int32_t>(ticks);
            break;
        }
        case CommandKind::Cancel: {
            const std::size_t cell = static_cast<std::size_t>(c.slot);
            if (bld_at_[cell] == 0) break;
            const std::size_t k = static_cast<std::size_t>(bld_at_[cell] - 1);
            if (!b_built_[k]) {
                // 未完工时取消施工，全额返还最初支付的基础造价。施工进度不影响
                // 退款，避免玩家因误放或临时调整布局损失材料。
                const BldStats& s = stats_.of(b_type_[k]);
                stock_[static_cast<std::size_t>(Resource::Stone)] +=
                    s.cost_stone;
                stock_[static_cast<std::size_t>(Resource::Wood)] +=
                    s.cost_wood;
                destroy_bld(bld_pool_.id_at(static_cast<std::uint16_t>(k)));
            } else if (b_work_[k] > 0) {
                // 放弃维修：预付的木材不退。占位决定——若标定时把维修改成
                // 按进度结算，这一行跟着改。
                b_work_[k] = 0;
            } else if (b_upgrade_left_[k] > 0) {
                // 放弃升级：预付的石/木同样不退，与放弃维修同一条理由。
                b_upgrade_left_[k] = 0;
            }
            break;
        }
        case CommandKind::Demolish: {
            const std::size_t cell = static_cast<std::size_t>(c.slot);
            if (bld_at_[cell] == 0) break;
            const std::size_t k = static_cast<std::size_t>(bld_at_[cell] - 1);
            if (!b_built_[k]) break;                // 工地只能走 Cancel，退款语义不同
            if (b_type_[k] == BldType::Keep) break; // 堡垒丢失即败，不允许主动拆除
            const BldStats& s = stats_.of(b_type_[k]);
            const std::int32_t pm = stats_.global.demolish_refund_permille;
            stock_[static_cast<std::size_t>(Resource::Stone)] +=
                s.cost_stone * pm / 1000;
            stock_[static_cast<std::size_t>(Resource::Wood)] +=
                s.cost_wood * pm / 1000;
            destroy_bld(bld_pool_.id_at(static_cast<std::uint16_t>(k)));
            break;
        }
        case CommandKind::Upgrade: {
            const std::size_t cell = static_cast<std::size_t>(c.slot);
            if (bld_at_[cell] == 0) break;
            const std::size_t k = static_cast<std::size_t>(bld_at_[cell] - 1);
            if (!b_built_[k]) break;              // 工地不能升级，先盖完
            if (b_work_[k] > 0) break;             // 在建/在修：两件工程不能同时推进
            if (b_upgrade_left_[k] > 0) break;     // 已经在升了
            // `Keep` 本身不受等级上限约束（「堡垒等级本身不设上限」），
            // 其余建筑受 `building_level_cap()` 约束——它由 `Keep` 的等级推导。
            if (b_type_[k] != BldType::Keep &&
                b_level_[k] >= building_level_cap()) {
                break;
            }
            const BldStats& s = stats_.of(b_type_[k]);
            // **定价走 `bld_upgrade_cost_*()`，不读表里那个常数**：非 `Keep`
            // 的累计造价要与它买到的战力同阶（`∝ √B(L)`），否则最优档恒为
            // 1 级、这个输出是装饰品。理由见 `world.hpp` 那两条声明。
            const std::int64_t up_s = bld_upgrade_cost_stone(b_type_[k], b_level_[k]);
            const std::int64_t up_w = bld_upgrade_cost_wood(b_type_[k], b_level_[k]);
            if (stock_[static_cast<std::size_t>(Resource::Stone)] < up_s ||
                stock_[static_cast<std::size_t>(Resource::Wood)] < up_w) {
                break;
            }
            stock_[static_cast<std::size_t>(Resource::Stone)] -= up_s;
            stock_[static_cast<std::size_t>(Resource::Wood)] -= up_w;
            // 工期 <= 0（未标定表的诚实默认）当场完工——与 `Build` 同一条先例。
            // 完工逻辑与 `tick_economy` 里工时归零那一刻的分支逐字相同，
            // 这里直接调用，不留一个「倒计时为 0 但还没结算」的中间态。
            if (s.upgrade_ticks > 0) {
                b_upgrade_left_[k] = s.upgrade_ticks;
            } else {
                finish_upgrade(k);
            }
            break;
        }
        case CommandKind::Train: {
            const std::size_t cell = static_cast<std::size_t>(c.slot);
            if (bld_at_[cell] == 0) break;
            const std::size_t k = static_cast<std::size_t>(bld_at_[cell] - 1);
            const BldType bt = b_type_[k];
            // `Barrack` 是正职；`Keep` 是「兵营可退化为从堡垒出兵」那条兜底
            // （CLAUDE.md 建筑花名册砍单顺序的依据）。其余建筑不出兵。
            if (bt != BldType::Barrack && bt != BldType::Keep) break;
            if (!b_built_[k]) break;
            if (b_train_type_[k] != kNoTrain) break;   // 一次一名
            // 兵种等级上限（守方升级轴第三个输出）：`validate()` 只挡了
            // 「level 字段本身不合法」，顶没顶到 `unit_level_cap()` 是状态
            // 依赖的检查，落在这里——同 `Upgrade` 对 `building_level_cap()`
            // 的既定纪律。
            if (c.level > unit_level_cap()) break;
            // 人口上限（守方升级轴第一个输出）：满员静默拒绝，与「钱不够
            // 无操作」同款语义。人口是派生量（现算，含在训占位），不是
            // 新状态——见 `defender_pop()`。攻方不经 `Train` 出兵
            // （Composition/波次生成走 `spawn_unit`），不受此限。
            if (defender_pop() >= defender_pop_cap()) break;
            const UnitType ut = static_cast<UnitType>(c.what);
            const std::int64_t cost = train_cost_gold(ut, c.level);
            if (stock_[static_cast<std::size_t>(Resource::Gold)] < cost) break;
            stock_[static_cast<std::size_t>(Resource::Gold)] -= cost;
            b_train_type_[k] = c.what;
            b_train_left_[k] = train_ticks_at(ut, c.level);
            b_train_level_[k] = c.level;
            break;
        }
        case CommandKind::Clear: {
            // 同上：记指令，不代打。破坏与产出早已是机制（移动撞上自动开始
            // 破坏 + 最后一击归属产出），这条只是把「玩家要清这一格」放进
            // 脚本执行层读得到的地方。
            const std::size_t cell = static_cast<std::size_t>(c.slot);
            if (obstacle_at_[cell] == 0) break;
            o_clear_ordered_[static_cast<std::size_t>(obstacle_at_[cell] - 1)] = 1;
            break;
        }
    }
    // 全部命令都有解算分支（驻守已改为逐单位登墙意愿，不再是命令），未解算计数器按约定早已删除。
    // 这个 switch 没有 default，新增命令种类漏写分支会被 -Wswitch / /w14062 逮住。
}

void World::advance(int ticks) {
    if (ticks < 0) throw ContractError("advance 的 tick 数不得为负");
    for (int t = 0; t < ticks; ++t) {
        // 阶段顺序的规范在头文件 `advance()` 的注释里，**这里只是照着做**。
        // 顺序固定是确定性的一部分：守方先、攻方后，各按提交顺序。
        for (int s = 0; s < kSideCount; ++s) {
            std::vector<Command>& q = cmd_queue_[static_cast<std::size_t>(s)];
            for (const Command& c : q) apply_one(c);
            q.clear();
        }

        // 机制的六个阶段（src/mechanics.cpp）。
        // 原先这里那两个「纯计数器递减」循环已被吸收：前摇归 tick_unit_combat
        // （只有已承诺的攻击才有前摇可递减），施工进度归 tick_economy
        // （第二批起要工匠在场才推进，不再是纯递减）。
        tick_unit_combat();
        tick_movement();
        tick_garrison();
        tick_bld_combat();
        tick_projectiles();
        tick_economy();
        tick_vision();

        ++tick_;
    }
}

// ——实体——

UnitId World::spawn_unit(UnitType type, Vec2 pos, std::int32_t level, std::int64_t hp,
                         std::int64_t max_hp) {
    if (static_cast<int>(type) >= kUnitTypeCount) {
        throw ContractError("单位类型越界");
    }
    if (level < kMinUnitLevel) {
        // 等级从 1 起不是风格：观测的等级通道存**和**，「和 = 0 ⟺ 该格无敌人」
        // 依赖它（`rts/roster.hpp` 的 kMinUnitLevel 那段）。
        throw ContractError(with_int("单位等级不得小于 kMinUnitLevel，实际 ", level));
    }
    if (hp <= 0 || hp > max_hp) throw ContractError("单位血量必须满足 0 < hp <= max_hp");

    const std::uint16_t k = unit_pool_.acquire();
    const std::size_t n = unit_pool_.slot_count();
    if (u_type_.size() < n) {
        u_type_.resize(n);
        u_level_.resize(n);
        u_hp_.resize(n);
        u_max_hp_.resize(n);
        u_pos_.resize(n);
        u_windup_.resize(n);
        u_action_.resize(n);
        u_garrison_.resize(n);
        u_garrison_target_.resize(n);
        u_mount_.resize(n);
        u_charge_.resize(n);
        u_cd_.resize(n);
        u_tgt_kind_.resize(n);
        u_tgt_raw_.resize(n);
        u_aim_.resize(n);
    }
    u_type_[k] = type;
    u_level_[k] = level;
    u_hp_[k] = hp;
    u_max_hp_[k] = max_hp;
    u_pos_[k] = pos;
    u_windup_[k] = 0;
    // 新单位的默认动作是 `Stop`。零初始化的动作数组等于「全体停住」，
    // 那是唯一安全的默认值（`rts/action.hpp`）。
    u_action_[k] = UnitAction::Stop;
    u_garrison_[k] = kNoSlot;
    u_garrison_target_[k] = kNoSlot;
    u_mount_[k] = 0;
    u_charge_[k] = 0.0f;
    u_cd_[k] = 0;
    u_tgt_kind_[k] = TgtKind::None;
    u_tgt_raw_[k] = 0;
    u_aim_[k] = Vec2{};
    return unit_pool_.id_at(k);
}

BldId World::place_bld(BldType type, GridPos pos, std::int64_t hp, std::int64_t max_hp,
                       std::int32_t work_left) {
    if (static_cast<int>(type) >= kBldTypeCount) throw ContractError("建筑类型越界");
    if (!terrain_.in_bounds(pos.i, pos.j)) throw ContractError("建筑坐标越界");
    if (hp <= 0 || hp > max_hp) throw ContractError("建筑血量必须满足 0 < hp <= max_hp");
    if (work_left < 0) throw ContractError("施工进度不得为负");

    const std::uint16_t k = bld_pool_.acquire();
    const std::size_t n = bld_pool_.slot_count();
    if (b_type_.size() < n) {
        b_type_.resize(n);
        b_pos_.resize(n);
        b_hp_.resize(n);
        b_max_hp_.resize(n);
        b_work_.resize(n);
        b_cd_.resize(n);
        b_windup_.resize(n);
        b_tgt_raw_.resize(n);
        b_aim_.resize(n);
        b_built_.resize(n);
        b_train_type_.resize(n);
        b_train_left_.resize(n);
        b_train_level_.resize(n);
        b_level_.resize(n);
        b_upgrade_left_.resize(n);
    }
    b_type_[k] = type;
    b_pos_[k] = pos;
    b_hp_[k] = hp;
    b_max_hp_[k] = max_hp;
    b_work_[k] = work_left;
    b_cd_[k] = 0;
    b_windup_[k] = 0;
    b_tgt_raw_[k] = UnitId::kInvalidRaw;
    b_aim_[k] = Vec2{};
    // 带工时进场的是工地（`Build` 命令那条路），不带的当场就是完工建筑
    // （初始城圈与测试直摆的都走这里）。
    b_built_[k] = (work_left == 0) ? std::uint8_t{1} : std::uint8_t{0};
    b_train_type_[k] = kNoTrain;
    b_train_left_[k] = 0;
    b_train_level_[k] = kMinUnitLevel;
    b_level_[k] = 1;
    b_upgrade_left_[k] = 0;
    bld_at_[static_cast<std::size_t>(pos.j) * static_cast<std::size_t>(width()) +
            static_cast<std::size_t>(pos.i)] = static_cast<std::uint16_t>(k + 1);
    return bld_pool_.id_at(k);
}

ObstacleId World::place_obstacle(ObstacleType type, GridPos pos, std::int64_t hp,
                                 std::int64_t max_hp) {
    if (static_cast<int>(type) >= kObstacleTypeCount) {
        throw ContractError("障碍类型越界");
    }
    if (!terrain_.in_bounds(pos.i, pos.j)) throw ContractError("障碍坐标越界");
    if (hp <= 0 || hp > max_hp) throw ContractError("障碍血量必须满足 0 < hp <= max_hp");

    const std::uint16_t k = obstacle_pool_.acquire();
    const std::size_t n = obstacle_pool_.slot_count();
    if (o_type_.size() < n) {
        o_type_.resize(n);
        o_pos_.resize(n);
        o_hp_.resize(n);
        o_max_hp_.resize(n);
        o_clear_ordered_.resize(n);
    }
    o_type_[k] = type;
    o_pos_[k] = pos;
    o_hp_[k] = hp;
    o_max_hp_[k] = max_hp;
    o_clear_ordered_[k] = 0;
    obstacle_at_[static_cast<std::size_t>(pos.j) * static_cast<std::size_t>(width()) +
                 static_cast<std::size_t>(pos.i)] = static_cast<std::uint16_t>(k + 1);
    return obstacle_pool_.id_at(k);
}

// 三个销毁函数都把槽位的字段**清成规范值**。
//
// 不清也能跑（`WorldView` 要求调用方按 alive 过滤），但那会让 `state_hash`
// 取决于「这个槽以前装的是什么」——两个走到同一状态的世界会算出不同的哈希，
// 而回放测试于是变成偶发失败。清一遍是 O(1)。
void World::kill_unit(UnitId id) {
    const std::size_t k = require(id);
    u_type_[k] = UnitType::Archer;
    u_level_[k] = kMinUnitLevel;
    u_hp_[k] = 0;
    u_max_hp_[k] = 0;
    u_pos_[k] = Vec2{};
    u_windup_[k] = 0;
    u_action_[k] = UnitAction::Stop;
    u_garrison_[k] = kNoSlot;
    u_garrison_target_[k] = kNoSlot;
    u_mount_[k] = 0;
    u_charge_[k] = 0.0f;
    u_cd_[k] = 0;
    u_tgt_kind_[k] = TgtKind::None;
    u_tgt_raw_[k] = 0;
    u_aim_[k] = Vec2{};
    unit_pool_.release(static_cast<std::uint16_t>(k));
}

void World::destroy_bld(BldId id) {
    const std::size_t k = require(id);
    {
        const std::size_t c =
            static_cast<std::size_t>(b_pos_[k].j) * static_cast<std::size_t>(width()) +
            static_cast<std::size_t>(b_pos_[k].i);
        // 只清自己占的那一格：两座建筑落在同一格时后放的覆盖了前者，
        // 先拆前者不该把后者的占位抹掉。
        if (bld_at_[c] == static_cast<std::uint16_t>(k + 1)) bld_at_[c] = 0;
        // 墙塌了，上面的人**当场落地站在缺口里**（缺口在修复前可通行，
        // CLAUDE.md）——不给延迟：那不是「下墙」，是脚下的东西没了。
        // 在爬的一并解除（人本来就还在地面）。指着这段墙的登墙意愿随墙作废
        // ——不清的话，原地重建一座新墙会让旧意愿突然复活，那是「等一座
        // 还没许诺的新墙」的同款形态。
        if (bld_at_[c] == 0) {
            const std::uint16_t slot = slot_of(b_pos_[k], width());
            for (std::size_t u = 0; u < unit_pool_.slot_count(); ++u) {
                if (!unit_pool_.alive_at(static_cast<std::uint16_t>(u))) continue;
                if (u_garrison_[u] == slot) {
                    u_garrison_[u] = kNoSlot;
                    u_mount_[u] = 0;
                }
                if (u_garrison_target_[u] == slot) u_garrison_target_[u] = kNoSlot;
            }
        }
    }
    b_type_[k] = BldType::Keep;
    b_pos_[k] = GridPos{};
    b_hp_[k] = 0;
    b_max_hp_[k] = 0;
    b_work_[k] = 0;
    b_cd_[k] = 0;
    b_windup_[k] = 0;
    b_tgt_raw_[k] = UnitId::kInvalidRaw;
    b_aim_[k] = Vec2{};
    b_built_[k] = 0;
    b_train_type_[k] = kNoTrain;
    b_train_left_[k] = 0;
    b_train_level_[k] = kMinUnitLevel;
    b_level_[k] = 1;
    b_upgrade_left_[k] = 0;
    bld_pool_.release(static_cast<std::uint16_t>(k));
}

void World::destroy_obstacle(ObstacleId id) {
    const std::size_t k = require(id);
    {
        const std::size_t c =
            static_cast<std::size_t>(o_pos_[k].j) * static_cast<std::size_t>(width()) +
            static_cast<std::size_t>(o_pos_[k].i);
        if (obstacle_at_[c] == static_cast<std::uint16_t>(k + 1)) obstacle_at_[c] = 0;
    }
    o_type_[k] = ObstacleType::Stump;
    o_pos_[k] = GridPos{};
    o_hp_[k] = 0;
    o_max_hp_[k] = 0;
    o_clear_ordered_[k] = 0;
    obstacle_pool_.release(static_cast<std::uint16_t>(k));
}

int World::live_unit_count(Side side) const noexcept {
    int n = 0;
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        if (unit_pool_.alive_at(static_cast<std::uint16_t>(k)) &&
            side_of(u_type_[k]) == side) {
            ++n;
        }
    }
    return n;
}

void World::enumerate_units(Side side, std::vector<UnitId>& out) const {
    out.clear();
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        const std::uint16_t s = static_cast<std::uint16_t>(k);
        if (unit_pool_.alive_at(s) && side_of(u_type_[k]) == side) {
            out.push_back(unit_pool_.id_at(s));
        }
    }
}

std::size_t World::require(UnitId id) const {
    if (!unit_pool_.alive(id)) throw ContractError("UnitId 已失效（代数不符或槽位已空）");
    return id.index();
}

std::size_t World::require(BldId id) const {
    if (!bld_pool_.alive(id)) throw ContractError("BldId 已失效");
    return id.index();
}

std::size_t World::require(ObstacleId id) const {
    if (!obstacle_pool_.alive(id)) throw ContractError("ObstacleId 已失效");
    return id.index();
}

UnitType World::unit_type(UnitId id) const { return u_type_[require(id)]; }
Side World::unit_side(UnitId id) const { return side_of(u_type_[require(id)]); }
std::int32_t World::unit_level(UnitId id) const { return u_level_[require(id)]; }
std::int64_t World::unit_hp(UnitId id) const { return u_hp_[require(id)]; }
Vec2 World::unit_pos(UnitId id) const { return u_pos_[require(id)]; }
UnitAction World::unit_action(UnitId id) const { return u_action_[require(id)]; }
std::int32_t World::unit_windup(UnitId id) const { return u_windup_[require(id)]; }
std::int32_t World::unit_cooldown(UnitId id) const { return u_cd_[require(id)]; }
TgtKind World::unit_target_kind(UnitId id) const { return u_tgt_kind_[require(id)]; }
Vec2 World::unit_aim(UnitId id) const { return u_aim_[require(id)]; }
std::uint16_t World::unit_garrison(UnitId id) const { return u_garrison_[require(id)]; }
std::int32_t World::unit_mount(UnitId id) const { return u_mount_[require(id)]; }
float World::unit_charge(UnitId id) const { return u_charge_[require(id)]; }

BldType World::bld_type(BldId id) const { return b_type_[require(id)]; }
GridPos World::bld_pos(BldId id) const { return b_pos_[require(id)]; }
std::int64_t World::bld_hp(BldId id) const { return b_hp_[require(id)]; }
bool World::bld_complete(BldId id) const { return b_built_[require(id)] != 0; }
std::int32_t World::bld_level(BldId id) const { return b_level_[require(id)]; }
std::int32_t World::bld_upgrade_left(BldId id) const {
    return b_upgrade_left_[require(id)];
}

bool World::obstacle_clear_ordered(ObstacleId id) const {
    return o_clear_ordered_[require(id)] != 0;
}

void World::set_stock(Resource r, std::int64_t v) noexcept {
    stock_[static_cast<std::size_t>(r)] = v;
}

bool World::spawn_chosen(std::size_t spawn_index) const {
    if (spawn_index >= spawn_chosen_.size()) throw ContractError("集结点下标越界");
    return spawn_chosen_[spawn_index] != 0;
}

// ——掩码——

std::uint16_t World::action_mask(UnitId id) const {
    const std::size_t k = require(id);
    const Mobility mob = is_aerial(u_type_[k]) ? Mobility::Aerial : Mobility::Ground;
    const GridPos here = grid_of(u_pos_[k]);

    std::uint16_t mask = bit_of(UnitAction::Stop);   // 停住永远合法

    // 在爬墙：既不能打也不能走（上墙延迟的代价就是这段不设防），只剩停住。
    if (u_garrison_[k] != kNoSlot && u_mount_[k] > 0) return mask;

    // 攻击 4 位：**精确**——射程内有没有一个合法目标（§1.1.1 乙的另一半）。
    // 与战斗阶段用同一个 `pick_target` 与同一个 `effective_range`
    // （远程驻守的高度加成在内），两处不会各判一套。
    {
        const float range = effective_range(k);
        const float r2 = range * range;
        constexpr UnitAction kAtk[] = {UnitAction::AtkNear, UnitAction::AtkWeak,
                                       UnitAction::AtkBld, UnitAction::AtkWall};
        for (const UnitAction a : kAtk) {
            const TargetPick t = pick_target(k, a);
            if (t.found && t.dist2 <= r2) {
                mask = static_cast<std::uint16_t>(mask | bit_of(a));
            }
        }
    }

    // 已登顶：钉在那一段，移动 8 位全非法（「上墙的代价是机动性」——这不是
    // 「未定默认允许」那类位，钉住是结构，错误地允许会让策略学出幽灵换位）。
    if (u_garrison_[k] != kNoSlot) return mask;

    for (int d = 0; d < kMoveDirCount; ++d) {
        const UnitAction a = move_of(d);
        const GridDelta delta = move_delta(a);
        const int nx = here.i + delta.di;
        const int ny = here.j + delta.dj;
        if (!terrain_.in_bounds(nx, ny)) continue;
        if (!terrain_.passable(nx, ny, mob)) continue;
        // **斜向穿角。** 注意「斜向」指的是**格坐标**的斜向，即屏幕上的正北东南西
        // ——按名字猜会恰好猜反，所以走 `is_grid_diagonal()`（`rts/action.hpp`）。
        if (kDiagonalNeedsBothOrthogonal && is_grid_diagonal(a)) {
            const bool ok_i = terrain_.in_bounds(nx, here.j) &&
                              terrain_.passable(nx, here.j, mob);
            const bool ok_j = terrain_.in_bounds(here.i, ny) &&
                              terrain_.passable(here.i, ny, mob);
            if (!ok_i || !ok_j) continue;
        }
        mask = static_cast<std::uint16_t>(mask | bit_of(a));
    }
    return mask;
}

std::uint16_t World::command_mask(Side side) const noexcept {
    std::uint16_t mask = 0;
    for (int k = 0; k < kCommandKindCount; ++k) {
        const CommandKind kind = command_kind_at(k);
        if (!is_legal_for(kind, side)) continue;
        // `Summon` 只在建造阶段有意义——它就是「提前结束建造阶段」。
        if (kind == CommandKind::Summon && phase_ != WavePhase::Build) continue;
        // 买得起买不起、槽位占没占，要造价与占用表，属 1c。默认允许，同上。
        mask = static_cast<std::uint16_t>(mask | bit_of(kind));
    }
    return mask;
}

std::int32_t World::building_level_cap() const noexcept {
    const std::size_t cell =
        static_cast<std::size_t>(keep_.j) * static_cast<std::size_t>(width()) +
        static_cast<std::size_t>(keep_.i);
    // `keep_` 处恰有一座 `Keep`（构造即校验），所以这个下标必然有效——
    // World 存活期内 Keep 不会被拆（丢失即败，游戏在那之前已经结束）。
    const std::size_t k = static_cast<std::size_t>(bld_at_[cell] - 1);
    const std::int32_t divisor = stats_.global.building_level_cap_divisor;
    return (b_level_[k] + divisor - 1) / divisor;
}

std::int32_t World::unit_level_cap() const noexcept {
    // 同 `building_level_cap()` 的 Keep 槙位定位，公式不同：`兵种等级上限(K)
    // = K`（`波次预算曲线与堡垒等级曲线.md` §2），没有除数。
    const std::size_t cell =
        static_cast<std::size_t>(keep_.j) * static_cast<std::size_t>(width()) +
        static_cast<std::size_t>(keep_.i);
    const std::size_t k = static_cast<std::size_t>(bld_at_[cell] - 1);
    return b_level_[k];
}

int World::defender_pop() const noexcept {
    // 存活守方单位 + 在训占位，各占 1 格。现算，不存——人口是纯派生量
    // （`world.hpp` 的声明注释写了这条与「不进哈希」的理由）。
    int n = 0;
    for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
        if (unit_pool_.alive_at(static_cast<std::uint16_t>(k)) &&
            side_of(u_type_[k]) == Side::Defender) {
            ++n;
        }
    }
    for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
        if (bld_pool_.alive_at(static_cast<std::uint16_t>(k)) &&
            b_train_type_[k] != kNoTrain) {
            ++n;
        }
    }
    return n;
}

int World::defender_pop_cap() const noexcept {
    // Keep 格位定位与 `unit_level_cap()` 逐字同款（构造期校验恰好一座，
    // 存活期内不会被拆），公式不同：`cap = base + per × 堡垒等级`。
    const std::size_t cell =
        static_cast<std::size_t>(keep_.j) * static_cast<std::size_t>(width()) +
        static_cast<std::size_t>(keep_.i);
    const std::size_t k = static_cast<std::size_t>(bld_at_[cell] - 1);
    return pop_cap_base_ + pop_cap_per_keep_level_ * b_level_[k];
}

std::int64_t World::train_cost_gold(UnitType ut, std::int32_t level) const noexcept {
    // `c ∝ √B(L)`（`数值设计与成本产出矩阵.md` §12.5，2026-09-02 落地）：
    // `base × √(1 + k(L−1))`，与血量/伤害同一条 `level_permille` 曲线、
    // 同一个 k（hp 与 dmg 两个系数相等由 StatsLoader 拦，取哪个都一样）。
    // 取整走 `apply_permille`（`(x×p+500)/1000` 四舍五入、钳 >= 1）——
    // 与 `train_ticks_at` 同款；旧注释「apply_permille 对精确乘法没有必要」
    // 随线性公式一起退役，这里本来就不是精确乘法。
    // L=1 恒等于原价：`level_permille(1,·) = 1000`，不缩放。
    return apply_permille(
        stats_.of(ut).cost_gold,
        {level_permille(level, stats_.global.hp_permille_per_level)});
}

std::int32_t World::train_ticks_at(UnitType ut, std::int32_t level) const noexcept {
    const std::int64_t linear =
        kPermilleOne + static_cast<std::int64_t>(stats_.global.train_ticks_permille_per_level) *
                           (static_cast<std::int64_t>(level) - 1);
    const std::int64_t ticks =
        (stats_.of(ut).train_ticks * linear + kPermilleOne / 2) / kPermilleOne;
    return static_cast<std::int32_t>(ticks);
}

namespace {

// 「把 `base` 缩放到 L 级」——与 `train_cost_gold` 逐字同式（`base × √B(L)`）。
//
// **`base <= 0` 直接返回 0，不走 `apply_permille`**：那个函数把结果钳到 >= 1
// （0 伤害凭空造出一种免疫，`combat_math.hpp`），而这里 0 是有意义的取值——
// `Fence` 的石材标度就是 0（它是纯木制应急工事）。钳成 1 会让它每级收 1 石，
// 数额可忽略但语义是错的：那会凭空给一座木制建筑安上石材开销。
std::int64_t level_scaled_cost(std::int64_t base, std::int32_t level,
                               std::int32_t per_level) noexcept {
    if (base <= 0) return 0;
    return apply_permille(base, {level_permille(level, per_level)});
}

}   // namespace

// 建筑升级定价。理由、两条分支的出处与那笔溢价为什么只收一次，全在
// `world.hpp` 的声明处——这里只写实现。
//
// **两级累计值之差，而不是「增量公式」**：定价是累计曲线
// `cost + up × (√B(L) − 1)` 的差分，所以逐级加起来必然精确等于累计值。
// 直接写增量再取整会让「逐级升到 L」与「累计定价」在舍入上分叉，
// 于是那条「每石买到的火力与等级无关」的性质在某些等级上悄悄不成立。
std::int64_t World::bld_upgrade_cost_stone(BldType bt,
                                          std::int32_t from_level) const noexcept {
    const BldStats& s = stats_.of(bt);
    if (bt == BldType::Keep) return s.upgrade_cost_stone;
    const std::int32_t k = stats_.global.hp_permille_per_level;
    return level_scaled_cost(s.upgrade_cost_stone, from_level + 1, k) -
           level_scaled_cost(s.upgrade_cost_stone, from_level, k);
}

std::int64_t World::bld_upgrade_cost_wood(BldType bt,
                                         std::int32_t from_level) const noexcept {
    const BldStats& s = stats_.of(bt);
    if (bt == BldType::Keep) return s.upgrade_cost_wood;
    const std::int32_t k = stats_.global.hp_permille_per_level;
    return level_scaled_cost(s.upgrade_cost_wood, from_level + 1, k) -
           level_scaled_cost(s.upgrade_cost_wood, from_level, k);
}

// ——状态哈希——
//
// **喂入顺序就是下面这个顺序，改它等于让所有已录回放失效。**
//
//   1. 格式标签（`kWorldHashTag`）—— 改了布局就改这个串，于是旧回放当场对不上
//      而不是悄悄给出一个不同的数。它公开在头文件里，因为回放文件头要存一份，
//      好让「口径变了」与「跑歪了」在诊断上分开
//   2. 地图身份：map_id、content_hash、地形三张位图的 layout_hash；
//      **以及数值表指纹**——数值表是外生输入，它变了第 0 tick 就该分歧（早报），
//      成因翻译（「是数值表变了」）由回放头里的那一份承担，两个落点各担一职
//      （`rts/stats.hpp` 文件头）
//   3. 时间与波次：tick、wave、phase、nominal_level
//   4. RNG 状态
//   5. 四组实体：前三组各自的槽位池（alive + 代数 + 空闲表）+ 全部字段数组；
//      弹丸无槽位池（保序压实的 SoA），**先喂数量再喂字段**——变长数组
//      不喂长度会让不同的 (数量, 内容) 组合拼出相同的字节流
//   6. 资源、编成位、集结点选择
//   7. **两侧的待排空命令队列**
//   8. 两侧迷雾
//
// （原第 8 项——未解算命令的计数器——随第三批删除：全部命令均有解算。）
//
// 第 7 项是写回放格式时补的，理由值得记：队列是**跨 `submit` / `advance` 边界存活**
// 的状态，所以它是仿真状态。漏掉它，哈希的承诺「同一个哈希 ⇒ 同一个未来」就不成立
// ——两个队列不同的世界会算出同一个数，然后在下一次 `advance` 分叉。
// 分叉最终仍会被发现（下一个哈希点），但报出来的 tick 比真正出问题的地方晚，
// 而回放测试的价值恰好在于**它指的那个 tick 就是第一个错的 tick**。
std::uint64_t World::state_hash() const noexcept {
    StateHash h;
    h.feed_text(kWorldHashTag);

    h.feed_text(map_id_);
    h.feed(map_content_hash_.data(), map_content_hash_.size());
    h.feed_pod(terrain_.layout_hash());
    h.feed_pod(stats_fp_);

    h.feed_pod(tick_);
    h.feed_pod(wave_);
    h.feed_pod(phase_);
    h.feed_pod(nominal_level_);

    const Rng::State rs = rng_.state();
    h.feed(rs.data(), rs.size() * sizeof(std::uint32_t));

    unit_pool_.feed_hash(h);
    h.feed(u_type_.data(), u_type_.size() * sizeof(UnitType));
    h.feed(u_level_.data(), u_level_.size() * sizeof(std::int32_t));
    h.feed(u_hp_.data(), u_hp_.size() * sizeof(std::int64_t));
    h.feed(u_max_hp_.data(), u_max_hp_.size() * sizeof(std::int64_t));
    // 位置是浮点，**必须按位喂**（`rts/hash.hpp` 的 feed_f32）：
    // `feed_pod(Vec2)` 会被那条 has_unique_object_representations 断言挡下，
    // 因为 ±0.0 值相等而字节不同。
    for (const Vec2& p : u_pos_) {
        h.feed_f32(p.x);
        h.feed_f32(p.y);
    }
    h.feed(u_windup_.data(), u_windup_.size() * sizeof(std::int32_t));
    h.feed(u_action_.data(), u_action_.size() * sizeof(UnitAction));
    h.feed(u_garrison_.data(), u_garrison_.size() * sizeof(std::uint16_t));
    h.feed(u_garrison_target_.data(), u_garrison_target_.size() * sizeof(std::uint16_t));
    h.feed(u_mount_.data(), u_mount_.size() * sizeof(std::int32_t));
    // 冲锋动量是浮点，与 u_pos_ 同一条纪律：按位喂。
    for (const float c : u_charge_) h.feed_f32(c);
    h.feed(u_cd_.data(), u_cd_.size() * sizeof(std::int32_t));
    h.feed(u_tgt_kind_.data(), u_tgt_kind_.size() * sizeof(TgtKind));
    h.feed(u_tgt_raw_.data(), u_tgt_raw_.size() * sizeof(std::uint32_t));
    // 锁定落点是浮点，与 u_pos_ 同一条纪律：按位喂。
    for (const Vec2& p : u_aim_) {
        h.feed_f32(p.x);
        h.feed_f32(p.y);
    }

    bld_pool_.feed_hash(h);
    h.feed(b_type_.data(), b_type_.size() * sizeof(BldType));
    h.feed(b_pos_.data(), b_pos_.size() * sizeof(GridPos));
    h.feed(b_hp_.data(), b_hp_.size() * sizeof(std::int64_t));
    h.feed(b_max_hp_.data(), b_max_hp_.size() * sizeof(std::int64_t));
    h.feed(b_work_.data(), b_work_.size() * sizeof(std::int32_t));
    h.feed(b_cd_.data(), b_cd_.size() * sizeof(std::int32_t));
    h.feed(b_windup_.data(), b_windup_.size() * sizeof(std::int32_t));
    h.feed(b_tgt_raw_.data(), b_tgt_raw_.size() * sizeof(std::uint32_t));
    for (const Vec2& p : b_aim_) {
        h.feed_f32(p.x);
        h.feed_f32(p.y);
    }
    h.feed(b_built_.data(), b_built_.size());
    h.feed(b_train_type_.data(), b_train_type_.size());
    h.feed(b_train_left_.data(), b_train_left_.size() * sizeof(std::int32_t));
    h.feed(b_train_level_.data(), b_train_level_.size() * sizeof(std::int32_t));
    h.feed(b_level_.data(), b_level_.size() * sizeof(std::int32_t));
    h.feed(b_upgrade_left_.data(), b_upgrade_left_.size() * sizeof(std::int32_t));

    obstacle_pool_.feed_hash(h);
    h.feed(o_type_.data(), o_type_.size() * sizeof(ObstacleType));
    h.feed(o_pos_.data(), o_pos_.size() * sizeof(GridPos));
    h.feed(o_hp_.data(), o_hp_.size() * sizeof(std::int64_t));
    h.feed(o_max_hp_.data(), o_max_hp_.size() * sizeof(std::int64_t));
    h.feed(o_clear_ordered_.data(), o_clear_ordered_.size());

    // 第四组：在途弹丸（第五批）。数量先行（见上面第 5 项的理由）；
    // 浮点按位喂，同 u_pos_ 那条纪律。
    h.feed_pod(static_cast<std::uint64_t>(p_pos_.size()));
    for (const Vec2& p : p_pos_) {
        h.feed_f32(p.x);
        h.feed_f32(p.y);
    }
    for (const Vec2& p : p_aim_) {
        h.feed_f32(p.x);
        h.feed_f32(p.y);
    }
    for (const float s : p_speed_) h.feed_f32(s);
    h.feed(p_kind_.data(), p_kind_.size() * sizeof(TgtKind));
    h.feed(p_raw_.data(), p_raw_.size() * sizeof(std::uint32_t));
    h.feed(p_dmg_.data(), p_dmg_.size() * sizeof(std::int64_t));
    h.feed(p_lvl_pm_.data(), p_lvl_pm_.size() * sizeof(std::int64_t));
    h.feed(p_from_high_.data(), p_from_high_.size());
    for (const float a : p_aoe_) h.feed_f32(a);
    h.feed(p_side_.data(), p_side_.size() * sizeof(Side));
    h.feed(p_src_bld_.data(), p_src_bld_.size());

    h.feed(stock_.data(), stock_.size() * sizeof(std::int64_t));
    h.feed(composition_.data(), composition_.size() * sizeof(std::uint16_t));
    h.feed(spawn_chosen_.data(), spawn_chosen_.size());

    // 待排空的命令队列。**长度必须一起喂**，否则「守方一条、攻方两条」与
    // 「守方两条、攻方一条」在拼接之后字节相同。
    for (const std::vector<Command>& q : cmd_queue_) {
        const std::uint64_t n = q.size();
        h.feed_pod(n);
        h.feed(q.data(), q.size() * sizeof(Command));
    }

    // 迷雾是累积状态，不是每 tick 重算出来的——两个实体完全一致的世界可以
    // 记忆不同，此后立刻分叉。见 `rts/fog.hpp` 里 feed_hash 那段。
    for (const FogLayer& f : fog_) f.feed_hash(h);

    return h.value();
}

WorldView World::view(Side side) const noexcept { return WorldView(*this, side); }

}  // namespace rts
