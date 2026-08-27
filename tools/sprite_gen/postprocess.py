"""渲染后处理：描边、地面投影、对照图。

Blender 出的是干净的透明底 PNG，这里补上两样游戏里必需但不该在 3D 里做的东西：

1. **描边** —— 不是装饰。暗色阵营站在暗色地表上会整个糊掉，
   剪影读不出来就违反了 CLAUDE.md「风味名不得遮蔽机制可读性」那条设计原则。
2. **地面投影** —— 用平面椭圆而非真实阴影，因为它同时要表达"高度"：
   空中单位的投影留在地面、本体抬高，玩家一眼就知道它在飞。

用法:  python postprocess.py <渲染目录> [--manifest assets.json]
"""
import sys, os, json, argparse
from PIL import Image, ImageDraw, ImageFilter, ImageFont


def add_outline(img, color=(10, 10, 14, 205), w=1):
    """沿 alpha 轮廓外扩一圈描边，保证任意地表上剪影都可读。"""
    if w <= 0:
        return img
    grown = img.getchannel("A").filter(ImageFilter.MaxFilter(2 * w + 1))
    layer = Image.new("RGBA", img.size, color)
    layer.putalpha(grown)
    return Image.alpha_composite(layer, img)


def add_shadow(img, spec, tile_ratio=2.0):
    """在脚底画等距压扁的椭圆投影。lift 越大投影越小越淡，读作"飞得越高"。"""
    if spec.get("shadow") is False:
        return img
    lift = spec.get("lift", 0.0)
    W, H = img.size
    a = img.getchannel("A")
    bbox = a.getbbox()
    if bbox is None:
        return img
    # 地面基准线：贴地单位取像素底边；空中单位需按抬升量往下推回地面
    ground_y = bbox[3] if lift <= 0 else min(H - 2, int(bbox[3] + lift * W * 0.5))
    cx = (bbox[0] + bbox[2]) / 2
    rx = max(6.0, (bbox[2] - bbox[0]) * (0.42 if lift <= 0 else 0.30))
    ry = rx / tile_ratio
    alpha = 95 if lift <= 0 else 55

    sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([cx - rx, ground_y - ry, cx + rx, ground_y + ry],
                               fill=(0, 0, 0, alpha))
    sh = sh.filter(ImageFilter.GaussianBlur(1.2))
    return Image.alpha_composite(sh, img)


def _font(size):
    for p in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf",
              r"C:\Windows\Fonts\segoeui.ttf"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                pass
    return ImageFont.load_default()


def contact_sheet(rows, path, cell_px):
    """深浅两种地表各画一遍：剪影在草地和焦土上都得能认出来。"""
    if not rows:
        return
    pad, label_w, head = 10, 150, 34
    ncol = max(len(f) for _, f in rows)
    cell = cell_px + pad
    W = label_w + cell * ncol * 2 + pad * 3
    H = head + cell * len(rows) + pad
    sheet = Image.new("RGB", (W, H), (238, 238, 240))
    d = ImageDraw.Draw(sheet)
    f, fs = _font(16), _font(13)
    x_light = label_w
    x_dark = label_w + cell * ncol + pad * 2
    d.rectangle([x_dark - pad, head - 4, x_dark + cell * ncol, H - 6], fill=(52, 58, 52))
    d.text((pad, 8), "浅色 / 深色地表", font=f, fill=(30, 30, 30))
    for r, (ident, frames) in enumerate(rows):
        y = head + r * cell
        d.text((pad, y + cell_px // 2 - 8), ident, font=f, fill=(20, 20, 20))
        for i, fr in enumerate(frames):
            sheet.paste(fr, (x_light + i * cell, y), fr)
            sheet.paste(fr, (x_dark + i * cell, y), fr)
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("indir")
    ap.add_argument("--manifest", default=os.path.join(os.path.dirname(__file__), "assets.json"))
    ap.add_argument("--outdir", default=None)
    a = ap.parse_args()

    with open(a.manifest, "r", encoding="utf-8") as f:
        mf = json.load(f)
    dfl = mf.get("defaults", {})
    specs = {}
    for g in ("units", "buildings"):
        specs.update(mf.get(g, {}))
    ol_col = tuple(dfl.get("outline", [10, 10, 14, 205]))
    ol_w = dfl.get("outline_width", 1)
    ratio = dfl.get("tile_ratio", 2.0)
    outdir = a.outdir or a.indir
    os.makedirs(outdir, exist_ok=True)

    groups, cell = {}, 0
    for fn in sorted(os.listdir(a.indir)):
        if not fn.endswith(".png") or fn.startswith("_"):
            continue
        ident = fn.split("_")[0]
        img = Image.open(os.path.join(a.indir, fn)).convert("RGBA")
        cell = max(cell, img.size[0])
        # 顺序不可颠倒：先描边，再把投影合成到描边之下。
        # 反过来的话投影自己也会被描边，变成带黑边的独立圆盘。
        img = add_outline(img, ol_col, ol_w)
        img = add_shadow(img, specs.get(ident, {}), ratio)
        img.save(os.path.join(outdir, fn))
        groups.setdefault(ident, []).append(img)

    if not groups:
        print(f"{a.indir} 里没有可处理的 PNG。先跑 blender_render.py。")
        return
    for ident, frames in groups.items():
        print(f"  {ident:8s} {len(frames)} 帧")
    sheet = os.path.join(outdir, "_contact_sheet.png")
    contact_sheet(sorted(groups.items()), sheet, cell)
    print(f"\n对照图: {sheet}")


if __name__ == "__main__":
    main()
