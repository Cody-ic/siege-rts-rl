"""在 Blender 中把 3D 模型批量渲染为等距精灵。

用法（headless）:
    blender --background --python blender_render.py -- --manifest assets.json --out out_3d
    blender --background --python blender_render.py -- --only Knight,Phoenix

为什么走这条路：不同来源的 CC0 模型（KayKit / Quaternius 等）风格并不完全一致，
但只要全部经过**同一个相机 + 同一套灯光 + 同一分辨率**渲染，差异就被压平了。
这是混用 2D 精灵图永远做不到的——2D 图的投影角与光照方向已经烤死在像素里。

相机为正交投影，俯角 30°（水平面以上），即绕 X 轴 60°、绕 Z 轴 45°，
投影出来的瓦片正好是 2:1，与 isolib.TILE_W / TILE_H 一致。

朝向通过**旋转模型**实现，相机与灯光固定不动。这样各朝向的受光不同，
符合等距游戏的观感；若改为旋转相机，模型受光恒定，看起来会很平。
"""
import bpy, sys, os, json, math, colorsys
from mathutils import Vector, Matrix

# 俯角 30° => 投影瓦片 2:1。真等距（各轴等比）是 35.264°，本项目不用。
CAM_PITCH = math.radians(60.0)
CAM_YAW = math.radians(45.0)

# 一格地砖的世界边长。约定「1 世界单位 = px_per_tile 像素（沿屏幕横轴）」，
# 而边长 s 的正方形投影后横跨 sqrt(2)*s，故 s = 1/sqrt(2) 才能让菱形正好
# 占满 px_per_tile 像素宽、px_per_tile/2 像素高。改这个数等于改掉整套地砖。
TILE_SIDE = 1.0 / math.sqrt(2.0)

# 朝向 -> 模型绕 Z 轴的额外旋转（度）。
# 首次接入新模型时请渲一张出来目视确认，不对就改 assets.json 里的 yaw。
#
# `FREE` 不是一个方位，它表示「**这张图不承诺任何朝向，方向由前端在运行时旋转**」。
# 目前只有弹丸用它（`projectiles` 分组），理由见 README §8：弹丸的飞行方向是
# **连续的**，四个量化方位本质上表达不了——初版硬渲四向的结果是三长一短
# （某个方位恰好接近视线方向，内容框被压到不足一半）。
#
# 它的取值 45° 是**推导出来再实测确认的**，不是试出来的：相机 `rotation_euler`
# 是 `(PITCH, 0, YAW)`，于是屏幕水平轴（相机 local +X）在世界里是
# `(cos YAW, sin YAW, 0)`。让模型的长轴与它平行，投影后就水平——而水平是
# 2D 旋转的正确起点（前端加 `atan2` 出来的屏幕角即可）。
#
# **`SE` 那个 0° 不能拿来当起点**：它让箭在屏幕上斜着，前端旋转时得先减掉这个
# 未写在任何地方的固有偏角。这条偏角一旦漏掉，症状是「箭大致朝目标飞，但总歪一点」,
# 而那看着像弹道算错，不像资产不对。
#
# **`SW` 与 `NE` 曾经写反，2026-08-30 订正**（`SW` 是 90、`NE` 是 270）。
# 推导：相机 `rotation_euler = (PITCH, 0, YAW=45°)`、位于 `(+x, −y, +z)`，
# 于是四个水平世界轴投到屏幕上是
#
#     +x → 右下（`SE`）   −y → 左下（`SW`）   −x → 左上（`NW`）   +y → 右上（`NE`）
#
# 模型绕 Z 轴 **+90° 是逆时针**（从 +Z 俯视），把朝向 `+x` 转成 `+y` = 右上 = `NE`。
# 所以 `SW` 要的是 −90°（即 270），`NE` 要的是 90——原来的表把这两个对调了。
#
# 目视印证：`siege-ram.glb` 的撞锤头在 `SE` 指右下（对）、在旧的 `SW` 指**右上**
# （错，那是 `NE`）。症状是**沿 ±gj 走的单位背朝行进方向**，玩家的原话是
# 「骷髅兵横着走」；而沿 ±gi 走的单位一直是对的，所以它看起来像「有时对有时错」。
#
# 已入库的 242 张 `_SW` / `_NE` 图**按此互换了文件名**（内容一字未动——
# 那两张图本来就是彼此的那个朝向），因此这张表改完与磁盘上的图是一致的，
# 重渲一遍会得到同样的名字。
DIR_YAW = {"SE": 0, "SW": 270, "NE": 90, "NW": 180, "FREE": 45}

# 用 `FREE` 的条目必须渲成横躺。判据见 `check_assets.py`：内容框的高宽比要小。
FREE_DIR = "FREE"


def argv_after_ddash():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def parse_args():
    a = argv_after_ddash()
    out = {"manifest": os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets.json"), "out": "out_3d", "only": None, "size": None}
    i = 0
    while i < len(a):
        k = a[i].lstrip("-")
        if k in out and i + 1 < len(a):
            out[k] = a[i + 1]
            i += 2
        else:
            i += 1
    if out["size"]:
        out["size"] = int(out["size"])
    if out["only"]:
        out["only"] = {s.strip() for s in out["only"].split(",") if s.strip()}
    return out


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def setup_render(W, H, engine_samples=64):
    sc = bpy.context.scene
    for name in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        try:
            sc.render.engine = name
            break
        except TypeError:
            continue
    sc.render.resolution_x, sc.render.resolution_y = W, H
    sc.render.resolution_percentage = 100
    sc.render.film_transparent = True            # 透明底，精灵必需
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"
    ev = getattr(sc, "eevee", None)
    if ev is not None:
        for attr in ("taa_render_samples", "samples"):
            if hasattr(ev, attr):
                setattr(ev, attr, engine_samples)
        for attr in ("use_gtao", "use_raytracing"):
            if hasattr(ev, attr):
                setattr(ev, attr, True)


def cam_basis():
    """相机的右向量与上向量（世界坐标）。

    等距投影下**水平位移同样会产生纵向的屏幕位移**（up 的 x、y 分量非零）。
    马这类又长又矮的实体，纵向占位远大于其高度，按 height 估画布必然裁掉马蹄。
    """
    cp, sp = math.cos(CAM_PITCH), math.sin(CAM_PITCH)
    cy, sy = math.cos(CAM_YAW), math.sin(CAM_YAW)
    return Vector((cy, sy, 0.0)), Vector((-sy * cp, cy * cp, sp))


def setup_camera(ortho_scale, shift_x=0.0, shift_y=0.22):
    """正交等距相机。

    shift 把取景框对准实体的实际投影范围，使地面点落在画布下方而非正中。
    不做偏移的话脚底位于画布中心，下半张全是空白，而长矛、塔顶、展翅这些
    向上延伸的部分反而会顶出画面。偏移量由 `frame_for` 实测算出。
    """
    cam_data = bpy.data.cameras.new("IsoCam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = ortho_scale
    cam_data.shift_x = shift_x
    cam_data.shift_y = shift_y
    cam = bpy.data.objects.new("IsoCam", cam_data)
    bpy.context.collection.objects.link(cam)
    cam.rotation_euler = (CAM_PITCH, 0.0, CAM_YAW)
    # 沿视线方向后退，正交投影下距离不影响成像，只要不裁剪即可
    d = 20.0
    cam.location = (
        d * math.sin(CAM_PITCH) * math.sin(CAM_YAW),
        -d * math.sin(CAM_PITCH) * math.cos(CAM_YAW),
        d * math.cos(CAM_PITCH),
    )
    cam_data.clip_start, cam_data.clip_end = 0.1, 100.0
    bpy.context.scene.camera = cam
    return cam


def setup_lights():
    """固定灯光：主光 + 补光 + 轮廓光。所有模型共用，保证跨来源观感一致。"""
    def sun(name, rot, energy, color):
        d = bpy.data.lights.new(name, type="SUN")
        d.energy, d.color = energy, color
        d.angle = math.radians(12)
        o = bpy.data.objects.new(name, d)
        o.rotation_euler = [math.radians(v) for v in rot]
        bpy.context.collection.objects.link(o)

    sun("Key",  (55, 0, 30),   4.2, (1.00, 0.97, 0.90))   # 主光，左上偏前
    sun("Fill", (65, 0, 200),  1.5, (0.72, 0.78, 0.95))   # 冷调补光，压暗部
    sun("Rim",  (110, 0, 145), 2.6, (0.95, 0.95, 1.00))   # 轮廓光，把剪影从背景推开

    world = bpy.data.worlds.new("W")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.5, 0.55, 0.62, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.35
    bpy.context.scene.world = world


def make_primitive(spec, ident):
    """就地生成一个几何体，用在**素材库里没有、而借用别的模型会误导**的场合。

    目前唯一的用户是魔法弹（`Magic`）。它不能借用现成模型，理由与当初否掉
    「让法师射 `Arrow`」完全相同——**发射器与投射物必须匹配**：石头（`rocks-small`）
    是 `Rubble` 的主体、史莱姆是一只怪物，两者都会让玩家读出错误的东西。
    而法杖顶上那颗球虽然形状正对，却与杖身合并成了一块网格（`Wizard_Staff` 792 顶点
    整块），`only_meshes` 取不出来。

    **用 icosphere 而不是 uv_sphere**：整套素材都是低面数硬边风格，
    光滑球在里面像个塑料珠子。2 级细分 = 80 面，棱面清晰。

    这不违反「只用 CC0 素材」——那条约束的对象是**别人做的**素材（许可要允许我们
    再分发）；就地生成的几何是我们自己的，不存在许可问题。
    """
    kind = spec.get("kind_geom", "icosphere")
    before = set(bpy.data.objects)
    if kind == "icosphere":
        bpy.ops.mesh.primitive_ico_sphere_add(
            subdivisions=int(spec.get("subdiv", 2)), radius=float(spec.get("radius", 1.0)))
    elif kind == "uvsphere":
        bpy.ops.mesh.primitive_uv_sphere_add(radius=float(spec.get("radius", 1.0)))
    else:
        print(f"  [跳过] {ident}: 未知的 primitive.kind_geom = {kind!r}")
        return []
    new = [o for o in bpy.data.objects if o not in before]
    for o in new:
        o.name = spec.get("name", f"{ident}_prim")
        # 自带一份材质，否则 `apply_tint` 无处可染（新建的网格没有材质槽）。
        # 用 Emission 而不是 Principled：魔法弹该是**自己发光**的，
        # 受光的漫反射球在暗色地表上会跟着环境一起暗下去，读不出「这是能量」。
        m = bpy.data.materials.new(f"{ident}_mat")
        m.use_nodes = True
        nt = m.node_tree
        for n in list(nt.nodes):
            if n.type != "OUTPUT_MATERIAL":
                nt.nodes.remove(n)
        em = nt.nodes.new("ShaderNodeEmission")
        em.inputs["Strength"].default_value = float(spec.get("emission", 1.0))
        out = next(n for n in nt.nodes if n.type == "OUTPUT_MATERIAL")
        nt.links.new(em.outputs[0], out.inputs["Surface"])
        o.data.materials.append(m)
    return new


def import_model(path):
    ext = os.path.splitext(path)[1].lower()
    before = set(bpy.data.objects)
    if ext in (".gltf", ".glb"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        # Quaternius 的 FBX 动画已知会损坏，优先用 glb/blend
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext == ".obj":
        (bpy.ops.wm.obj_import if hasattr(bpy.ops.wm, "obj_import")
         else bpy.ops.import_scene.obj)(filepath=path)
    elif ext == ".blend":
        with bpy.data.libraries.load(path) as (src, dst):
            dst.objects = list(src.objects)
            # 动作必须显式载入：只拉 objects 时，仅有当前绑在某个对象上的动作会
            # 跟着进来。马的 Run 就因此缺失（文件里有，但没绑对象），而 set_action
            # 找不到时只是打印警告、模型停在默认姿态，不会中断渲染。
            dst.actions = list(src.actions)
        for o in dst.objects:
            if o is not None:
                bpy.context.collection.objects.link(o)
    else:
        raise ValueError(f"不支持的模型格式: {ext}")
    return [o for o in bpy.data.objects if o not in before]


def filter_meshes(objs, keep, ident):
    """只保留指定名字的网格，其余删掉。`keep` 为空则原样返回。

    一个 `.blend` 里常有多个网格，而这里只想要其中一个：`Ranger_Bow.blend` 同时含
    `Ranger_Bow` 与 `Ranger_Arrow`——弓手本体（`Ranger.blend`）**自带弓但不带箭**，
    所以要单独把箭挂上去，不筛就会多出第二把弓叠在原来那把上；
    而在途箭矢那个条目要的**只有箭**，不筛渲出来的就是一把弓。

    这两处一个走 `parts`、一个走顶层 `model`，所以筛选必须两条路径都有。
    第一版只做了 `parts` 那条，结果箭矢精灵渲出来是一把弓——**而它不报错**，
    只是图不对。
    """
    if not keep:
        return objs
    have = {o.name for o in objs}
    # **名字受加载顺序影响，所以按「精确优先、否则唯一后缀」解析。**
    # Blender 在重名时给后载入的对象加 `.001`：`Rogue.blend` 与 `Wizard.blend`
    # 都有一个叫 `Face` 的对象，谁先载入谁保住原名。于是清单里该写 `Face` 还是
    # `Face.001`，**取决于同一条目里前一个 part 有没有把它那块 `Face` 筛掉**——
    # 一个纯属实现细节的东西泄漏进了数据文件，而且改动 part 顺序就会静默失配。
    #
    # 规则刻意不是「一律去掉后缀再比」：`Wizard.001` 是 `Wizard.blend` 里的**原名**
    # （与空对象 `Wizard` 并存），去后缀会让两者撞在一起。**精确匹配优先**正好避开它。
    resolved, miss = set(), []
    for want in keep:
        if want in have:
            resolved.add(want)
            continue
        cand = [n for n in have if n.rsplit(".", 1)[0] == want and n != want]
        if len(cand) == 1:
            resolved.add(cand[0])
        else:
            miss.append(want)
    if miss:
        print(f"    [警告] {ident}: only_meshes 找不到 {sorted(miss)}，"
              f"该文件里有 {sorted(o.name for o in objs if o.type == 'MESH')}")
    drop = [o for o in objs if o.type == "MESH" and o.name not in resolved]
    for o in drop:
        bpy.data.objects.remove(o, do_unlink=True)
    return [o for o in objs if o not in drop]


def world_bbox(objs):
    """求值后的世界包围盒。

    必须用 depsgraph 求值后的网格：`Object.bound_box` 只反映静止姿态，
    不含骨骼形变，而展翅与收翅的实际尺寸可以差好几倍，
    用静止姿态定标会导致缩放严重失准。
    """
    dg = bpy.context.evaluated_depsgraph_get()
    lo = Vector((1e9,) * 3)
    hi = Vector((-1e9,) * 3)
    found = False
    for o in objs:
        if o.type != "MESH" or o.hide_render:
            continue        # 逐帧隐藏的部件（射出去的箭）不该把画布撑大
        oe = o.evaluated_get(dg)
        try:
            me = oe.to_mesh()
        except RuntimeError:
            continue
        if me is None:
            continue
        mw = oe.matrix_world
        for v in me.vertices:
            w = mw @ v.co
            lo = Vector((min(lo[i], w[i]) for i in range(3)))
            hi = Vector((max(hi[i], w[i]) for i in range(3)))
            found = True
        oe.to_mesh_clear()
    return (lo, hi) if found else None


def normalize(objs, target_h, lift, max_w=1.45, tile=False, modules=None):
    """把模型定标、底面贴地，并挂到一个空物体上便于整体旋转。

    **尺寸一律以「格子边长」为 1**，不是以菱形的屏幕宽度为 1。后者是早先的约定，
    导致所有实体被放大了 √2 倍——单位比地砖还大，大树比战马还矮。

    三种定标方式：

    - `modules=k`：**1 个模型单位 = k 格边长**，不做归一化。Kenney 整套件都建在
      1.0 模数上且内部自洽（墙高 1.31、塔身 1.01、大树 1.85、攻城锤长 2.09），
      统一乘一个系数即可让全套件与格子、以及彼此之间的比例自动正确。
      逐个手调高度反而会破坏这份自洽。
    - `tile=True`：按水平 footprint 定标到一格。地砖尺寸由格子决定，与自身高度无关。
    - 默认：按 `target_h`（单位：格边长）定标，并用 `max_w` 约束水平尺寸——
      展翅的鸟类宽度可达高度的 8 倍，只按高度归一化会让翼展撑爆画面。
      Quaternius 的角色原始尺度毫无可比性，只能走这条。
    """
    # 素材包中常有对象被作者关掉渲染可见性（如 Quaternius 的 Skeleton），
    # 不强制打开会渲出空图。
    for o in objs:
        o.hide_render = False
        o.hide_viewport = False

    bb = world_bbox(objs)
    pivot = bpy.data.objects.new("Pivot", None)
    bpy.context.collection.objects.link(pivot)
    if bb is None:
        return pivot
    lo, hi = bb
    h = max(hi.z - lo.z, 1e-6)
    w = max(hi.x - lo.x, hi.y - lo.y, 1e-6)
    if modules:
        s = TILE_SIDE * modules
    elif tile:
        s = TILE_SIDE / w
    else:
        s = target_h * TILE_SIDE / h
        if w * s > max_w * TILE_SIDE:      # 宽度超限时改按宽度定标
            s = max_w * TILE_SIDE / w
    cx, cy = (lo.x + hi.x) / 2, (lo.y + hi.y) / 2
    for o in objs:
        if o.parent is None:
            o.parent = pivot
    pivot["ground_reference"] = (cx, cy, lo.z)
    pivot.scale = (s, s, s)
    pivot.location = (-cx * s, -cy * s, -lo.z * s + lift * TILE_SIDE)
    return pivot


def _mix_node(nt, src, color, k, mode, loc):
    """在 src 之后插入一个混色节点，返回新的输出 socket。"""
    try:
        mix = nt.nodes.new("ShaderNodeMix")
        mix.data_type, mix.blend_type = "RGBA", mode
        mix.inputs[0].default_value = k     # Factor
        mix.inputs[7].default_value = color  # B(Color)
        nt.links.new(src, mix.inputs[6])     # A(Color)
        out = mix.outputs[2]                 # Result(Color)
    except (RuntimeError, TypeError):        # 旧版本回退
        mix = nt.nodes.new("ShaderNodeMixRGB")
        mix.blend_type = mode
        mix.inputs[0].default_value = k
        mix.inputs[2].default_value = color
        nt.links.new(src, mix.inputs[1])
        out = mix.outputs[0]
    mix.location = loc
    return out


def apply_tint(objs, tint, mode="COLOR", brightness=None):
    """阵营染色：把同一模型改成不同阵营配色，省掉重复找素材。

    **默认用 Color 混合，而不是线性混到一个平色。** Color 混合取染色的色相与
    饱和度、保留原纹理的明度，因此明暗层次完全不损失；线性混合则按比例抹掉层次，
    强度调到能一眼认出阵营时模型已经糊成一块平色——亡灵弓手曾因此丢掉五官与衣褶，
    幽灵战马被染成淡粉。要"更明显"时应当加大饱和度或压暗，而不是加大混合强度。

    `brightness` 在换色之后再整体调整亮度（<1 压暗，>1 提亮）。之所以要两步：Color 混合
    只取色相与饱和度、明度照抄原图，无法压暗；而 MULTIPLY 只能压暗、加不出原图
    没有的颜色——棕马乘幽紫只会变成暗红。冷色的幽灵战马必须"先换色相再压暗"。

    Base Color 未连接时直接改默认值；已接纹理时插入混色节点，
    否则染色对带贴图的模型完全无效（本项目的骷髅与蝙蝠都属此类）。
    """
    r, g, b, k = tint[0] / 255.0, tint[1] / 255.0, tint[2] / 255.0, tint[3]
    col = (r, g, b, 1.0)
    th, ts, _ = colorsys.rgb_to_hsv(r, g, b)
    # 不同素材包用的着色器不同：Principled 的接口叫 Base Color，
    # 而 Diffuse/Emission 叫 Color。只认前者会导致对部分模型静默失效
    # （本项目的骷髅材质就有两个输出节点，实际生效的是 Diffuse BSDF）。
    NAMES = ("Base Color", "Color")
    for o in objs:
        if o.type != "MESH":
            continue
        for slot in o.material_slots:
            m = slot.material
            if not m or not m.use_nodes:
                continue
            nt = m.node_tree
            for n in list(nt.nodes):
                if not hasattr(n, "inputs"):
                    continue
                inp = next((n.inputs[nm] for nm in NAMES if nm in n.inputs), None)
                if inp is None:
                    continue
                if not inp.is_linked:
                    # 这里必须自己实现混合模式。照搬线性插值会让 MULTIPLY 变成
                    # "刷成平色"——幽灵战马因此不但没压暗，反而被刷亮成了淡紫。
                    c = inp.default_value
                    if mode == "MULTIPLY":
                        tr, tg, tb = c[0] * r, c[1] * g, c[2] * b
                    elif mode == "COLOR":   # 保留原色明度，只换色相与饱和度
                        v = colorsys.rgb_to_hsv(c[0], c[1], c[2])[2]
                        tr, tg, tb = colorsys.hsv_to_rgb(th, ts, v)
                    else:
                        tr, tg, tb = r, g, b
                    d = 1.0 if brightness is None else brightness
                    inp.default_value = (min(1.0, (c[0] * (1 - k) + tr * k) * d),
                                         min(1.0, (c[1] * (1 - k) + tg * k) * d),
                                         min(1.0, (c[2] * (1 - k) + tb * k) * d), c[3])
                    continue
                out = _mix_node(nt, inp.links[0].from_socket, col, k, mode,
                                (n.location.x - 300, n.location.y - 180))
                if brightness is not None:
                    # 必须用向量缩放，不能用 MULTIPLY 混色节点：后者的颜色输入被
                    # 钳制在 0~1，brightness>1 传进去就变成白色，等于静默失效
                    # ——不死鸟提亮到 2.8 仍然一片暗，就是撞在这里。
                    sc = nt.nodes.new("ShaderNodeVectorMath")
                    sc.operation = "SCALE"
                    sc.inputs[3].default_value = brightness
                    sc.location = (n.location.x - 150, n.location.y - 180)
                    nt.links.new(out, sc.inputs[0])
                    out = sc.outputs[0]
                nt.links.new(out, inp)


def relocate_missing_textures(model_path, levels=3):
    """把找不到的纹理按文件名重新定位到素材包内的实际位置。

    多数 CC0 包的 .blend 把纹理路径写成与自身同级，但分发时纹理被放进了
    单独的 Textures/ 目录，路径因此对不上，模型会渲染成品红色。
    Blender 的 file.find_missing_files 算子在无头模式下不可靠，
    这里改为沿模型路径向上逐级搜索同名文件并直接改写 filepath。
    """
    missing = [img for img in bpy.data.images
               if img.source == "FILE" and img.filepath
               and not os.path.exists(bpy.path.abspath(img.filepath))]
    if not missing:
        return

    d = os.path.dirname(os.path.abspath(model_path))
    index = {}
    for _ in range(levels):
        for dirpath, _dirs, files in os.walk(d):
            for f in files:
                index.setdefault(f.lower(), os.path.join(dirpath, f))
        if any(os.path.basename(i.filepath).lower() in index for i in missing):
            break
        d = os.path.dirname(d)

    unresolved = []
    for img in missing:
        hit = index.get(os.path.basename(img.filepath).lower())
        if hit:
            img.filepath = hit
            img.reload()
        else:
            unresolved.append(img.name)
    if unresolved:
        print(f"    [警告] 纹理仍缺失，将渲染为品红: {unresolved}")


def set_action(objs, action_name, pool=None):
    """给骨架指定动作。

    通过 libraries.load 载入 .blend 时，动作数据块虽然进来了，却不会自动绑到
    骨架上，模型会停在默认姿态——鹰的默认姿态是双翼下垂，渲出来只剩两片翅膀。
    素材包普遍自带 Idle / Walk / Flying / Attack 等动作，用它挑出合适的姿势。

    `pool` 把候选限定在**本部件刚载入的**动作里。多包拼装时动作必然重名
    （马与盗贼都有 Idle），后载入的会被 Blender 改成 `Idle.001`，
    按全局名查找就会张冠李戴：把马的动作绑到骑手骨架上。骨骼名对不上时
    Blender 不报错，骑手只是静默停在默认姿态——这类 bug 只能靠肉眼发现。
    """
    cands = list(pool) if pool is not None else list(bpy.data.actions)
    act = next((a for a in cands if a.name == action_name), None)
    if act is None:                       # 载入时被加了 .001 之类的去重后缀
        act = next((a for a in cands if a.name.rsplit(".", 1)[0] == action_name), None)
    if act is None:
        print(f"    [警告] 找不到动作 {action_name!r}，可用: {[a.name for a in cands]}")
        return
    for o in objs:
        if o.type != "ARMATURE":
            continue
        if o.animation_data is None:
            o.animation_data_create()
        o.animation_data.action = act
        # Blender 4.4+ 引入 action slot，未指定时动作不会生效
        slots = getattr(act, "slots", None)
        if slots and hasattr(o.animation_data, "action_slot"):
            try:
                o.animation_data.action_slot = slots[0]
            except (TypeError, IndexError):
                pass


def set_pose(objs, pose, freeze_frame):
    """在动作之上叠加手工骨骼姿态（欧拉角，度）。

    **必须先把动作在该帧求值后清除**：动作会在每次换帧时重写骨骼变换，
    不清除的话手工设的角度会被动画系统悄悄冲掉，且不报错。
    清除后骨骼保留最后一次求值的姿态，手工覆写才生效。

    素材包普遍没有"骑乘"动作，骑手直接放上马背时双腿会插进马身。
    这里用它把大腿前摆、小腿外张，绕开马腹。

    `freeze_frame` 默认是 `frames[0]`，但可用 `pose_frame` 单独指定。
    这个区分在做**骑兵的攻击态**时是必需的：定格的骑手的手臂姿态来自
    「该动作在 freeze_frame 上的求值」，而 `frames` 同时还在驱动**马**的步态。
    两者绑在一起的话，要让骑手停在挥击帧就得把马的步态也从那一帧起采，
    于是攻击态与移动态的马腿顺序不同、看着像卡了一下。
    """
    bpy.context.scene.frame_set(freeze_frame)
    for o in objs:
        if o.type != "ARMATURE":
            continue
        if o.animation_data:
            o.animation_data.action = None      # 定格，后续换帧不再覆写
        for name, v in pose.items():
            b = o.pose.bones.get(name)
            if b is None:
                print(f"    [警告] 找不到骨骼 {name!r}，可用: {[x.name for x in o.pose.bones]}")
                continue
            s = v if isinstance(v, dict) else {"rot": v}
            if "rot" in s:
                b.rotation_mode = "XYZ"
                b.rotation_euler = [math.radians(x) for x in s["rot"]]
            if "loc" in s:
                b.location = s["loc"]


def apply_mesh_tint(objs, mesh_tint, ident):
    """逐**网格**染色，覆盖该部件/整体的染色。名字匹配同 `mesh_anim`（末尾 `*` 通配）。

    存在的理由是**一个模型里有两块该分开着色、却分不开的东西**。
    `Wizard.blend` 就是：`Face`（头发 + 面部）是独立对象，而头颅本身长在
    `Wizard.001`（整个袍身）里。想把「面部压暗成兜帽下的空洞」而袍身保持亮色，
    没有这条就只剩两条路，两条都不通：

    * **`only_meshes` 拆成两个 part** —— 同一个 `.blend` 加载两次，Blender 会给
      第二份全部对象加 `.001` 后缀，于是 `only_meshes: ["Face"]` 匹配不上
      （实测：报「找不到 Face，该文件里有 Face.001…」）。而**按去后缀匹配是错的**，
      因为 `Wizard.001` 本来就是这个文件里的原名，去掉后缀会与 `Wizard` 撞名
    * **整体压暗** —— 那是把袍子一起压暗，等于换了个配色，不是「面部看不清」

    与 `part.tint` 的关系是**覆盖**而非叠加：`apply_tint` 改的是材质节点，
    同一材质被染两次结果不可预测，所以这里在整体染色**之后**跑，
    对命中的网格重来一遍。

    **因此染之前必须先把材质变成独占副本。** 这一条不是防御性的余量——
    本函数的首个使用场景就撞上了：`Wizard.blend` 里 `Face` 与 `Wizard.001`（袍身）
    **共用同一个 `Wizard_Texture` 数据块**，不复制的话「把面部压黑」会连整件袍子
    一起压黑，渲出来是一个纯黑的人形。（这段注释的初版恰好断言了「本项目当前没有
    这种情形」，第一次运行就被推翻——**素材共用材质是常态而非例外**，
    同一个素材包里的角色几乎总共用一张纹理图集。）
    """
    for pattern, spec in mesh_tint.items():
        pref = pattern[:-1] if pattern.endswith("*") else None
        hit = [o for o in objs if o.type == "MESH"
               and (o.name.startswith(pref) if pref else o.name == pattern)]
        if not hit:
            print(f"    [警告] {ident}: mesh_tint 找不到对象 {pattern!r}，"
                  f"可用: {sorted(o.name for o in objs if o.type == 'MESH')}")
            continue
        for o in hit:
            for slot in o.material_slots:
                if slot.material and slot.material.users > 1:
                    slot.material = slot.material.copy()
        apply_tint(hit, spec["tint"], spec.get("tint_mode", "COLOR"),
                   spec.get("brightness"))


def collect_mesh_anim(objs, mesh_anim):
    """把 `mesh_anim` 的对象名解析成实际对象，并记下它们的静止变换。

    存在的理由是 Kenney 的攻城器械**没有任何动作，但有可分离的子网格**：
    `siege-ram.glb` 里 `ram`（撞木）与六个 `wheel` 各自是独立网格。
    撞木前后滑动就是撞击、轮子绕轴转就是行进——两者都是**纯物体变换**，
    不需要骨架，也就不需要素材包提供动作。

    早先把这类模型判成「静态网格、零动作、做不了攻击动画」是**只查了动作数**，
    没看结构。查动作之外还要看子网格。

    名字支持末尾通配（`wheel*` 命中六个轮子）。静止变换要在**归一化之后**取，
    因为归一化会改根对象的缩放，而子对象的局部位移会被那份缩放带着走——
    这正好是我们要的：偏移量用**模型单位**写，与素材的原始尺度一致。
    """
    rest = []
    for pattern, spec in mesh_anim.items():
        pref = pattern[:-1] if pattern.endswith("*") else None
        hit = [o for o in objs
               if (o.name.startswith(pref) if pref else o.name == pattern)]
        if not hit:
            print(f"    [警告] mesh_anim 找不到对象 {pattern!r}，"
                  f"可用: {sorted(o.name for o in objs)}")
            continue
        if spec.get("pivot_center"):
            # 旋转是绕**对象原点**的，而素材里原点常常不在几何中心：
            # `siege-ram.glb` 的 `wheel` 原点在 (0.67, 0.5, 0)、几何中心在 (0.47, 0.43, 0.20)，
            # 直接转它得到的是**绕车体公转**而不是自转——看着像轮子往外甩出去。
            # 症状只在多帧并排看时才显形，单帧完全正常。
            bpy.ops.object.select_all(action="DESELECT")
            for o in hit:
                o.select_set(True)
            bpy.context.view_layer.objects.active = hit[0]
            bpy.ops.object.origin_set(type="ORIGIN_GEOMETRY", center="BOUNDS")
            bpy.ops.object.select_all(action="DESELECT")
        for o in hit:
            rest.append((o, tuple(o.location), tuple(o.rotation_euler), spec))
    return rest


def apply_mesh_anim(rest, idx):
    """把第 `idx` 帧的偏移/旋转施加到子网格上（相对静止变换，不累加）。

    每帧都从静止量重算，而不是在上一帧基础上叠加——叠加的话渲染顺序一变
    （比如加了个朝向循环）结果就不同，而那种错**只在多帧时才显形**。
    """
    for o, loc0, rot0, spec in rest:
        vis = spec.get("visible")
        if vis is not None:
            # 逐帧显隐。弓手的箭要在**命中帧之后消失**——箭射出去了，弦上就该空。
            # 这也是 `attack` 之外的状态里把箭藏起来的手段：部件是实体级的、不分状态，
            # 而待机与奔跑时弓上本就不该搭着箭。
            o.hide_render = not bool(vis[idx % len(vis)])
        off = spec.get("offset")
        if off:
            d = off[idx % len(off)]
            o.location = (loc0[0] + d[0], loc0[1] + d[1], loc0[2] + d[2])
        rot = spec.get("rot")
        if rot:
            d = rot[idx % len(rot)]
            o.rotation_mode = "XYZ"
            o.rotation_euler = (rot0[0] + math.radians(d[0]),
                                rot0[1] + math.radians(d[1]),
                                rot0[2] + math.radians(d[2]))
    if rest:
        bpy.context.view_layer.update()   # matrix_world 要立刻可用（取景要读它）


def find_bone(name):
    """在场景已有的骨架中查找骨骼，返回持有它的骨架对象。"""
    for o in bpy.context.scene.objects:
        if o.type == "ARMATURE" and name in o.pose.bones:
            return o
    return None


def states_of(spec):
    """展开动画状态，返回 [(状态名, 该状态的完整 spec)]。

    未声明 states 的实体（建筑、攻城锤）视为只有一个 idle 状态，因此
    前端可以对所有实体一视同仁地按 (标识符, 状态) 取图。

    每个状态是一份**深拷贝**：状态之间共享 parts 字典的话，给 move 覆写
    动作会连带改掉 idle 的。
    """
    st = spec.get("states")
    if not st:
        return [("idle", spec)]
    out = []
    for name, ov in st.items():
        e = json.loads(json.dumps(spec))
        e.pop("states", None)
        for k in ("action", "frames", "pose_frame", "impact_frame", "mesh_anim", "rot"):
            if k in ov:
                e[k] = ov[k]
        for idx, pov in (ov.get("parts") or {}).items():
            e["parts"][int(idx)].update(pov)
        out.append((name, e))
    return out


def render_entry(ident, spec, base_dir, out_dir, px_per_tile, margin, dirs):
    reset_scene()
    setup_lights()
    frames = spec.get("frames", [1])
    bpy.context.scene.frame_set(frames[0])   # 骨骼摆位与归一化都以首帧为准

    # Kenney 城堡包是模块化的：塔身分为底座/中段/顶层/屋顶数块，
    # 每块都从 z=0 起建模，单独渲染只是塔的一截。parts 按累计高度自下而上堆叠。
    if spec.get("parts"):
        objs, z, tinted = [], 0.0, []
        for part in spec["parts"]:
            pp = os.path.join(base_dir, part["model"])
            if not os.path.exists(pp):
                print(f"  [跳过] {ident}: 模型不存在 {part['model']}")
                return 0, None
            before_acts = set(bpy.data.actions)
            po = import_model(pp)
            new_acts = [a for a in bpy.data.actions if a not in before_acts]
            relocate_missing_textures(pp)
            po = filter_meshes(po, part.get("only_meshes"), ident)
            if part.get("action"):
                set_action(po, part["action"], new_acts)
            if part.get("pose"):
                set_pose(po, part["pose"], part.get("pose_frame", frames[0]))
            roots = [o for o in po if o.parent is None]
            # 各素材包尺度互不相干，跨包组合（如骑手上马）须先按 scale 对齐比例
            s = part.get("scale", 1.0)
            if s != 1.0:
                for o in roots:
                    o.scale = tuple(v * s for v in o.scale)
                    o.location = tuple(v * s for v in o.location)
            # rot 绕部件自身原点旋转（度）。武器模型的原点都在握把处，
            # 因此这正好等于"改变握持角度"——骑枪压平指向前方靠的就是它。
            rot = part.get("rot")
            if rot:
                for o in roots:
                    o.rotation_euler = [o.rotation_euler[i] + math.radians(rot[i])
                                        for i in range(3)]
            bpy.context.view_layer.update()
            off = part.get("offset")
            bone = part.get("attach")
            if bone:
                # 建立真正的骨骼父子关系，而不是算一次世界矩阵摆上去。
                # 摆放式绑定只在定格的部件上成立：骷髅一跑起来手臂摆动，
                # 静态摆放的剑就留在原地脱手了。
                arm = find_bone(bone)
                if arm is None:
                    print(f"    [警告] 找不到用于绑定的骨骼 {bone!r}")
                else:
                    # 子对象会继承骨架与骨骼的缩放，而清单里的 scale 应当是
                    # 世界尺度（与非绑定部件同义），故在此除掉继承的那一份。
                    bsc = (arm.matrix_world @ arm.pose.bones[bone].matrix).to_scale()
                    # `attach_rest`：**保持部件原位，再跟随骨骼**，而不是把它的原点
                    # 搬到骨尾。两种绑定对应两类素材，选错了偏差大到一眼可见：
                    #
                    # * 默认（原点对骨尾）适合**武器**——它们的原点就在握把处，
                    #   所以 offset 通常是 0
                    # * `attach_rest` 适合**同骨架角色之间移植的身体部件**：兜帽的原点
                    #   在角色脚下（0,0,0），照默认那条会被抬到骨尾去，高出去两个多单位
                    #
                    # 实现就是 `matrix_parent_inverse` 的本义（Blender「保持变换」父子化
                    # 做的同一件事），基准取**静止姿态**而非当前帧：取当前帧的话，
                    # 每个状态的首帧姿势不同，同一个部件在 idle / move / attack 里
                    # 会各自落在不同的地方——而那种错**只在跨状态并排看时才显形**。
                    rest_inv = Matrix.Identity(4)
                    if part.get("attach_rest"):
                        b = arm.data.bones[bone]
                        rest_inv = (arm.matrix_world @ b.matrix_local
                                    @ Matrix.Translation((0.0, b.length, 0.0))).inverted()
                    for o in roots:
                        keep = o.matrix_world.copy()
                        o.parent = arm
                        o.parent_type = "BONE"
                        o.parent_bone = bone
                        o.matrix_parent_inverse = rest_inv
                        if part.get("attach_rest"):
                            # 局部矩阵原样保留（`rot` / `scale` 仍然生效），
                            # 位置由 matrix_parent_inverse 承担；`offset` 在这条路上
                            # 是**静止姿态下的世界微调**（把兜帽往下压半格之类），
                            # 与默认那条「骨骼局部坐标」不同义——两者本来就在不同的空间里
                            o.matrix_basis = (Matrix.Translation(off) @ keep) if off else keep
                            continue
                        o.scale = tuple(o.scale[i] / max(abs(bsc[i]), 1e-6) for i in range(3))
                        # 骨骼父子关系以**骨尾**为原点、局部 Y 沿骨长，
                        # 因此手部武器的 offset 通常就是 0
                        o.location = tuple(off) if off else (0.0, 0.0, 0.0)
            elif off:
                # 显式定位：用于组合而非堆叠，不参与累计高度
                for o in roots:
                    o.location.x += off[0]
                    o.location.y += off[1]
                    o.location.z += off[2]
            else:
                gap = part.get("gap", 0.0)
                bb = world_bbox(po)
                for o in roots:
                    o.location.z += z + gap
                z += (bb[1].z - bb[0].z if bb else 0.0) + gap
            # 逐部件染色：整体染色会把骷髅也染成紫的，而骨头本该是骨白。
            # 幽灵战马压暗成幽紫、骑手保持骨白、骑枪留冷钢色，三者才拉得开层次。
            if part.get("tint"):
                apply_tint(po, part["tint"], part.get("tint_mode", "COLOR"),
                           part.get("brightness"))
                tinted.extend(po)
            # 顺序不可颠倒：逐网格染色是**覆盖**整体染色，见 apply_mesh_tint
            if part.get("mesh_tint"):
                apply_mesh_tint(po, part["mesh_tint"], ident)
            objs.extend(po)
        path = os.path.join(base_dir, spec["parts"][0]["model"])
    elif spec.get("primitive"):
        # 就地生成的几何：没有文件、没有纹理、没有动作，所以三件事都跳过
        objs = make_primitive(spec["primitive"], ident)
        if not objs:
            return 0, None
        tinted = []
    else:
        path = os.path.join(base_dir, spec["model"])
        if not os.path.exists(path):
            print(f"  [跳过] {ident}: 模型不存在 {spec['model']}")
            return 0, None
        objs = import_model(path)
        relocate_missing_textures(path)
        objs = filter_meshes(objs, spec.get("only_meshes"), ident)
        tinted = []
    # 顶层 action 只对单模型有效：parts 拼装时每个部件的动作各不相同（马在腾跃、
    # 骑手在待机），在这里统一绑定会把先前逐部件设好的动作全部覆盖掉。
    if spec.get("action") and not spec.get("parts"):
        set_action(objs, spec["action"])
    # 顶层 rot：整体俯仰 / 翻滚，`yaw` 管不了的那两个轴。
    #
    # 两处要用：**在途箭矢**的模型长轴是竖直的（Z 向 1.93），不放平就是一根竖着飞的箭；
    # **不死鸟俯冲**同理——Eagle.blend 只有 `Flying` 与 `Idle` 两个动作、没有攻击，
    # 而俯冲本来也不该靠动作表达，它是整只鸟的姿态。
    #
    # 必须在归一化**之前**施加：归一化按包围盒定标，而俯仰会改变包围盒
    # （竖直的箭放平之后，高度与宽度整个对调）。
    if spec.get("rot"):
        rx, ry, rz = [math.radians(v) for v in spec["rot"]]
        for o in [o for o in objs if o.parent is None]:
            o.rotation_euler = (o.rotation_euler[0] + rx,
                                o.rotation_euler[1] + ry,
                                o.rotation_euler[2] + rz)
        bpy.context.view_layer.update()
    if spec.get("pose"):
        set_pose(objs, spec["pose"], spec.get("pose_frame", frames[0]))
    if spec.get("tint"):        # 已单独指定染色的部件不再被整体染色覆盖
        apply_tint([o for o in objs if o not in tinted], spec["tint"],
                   spec.get("tint_mode", "COLOR"), spec.get("brightness"))
    if spec.get("mesh_tint"):   # 顶层逐网格染色，同样是覆盖（见 apply_mesh_tint）
        apply_mesh_tint(objs, spec["mesh_tint"], ident)
    # 归一化必须在设定姿势之后：包围盒取自求值网格，姿势会改变它
    bpy.context.scene.frame_set(frames[0])
    pivot = normalize(objs, spec.get("height", 0.9), spec.get("lift", 0.0),
                      spec.get("max_width", 1.45), spec.get("kind") == "tile",
                      spec.get("modules"))

    # 子网格的逐帧变换要在归一化之后收集（静止量含归一化后的缩放），
    # 且必须在取景之前——撞木伸出去那一帧决定了画布该多宽。
    mesh_rest = collect_mesh_anim(objs, spec["mesh_anim"]) if spec.get("mesh_anim") else []

    # 取景须在归一化之后：画布按实测投影范围确定，而非按声明尺寸估算
    W, H, ortho, shift_x, shift_y = frame_for(objs, spec, dirs, frames, px_per_tile,
                                              margin, mesh_rest)
    setup_render(W, H)
    cam = setup_camera(ortho, shift_x, shift_y)

    directional_anchors = {}
    n = 0
    for d in dirs:
        for i, fr in enumerate(frames):
            bpy.context.scene.frame_set(fr)
            apply_mesh_anim(mesh_rest, i)      # 顺序不可颠倒：frame_set 之后才施加
            pivot.rotation_euler[2] = math.radians(DIR_YAW[d] + spec.get("yaw", 0))
            if spec.get("directional_ground_anchor"):
                bpy.context.view_layer.update()
                point = pivot.matrix_world @ Vector(pivot["ground_reference"])
                directional_anchors[d] = ground_anchor(cam, W, H, point)
            suffix = f"_{fr}" if len(frames) > 1 else ""
            bpy.context.scene.render.filepath = os.path.join(out_dir, f"{ident}_{d}{suffix}.png")
            bpy.ops.render.render(write_still=True)
            n += 1
    print(f"  {ident:8s} -> {n} 帧  {W}x{H}")
    # frames 写进元数据：多帧时文件名带 _<帧号> 后缀，单帧时不带，前端据此取名
    info = {"canvas": [W, H], "ground_anchor": ground_anchor(cam, W, H),
            "frames": list(frames)}
    if directional_anchors:
        info["ground_anchor_by_facing"] = directional_anchors
    if spec.get("muzzle_uv_by_facing"):
        info["muzzle_by_facing"] = {d: [round(v[0]*W, 2), round(v[1]*H, 2)] for d, v in spec["muzzle_uv_by_facing"].items()}
    if spec.get("kind"):
        info["kind"] = spec["kind"]        # 后处理据此跳过描边与地面投影
    if spec.get("kind") == "projectile":
        # **弹丸另给一个 `pivot`：前端要绕它旋转的那个点。**
        #
        # `ground_anchor` 对弹丸是没有意义的——它是「实体脚底」，而弹丸不站在
        # 地上。但它也不能就这么被当成旋转中心用：它是**世界原点**的投影，
        # 而 `normalize()` 把 bbox 中心移到原点之后 `pivot` 又绕自身 location
        # 旋转了 45°，于是原点的投影与箭的视觉中心实测差 19–26 px。
        # 拿它当旋转中心，箭会绕一个看不见的点公转——那是很显眼的错，
        # 但**在静态精灵上完全看不出来**，只有转起来才暴露。
        #
        # 所以这里算的是**几何中心**（pivot 下所有网格的世界 bbox 中心），
        # 而不是原点。它与 alpha 内容框中心一致（描边对称，不改中心）。
        info["pivot"] = mesh_center_px(pivot, cam, W, H)
        # **`oriented`：这枚弹丸有没有「朝向」这回事。**
        #
        # 箭与弩矢有：它们横躺一张、由前端按飞行角旋转，所以那张图必须真的躺平
        # （`check_assets.py` 的宽高比检查守着这条）。**魔法弹没有**——它是个球，
        # 旋转它是恒等变换，宽高比检查对它不成立（球的比是 1.0，永远过不了）。
        #
        # 写进元数据而不是只在检查脚本里判，是因为它同样是**前端契约的一部分**：
        # 前端可以据它跳过那次旋转。现在的 `scene_renderer` 一律旋转，
        # 对球而言只是白转一次、外加把棱面转起来（看着像自转，无害），
        # 所以不强制它改——但这个事实该记在数据里，而不是靠读代码推断。
        info["oriented"] = bool(spec.get("oriented", True))
    # impact_frame：本状态里「打出去」的那一帧（弓弦松开 / 刀锋落下）。
    #
    # 它存在的理由是**精灵资产不得编码任何待定数值**：`CLAUDE.md` 有「弓手有攻击前摇」
    # 这条机制，而前摇时长是待标定的数值。若前端按「N 帧循环播完」来放攻击动画，
    # 改前摇就要重渲精灵。给出命中帧之后，前端可以**保持命中前的帧**直到仿真说前摇结束，
    # 再播其余帧 —— 帧数与 tick 因此解耦，动画只提供关键姿态、不提供时长。
    if spec.get("impact_frame") is not None:
        info["impact_frame"] = spec["impact_frame"]
    return n, info


# 底部预留量，须覆盖 postprocess.add_shadow 画的椭圆（其半高约为实体像素宽的 0.21 倍）。
# 不预留则宽实体（骑兵）的地面投影会被画布下边缘切掉半个。
SHADOW_PAD = 0.24


def frame_for(objs, spec, dirs, frames, px_per_tile, margin, mesh_rest=()):
    """把归一化后的几何投影到相机平面，按**实测范围**定画布与相机参数。

    必须在 normalize 之后调用。早先的做法是按 `height` / `max_width` 估算，
    有两个错处：等距投影下水平位移也会产生纵向屏幕位移（长而矮的马被裁掉蹄子），
    且 `max_width` 是归一化的**上限**而非实际宽度（步兵的画布因此宽了近一倍）。

    实体高度跨度很大（幽影窥使 0.45 瓦片 ~ 领主堡垒 2.4 瓦片，约 5 倍），
    统一画布必然二选一：要么裁掉高的，要么把矮的挤成一小点。因此这里
    **保持统一的世界→像素缩放**（相对大小才正确，单位本就该比塔矮），
    只让画布尺寸随实体变化。代价是前端需按精灵读取各自的锚点，
    这已由 `_sprite_meta.json` 承担。
    """
    right, up = cam_basis()
    # 起始含世界原点：地面锚点必须落在画布内，否则前端无法对齐格子、
    # 后处理也无处画投影（空中单位抬升后本体整个在原点上方）。
    us, vs = [0.0], [0.0]
    # 各朝向、各动画帧共用同一画布，故取全部的并集。只按首帧定画布的话，
    # 奔跑循环里前后腿全展的那几帧会被悄悄裁掉。
    for i, f in enumerate(frames):
        bpy.context.scene.frame_set(f)
        apply_mesh_anim(mesh_rest, i)   # 撞木伸出去的那一帧也要算进画布，否则被裁掉
        bb = world_bbox(objs)
        if not bb:
            continue
        lo, hi = bb
        corners = [Vector((x, y, z)) for x in (lo.x, hi.x)
                   for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
        for d in dirs:
            a = math.radians(DIR_YAW[d] + spec.get("yaw", 0))
            ca, sa = math.cos(a), math.sin(a)
            for c in corners:
                r = Vector((c.x * ca - c.y * sa, c.x * sa + c.y * ca, c.z))
                us.append(r.dot(right))
                vs.append(r.dot(up))
    bpy.context.scene.frame_set(frames[0])
    u0, u1, v0, v1 = min(us), max(us), min(vs), max(vs)
    if spec.get("kind") == "tile":
        # 地砖不留白、不预留投影：菱形必须正好占满画布，相邻格才能严丝合缝
        pass
    else:
        v0 -= max(margin, SHADOW_PAD * (u1 - u0))   # 留白与投影预留取其大，不叠加
        v1 += margin
        u0 -= margin
        u1 += margin

    W = int(round((u1 - u0) * px_per_tile))
    H = int(round((v1 - v0) * px_per_tile))
    W += W % 2                                  # 偶数尺寸，避免半像素锚点
    H += H % 2
    ortho = max(W, H) / px_per_tile             # 世界跨度 = 大边像素 / 每瓦片像素
    # 取景中心即相机偏移：Blender 的 shift 以大边为单位，故除以 ortho_scale。
    return W, H, ortho, (u0 + u1) / 2 / ortho, (v0 + v1) / 2 / ortho


def ground_anchor(cam, W, H, point=None):
    """世界原点在画布中的像素坐标 —— 即单位脚底所在处。

    前端要把精灵对齐到等距格子，必须知道图里哪个像素是脚底。
    由相机投影精确算出，而不是靠 alpha 包围盒底边推算：后者在单位带
    披风、长矛、尾羽时会明显偏移。
    """
    from bpy_extras.object_utils import world_to_camera_view
    co = world_to_camera_view(bpy.context.scene, cam, Vector((0.0, 0.0, 0.0)) if point is None else point)
    return [round(co.x * W, 1), round((1.0 - co.y) * H, 1)]


def mesh_center_px(pivot, cam, W, H):
    """`pivot` 下所有网格的世界 bbox 中心在画布中的像素坐标。

    只给弹丸用（见 `render_entry`）。与 `ground_anchor` 的区别是它取**几何中心**
    而不是世界原点——旋转中心必须落在物体自己身上，而原点在旋转之后不一定还在。

    投影的是 bbox 的**八个角点再取像素均值**，不是「先算世界中心再投影」。
    两者在正交投影下等价，但前者对 `world_to_camera_view` 的实现细节
    （shift / ortho_scale 怎么进矩阵）不做任何假设，改相机时不会静默偏掉。
    """
    from bpy_extras.object_utils import world_to_camera_view
    objs = [o for o in bpy.data.objects
            if o.type == "MESH" and _has_ancestor(o, pivot)]
    bb = world_bbox(objs)
    if bb is None:
        return ground_anchor(cam, W, H)
    lo, hi = bb
    xs, ys = [], []
    for x in (lo.x, hi.x):
        for y in (lo.y, hi.y):
            for z in (lo.z, hi.z):
                co = world_to_camera_view(bpy.context.scene, cam, Vector((x, y, z)))
                xs.append(co.x * W)
                ys.append((1.0 - co.y) * H)
    return [round((min(xs) + max(xs)) / 2, 1), round((min(ys) + max(ys)) / 2, 1)]


def _has_ancestor(obj, anc):
    p = obj.parent
    while p is not None:
        if p is anc:
            return True
        p = p.parent
    return False


def write_meta(out_dir, px_per_tile, dirs, sprites):
    # 与已有元数据合并：--only 只重渲部分实体时，直接覆盖会丢掉其余条目
    path = os.path.join(out_dir, "_sprite_meta.json")
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as f:
                merged = json.load(f).get("sprites", {})
            merged.update(sprites)
            sprites = merged
        except (OSError, ValueError):
            pass
    meta = {
        "px_per_tile": px_per_tile,
        "tile": [px_per_tile, px_per_tile // 2],   # 一格菱形的像素尺寸，固定 2:1
        "dirs": dirs,
        "note": ("文件名 <标识符>_<状态>_<朝向>[_<帧号>].png，单帧时无帧号后缀。"
                 "kind==tile 的条目是地砖：画布正好等于一格菱形、无留白无投影，"
                 "锚点在菱形中心，按格心贴图即可无缝平铺。"
                 "所有精灵共用同一世界->像素缩放（px_per_tile），故相对大小正确；"
                 "画布逐状态适配，canvas 与 ground_anchor 必须按 (标识符,状态) 读取。"
                 "状态间画布不同但锚点对齐，故状态切换不会跳动。"
                 "ground_anchor 是画布内代表实体脚底的像素坐标。"
                 "状态名不是固定集合，按实体声明：单位有 idle/move，能攻击的还有 attack，"
                 "工匠是 work（它无战力，叫 attack 是错的）；未声明状态的实体只有 idle。"
                 "impact_frame 是该状态里「打出去」的那一帧（弓弦松开 / 刀锋落下）。"
                 "前端应当**保持命中前的帧**直到仿真说前摇结束、再播其余帧 —— "
                 "不要按固定节奏播完，攻击前摇是待标定数值，精灵不编码时长。"
                 "顶层 dirs 是默认朝向集合；某个实体若自带 dirs 字段则以它为准"
                 "（没有该字段就用顶层的，这是向后兼容的读法）。"
                 "朝向名 FREE 表示**这张图不承诺任何朝向**：它渲成横躺、箭头朝右"
                 "（屏幕 +X 即 0 弧度），方向由前端在运行时按飞行角 2D 旋转。"
                 "弹丸用它，因为飞行方向是连续的、四个量化方位表达不了。"
                 "kind==projectile 的条目另有 pivot 字段：**要绕它旋转的那个像素点**。"
                 "不要拿 ground_anchor 当旋转中心——那是世界原点的投影、代表脚底，"
                 "而弹丸不站在地上，两者实测差 19–26 px，"
                 "用错的症状是箭绕一个看不见的点公转（静态图上看不出来）。"),
        "sprites": sprites,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)


def main():
    args = parse_args()
    base = os.path.dirname(os.path.abspath(args["manifest"]))
    with open(args["manifest"], "r", encoding="utf-8") as f:
        mf = json.load(f)
    dfl = mf.get("defaults", {})
    px = dfl.get("px_per_tile", 128)
    margin = dfl.get("margin", 0.22)
    dirs = dfl.get("dirs", ["SE", "SW", "NE", "NW"])
    # 取景比例：世界单位跨度 = 画布边长 / 瓦片像素宽。
    # 值越小单位在画面中越大、细节越多；2.0 对应 256px 画布下 128px 的瓦片。

    out_dir = os.path.abspath(args["out"])
    os.makedirs(out_dir, exist_ok=True)

    entries = {}
    for group in ("units", "buildings", "terrain", "obstacles", "projectiles"):
        # 跳过 `_` 开头的键：分组内允许放 `_comment` 之类的说明，它们不是实体
        entries.update({k: v for k, v in mf.get(group, {}).items()
                        if not k.startswith("_")})

    total, sprites = 0, {}
    for ident, spec in entries.items():
        if args["only"] and ident not in args["only"]:
            continue
        # **每实体可覆盖 `dirs`。** 口子开在这里而不是给 `projectiles` 分组写一条
        # 特例分支：分组名是给人读的归类，不该同时是渲染行为的开关——那样
        # 「换个分组会不会改变出图」就成了一个要读代码才能回答的问题。
        entry_dirs = spec.get("dirs", dirs)
        st_info = {}
        for st_name, eff in states_of(spec):
            n, info = render_entry(f"{ident}_{st_name}", eff, base, out_dir, px,
                                   margin, entry_dirs)
            total += n
            if info:
                st_info[st_name] = info
        if st_info:
            rec = {"cn": spec.get("cn", ident), "states": st_info}
            # **只在与全局不同时才写**，于是 35 个老条目的元数据一个字节都不变，
            # 而前端「没有这个字段就用顶层 dirs」是一条向后兼容的读法。
            if entry_dirs != dirs:
                rec["dirs"] = entry_dirs
            sprites[ident] = rec
    if total:
        write_meta(out_dir, px, dirs, sprites)
    print(f"\n共 {total} 帧 -> {out_dir}")
    print("下一步: python postprocess.py " + out_dir)


if __name__ == "__main__":
    main()
