#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>
#include <utility>

#include "game/iso_projection.hpp"
#include "rts/action.hpp"

namespace {

// 符号函数。方向检查只关心「往哪边」，不关心走了多少像素。
int sign_of(float v) noexcept { return v > 0.0f ? 1 : (v < 0.0f ? -1 : 0); }

}  // namespace

TEST_CASE("Stop 是 0，且 8 个移动动作连续", "[action]") {
    // Stop == 0 的两个理由都在 rts/action.hpp 里：零初始化的动作数组等于「全体停住」，
    // 以及掩码里 0 通常是那个「兜底一定合法」的动作。
    REQUIRE(static_cast<int>(rts::UnitAction::Stop) == 0);

    int moves = 0;
    for (int i = 0; i < rts::kUnitActionCount; ++i) {
        if (rts::is_move(rts::action_at(i))) ++moves;
    }
    REQUIRE(moves == rts::kMoveDirCount);
}

TEST_CASE("dir_index 与 move_of 往返", "[action]") {
    for (int k = 0; k < rts::kMoveDirCount; ++k) {
        const rts::UnitAction a = rts::move_of(k);
        REQUIRE(rts::is_move(a));
        REQUIRE(rts::dir_index(a) == k);
    }
}

TEST_CASE("方向名与等距投影一致", "[action]") {
    // **这是本文件里唯一一条不能靠读代码替代的检查。**
    //
    // 动作名取的是**屏幕**方位（`MoveNE` 等），而字段是**格**增量。两者的对应关系
    // 由等距投影决定，而那是另一个文件里的两行数学。若我把某两个方向写反了：
    //
    //   * 编译过、所有 static_assert 过、往返测试过
    //   * 观测里的方向场与玩家界面上的方向**相差一个反射**
    //   * 没有任何东西会红。AI 学到的策略在人看来是「往反方向走」，
    //     而排查会先怀疑寻路、再怀疑奖励，最后才怀疑一个枚举的注释
    //
    // 所以让真的 `IsoProjection` 来判。px_per_tile 用素材的真值 256（§7.1）。
    const game::IsoProjection proj(256);

    // 屏幕坐标 y 向下，所以「北」= y 为负。
    const std::pair<rts::UnitAction, std::pair<int, int>> want[] = {
        {rts::UnitAction::MoveN,  { 0, -1}},
        {rts::UnitAction::MoveNE, {+1, -1}},
        {rts::UnitAction::MoveE,  {+1,  0}},
        {rts::UnitAction::MoveSE, {+1, +1}},
        {rts::UnitAction::MoveS,  { 0, +1}},
        {rts::UnitAction::MoveSW, {-1, +1}},
        {rts::UnitAction::MoveW,  {-1,  0}},
        {rts::UnitAction::MoveNW, {-1, -1}},
    };

    const rts::Vec2 origin = proj.grid_to_screen(rts::GridPos{0, 0});
    for (const auto& [action, want_sign] : want) {
        const rts::GridDelta d = rts::move_delta(action);
        const rts::Vec2 there = proj.grid_to_screen(
            rts::GridPos{static_cast<std::int16_t>(d.di),
                         static_cast<std::int16_t>(d.dj)});
        INFO("动作 " << rts::ident_of(action) << " 格增量 (" << int{d.di} << ", "
                     << int{d.dj} << ")");
        REQUIRE(sign_of(there.x - origin.x) == want_sign.first);
        REQUIRE(sign_of(there.y - origin.y) == want_sign.second);
    }
}

TEST_CASE("格坐标斜向恰好是屏幕上的正北东南西", "[action]") {
    // 文件头那张表的可执行形式。**这条差别按名字猜会恰好猜反**，
    // 而「斜向能不能穿角」那条规则的适用对象正是这四个。
    //
    // 已经有一个下游消费者踩在这上面：tools/map_gen/ 的校验器
    // （#31 / #33）先取了最保守的一种，等接口定稿回来对齐。
    REQUIRE(rts::is_grid_diagonal(rts::UnitAction::MoveN));
    REQUIRE(rts::is_grid_diagonal(rts::UnitAction::MoveE));
    REQUIRE(rts::is_grid_diagonal(rts::UnitAction::MoveS));
    REQUIRE(rts::is_grid_diagonal(rts::UnitAction::MoveW));

    REQUIRE_FALSE(rts::is_grid_diagonal(rts::UnitAction::MoveNE));
    REQUIRE_FALSE(rts::is_grid_diagonal(rts::UnitAction::MoveSE));
    REQUIRE_FALSE(rts::is_grid_diagonal(rts::UnitAction::MoveSW));
    REQUIRE_FALSE(rts::is_grid_diagonal(rts::UnitAction::MoveNW));

    // 四条轴向 + 四条斜向，正好分完 8 个。
    int diag = 0;
    for (int k = 0; k < rts::kMoveDirCount; ++k) {
        if (rts::is_grid_diagonal(rts::move_of(k))) ++diag;
    }
    REQUIRE(diag == 4);
}

TEST_CASE("方向顺时针排列，反向是 (k + 4) % 8", "[action]") {
    // 这个性质有实际用途（「往回退一步」= 拉扯脚本要用的动作），
    // 而它成立**完全取决于枚举的排列顺序**——有人按字母序重排一次就没了，
    // 且不会有任何别的东西报错。
    for (int k = 0; k < rts::kMoveDirCount; ++k) {
        const rts::GridDelta a = rts::move_delta(rts::move_of(k));
        const rts::GridDelta b =
            rts::move_delta(rts::move_of((k + rts::kMoveDirCount / 2) %
                                         rts::kMoveDirCount));
        INFO("方向 " << k << " 与它的反向");
        REQUIRE(a.di == -b.di);
        REQUIRE(a.dj == -b.dj);
    }
}

TEST_CASE("8 个增量互不相同、都非零、且都是单步", "[action]") {
    std::set<std::pair<int, int>> seen;
    for (int k = 0; k < rts::kMoveDirCount; ++k) {
        const rts::GridDelta d = rts::move_delta(rts::move_of(k));
        // 多一层括号是 Catch2 的要求（它会把 `a == b && c == d` 拆成链式比较并拒绝），
        // 不是排版。
        REQUIRE_FALSE((d.di == 0 && d.dj == 0));
        // 单步：一次决策最多跨一格。跨多格会让「穿角」与碰撞检测都失去意义。
        REQUIRE(d.di >= -1);
        REQUIRE(d.di <= 1);
        REQUIRE(d.dj >= -1);
        REQUIRE(d.dj <= 1);
        REQUIRE(seen.insert({d.di, d.dj}).second);
    }
    REQUIRE(seen.size() == 8);
}

TEST_CASE("非移动动作的增量为零", "[action]") {
    // 攻击动作**不含位移**。这不是显然的：「攻击最近敌人」很容易被实现成
    // 「朝它走一步再打」，而那会让一个动作干两件事、
    // 使「8 方向移动 + 攻击」这套最小动作空间的语义变糊。
    for (const rts::UnitAction a :
         {rts::UnitAction::Stop, rts::UnitAction::AtkNear, rts::UnitAction::AtkWeak,
          rts::UnitAction::AtkBld, rts::UnitAction::AtkWall}) {
        REQUIRE_FALSE(rts::is_move(a));
        REQUIRE(rts::move_delta(a) == rts::GridDelta{0, 0});
    }
}

TEST_CASE("13 个动作各有唯一名字", "[action]") {
    std::set<std::string_view> seen;
    for (int i = 0; i < rts::kUnitActionCount; ++i) {
        const std::string_view id = rts::ident_of(rts::action_at(i));
        INFO("下标 " << i);
        REQUIRE_FALSE(id.empty());
        REQUIRE(seen.insert(id).second);
    }
    REQUIRE(seen.size() == 13);
}

TEST_CASE("攻击建筑与攻击城墙是两个动作", "[action]") {
    // 合成一个的话，「攻其必救」这条核心战术就没有对应的动作可学——
    // AI 只能打「最近的」，而最近的永远是墙。
    REQUIRE(rts::UnitAction::AtkBld != rts::UnitAction::AtkWall);
}

TEST_CASE("决策周期不是每 tick", "[action]") {
    // CLAUDE.md：「每 4–8 tick 决策一次，不是每 tick」。
    // 具体取值待定（与速度耦合），但下界是硬的。
    REQUIRE(rts::kDecisionPeriodMin >= 2);
    REQUIRE(rts::kDecisionPeriodMin <= rts::kDecisionPeriodMax);
}
