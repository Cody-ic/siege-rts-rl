"""把精灵拼成一张等距场景图。

这既是渲染结果的目视校验，也是**前端契约的可执行示例**——
怎么读 `_sprite_meta.json`、怎么按地面锚点贴图、怎么排深度顺序，
看下面 `place()` 与主循环那几行比看文档快。

用法:  python preview_map.py [精灵目录] [输出文件]
"""
import sys, os, json
from PIL import Image

DIR = sys.argv[1] if len(sys.argv) > 1 else "out_3d"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(DIR, "_scene_preview.png")

with open(os.path.join(DIR, "_sprite_meta.json"), encoding="utf-8") as f:
    META = json.load(f)
SPRITES = META["sprites"]
TW = META["px_per_tile"]          # 一格菱形的像素宽
TH = TW // 2                      # 固定 2:1


def sprite(ident, state="idle", d="SE", frame=None):
    """取图并返回它的地面锚点。锚点是画布内代表「脚底/格心」的像素坐标。"""
    st = SPRITES[ident]["states"][state]
    name = f"{ident}_{state}_{d}" + (f"_{frame}" if frame else "") + ".png"
    return Image.open(os.path.join(DIR, name)).convert("RGBA"), st["ground_anchor"]


def grid_to_screen(gi, gj, ox, oy):
    """格坐标 -> 屏幕像素。等距的全部数学就这两行。"""
    return ox + (gi - gj) * TW // 2, oy + (gi + gj) * TH // 2


def build(n=9):
    canvas = Image.new("RGBA", (TW * n + TW, TH * n + TW * 3), (0, 0, 0, 0))
    ox, oy = canvas.width // 2, TW * 2   # 顶部留够：塔与堡垒高达 4 格边长

    def place(ident, gi, gj, **kw):
        # 关键一步：把**锚点**对齐到格心，而不是把图片左上角对齐到格心。
        # 各精灵画布尺寸不同，按左上角贴会让高个子单位整体上浮。
        img, (ax, ay) = sprite(ident, **kw)
        x, y = grid_to_screen(gi, gj, ox, oy)
        canvas.alpha_composite(img, (int(x - ax), int(y - ay)))

    # 一条隘口：巨石与密林把进攻收束到中间两格，水域上架桥
    road = {(4, j) for j in range(n)} | {(i, 5) for i in range(5, n)}
    water = {(i, 8) for i in range(n)} - {(4, 8)}
    ash = {(7, 7), (8, 7), (7, 8)} - water

    def ground(k):
        if k in water:
            return "Water"
        if k in road:
            return "Road"
        if k in ash:
            return "Ash"
        return "Grass2" if (k[0] + k[1]) % 3 == 0 else "Grass"

    # 画家算法：按 gi+gj（即离屏幕上方的深度）从小到大画，后画的自然遮住先画的。
    # 地面与其上的物件都遵循同一个顺序，混排也不会穿插错。
    for s in range(2 * n - 1):
        for gi in range(n):
            gj = s - gi
            if not 0 <= gj < n:
                continue
            place(ground((gi, gj)), gi, gj)

    objs = [
        (4, 8, "Bridge", {}),
        (2, 2, "Boulder", {}), (3, 2, "Boulder", {}), (6, 2, "Woods", {}),
        (7, 2, "Woods", {}), (0, 6, "Woods", {}), (8, 4, "Boulder", {}),
        (1, 1, "TreeSmall", {}), (8, 1, "RockSmall", {}), (2, 6, "Stump", {}),
        (1, 4, "Wall", {}), (2, 4, "Wall", {}), (3, 4, "Gate", {}),
        (0, 4, "Tower", {}), (0, 2, "Keep", {}), (6, 6, "Mine", {}),
        (4, 1, "Watch", {}), (5, 0, "Barrack", {}),
        (4, 3, "Archer", {}), (5, 4, "Spear", {}),
        (5, 6, "Ghoul", {"state": "move", "frame": 8}),
        (6, 4, "Knight", {"state": "move", "frame": 6}),
        (7, 4, "Shade", {}), (3, 7, "Ranger", {"state": "move", "frame": 6}),
    ]
    for gi, gj, ident, kw in sorted(objs, key=lambda o: o[0] + o[1]):
        place(ident, gi, gj, **kw)
    return canvas


def main():
    canvas = build()
    bg = Image.new("RGB", canvas.size, (38, 42, 38))
    bg.paste(canvas, (0, 0), canvas)
    bg.crop(canvas.getbbox()).save(OUT, optimize=True)
    print(f"场景图: {OUT}")


if __name__ == "__main__":
    main()
