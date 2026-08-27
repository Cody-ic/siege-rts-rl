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
CAM_PITCH = math.radians(60.0)
CAM_YAW = math.radians(45.0)

# 朝向 -> 模型绕 Z 轴的额外旋转（度）。
# 首次接入新模型时请渲一张出来目视确认，不对就改 assets.json 里的 yaw。
DIR_YAW = {"SE": 0, "SW": 90, "NE": 270, "NW": 180}


def argv_after_ddash():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def parse_args():
    a = argv_after_ddash()
    out = {"manifest": "assets.json", "out": "out_3d", "only": None, "size": None}
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


def setup_render(size, engine_samples=32):
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


def setup_camera(ortho_scale):
    cam_data = bpy.data.cameras.new("IsoCam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = ortho_scale
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
    lo = Vector((1e9,) * 3)
    hi = Vector((-1e9,) * 3)
    found = False
    for o in objs:
        if o.type != "MESH":
            continue
        found = True
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            lo = Vector((min(lo[i], w[i]) for i in range(3)))
            hi = Vector((max(hi[i], w[i]) for i in range(3)))
    return (lo, hi) if found else None


def normalize(objs, target_h, lift):
    """把模型缩放到统一高度、底面贴地，并挂到一个空物体上便于整体旋转。

    统一高度是跨来源风格一致的第一步：各家模型的原始尺度毫无可比性。
    """
    bb = world_bbox(objs)
    pivot = bpy.data.objects.new("Pivot", None)
    bpy.context.collection.objects.link(pivot)
    if bb is None:
        return pivot
    lo, hi = bb
    h = max(hi.z - lo.z, 1e-6)
    s = target_h / h
    cx, cy = (lo.x + hi.x) / 2, (lo.y + hi.y) / 2
    for o in objs:
        if o.parent is None:
            o.parent = pivot
    pivot.scale = (s, s, s)
    pivot.location = (-cx * s, -cy * s, -lo.z * s + lift)
    return pivot


def apply_tint(objs, tint):
    """整体染色：把同一模型改成不同阵营配色，省掉重复找素材。"""
    r, g, b, k = tint[0] / 255.0, tint[1] / 255.0, tint[2] / 255.0, tint[3]
    for o in objs:
        if o.type != "MESH":
            continue
        for slot in o.material_slots:
            m = slot.material
            if not m or not m.use_nodes:
                continue
            for n in m.node_tree.nodes:
                inp = n.inputs.get("Base Color") if hasattr(n, "inputs") else None
                if inp is None or inp.is_linked:
                    continue
                c = inp.default_value
                inp.default_value = (c[0] * (1 - k) + r * k,
                                     c[1] * (1 - k) + g * k,
                                     c[2] * (1 - k) + b * k, c[3])


def render_entry(ident, spec, base_dir, out_dir, size, dirs):
    reset_scene()
    setup_render(size)
    # 正交视野固定为 2.2 个瓦片宽：所有单位共用同一取景比例，尺寸才可比
    setup_camera(ortho_scale=2.2)
    setup_lights()

    path = os.path.join(base_dir, spec["model"])
    if not os.path.exists(path):
        print(f"  [跳过] {ident}: 模型不存在 {spec['model']}")
        return 0
    objs = import_model(path)
    if spec.get("tint"):
        apply_tint(objs, spec["tint"])
    pivot = normalize(objs, spec.get("height", 0.9), spec.get("lift", 0.0))

    frames = spec.get("frames", [1])
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
    return n


def main():
    args = parse_args()
    base = os.path.dirname(os.path.abspath(args["manifest"]))
    with open(args["manifest"], "r", encoding="utf-8") as f:
        mf = json.load(f)
    dfl = mf.get("defaults", {})
    size = args["size"] or dfl.get("size", 128)
    dirs = dfl.get("dirs", ["SE", "SW", "NE", "NW"])
    out_dir = os.path.abspath(args["out"])
    os.makedirs(out_dir, exist_ok=True)

    entries = {}
    for group in ("units", "buildings"):
        entries.update(mf.get(group, {}))

    total = 0
    for ident, spec in entries.items():
        if args["only"] and ident not in args["only"]:
            continue
        total += render_entry(ident, spec, base, out_dir, size, dirs)
    print(f"\n共 {total} 帧 -> {out_dir}")
    print("下一步: python postprocess.py " + out_dir)


if __name__ == "__main__":
    main()
