#include "rts/world.hpp"

#include <string>
#include <utility>

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
            break;
        case CommandKind::SelectForce:
            if (c.force == kNoForce) throw ContractError("SelectForce 未指定编队");
            break;
        case CommandKind::MoveForce:
        case CommandKind::Garrison:
            if (c.force == kNoForce) throw ContractError("未指定编队");
            if (c.slot == kNoSlot || static_cast<std::size_t>(c.slot) >= cells) {
                throw ContractError("目标槽位越界");
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

void World::apply_one(Side side, const Command& c) {
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
        case CommandKind::SelectForce:
            selected_force_[static_cast<std::size_t>(index_of(side))] = c.force;
            break;
        case CommandKind::Composition:
            composition_[static_cast<std::size_t>(c.what)] = composition_slots(c);
            break;
        case CommandKind::PickSpawn:
            // 可对多个集结点各下一条 = 分兵佯攻，所以是置位而不是赋值。
            spawn_chosen_[static_cast<std::size_t>(c.slot)] = std::uint8_t{1};
            break;
        // ——以下六种需要造价 / 耗时，本版不解算，但**不静默丢弃**——
        case CommandKind::Build:
        case CommandKind::Repair:
        case CommandKind::Cancel:
        case CommandKind::Train:
        case CommandKind::MoveForce:
        case CommandKind::Garrison:
            ++deferred_[static_cast<std::size_t>(c.kind)];
            break;
    }
}

void World::advance(int ticks) {
    if (ticks < 0) throw ContractError("advance 的 tick 数不得为负");
    for (int t = 0; t < ticks; ++t) {
        // 顺序固定：守方先、攻方后，各按提交顺序。**改它等于让所有已录回放失效。**
        for (int s = 0; s < kSideCount; ++s) {
            std::vector<Command>& q = cmd_queue_[static_cast<std::size_t>(s)];
            for (const Command& c : q) apply_one(static_cast<Side>(s), c);
            q.clear();
        }

        // 两个纯计数器。**它们不需要任何数值就能推进**（初值来自数值表，
        // 递减不来自任何表），所以本版做完；也正因为有它们，
        // `advance()` 不是空操作，回放测试才不是永远绿的摆设。
        for (std::size_t k = 0; k < unit_pool_.slot_count(); ++k) {
            if (unit_pool_.alive_at(static_cast<std::uint16_t>(k)) && u_windup_[k] > 0) {
                --u_windup_[k];
            }
        }
        for (std::size_t k = 0; k < bld_pool_.slot_count(); ++k) {
            if (bld_pool_.alive_at(static_cast<std::uint16_t>(k)) && b_work_[k] > 0) {
                --b_work_[k];
            }
        }

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
        u_force_.resize(n);
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
    u_force_[k] = kNoForce;
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
    }
    b_type_[k] = type;
    b_pos_[k] = pos;
    b_hp_[k] = hp;
    b_max_hp_[k] = max_hp;
    b_work_[k] = work_left;
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
    }
    o_type_[k] = type;
    o_pos_[k] = pos;
    o_hp_[k] = hp;
    o_max_hp_[k] = max_hp;
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
    u_force_[k] = kNoForce;
    unit_pool_.release(static_cast<std::uint16_t>(k));
}

void World::destroy_bld(BldId id) {
    const std::size_t k = require(id);
    b_type_[k] = BldType::Keep;
    b_pos_[k] = GridPos{};
    b_hp_[k] = 0;
    b_max_hp_[k] = 0;
    b_work_[k] = 0;
    bld_pool_.release(static_cast<std::uint16_t>(k));
}

void World::destroy_obstacle(ObstacleId id) {
    const std::size_t k = require(id);
    o_type_[k] = ObstacleType::Stump;
    o_pos_[k] = GridPos{};
    o_hp_[k] = 0;
    o_max_hp_[k] = 0;
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

BldType World::bld_type(BldId id) const { return b_type_[require(id)]; }
GridPos World::bld_pos(BldId id) const { return b_pos_[require(id)]; }
std::int64_t World::bld_hp(BldId id) const { return b_hp_[require(id)]; }
bool World::bld_complete(BldId id) const { return b_work_[require(id)] == 0; }

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

    // 攻击四位与 Stop 默认允许，理由见头文件（错误地禁止比错误地允许坏得多）。
    std::uint16_t mask = static_cast<std::uint16_t>(
        bit_of(UnitAction::Stop) | bit_of(UnitAction::AtkNear) |
        bit_of(UnitAction::AtkWeak) | bit_of(UnitAction::AtkBld) |
        bit_of(UnitAction::AtkWall));

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

// ——状态哈希——
//
// **喂入顺序就是下面这个顺序，改它等于让所有已录回放失效。**
//
//   1. 格式标签（"World/1"）—— 改了布局就改这个串，于是旧回放当场对不上
//      而不是悄悄给出一个不同的数
//   2. 地图身份：map_id、content_hash、地形三张位图的 layout_hash
//   3. 时间与波次：tick、wave、phase、nominal_level
//   4. RNG 状态
//   5. 三组实体：各自的槽位池（alive + 代数 + 空闲表）+ 全部字段数组
//   6. 资源、编成位、集结点选择、选中编队
//   7. 未解算命令的计数器（本版特有，1c 删）
//   8. 两侧迷雾
std::uint64_t World::state_hash() const noexcept {
    StateHash h;
    h.feed_text("World/1");

    h.feed_text(map_id_);
    h.feed(map_content_hash_.data(), map_content_hash_.size());
    h.feed_pod(terrain_.layout_hash());

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
    h.feed(u_force_.data(), u_force_.size());

    bld_pool_.feed_hash(h);
    h.feed(b_type_.data(), b_type_.size() * sizeof(BldType));
    h.feed(b_pos_.data(), b_pos_.size() * sizeof(GridPos));
    h.feed(b_hp_.data(), b_hp_.size() * sizeof(std::int64_t));
    h.feed(b_max_hp_.data(), b_max_hp_.size() * sizeof(std::int64_t));
    h.feed(b_work_.data(), b_work_.size() * sizeof(std::int32_t));

    obstacle_pool_.feed_hash(h);
    h.feed(o_type_.data(), o_type_.size() * sizeof(ObstacleType));
    h.feed(o_pos_.data(), o_pos_.size() * sizeof(GridPos));
    h.feed(o_hp_.data(), o_hp_.size() * sizeof(std::int64_t));
    h.feed(o_max_hp_.data(), o_max_hp_.size() * sizeof(std::int64_t));

    h.feed(stock_.data(), stock_.size() * sizeof(std::int64_t));
    h.feed(composition_.data(), composition_.size() * sizeof(std::uint16_t));
    h.feed(spawn_chosen_.data(), spawn_chosen_.size());
    h.feed(selected_force_.data(), selected_force_.size());

    h.feed(deferred_.data(), deferred_.size() * sizeof(std::int64_t));

    // 迷雾是累积状态，不是每 tick 重算出来的——两个实体完全一致的世界可以
    // 记忆不同，此后立刻分叉。见 `rts/fog.hpp` 里 feed_hash 那段。
    for (const FogLayer& f : fog_) f.feed_hash(h);

    return h.value();
}

WorldView World::view(Side side) const noexcept { return WorldView(*this, side); }

}  // namespace rts
