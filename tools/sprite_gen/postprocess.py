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
from PIL.PngImagePlugin import PngInfo

# 写入 PNG 文本块的处理标记。后处理默认原地覆盖，若无此标记，
# 重复运行会不断叠加描边与投影且不报错——图只是一次比一次糟。
DONE_KEY = "siege_postprocessed"


def already_done(img):
    return img.info.get(DONE_KEY) == "1"


def save_marked(img, path):
    meta = PngInfo()
    meta.add_text(DONE_KEY, "1")
    img.save(path, pnginfo=meta)


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


def write_meta(outdir, size, dfl):
    """随精灵输出一份元数据，作为前端的显式契约。

    前端要把精灵贴到等距格子上，必须知道图里**哪个像素是脚底**。
    靠 alpha 包围盒底边推算在单位带披风、长矛、尾羽时会偏，因此这里
    把画布尺寸与地面锚点写成显式约定，两条生成路线都必须遵守。

    3D 路线的 `blender_render.py` 会用相机投影精确算出锚点并先行写入，
    **不得覆盖**——本函数只在文件缺失时补一份估算值（供占位路线使用）。
    """
    path = os.path.join(outdir, "_sprite_meta.json")
    if os.path.exists(path):
        return
    meta = {
        "canvas": size,
        "tile": [dfl.get("tile_w", 64), dfl.get("tile_h", 32)],
        "ground_anchor": dfl.get("ground_anchor", [size // 2, int(size * 0.72)]),
        "dirs": dfl.get("dirs", ["SE", "SW", "NE", "NW"]),
        "note": "ground_anchor 是画布内代表单位脚底的像素坐标，前端据此把精灵对齐到格子中心。",
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("indir")
    ap.add_argument("--manifest", default=os.path.join(os.path.dirname(__file__), "assets.json"))
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--force", action="store_true",
                    help="对已带处理标记的图强制重新处理（会叠加描边，仅用于原始渲染输出）")
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

    groups, cell, skipped = {}, 0, 0
    for fn in sorted(os.listdir(a.indir)):
        if not fn.endswith(".png") or fn.startswith("_"):
            continue
        ident = fn.split("_")[0]
        src = Image.open(os.path.join(a.indir, fn))
        done = already_done(src)
        img = src.convert("RGBA")
        cell = max(cell, img.size[0])
        if done and not a.force:
            skipped += 1                      # 已处理过，直接收入对照图，不再叠加
        else:
            # 顺序不可颠倒：先描边，再把投影合成到描边之下。
            # 反过来的话投影自己也会被描边，变成带黑边的独立圆盘。
            img = add_outline(img, ol_col, ol_w)
            img = add_shadow(img, specs.get(ident, {}), ratio)
            save_marked(img, os.path.join(outdir, fn))
        groups.setdefault(ident, []).append(img)

    if not groups:
        print(f"{a.indir} 里没有可处理的 PNG。先跑 blender_render.py。")
        return
    for ident, frames in groups.items():
        print(f"  {ident:8s} {len(frames)} 帧")
    if skipped:
        print(f"\n跳过 {skipped} 张已处理的图（--force 可强制重处理，"
              f"但只应对未经后处理的原始渲染输出使用）")
    sheet = os.path.join(outdir, "_contact_sheet.png")
    contact_sheet(sorted(groups.items()), sheet, cell)
    write_meta(outdir, cell, dfl)
    print(f"\n对照图: {sheet}")


if __name__ == "__main__":
    main()
