"""五个单位的占位精灵绘制。

设计原则（见 CLAUDE.md「风味名不得遮蔽机制可读性」）：
剪影承载机制信息 —— 步兵直立紧凑、骑兵横向拉长、空中单位带高度偏移与投影。
"""
import math
from isolib import Canvas, DIR_VEC, shade


def _humanoid(c, pal, fx, fy, z=0.0, scale=1.0, hunch=0.0):
    """通用人形：双腿 + 躯干 + 头。hunch 用于亡灵的佝偻姿态。"""
    s = scale
    # 双腿（垂直于朝向左右分开）
    for side in (-1, 1):
        ox, oy = -fy * side * 0.09, fx * side * 0.09
        c.box(ox, oy, z, 0.10 * s, 0.10 * s, 0.26 * s, shade(pal["main"], 0.75))
    # 躯干，向朝向轻微前倾
    c.box(fx * hunch * 0.05, fy * hunch * 0.05, z + 0.26 * s,
          0.24 * s, 0.24 * s, 0.34 * s, pal["main"])
    # 肩甲
    c.box(0, 0, z + 0.56 * s, 0.30 * s, 0.30 * s, 0.07 * s, pal["metal"])
    # 头
    c.orb(fx * hunch * 0.10, fy * hunch * 0.10, z + 0.70 * s, 0.075 * s, pal["skin"])


def spear(c, pal, d):
    """铁壁枪卫 / 亡灵步兵通用底子：直立 + 竖直长矛，剪影紧凑偏高。"""
    fx, fy = DIR_VEC[d]
    c.shadow()
    _humanoid(c, pal, fx, fy)
    # 长矛：竖直，是步兵剪影的识别特征
    hx, hy = fx * 0.20, fy * 0.20
    c.stick((hx, hy, 0.02), (hx, hy, 1.18), pal["metal"], 2.0)
    c.poly([c.pt(hx, hy, 1.30), c.pt(hx - 0.06, hy - 0.06, 1.14),
            c.pt(hx + 0.06, hy + 0.06, 1.14)], pal["alt"])
    # 盾（背对朝向的一侧）：贴身且偏小，避免盖住躯干
    sx, sy = -fy * 0.17, fx * 0.17
    c.box(sx, sy, 0.30, 0.05, 0.14, 0.20, pal["accent"])


def ghoul(c, pal, d):
    """亡灵步兵：同为步兵剪影，但佝偻、无盾、持骨刃，与人类步兵可区分。"""
    fx, fy = DIR_VEC[d]
    c.shadow(alpha=70)
    # 躯干不缩小：亡灵靠姿态而非体型与人类步兵区分
    _humanoid(c, pal, fx, fy, scale=1.0, hunch=1.0)
    # 骨刃：单把、明确朝前下方斜持，是唯一的手持物
    hx, hy = fx * 0.24, fy * 0.24
    c.stick((hx, hy, 0.66), (hx * 2.1, hy * 2.1, 0.16), pal["accent"], 2.4)
    # 背部骨刺：短、贴背，只做轮廓毛边，不做长杆
    for i, h in enumerate((0.44, 0.56, 0.66)):
        k = 0.085 - i * 0.016
        c.stick((-fx * 0.13, -fy * 0.13, h), (-fx * (0.13 + k), -fy * (0.13 + k), h + k * 0.8),
                pal["accent"], 1.4)


def _horse(c, pal, fx, fy, body_col, leg_col):
    """四足坐骑：横向拉长，骑兵剪影的识别特征。"""
    # 四条腿
    for a in (-1, 1):
        for b in (-1, 1):
            lx = fx * 0.20 * a - fy * 0.11 * b
            ly = fy * 0.20 * a + fx * 0.11 * b
            c.box(lx, ly, 0, 0.075, 0.075, 0.30, leg_col)
    # 躯干：沿朝向拉长
    c.box(0, 0, 0.30, 0.24 + abs(fx) * 0.34, 0.24 + abs(fy) * 0.34, 0.24, body_col)
    # 颈与头
    nx, ny = fx * 0.30, fy * 0.30
    c.box(nx, ny, 0.44, 0.13, 0.13, 0.22, shade(body_col, 0.92))
    c.box(nx * 1.30, ny * 1.30, 0.62, 0.17, 0.17, 0.12, body_col)


def cavalry(c, pal, d, undead=False):
    """逐风猎骑 / 鬼域骑士团：骑手 + 坐骑，横向剪影。"""
    fx, fy = DIR_VEC[d]
    c.shadow(rx=0.46, ry=0.23, alpha=95 if not undead else 75)
    body = shade(pal["metal"], 0.85) if undead else (110, 82, 60)
    _horse(c, pal, fx, fy, body, shade(body, 0.78))
    # 骑手坐在马背上
    _humanoid(c, pal, fx, fy, z=0.54, scale=0.82, hunch=0.6 if undead else 0.0)
    # 骑枪：前倾，与步兵的竖直长矛形成对比
    hx, hy = fx * 0.26, fy * 0.26
    c.stick((hx - fx * 0.5, hy - fy * 0.5, 0.86), (hx + fx * 0.86, hy + fy * 0.86, 0.70),
            pal["accent"] if undead else pal["metal"], 2.0)
    if undead:
        # 破损披风，区别于人类骑兵的整洁轮廓
        for i in (-1, 0, 1):
            c.stick((-fx * 0.22 - fy * 0.07 * i, -fy * 0.22 + fx * 0.07 * i, 0.92),
                    (-fx * 0.40 - fy * 0.09 * i, -fy * 0.40 + fx * 0.09 * i, 0.40 + 0.06 * abs(i)),
                    pal["main"], 2.6)


def phoenix(c, pal, d):
    """不死鸟：空中单位，投影留在地面、本体抬高，直观表达高度。"""
    fx, fy = DIR_VEC[d]
    ALT = 1.15                       # 飞行高度
    c.shadow(rx=0.30, ry=0.15, alpha=55)
    SPEC = (150, 226, 240)           # 幽蓝灵焰，与亡灵配色区分
    # 双翼：向两侧大幅展开，是空中单位最强的识别特征
    for side in (-1, 1):
        px, py = -fy * side, fx * side
        c.poly([c.pt(0, 0, ALT + 0.06),
                c.pt(px * 0.62, py * 0.62, ALT + 0.34),
                c.pt(px * 0.80, py * 0.80, ALT + 0.02),
                c.pt(px * 0.34, py * 0.34, ALT - 0.06)],
               shade(SPEC, 0.74 if side < 0 else 0.92))
    # 躯体与头
    c.box(0, 0, ALT - 0.04, 0.20, 0.20, 0.22, shade(SPEC, 0.86))
    c.orb(fx * 0.16, fy * 0.16, ALT + 0.24, 0.070, SPEC)
    # 尾羽：向后拖三束
    for i in (-1, 0, 1):
        c.stick((-fx * 0.16 - fy * 0.05 * i, -fy * 0.16 + fx * 0.05 * i, ALT + 0.04),
                (-fx * 0.66 - fy * 0.13 * i, -fy * 0.66 + fx * 0.13 * i, ALT - 0.16 + 0.07 * abs(i)),
                shade(SPEC, 1.05), 2.2)
    # 灵焰余烬
    for i, (t, h) in enumerate(((0.30, 0.26), (0.52, 0.10), (0.72, -0.04))):
        c.orb(-fx * t, -fy * t, ALT + h, 0.030 - i * 0.006, (235, 250, 255))
