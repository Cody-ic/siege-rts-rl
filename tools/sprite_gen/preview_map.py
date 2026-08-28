"""把精灵拼成一张等距场景图。

这份脚本有两个职责，代码里也**分成两段**：

1. **前端契约的可执行示例**（下面「契约」一段）——怎么读 `_sprite_meta.json`、
   怎么按地面锚点贴图、地形枚举怎么展开成两张图、深度怎么排。
   写 `render/` 时照着抄这一段，比看文档快。
   `地图与场景设计.md` 4.2.1 声明本文件的 `OVERLAY` 表是该映射的**规范来源**。
2. **渲染结果的目视校验**（下面「演示地图」一段）——把实体摆进一个能讲清设计的
   布局里。单独看每张精灵都没问题，**相邻摆放才会暴露撞色、撞形、比例错**。

用法:
    python preview_map.py [精灵目录] [输出文件] [--full]

`--full` 输出原始分辨率（约 4000 px 宽）；默认缩到 2600 px 以内，够看且不撑大仓库。
"""
import json
import os
import sys

from PIL import Image

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
FULL = "--full" in sys.argv

DIR = ARGS[0] if len(ARGS) > 0 else "out_3d"
OUT = ARGS[1] if len(ARGS) > 1 else os.path.join(DIR, "_scene_preview.png")
MAX_W = 2600

with open(os.path.join(DIR, "_sprite_meta.json"), encoding="utf-8") as f:
    META = json.load(f)
SPRITES = META["sprites"]
TW = META["px_per_tile"]          # 一格菱形的像素宽
TH = TW // 2                      # 固定 2:1


# ══════════════════════════════════════════════════════════════════
#  契约：写前端时照抄这一段
# ══════════════════════════════════════════════════════════════════

def sprite(ident, state="idle", d="SE", frame=None):
    """取图并返回它的地面锚点。锚点是画布内代表「脚底 / 格心」的像素坐标。"""
    st = SPRITES[ident]["states"][state]
    name = f"{ident}_{state}_{d}" + (f"_{frame}" if frame else "") + ".png"
    return Image.open(os.path.join(DIR, name)).convert("RGBA"), st["ground_anchor"]


def grid_to_screen(gi, gj, ox, oy):
    """格坐标 -> 屏幕像素。等距的全部数学就这两行。"""
    return ox + (gi - gj) * TW // 2, oy + (gi + gj) * TH // 2


# 地形枚举 -> (底衬地砖, 可选叠加物件)。**一格要画两张图，不是一张。**
#
# 只有 Plain* 与 Water 有自己的地砖（`kind: "tile"`：画布正好占满菱形、锚点在格心、
# 不描边不投影）。Rock / Forest / Bridge 是立体物件——锚点在脚底、带描边与地面投影，
# 单独贴上去底下是透明的，必须叠在某张地砖之上。
#
# 桥的底衬是 `Water` 而不是平地：真桥看得见水从底下过，铺平地会让河在桥这里断掉。
OVERLAY = {
    "Rock":   ("Plain", "Rock"),
    "Forest": ("Plain", "Forest"),
    "Bridge": ("Water", "Bridge"),
}


def expand(terrain):
    """地形枚举 -> (地砖标识符, 叠加物标识符或 None)。

    `Plain` 的贴图变体不占枚举位，由调用方按坐标挑（见 `variant()`）。
    """
    return OVERLAY.get(terrain, (terrain, None))


def run_dir(cells, gi, gj):
    """**线性结构（墙 / 桥）的朝向由它的走向决定，不是固定 `SE`。**

    这条很容易漏：单位的 `d` 是朝向，而墙的 `d` 是**走向**——同一段墙沿 gi 铺和沿 gj
    铺要用不同的精灵，否则相邻墙段接不上，一条边读作一排分开的板子。

    哪个朝向无缝是**实测出来的**（渲四个朝向各排三格看哪个接得上）：
    沿 gi 走用 `SW`，沿 gj 走用 `NW`。换素材后要重测。
    """
    return "SW" if ((gi - 1, gj) in cells or (gi + 1, gj) in cells) else "NW"


def draw_scene(canvas, ox, oy, terrain, variant, objs, overlay_kw=None):
    """两遍绘制。这是深度排序的全部内容。

    `terrain` 是 {(gi,gj): 枚举}，`objs` 是 [(gi, gj, 标识符, 取图参数)]，
    `overlay_kw(gi, gj, 标识符) -> dict` 给叠加物补取图参数（桥要按走向取朝向）。
    """
    def place(ident, gi, gj, **kw):
        # 关键一步：把**锚点**对齐到格心，而不是把图片左上角对齐到格心。
        # 各精灵画布尺寸不同，按左上角贴会让高个子单位整体上浮。
        img, (ax, ay) = sprite(ident, **kw)
        x, y = grid_to_screen(gi, gj, ox, oy)
        canvas.alpha_composite(img, (int(x - ax), int(y - ay)))

    # 第一遍：只铺地砖。地砖是平的、永远不遮挡别的东西，所以可以先整片铺完。
    overlays = []
    for (gi, gj), t in sorted(terrain.items()):
        tile, over = expand(t)
        place(variant(tile, (gi, gj)), gi, gj)
        if over:
            overlays.append((gi, gj, over,
                             overlay_kw(gi, gj, over) if overlay_kw else {}))

    # 第二遍：叠加物件与单位混在**同一个**深度序列里排序。
    # 画家算法：按 gi+gj（离屏幕上方的深度）从小到大画，后画的自然遮住先画的。
    # 岩壁与单位必须一起排——站在岩壁前面的单位要遮住岩壁，反之则被遮住。
    for gi, gj, ident, kw in sorted(overlays + objs, key=lambda o: o[0] + o[1]):
        place(ident, gi, gj, **kw)


# ══════════════════════════════════════════════════════════════════
#  演示地图：与契约无关，只为把设计摆出来看
# ══════════════════════════════════════════════════════════════════
#
# 屏幕方位（等距）：gi+gj 小 = 上，大 = 下；gi-gj 大 = 右，小 = 左。
# 因此格坐标里的正方形城区，在屏幕上是一个**菱形**，四角分别朝上 / 右 / 下 / 左。
#
# 这张图刻意演示 `地图与场景设计.md` 的几条结构约束，而不是随机撒物件：
#
#   2.2  天然岩壁承担大部分周长，只有走廊口需要人工墙
#   2.2  每个走廊性质不同（开阔平原 / 林地 / 隘口）
#   2.3  初始城圈留缺口 —— 「AI 是否发现并利用已有缺口」第 1 波就能测
#   4.1  五种地形俱全，且 Water/Bridge 演示「看得见对岸但过不来」
#   4.4  城内三种资源点全覆盖；外部资源簇在墙外，必须出城争
#   §4   外部簇周围铺森林带（可通行、不可建造、双向挡视野）
#
N = 15
CX, CY = 7, 7          # 城区中心
CITY_R = 3             # 城区半径（切比雪夫），城圈落在这个半径上


def ring(r):
    return {(CX + di, CY + dj)
            for di in range(-r, r + 1) for dj in range(-r, r + 1)
            if max(abs(di), abs(dj)) == r}


CITY_RING = ring(CITY_R)

# **摆位受等距遮挡支配，不能只照设计图摆。** 画家算法下 gi+gj 大的后画、盖住前面的，
# 所以把 1.5 模数的岩壁摆满一整圈，城内会被整个埋掉（第一版正是如此）。
# 规则：**高物件只放后半弧（gi+gj 小），面向相机的两条边留给矮的城墙。**
BACK_ARC = {k for k in CITY_RING if sum(k) <= CX + CY + CITY_R - 1}
FRONT_L = [(gi, CY + CITY_R) for gi in range(CX - CITY_R, CX + CITY_R + 1)]  # 左前，有城门
FRONT_R = [(CX + CITY_R, gj) for gj in range(CY - CITY_R, CY + CITY_R + 1)]  # 右前，有缺口

GATE = (CX, CY + CITY_R)
# 2.3 要的初始城圈缺口 —— 「AI 是否发现并利用已有缺口」第 1 波就能测。
# 这两格**什么都不放**，玩家只来得及在旁边插一道木栅。
BREACH = {(CX + CITY_R, CY), (CX + CITY_R, CY + 1)}
# 墙上每隔几格嵌一座塔。不这么做，一条边七块同样的板子读作栅栏而不是城墙
WALL_TOWERS = {(CX - CITY_R, CY + CITY_R), (CX + CITY_R, CY + CITY_R),
               (CX + CITY_R, CY - CITY_R)}

# 河沿地图右缘，只有一座桥。桥是「最窄的隘口」；对岸看得见、走不过去
# 河内移一格，让对岸有真正的落点——贴着地图边缘的话「跨过去」没有意义
RIVER = {(gi, gj) for gi in (11, 12) for gj in range(N)}
BRIDGE = {(11, 7), (12, 7)}     # 两格才读得出「跨过去」；一格宽的河上放桥像河里立了段墙

# 外部资源簇在左侧野地，四周铺森林带（可通行 / 不可建造 / 双向挡视野）
OUTER_CLUSTER = [(1, 9, "Quarry"), (0, 11, "Lumber"), (14, 7, "Mine")]
# 对岸那簇**不铺**森林带：1.35 模数的树在桥头会把桥和矿场一起埋掉。
# 这是等距摆位的通则——高物件不能放在你想让人看见的东西前面。
FOREST_BELT = {(2, 8), (2, 10), (0, 8), (1, 12), (3, 11), (0, 13)}
# 林地走廊：从地图左上通向城区，「侦查困难、佯攻可信」的那条
FOREST_CORRIDOR = {(1, 3), (2, 4), (0, 4), (1, 5), (3, 5)}
# 隘口：右下用岩壁夹出一条窄道
DEFILE_ROCK = {(9, 12), (10, 13), (8, 14)}


def make_terrain():
    t = {}
    for gi in range(N):
        for gj in range(N):
            k = (gi, gj)
            if k in BRIDGE:
                t[k] = "Bridge"
            elif k in RIVER:
                t[k] = "Water"
            elif k in BACK_ARC:
                t[k] = "Rock"                      # 2.2 天然岩壁承担大部分周长
            elif k in FOREST_BELT or k in FOREST_CORRIDOR:
                t[k] = "Forest"
            elif k in DEFILE_ROCK:
                t[k] = "Rock"
            else:
                t[k] = "Plain"
    return t


ROAD = {(CX, gj) for gj in range(CY + CITY_R, N)} | {(gi, CY) for gi in range(CX, CX + 5)}
ASH = {(9, 9), (10, 8), (9, 8)}           # 上一波在缺口外打出来的焦土，纯美术


def variant(tile, k):
    """`Plain` 的贴图变体按坐标挑，不写进地图文件（契约 4.1：变体不占枚举位）。"""
    if tile != "Plain":
        return tile
    if k in ROAD:
        return "PlainRoad"
    if k in ASH:
        return "PlainAsh"
    if max(abs(k[0] - CX), abs(k[1] - CY)) > CITY_R:
        return "PlainDirt"                # 城外是野地
    return "PlainB" if (k[0] + k[1]) % 3 == 0 else "Plain"


def make_objects():
    o = []

    # ── 城内。堡垒放后排：它高 2 格边长，在后面反而更显眼，也不挡前排 ────
    o += [(CX - 1, CY - 1, "Keep", {})]
    # 三种资源点全在城内（4.4：inner 三种全覆盖，否则龟缩时少一条决策轴）
    o += [(CX + 2, CY - 1, "Quarry", {}), (CX - 1, CY + 2, "Mine", {}),
          (CX + 2, CY + 2, "Lumber", {})]
    o += [(CX - 2, CY, "Barrack", {}), (CX, CY - 2, "Watch", {}),
          (CX + 1, CY + 1, "Flak", {})]

    # ── 城圈：岩壁承担后半周，人工墙只在两条正面；塔嵌在墙线上 ──────────
    line = set(FRONT_L) | set(FRONT_R)
    for k in FRONT_L + FRONT_R:
        if k in BREACH:
            continue                       # 缺口：刻意什么都不放
        ident = ("Tower" if k in WALL_TOWERS else "Gate" if k == GATE else "Wall")
        # 塔是回转体、朝向无所谓；墙与门必须按走向取，否则相邻段接不上
        kw = {} if ident == "Tower" else {"d": run_dir(line, *k)}
        o.append((k[0], k[1], ident, kw))
    # 玩家来不及重建，先插一道木栅——波次中可即时放置的应急工事
    o += [(CX + CITY_R, CY - 1, "Fence", {})]

    # ── 城外：外部资源簇，被森林带围住，必须出城争 ─────────────────────
    o += [(gi, gj, ident, {}) for gi, gj, ident in OUTER_CLUSTER]

    # ── 景物 / 可破坏障碍（矮而单薄），与地形的高厚重成对照 ────────────
    o += [(5, 13, "Stump", {}), (10, 2, "Rubble", {}), (4, 12, "Sapling", {}),
          (14, 2, "ForestB", {}), (13, 12, "Sapling", {}), (14, 14, "ForestB", {})]

    # ── 守方 ─────────────────────────────────────────────────────
    o += [(CX - 1, CY + 2, "Archer", {}), (CX + 1, CY + 2, "Archer", {})]
    o += [(CX + 2, CY, "Spear", {}), (CX + 2, CY + 1, "Mason", {})]   # 堵缺口 + 抢修
    o += [(3, 10, "Ranger", {"state": "move", "frame": 6})]           # 出城去外部簇
    o += [(5, 11, "Scout", {"state": "move", "frame": 11})]

    # ── 攻方：主攻压城门，一支从右侧摸缺口（头号演示画面）────────────
    o += [(CX, CY + 5, "Ram", {}),
          (CX - 1, CY + 5, "Ghoul", {"state": "move", "frame": 8}),
          (CX + 1, CY + 5, "Ghoul", {"state": "move", "frame": 15}),
          (CX, CY + 6, "Shade", {}),
          (CX - 2, CY + 5, "Knight", {"state": "move", "frame": 6})]
    o += [(CX + 5, CY, "Ghoul", {"state": "move", "frame": 22}),
          (CX + 5, CY - 1, "Wraith", {"state": "move", "frame": 8}),
          (CX + 4, CY + 1, "Shade", {})]
    o += [(3, 7, "Phoenix", {"state": "move", "frame": 8})]   # 空军绕开正面点经济

    return o


def build():
    terrain = make_terrain()
    objs = make_objects()

    missing = sorted({i for _, _, i, _ in objs} | set(terrain.values())
                     | {v for pair in OVERLAY.values() for v in pair})
    missing = [i for i in missing if i not in SPRITES]
    if missing:
        sys.exit(f"演示地图引用了不存在的精灵：{'、'.join(missing)}\n"
                 f"    先渲染它们，或改 make_objects()。")

    canvas = Image.new("RGBA", (TW * N + TW, TH * N + TW * 4), (0, 0, 0, 0))
    ox, oy = canvas.width // 2, TW * 2      # 顶部留够：堡垒高达 4 格边长
    def overlay_kw(gi, gj, ident):
        # 桥是线性结构，朝向按走向取；岩壁与密林是团块，朝向无所谓
        return {"d": run_dir(BRIDGE, gi, gj)} if ident == "Bridge" else {}

    draw_scene(canvas, ox, oy, terrain, variant, objs, overlay_kw)
    return canvas


def main():
    canvas = build()
    bg = Image.new("RGB", canvas.size, (34, 38, 34))
    bg.paste(canvas, (0, 0), canvas)
    out = bg.crop(canvas.getbbox())
    if not FULL and out.width > MAX_W:
        out = out.resize((MAX_W, round(out.height * MAX_W / out.width)), Image.LANCZOS)
    out.save(OUT, optimize=True)
    print(f"场景图: {OUT}  {out.width}×{out.height}"
          f"{'（原始分辨率）' if FULL else f'（已缩到 {MAX_W} px 宽，--full 出原图）'}")


if __name__ == "__main__":
    main()
