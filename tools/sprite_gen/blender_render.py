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
import bpy, sys, os, json, math
from mathutils import Vector

# 俯角 30° => 投影瓦片 2:1。真等距（各轴等比）是 35.264°，本项目不用。
TILE_W, TILE_H = 128, 64       # 等距瓦片，固定 2:1，与 isolib 一致
CAM_PITCH = math.radians(60.0)
CAM_YAW = math.radians(45.0)

# 朝向 -> 模型绕 Z 轴的额外旋转（度）。
# 首次接入新模型时请渲一张出来目视确认，不对就改 assets.json 里的 yaw。
DIR_YAW = {"SE": 0, "SW": 90, "NE": 270, "NW": 180}


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


def setup_render(size, engine_samples=64):
    sc = bpy.context.scene
    for name in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        try:
            sc.render.engine = name
            break
        except TypeError:
            continue
    sc.render.resolution_x = sc.render.resolution_y = size
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


def setup_camera(ortho_scale, shift_y=0.22):
    """正交等距相机。

    shift_y 把画面上移，使**地面点落在画布约 72% 高度处**而非正中。
    不做偏移的话脚底位于画布中心，下半张全是空白，而长矛、塔顶、
    展翅这些向上延伸的部分反而会顶出画面。该比例与占位路线的
    isolib.ANCHOR 一致，两条路线共用同一份前端契约。
    """
    cam_data = bpy.data.cameras.new("IsoCam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = ortho_scale
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
        for o in dst.objects:
            if o is not None:
                bpy.context.collection.objects.link(o)
    else:
        raise ValueError(f"不支持的模型格式: {ext}")
    return [o for o in bpy.data.objects if o not in before]


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
        if o.type != "MESH":
            continue
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


def normalize(objs, target_h, lift, max_w=1.45):
    """把模型缩放到统一高度、底面贴地，并挂到一个空物体上便于整体旋转。

    统一尺度是跨来源风格一致的第一步：各家模型的原始尺度毫无可比性。
    除高度外还需约束**水平尺寸**——展翅的鸟类宽度可达高度的 8 倍，
    只按高度归一化会让翼展撑爆画面。
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
    s = target_h / h
    if w * s > max_w:                      # 宽度超限时改按宽度定标
        s = max_w / w
    cx, cy = (lo.x + hi.x) / 2, (lo.y + hi.y) / 2
    for o in objs:
        if o.parent is None:
            o.parent = pivot
    pivot.scale = (s, s, s)
    pivot.location = (-cx * s, -cy * s, -lo.z * s + lift)
    return pivot


def apply_tint(objs, tint):
    """整体染色：把同一模型改成不同阵营配色，省掉重复找素材。

    Base Color 未连接时直接混改默认值；已接纹理时插入一个混色节点，
    否则染色对带贴图的模型完全无效（本项目的骷髅与鹰都属此类）。
    """
    r, g, b, k = tint[0] / 255.0, tint[1] / 255.0, tint[2] / 255.0, tint[3]
    col = (r, g, b, 1.0)
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
                    c = inp.default_value
                    inp.default_value = (c[0] * (1 - k) + r * k,
                                         c[1] * (1 - k) + g * k,
                                         c[2] * (1 - k) + b * k, c[3])
                    continue
                src = inp.links[0].from_socket
                try:
                    mix = nt.nodes.new("ShaderNodeMix")
                    mix.data_type, mix.blend_type = "RGBA", "MIX"
                    mix.inputs[0].default_value = k     # Factor
                    mix.inputs[7].default_value = col   # B(Color)
                    nt.links.new(src, mix.inputs[6])    # A(Color)
                    nt.links.new(mix.outputs[2], inp)   # Result(Color)
                except RuntimeError:                     # 旧版本回退
                    mix = nt.nodes.new("ShaderNodeMixRGB")
                    mix.blend_type = "MIX"
                    mix.inputs[0].default_value = k
                    mix.inputs[2].default_value = col
                    nt.links.new(src, mix.inputs[1])
                    nt.links.new(mix.outputs[0], inp)
                mix.location = (n.location.x - 260, n.location.y - 180)


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


def set_action(objs, action_name):
    """给骨架指定动作。

    通过 libraries.load 载入 .blend 时，动作数据块虽然进来了，却不会自动绑到
    骨架上，模型会停在默认姿态——鹰的默认姿态是双翼下垂，渲出来只剩两片翅膀。
    素材包普遍自带 Idle / Walk / Flying / Attack 等动作，用它挑出合适的姿势。
    """
    act = bpy.data.actions.get(action_name)
    if act is None:
        cand = [a.name for a in bpy.data.actions]
        print(f"    [警告] 找不到动作 {action_name!r}，可用: {cand}")
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


def render_entry(ident, spec, base_dir, out_dir, size, dirs, ortho=2.0):
    reset_scene()
    setup_render(size)
    # 正交视野固定为 2.2 个瓦片宽：所有单位共用同一取景比例，尺寸才可比
    cam = setup_camera(ortho_scale=ortho)
    setup_lights()

    path = os.path.join(base_dir, spec["model"])
    if not os.path.exists(path):
        print(f"  [跳过] {ident}: 模型不存在 {spec['model']}")
        return 0, None
    objs = import_model(path)
    relocate_missing_textures(path)
    if spec.get("action"):
        set_action(objs, spec["action"])
    if spec.get("tint"):
        apply_tint(objs, spec["tint"])
    frames = spec.get("frames", [1])
    # 归一化必须在设定姿势之后：包围盒取自求值网格，姿势会改变它
    bpy.context.scene.frame_set(frames[0])
    pivot = normalize(objs, spec.get("height", 0.9), spec.get("lift", 0.0),
                      spec.get("max_width", 1.45))

    n = 0
    for d in dirs:
        for fr in frames:
            bpy.context.scene.frame_set(fr)
            pivot.rotation_euler[2] = math.radians(DIR_YAW[d] + spec.get("yaw", 0))
            suffix = f"_{fr}" if len(frames) > 1 else ""
            bpy.context.scene.render.filepath = os.path.join(out_dir, f"{ident}_{d}{suffix}.png")
            bpy.ops.render.render(write_still=True)
            n += 1
    print(f"  {ident:8s} -> {n} 帧")
    return n, ground_anchor(cam, size)


def ground_anchor(cam, size):
    """世界原点在画布中的像素坐标 —— 即单位脚底所在处。

    前端要把精灵对齐到等距格子，必须知道图里哪个像素是脚底。
    由相机投影精确算出，而不是靠 alpha 包围盒底边推算：后者在单位带
    披风、长矛、尾羽时会明显偏移。
    """
    from bpy_extras.object_utils import world_to_camera_view
    co = world_to_camera_view(bpy.context.scene, cam, Vector((0.0, 0.0, 0.0)))
    return [round(co.x * size, 1), round((1.0 - co.y) * size, 1)]


def write_meta(out_dir, size, dirs, anchor):
    meta = {
        "canvas": size,
        "tile": [TILE_W, TILE_H],
        "ground_anchor": anchor,
        "dirs": dirs,
        "note": "ground_anchor 是画布内代表单位脚底的像素坐标，前端据此把精灵对齐到格子中心。",
    }
    with open(os.path.join(out_dir, "_sprite_meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)


def main():
    args = parse_args()
    base = os.path.dirname(os.path.abspath(args["manifest"]))
    with open(args["manifest"], "r", encoding="utf-8") as f:
        mf = json.load(f)
    dfl = mf.get("defaults", {})
    size = args["size"] or dfl.get("size", 128)
    dirs = dfl.get("dirs", ["SE", "SW", "NE", "NW"])
    # 取景比例：世界单位跨度 = 画布边长 / 瓦片像素宽。
    # 值越小单位在画面中越大、细节越多；2.0 对应 256px 画布下 128px 的瓦片。
    ortho = dfl.get("ortho_scale", 2.0)
    out_dir = os.path.abspath(args["out"])
    os.makedirs(out_dir, exist_ok=True)

    entries = {}
    for group in ("units", "buildings"):
        entries.update(mf.get(group, {}))

    total, last_anchor = 0, [size / 2, size * 0.72]
    for ident, spec in entries.items():
        if args["only"] and ident not in args["only"]:
            continue
        n, anchor = render_entry(ident, spec, base, out_dir, size, dirs, ortho)
        total += n
        if anchor:
            last_anchor = anchor
    if total:
        write_meta(out_dir, size, dirs, last_anchor)
    print(f"\n共 {total} 帧 -> {out_dir}")
    print("下一步: python postprocess.py " + out_dir)


if __name__ == "__main__":
    main()
