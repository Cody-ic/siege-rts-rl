"""生成等距占位精灵，并输出一张便于审阅的对照图。

用法:  python gen.py [输出目录]
输出:  <out>/<Ident>_<方向>.png  逐帧精灵
       <out>/_contact_sheet.png  对照图
"""
import sys, os
from PIL import Image, ImageDraw, ImageFont
from isolib import Canvas, KINGDOM, NETHER, DIRS, CANVAS
import units

# 标识符 -> (中文名, 阵营配色, 绘制函数)
ROSTER = [
    ("Spear",   "铁壁枪卫",   KINGDOM, lambda c, p, d: units.spear(c, p, d)),
    ("Ranger",  "逐风猎骑",   KINGDOM, lambda c, p, d: units.cavalry(c, p, d, undead=False)),
    ("Ghoul",   "亡灵步兵",   NETHER,  lambda c, p, d: units.ghoul(c, p, d)),
    ("Knight",  "鬼域骑士团", NETHER,  lambda c, p, d: units.cavalry(c, p, d, undead=True)),
    ("Phoenix", "不死鸟",     NETHER,  lambda c, p, d: units.phoenix(c, p, d)),
]


def _font(size):
    for p in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf",
              r"C:\Windows\Fonts\segoeui.ttf"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                pass
    return ImageFont.load_default()


def render(ident, pal, fn):
    frames = []
    for d in DIRS:
        c = Canvas()
        fn(c, pal, d)
        frames.append(c.out())
    return frames


def contact_sheet(rows, path):
    """深浅两种背景各画一遍，用于检查剪影在不同地表上的可读性。"""
    pad, label_w, head = 10, 132, 34
    cell = CANVAS + pad
    W = label_w + cell * len(DIRS) * 2 + pad * 3
    H = head + cell * len(rows) + pad
    sheet = Image.new("RGB", (W, H), (238, 238, 240))
    d = ImageDraw.Draw(sheet)
    f, fs = _font(16), _font(13)

    x_light = label_w
    x_dark = label_w + cell * len(DIRS) + pad * 2
    d.rectangle([x_dark - pad, head - 4, x_dark + cell * len(DIRS), H - 6], fill=(52, 58, 52))
    # 标题放在左侧标签栏内，避免遮挡第一列的方向标签
    d.text((pad, 8), "浅色 / 深色地表", font=f, fill=(30, 30, 30))
    for i, dd in enumerate(DIRS):
        d.text((x_light + i * cell + 34, head - 22), dd, font=fs, fill=(90, 90, 90))
        d.text((x_dark + i * cell + 34, head - 22), dd, font=fs, fill=(200, 200, 200))

    for r, (ident, cn, frames) in enumerate(rows):
        y = head + r * cell
        d.text((pad, y + 30), f"{cn}", font=f, fill=(20, 20, 20))
        d.text((pad, y + 52), f"{ident}", font=fs, fill=(120, 120, 120))
        for i, fr in enumerate(frames):
            sheet.paste(fr, (x_light + i * cell, y), fr)
            sheet.paste(fr, (x_dark + i * cell, y), fr)
    sheet.save(path)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out, exist_ok=True)
    rows = []
    for ident, cn, pal, fn in ROSTER:
        frames = render(ident, pal, fn)
        for d, fr in zip(DIRS, frames):
            fr.save(os.path.join(out, f"{ident}_{d}.png"))
        rows.append((ident, cn, frames))
        print(f"  {ident:8s} {cn}  -> {len(frames)} 帧")
    sheet = os.path.join(out, "_contact_sheet.png")
    contact_sheet(rows, sheet)
    print(f"\n对照图: {sheet}")


if __name__ == "__main__":
    main()
