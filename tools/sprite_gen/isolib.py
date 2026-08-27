"""等距(2:1)占位精灵的绘制基元。

所有绘制在 SS 倍超采样画布上进行，最后缩回目标尺寸，以获得平滑边缘。
世界坐标约定：x 向右下、y 向左下，z 向上。
"""
from PIL import Image, ImageDraw, ImageFilter

SS = 4                    # 超采样倍数
TILE_W, TILE_H = 128, 64  # 等距瓦片，固定 2:1。128 px 才容得下模型细节
CANVAS = 256              # 单帧画布边长，与 3D 路线一致（前端契约）
ANCHOR = (128, 184)       # 地面锚点（脚底像素坐标），位于画布 72% 高度处


def shade(rgb, k):
    """按系数 k 调整明度，用于区分顶面/左面/右面。"""
    return tuple(max(0, min(255, int(c * k))) for c in rgb)


class Canvas:
    def __init__(self, size=CANVAS, anchor=ANCHOR):
        self.size = size
        self.ax, self.ay = anchor[0] * SS, anchor[1] * SS
        self.img = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)
        # 投影单独一层：它不参与描边，否则会变成带黑边的独立圆盘
        self.sh_img = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
        self.sh_d = ImageDraw.Draw(self.sh_img)

    def pt(self, x, y, z=0.0):
        """世界坐标 -> 超采样画布坐标。"""
        sx = self.ax + (x - y) * (TILE_W / 2) * SS
        sy = self.ay + (x + y) * (TILE_H / 2) * SS - z * (TILE_H) * SS
        return (sx, sy)

    def poly(self, pts, fill, outline=None, w=1):
        self.d.polygon(pts, fill=fill, outline=outline, width=w * SS)

    def shadow(self, rx=0.34, ry=0.17, alpha=90):
        """地面投影，椭圆按等距比例压扁。画在独立层上，不参与描边。"""
        cx, cy = self.pt(0, 0, 0)
        self.sh_d.ellipse(
            [cx - rx * TILE_W * SS, cy - ry * TILE_H * SS,
             cx + rx * TILE_W * SS, cy + ry * TILE_H * SS],
            fill=(0, 0, 0, alpha))

    def box(self, x, y, z, sx, sy, sz, col, edge=None):
        """等距立方体：绘制顶面 + 左面 + 右面三个可见面。"""
        hx, hy = sx / 2, sy / 2
        top = [self.pt(x - hx, y - hy, z + sz), self.pt(x + hx, y - hy, z + sz),
               self.pt(x + hx, y + hy, z + sz), self.pt(x - hx, y + hy, z + sz)]
        left = [top[3], top[2], self.pt(x + hx, y + hy, z), self.pt(x - hx, y + hy, z)]
        right = [top[1], top[2], self.pt(x + hx, y + hy, z), self.pt(x + hx, y - hy, z)]
        self.poly(left,  shade(col, 0.62), edge)
        self.poly(right, shade(col, 0.80), edge)
        self.poly(top,   shade(col, 1.00), edge)

    def orb(self, x, y, z, r, col):
        """球体近似：等距压扁的椭圆 + 高光。"""
        cx, cy = self.pt(x, y, z)
        rw, rh = r * TILE_W * SS, r * TILE_W * SS * 0.86
        self.d.ellipse([cx - rw, cy - rh, cx + rw, cy + rh], fill=col)
        self.d.ellipse([cx - rw * 0.5, cy - rh * 0.72,
                        cx + rw * 0.15, cy - rh * 0.10], fill=shade(col, 1.28))

    def stick(self, p0, p1, col, w=2.2):
        """世界坐标下的杆状物（长矛、旗杆等）。"""
        self.d.line([self.pt(*p0), self.pt(*p1)], fill=col, width=int(w * SS))

    def out(self, outline=(10, 10, 14, 205), w=2):
        """缩回目标尺寸，并沿轮廓加一圈描边。

        描边不是装饰：单位必须在任意地表上都能读出剪影，
        否则暗色阵营在暗色地面上会整个糊掉。
        """
        img = self.img.resize((self.size, self.size), Image.LANCZOS)
        if outline:
            grown = img.getchannel("A").filter(ImageFilter.MaxFilter(2 * w + 1))
            layer = Image.new("RGBA", img.size, outline)
            layer.putalpha(grown)
            img = Image.alpha_composite(layer, img)
        # 投影最后合成到描边之下
        sh = self.sh_img.resize((self.size, self.size), Image.LANCZOS)
        return Image.alpha_composite(sh, img)


# 阵营配色
KINGDOM = dict(
    main=(62, 92, 134), alt=(201, 212, 227), metal=(150, 162, 178),
    accent=(212, 165, 55), skin=(226, 190, 158), edge=(24, 32, 48, 255),
)
NETHER = dict(
    main=(74, 59, 99), alt=(127, 166, 92), metal=(96, 88, 112),
    accent=(200, 192, 168), skin=(146, 170, 128), edge=(18, 14, 26, 255),
)

# 四个朝向：相机固定，只需这四种
DIRS = ["SE", "SW", "NE", "NW"]
DIR_VEC = {"SE": (1, 0), "SW": (0, 1), "NE": (0, -1), "NW": (-1, 0)}
